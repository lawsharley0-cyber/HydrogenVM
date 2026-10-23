import Darwin
import CoreMotion
import SwiftUI
import UIKit

/// Owns one emulation session for the Run screen.
@Observable
@MainActor
final class RunModel {
    struct LogLine: Identifiable {
        let id: Int
        let text: String
        let level: HVMLogLevel
    }

    let app: InstalledApp
    private(set) var frame: CGImage?
    /// GPU-rendered GL drawable, shown under `frame` (nil on the software renderer).
    private(set) var glLayer: CALayer?
    private(set) var screenSize = CGSize(width: 320, height: 480)
    private(set) var log: [LogLine] = []
    private(set) var running = false
    private(set) var fps = 0
    private(set) var orientation = GuestOrientation.portrait
    private(set) var activity = PlayerActivity()
    private(set) var frameTimeP95 = 0.0
    var volume: Double = 1 { didSet { session?.outputVolume = Float(volume) } }
    var inputEnabled: Bool { running && frame != nil && !closing && activity.acceptsInput }
    @ObservationIgnored private var activityTimer: Timer?
    @ObservationIgnored private var activeTouches: [Int: CGPoint] = [:]

    private func cancelActiveTouches() {
        for (finger, point) in activeTouches {
            session?.touch(.cancelled, x: Float(point.x), y: Float(point.y), finger: UInt(finger))
        }
        activeTouches.removeAll()
    }

    func togglePause() {
        guard running, !closing else { return }
        cancelActiveTouches()
        activity.userPaused.toggle()
        reconcileActivity()
    }

    private func reconcileActivity() {
        guard let session else { return }
        guard !closing else { return }
        let command = activity.reconcile(ready: frame != nil, backgrounded: session.isBackgrounded, running: running)
        switch command {
        case .background: session.enterBackground(); stopSensors()
        case .foreground: session.enterForeground()
        case nil: break
        }
        if inputEnabled && !motion.isAccelerometerActive { startSensors() }
    }

    var crashReport: String?
    var savedCrash: CrashReport?
    var reportSaveError: String?
    @ObservationIgnored private var memoryTimer: Timer?
    @ObservationIgnored private var memorySamples: [[String: Any]] = []
    var exitMessage: String?
    /// Set when the session ended because the user pressed Stop.
    private(set) var finished = false
    /// The app was asked to close and is saving.
    private(set) var closing = false
    @ObservationIgnored private var userStopped = false
    @ObservationIgnored private var lifecycleObservers: [NSObjectProtocol] = []
    @ObservationIgnored private var backgroundTask: UIBackgroundTaskIdentifier = .invalid

    @ObservationIgnored private var session: HVMSession?
    @ObservationIgnored private let motion = CMMotionManager()
    @ObservationIgnored private var nextLogID = 0
    @ObservationIgnored private var frameCount = 0
    @ObservationIgnored private var lastPresented: UInt64 = 0
    @ObservationIgnored private var fpsTimer: Timer?

    init(app: InstalledApp) { self.app = app }

    /// CI hook: HVM_AUTORUN=<bundle id> launches that app at startup (see LibraryView);
    /// in that mode the guest log is mirrored to stdout and HVM_AUTOTAP="x,y@t;x,y@t"
    /// taps the screen t seconds after the first frame.
    static let automation = ProcessInfo.processInfo.environment["HVM_AUTORUN"] != nil
    @ObservationIgnored private var tapsScheduled = false

    /// Unbuffered write to stdout (captured by `simctl launch --console-pty`).
    nonisolated static func console(_ line: String) {
        FileHandle.standardOutput.write(Data((line + "\n").utf8))
    }

    private func scheduleAutoTaps() {
        guard Self.automation, !tapsScheduled,
              let spec = ProcessInfo.processInfo.environment["HVM_AUTOTAP"], !spec.isEmpty else { return }
        tapsScheduled = true
        for item in spec.split(separator: ";") {
            let parts = item.split(separator: "@")
            let xy = parts.first?.split(separator: ",").compactMap { Double($0) } ?? []
            guard xy.count == 2, parts.count == 2, let t = Double(parts[1]) else { continue }
            let p = CGPoint(x: xy[0], y: xy[1])
            Task { @MainActor [weak self] in
                try? await Task.sleep(for: .seconds(t))
                Self.console("[autotap] \(p.x),\(p.y)")
                self?.touch(.began, at: p, finger: 0)
                try? await Task.sleep(for: .milliseconds(50))
                self?.touch(.ended, at: p, finger: 0)
            }
        }
    }

    func start(keepGoing: Bool) {
        guard session == nil else { return }
        let s = HVMSession(bundlePath: app.info.bundlePath, homePath: app.homeURL.path(percentEncoded: false))
        s.keepGoing = keepGoing
        // GPU rendering unless the player turned it off (Settings: "softwareRenderer").
        s.useGPU = !UserDefaults.standard.bool(forKey: "softwareRenderer")
        // The bridge delivers every callback on the main queue.
        s.onFrame = { [weak self] image in
            MainActor.assumeIsolated {
                guard let self else { return }
                self.frame = image
                self.orientation = GuestOrientation(rawValue: self.session?.interfaceOrientation ?? 1) ?? .portrait
                self.frameCount += 1
                self.scheduleAutoTaps()
                if let size = self.session?.screenSize, size != self.screenSize { self.screenSize = size }
            }
        }
        s.onGLLayer = { [weak self] layer in
            MainActor.assumeIsolated {
                Self.console("[gpu] GL layer \(layer.map { "\($0.bounds.size)" } ?? "removed")")
                self?.glLayer = layer
            }
        }
        s.onLog = { [weak self] line, level in
            MainActor.assumeIsolated { self?.append(line, level) }
        }
        s.onExit = { [weak self] code, crash in
            MainActor.assumeIsolated {
                guard let self else { return }
                self.running = false
                self.closing = false
                self.cancelActiveTouches()
                self.stopSensors()
                self.activityTimer?.invalidate()
                self.fpsTimer?.invalidate()
                self.reconcileActivity()
                UIApplication.shared.isIdleTimerDisabled = false
                if Self.automation {
                    Self.console("[hvm-exit] status=\(code)\(crash.map { " crash:\n" + $0 } ?? "")")
                }
                self.endBackgroundTask()
                self.memoryTimer?.invalidate()
                if let crash {
                    var fields = self.session?.crashBundle.flatMap { $0.data(using: .utf8) }
                        .flatMap { try? JSONSerialization.jsonObject(with: $0) } as? [String: Any] ?? [:]
                    fields["summary"] = fields["summary"] ?? crash.components(separatedBy: "\n").first
                    fields["report"] = crash
                    fields["bundleId"] = self.app.info.bundleIdentifier
                    fields["memorySamples"] = self.memorySamples
                    do {
                        self.savedCrash = try CrashReport.save(fields, app: self.app.info.bundleIdentifier)
                        CrashReport.clearPending()
                    } catch {
                        self.reportSaveError = "Could not save the crash report: \(error.localizedDescription)"
                        self.crashReport = crash
                    }
                } else {
                    CrashReport.clearPending()
                    if self.userStopped { self.finished = true }
                    else { self.exitMessage = "The app exited (status \(code))." }
                }
            }
        }
        session = s
        sampleMemory()
        HVMCrashJournal.openSignalFile(CrashReport.signal.path)
        memoryTimer = Timer.scheduledTimer(withTimeInterval: 5, repeats: true) { [weak self] _ in
            MainActor.assumeIsolated { self?.sampleMemory() }
        }
        running = true
        activity.hostBackgrounded = UIApplication.shared.applicationState == .background
        s.outputVolume = Float(volume)
        s.start()
        activityTimer = Timer.scheduledTimer(withTimeInterval: 0.1, repeats: true) { [weak self] _ in
            MainActor.assumeIsolated { self?.reconcileActivity() }
        }
        observeHostLifecycle()
        UIApplication.shared.isIdleTimerDisabled = true
        startSensors()
        fpsTimer = Timer.scheduledTimer(withTimeInterval: 1, repeats: true) { [weak self] _ in
            MainActor.assumeIsolated {
                guard let self else { return }
                self.frameTimeP95 = self.session?.frameTimeP95Milliseconds ?? 0
                // GPU-presented games show frames without a new UIKit composite,
                // so count what the engine presented rather than delivered images.
                let presented = self.session?.framesPresented ?? 0
                self.fps = presented >= self.lastPresented ? max(Int(presented - self.lastPresented), self.frameCount) : self.frameCount
                self.lastPresented = presented
                self.frameCount = 0
            }
        }
    }

    private func sampleMemory() {
        memorySamples.append(["utc": ISO8601DateFormatter().string(from: Date()),
                              "availableBytes": HVMCrashJournal.availableMemory()])
        if memorySamples.count > 120 { memorySamples.removeFirst() }
        let fields: [String: Any] = ["bundleId": app.info.bundleIdentifier, "appVersion": app.info.version,
                                     "deviceModel": CrashReport.deviceModel,
                                     "iOSVersion": UIDevice.current.systemVersion,
                                     "host": ["pageSize": getpagesize()],
                                     "memorySamples": memorySamples,
                                     "logs": log.suffix(2000).map(\.text)]
        do {
            try FileManager.default.createDirectory(at: CrashReport.directory, withIntermediateDirectories: true)
            let data = try JSONSerialization.data(withJSONObject: fields)
            try data.write(to: CrashReport.pending, options: .atomic)
        } catch { reportSaveError = "Could not save the session snapshot: \(error.localizedDescription)" }
    }

    /// Close the app the way iOS would: it gets resign-active / background / terminate and
    /// saves, then stops (onExit follows). The session force-stops a hung app after a few seconds.
    func stop() {
        guard running, !closing else { return }
        cancelActiveTouches()
        userStopped = true
        closing = true
        session?.stopGracefully()
        stopSensors()
    }

    func teardown() {
        stop()
        memoryTimer?.invalidate()
        fpsTimer?.invalidate()
        fpsTimer = nil
        activityTimer?.invalidate()
        activityTimer = nil
        cancelActiveTouches()
        for o in lifecycleObservers { NotificationCenter.default.removeObserver(o) }
        lifecycleObservers.removeAll()
        endBackgroundTask()
        UIApplication.shared.isIdleTimerDisabled = false
    }

    /// HydrogenVM itself going to the background: pass it on so the game saves, and keep
    /// HydrogenVM awake long enough for that (the game is then paused, using no CPU).
    private func observeHostLifecycle() {
        let nc = NotificationCenter.default
        lifecycleObservers.append(nc.addObserver(forName: UIApplication.didEnterBackgroundNotification, object: nil, queue: .main) { [weak self] _ in
            MainActor.assumeIsolated {
                guard let self, self.running else { return }
                self.backgroundTask = UIApplication.shared.beginBackgroundTask(withName: "Save game") { [weak self] in
                    MainActor.assumeIsolated { self?.endBackgroundTask() }
                }
                self.cancelActiveTouches()
                self.activity.hostBackgrounded = true
                self.reconcileActivity()
                self.stopSensors()
                // Saving is quick; give the game a few seconds, then let iOS suspend us.
                Task { @MainActor [weak self] in
                    try? await Task.sleep(for: .seconds(4))
                    self?.endBackgroundTask()
                }
            }
        })
        lifecycleObservers.append(nc.addObserver(forName: UIApplication.willEnterForegroundNotification, object: nil, queue: .main) { [weak self] _ in
            MainActor.assumeIsolated {
                guard let self, self.running, !self.closing else { return }
                self.endBackgroundTask()
                self.activity.hostBackgrounded = false
                self.reconcileActivity()
            }
        })
        lifecycleObservers.append(nc.addObserver(forName: UIApplication.willTerminateNotification, object: nil, queue: .main) { [weak self] _ in
            MainActor.assumeIsolated {
                guard let self, self.running else { return }
                // HydrogenVM is being quit: save what we can in the little time iOS allows.
                self.session?.stopGracefully()
                let deadline = Date().addingTimeInterval(1.5)
                while self.running && Date() < deadline { RunLoop.current.run(until: Date().addingTimeInterval(0.05)) }
            }
        })
    }

    private func endBackgroundTask() {
        guard backgroundTask != .invalid else { return }
        UIApplication.shared.endBackgroundTask(backgroundTask)
        backgroundTask = .invalid
    }

    func touch(_ phase: HVMTouchPhase, at p: CGPoint, finger: Int) {
        guard phase == .cancelled || inputEnabled else { return }
        guard phase == .began || activeTouches[finger] != nil else { return }
        if phase == .ended || phase == .cancelled { activeTouches[finger] = nil }
        else { activeTouches[finger] = p }
        session?.touch(phase, x: Float(p.x), y: Float(p.y), finger: UInt(finger))
    }

    private func append(_ line: String, _ level: HVMLogLevel) {
        if Self.automation { Self.console(line) }
        var parts = line.split(separator: "\n", omittingEmptySubsequences: false)
        if parts.count > 1, parts.last?.isEmpty == true { parts.removeLast() }
        for part in parts {
            log.append(LogLine(id: nextLogID, text: String(part), level: level))
            nextLogID += 1
        }
        if log.count > 2000 { log.removeFirst(log.count - 1500) }
    }

    var logText: String { log.map(\.text).joined(separator: "\n") }

    private func startSensors() {
        guard inputEnabled, motion.isAccelerometerAvailable, !motion.isAccelerometerActive else { return }
        motion.accelerometerUpdateInterval = 1.0 / 60
        motion.startAccelerometerUpdates(to: .main) { [weak self] data, _ in
            guard let a = data?.acceleration else { return }
            MainActor.assumeIsolated {
                guard let self, self.inputEnabled else { return }
                let scene = UIApplication.shared.connectedScenes.compactMap { $0 as? UIWindowScene }
                    .first { $0.activationState == .foregroundActive }
                let host = GuestOrientation(rawValue: scene?.effectiveGeometry.interfaceOrientation.rawValue ?? 1) ?? .portrait
                let mapped = self.orientation.acceleration(x: a.x, y: a.y, z: a.z, host: host)
                self.session?.setAccelerationX(mapped.0, y: mapped.1, z: mapped.2)
            }
        }
    }

    private func stopSensors() { motion.stopAccelerometerUpdates() }
}

struct RunView: View {
    let app: InstalledApp
    let keepGoing: Bool
    @State private var model: RunModel
    @State private var showLog = false
    @State private var hudExpanded = false
    @State private var inspectAfterStop = false
    @State private var showFiles = false
    @State private var confirmInspect = false
    @State private var overrideOrientation: GuestOrientation?
    @AppStorage("player.scaleMode") private var scaleSetting = PlayerScaleMode.fit.rawValue
    @AppStorage("player.filter") private var filterSetting = PlayerFilter.linear.rawValue
    /// Read by the session at start (RunModel.start): GPU rendering unless this is on.
    @AppStorage("softwareRenderer") private var softwareRenderer = false
    @Environment(\.dismiss) private var dismiss

    init(app: InstalledApp, keepGoing: Bool) {
        self.app = app
        self.keepGoing = keepGoing
        _model = State(initialValue: RunModel(app: app))
    }

    private var effectiveOrientation: GuestOrientation {
        overrideOrientation ?? model.orientation
    }

    var body: some View {
        ZStack(alignment: .topTrailing) {
            Color.black.ignoresSafeArea()

            // Viewport & Game Canvas
            GeometryReader { geo in
                ZStack {
                    EmulatorScreen(image: model.frame, glLayer: model.glLayer, guestSize: model.screenSize,
                                   orientation: effectiveOrientation,
                                   mode: PlayerScaleMode(rawValue: scaleSetting) ?? .fit,
                                   filter: PlayerFilter(rawValue: filterSetting) ?? .linear,
                                   inputEnabled: model.inputEnabled) { phase, point, finger in
                        model.touch(phase, at: point, finger: finger)
                    }
                    if model.closing {
                        VStack(spacing: 12) {
                            ProgressView().tint(.white)
                            Text("Saving \(app.title)…").foregroundStyle(.white).font(.callout)
                        }
                        .padding(20)
                        .background(.black.opacity(0.6), in: RoundedRectangle(cornerRadius: 14))
                    } else if model.frame == nil && model.running {
                        VStack(spacing: 12) {
                            ProgressView().tint(.white)
                            Text("Starting \(app.title)…").foregroundStyle(.white.opacity(0.8)).font(.callout)
                        }
                    }
                }
                .frame(width: geo.size.width, height: geo.size.height)
            }

            // Bottom Log overlay if toggled
            if showLog {
                VStack {
                    Spacer()
                    LogPanel(lines: model.log, copyText: { model.logText })
                        .frame(height: 220)
                }
            }

            // Collapsible Floating Glassmorphic In-Game HUD Pill in Top Corner
            floatingHUDPill
                .padding(.top, 16)
                .padding(.trailing, 16)
        }
        .statusBarHidden(true)
        .preferredColorScheme(.dark)
        .persistentSystemOverlays(.hidden)
        .onAppear {
            OrientationLock.shared.lock(effectiveOrientation.mask)
            model.start(keepGoing: keepGoing)
        }
        .onChange(of: effectiveOrientation) { _, orientation in OrientationLock.shared.lock(orientation.mask) }
        .onChange(of: model.finished) { _, done in
            if done {
                OrientationLock.shared.lock(.all)
                if inspectAfterStop { showFiles = true }
                else { dismiss() }
            }
        }
        .sheet(isPresented: $showFiles, onDismiss: { if model.finished { dismiss() } }) {
            FileBrowserView(app: app)
        }
        .confirmationDialog("Save and stop the game to inspect files?", isPresented: $confirmInspect, titleVisibility: .visible) {
            Button("Save, Stop & Inspect") {
                inspectAfterStop = true
                if model.running { model.stop() } else { showFiles = true }
            }
        } message: {
            Text("The game closes before files or snapshots can change. Relaunch it from the Library after editing.")
        }
        .onDisappear {
            model.teardown()
            OrientationLock.shared.lock(.all)
        }
        .sheet(item: $model.savedCrash) { CrashReportView(report: $0) }
        .alert("\(app.title) crashed", isPresented: Binding(get: { model.crashReport != nil }, set: { if !$0 { model.crashReport = nil } })) {
            Button("Copy Report") { UIPasteboard.general.string = (model.crashReport ?? "") + "\n\n--- log ---\n" + model.logText; dismiss() }
            Button("Show Log") { showLog = true }
            Button("Close", role: .cancel) { dismiss() }
        } message: {
            Text((model.reportSaveError ?? "") + "\n" + String((model.crashReport ?? "").prefix(600)))
        }
        .alert(model.exitMessage ?? "", isPresented: Binding(get: { model.exitMessage != nil }, set: { if !$0 { model.exitMessage = nil } })) {
            Button("Show Log") { showLog = true }
            Button("Close", role: .cancel) { dismiss() }
        }
    }

    // MARK: - Floating Glassmorphic In-Game HUD Pill

    private var floatingHUDPill: some View {
        Group {
            if hudExpanded {
                VStack(alignment: .leading, spacing: 14) {
                    // Header Bar with Title, FPS & Frame Time, Collapse Button
                    HStack(spacing: 8) {
                        Image(systemName: "gamecontroller.fill")
                            .foregroundStyle(.tint)
                        Text(app.title)
                            .font(.subheadline.weight(.semibold))
                            .foregroundStyle(.white)
                            .lineLimit(1)

                        Spacer(minLength: 8)

                        // FPS & Frame-time Badge
                        HStack(spacing: 4) {
                            Text("\(model.fps) FPS")
                                .font(.caption2.monospacedDigit().weight(.bold))
                                .foregroundStyle(model.fps >= 30 ? .green : .yellow)
                            if model.frameTimeP95 > 0 {
                                Text("(\(String(format: "%.1f", model.frameTimeP95))ms)")
                                    .font(.caption2.monospacedDigit())
                                    .foregroundStyle(.secondary)
                            }
                        }
                        .padding(.horizontal, 6)
                        .padding(.vertical, 2)
                        .background(.black.opacity(0.4), in: Capsule())

                        Button {
                            withAnimation(.spring(response: 0.35, dampingFraction: 0.8)) {
                                hudExpanded = false
                            }
                        } label: {
                            Image(systemName: "chevron.up.circle.fill")
                                .font(.title3)
                                .foregroundStyle(.white.opacity(0.8))
                        }
                        .accessibilityIdentifier("hud-minimize")
                        .accessibilityLabel("Minimize Controls")
                    }

                    Divider().background(.white.opacity(0.2))

                    // Primary Action Controls
                    HStack(spacing: 12) {
                        // Pause / Resume Toggle
                        Button {
                            model.togglePause()
                        } label: {
                            Label(model.activity.userPaused ? "Resume" : "Pause",
                                  systemImage: model.activity.userPaused ? "play.fill" : "pause.fill")
                                .font(.caption.weight(.medium))
                        }
                        .buttonStyle(.borderedProminent)
                        .tint(model.activity.userPaused ? .green : .orange)
                        .controlSize(.small)
                        .disabled(!model.running || model.closing)
                        .accessibilityIdentifier("player-pause")

                        // Manual Orientation Toggle
                        Button {
                            withAnimation {
                                switch effectiveOrientation {
                                case .portrait: overrideOrientation = .landscapeLeft
                                case .landscapeLeft: overrideOrientation = .landscapeRight
                                case .landscapeRight: overrideOrientation = .portrait
                                case .upsideDown: overrideOrientation = .portrait
                                }
                            }
                        } label: {
                            Label(effectiveOrientation.landscape ? "Landscape" : "Portrait",
                                  systemImage: effectiveOrientation.landscape ? "rectangle.landscape.rotate" : "rectangle.portrait.rotate")
                                .font(.caption.weight(.medium))
                        }
                        .buttonStyle(.bordered)
                        .controlSize(.small)

                        Spacer()

                        // Direct Save & Files Inspector Button
                        Button {
                            confirmInspect = true
                        } label: {
                            Label("Save, Stop & Inspect Files", systemImage: "folder.fill")
                                .font(.caption.weight(.semibold))
                        }
                        .buttonStyle(.bordered)
                        .tint(.blue)
                        .controlSize(.small)
                        .disabled(model.closing)
                        .accessibilityIdentifier("player-inspect")
                    }

                    // Display Scaling & Interpolation Settings
                    VStack(alignment: .leading, spacing: 6) {
                        Text("Viewport Scaling & Interpolation")
                            .font(.caption2.weight(.semibold))
                            .foregroundStyle(.secondary)

                        Picker("Scale Mode", selection: $scaleSetting) {
                            ForEach(PlayerScaleMode.allCases) { mode in
                                Text(mode.rawValue).tag(mode.rawValue)
                            }
                        }
                        .pickerStyle(.segmented)

                        Picker("Interpolation", selection: $filterSetting) {
                            ForEach(PlayerFilter.allCases) { filter in
                                Text(filter.rawValue).tag(filter.rawValue)
                            }
                        }
                        .pickerStyle(.segmented)

                        Toggle(isOn: Binding(get: { !softwareRenderer }, set: { softwareRenderer = !$0 })) {
                            Text("GPU rendering (takes effect next launch)")
                                .font(.caption)
                        }
                        .accessibilityIdentifier("player-gpu-rendering")
                    }

                    // Audio & System Footer
                    HStack(spacing: 12) {
                        Button {
                            model.volume = model.volume == 0 ? 1 : 0
                        } label: {
                            Image(systemName: model.volume == 0 ? "speaker.slash.fill" : "speaker.wave.2.fill")
                                .frame(width: 44, height: 44)
                        }
                        .foregroundStyle(.white)
                        .accessibilityLabel(model.volume == 0 ? "Unmute" : "Mute")
                        .accessibilityIdentifier("player-mute")

                        Slider(value: $model.volume, in: 0...1)
                            .tint(.accentColor)
                            .accessibilityLabel("Game Volume")

                        Button {
                            withAnimation { showLog.toggle() }
                        } label: {
                            Image(systemName: showLog ? "terminal.fill" : "terminal")
                                .foregroundStyle(showLog ? .green : .white)
                        }

                        Button(role: .destructive) {
                            if model.running { model.stop() } else { dismiss() }
                        } label: {
                            Image(systemName: "xmark.circle.fill")
                                .font(.title3)
                                .foregroundStyle(.red)
                        }
                    }
                }
                .padding(14)
                .frame(maxWidth: 360)
                .background(.ultraThinMaterial, in: RoundedRectangle(cornerRadius: 18, style: .continuous))
                .overlay(
                    RoundedRectangle(cornerRadius: 18, style: .continuous)
                        .stroke(.white.opacity(0.2), lineWidth: 1)
                )
                .shadow(color: .black.opacity(0.4), radius: 12, x: 0, y: 6)
                .transition(.scale(scale: 0.9, anchor: .topTrailing).combined(with: .opacity))
            } else {
                // Minimized HUD Pill Handle
                Button {
                    withAnimation(.spring(response: 0.35, dampingFraction: 0.8)) {
                        hudExpanded = true
                    }
                } label: {
                    HStack(spacing: 8) {
                        Image(systemName: "slider.horizontal.3")
                            .font(.caption.weight(.bold))
                            .foregroundStyle(.white)

                        Text("\(model.fps) FPS")
                            .font(.caption2.monospacedDigit().weight(.bold))
                            .foregroundStyle(model.fps >= 30 ? .green : .yellow)

                        if model.activity.userPaused {
                            Text("PAUSED")
                                .font(.system(size: 9, weight: .bold))
                                .padding(.horizontal, 4)
                                .padding(.vertical, 1)
                                .background(Color.yellow, in: Capsule())
                                .foregroundStyle(.black)
                        }
                    }
                    .padding(.horizontal, 12)
                    .padding(.vertical, 7)
                    .background(.ultraThinMaterial, in: Capsule())
                    .overlay(
                        Capsule().stroke(.white.opacity(0.25), lineWidth: 1)
                    )
                    .shadow(color: .black.opacity(0.3), radius: 6, x: 0, y: 3)
                }
                .accessibilityIdentifier("player-controls")
                .accessibilityLabel("Player Controls")
                .transition(.scale(scale: 0.9, anchor: .topTrailing).combined(with: .opacity))
            }
        }
    }

    private var activityLabel: String {
        if model.closing { return "Saving…" }
        switch model.activity.phase {
        case .active: return "Running"
        case .pausing: return "Pausing…"
        case .paused: return "Paused"
        case .resuming: return "Resuming…"
        case .stopped: return "Stopped"
        }
    }

}

struct LogPanel: View {
    let lines: [RunModel.LogLine]
    let copyText: () -> String

    var body: some View {
        VStack(spacing: 0) {
            HStack {
                Text("Log").font(.caption.weight(.semibold))
                Spacer()
                Button("Copy", systemImage: "doc.on.doc") { UIPasteboard.general.string = copyText() }
                    .labelStyle(.iconOnly).font(.caption)
            }
            .padding(.horizontal, 12).padding(.vertical, 6)
            .foregroundStyle(.white)
            ScrollViewReader { proxy in
                ScrollView {
                    LazyVStack(alignment: .leading, spacing: 1) {
                        ForEach(lines) { line in
                            Text(line.text)
                                .font(.system(size: 11, design: .monospaced))
                                .foregroundStyle(color(line.level))
                                .frame(maxWidth: .infinity, alignment: .leading)
                                .id(line.id)
                        }
                    }
                    .padding(.horizontal, 12)
                    .textSelection(.enabled)
                }
                .onChange(of: lines.last?.id) { _, id in if let id { proxy.scrollTo(id, anchor: .bottom) } }
            }
        }
        .background(Color(white: 0.08))
    }

    private func color(_ l: HVMLogLevel) -> Color {
        switch l {
        case .error, .stderr: .red
        case .warning: .orange
        case .debug: .gray
        case .stdout: .green
        default: .white
        }
    }
}
