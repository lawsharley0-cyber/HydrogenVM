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
        s.onLog = { [weak self] line, level in
            MainActor.assumeIsolated { self?.append(line, level) }
        }
        s.onExit = { [weak self] code, crash in
            MainActor.assumeIsolated {
                guard let self else { return }
                self.running = false
                self.cancelActiveTouches()
                self.stopSensors()
                self.activityTimer?.invalidate()
                self.fpsTimer?.invalidate()
                self.reconcileActivity()
                UIApplication.shared.isIdleTimerDisabled = false
                if Self.automation {
                    Self.console("[hvm-exit] status=\(code)\(crash.map { " crash:\n" + $0 } ?? "")")
                }
                self.closing = false
                self.endBackgroundTask()
                if self.userStopped { self.finished = true }
                else if let crash { self.crashReport = crash }
                else { self.exitMessage = "The app exited (status \(code))." }
            }
        }
        session = s
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
                self.fps = self.frameCount
                self.frameCount = 0
            }
        }
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
    @State private var showControls = false
    @State private var inspectAfterStop = false
    @State private var showFiles = false
    @State private var confirmInspect = false
    @AppStorage("player.scaleMode") private var scaleSetting = PlayerScaleMode.fit.rawValue
    @AppStorage("player.filter") private var filterSetting = PlayerFilter.linear.rawValue
    @Environment(\.dismiss) private var dismiss

    init(app: InstalledApp, keepGoing: Bool) {
        self.app = app
        self.keepGoing = keepGoing
        _model = State(initialValue: RunModel(app: app))
    }

    var body: some View {
        ZStack(alignment: .top) {
            Color.black.ignoresSafeArea()
            VStack(spacing: 0) {
                toolbar
                GeometryReader { geo in
                    ZStack {
                        EmulatorScreen(image: model.frame, guestSize: model.screenSize,
                                       orientation: model.orientation,
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
                if showControls { controls }
                if showLog { LogPanel(lines: model.log, copyText: { model.logText }).frame(height: 220) }
            }
        }
        .statusBarHidden(true)
        .persistentSystemOverlays(.hidden)
        .onAppear {
            OrientationLock.shared.lock(model.orientation.mask)
            model.start(keepGoing: keepGoing)
        }
        .onChange(of: model.orientation) { _, orientation in OrientationLock.shared.lock(orientation.mask) }
        .onChange(of: model.finished) { _, done in
            if done {
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
        .alert("\(app.title) crashed", isPresented: Binding(get: { model.crashReport != nil }, set: { if !$0 { model.crashReport = nil } })) {
            Button("Copy Report") { UIPasteboard.general.string = (model.crashReport ?? "") + "\n\n--- log ---\n" + model.logText; dismiss() }
            Button("Show Log") { showLog = true }
            Button("Close", role: .cancel) { dismiss() }
        } message: {
            Text(String((model.crashReport ?? "").prefix(600)))
        }
        .alert(model.exitMessage ?? "", isPresented: Binding(get: { model.exitMessage != nil }, set: { if !$0 { model.exitMessage = nil } })) {
            Button("Show Log") { showLog = true }
            Button("Close", role: .cancel) { dismiss() }
        }
    }

    private var toolbar: some View {
        HStack(spacing: 16) {
            Button { if model.running { model.stop() } else { dismiss() } } label: {
                Label(model.running ? "Stop" : "Close", systemImage: model.running ? "stop.fill" : "xmark")
            }
            .disabled(model.closing)
            Spacer()
            Text(app.title).font(.subheadline.weight(.semibold)).lineLimit(1)
            if model.activity.userPaused { Text("Paused").font(.caption).foregroundStyle(.yellow) }
            if model.running { Text("\(model.fps) fps").font(.caption.monospacedDigit()).foregroundStyle(.secondary) }
            Spacer()
            Button { withAnimation { showControls.toggle() } } label: {
                Label("Player Controls", systemImage: "slider.horizontal.3")
            }
            .accessibilityIdentifier("player-controls")
            Button { withAnimation { showLog.toggle() } } label: {
                Label("Log", systemImage: showLog ? "text.alignleft" : "text.justify.left")
            }
        }
        .labelStyle(.iconOnly)
        .font(.title3)
        .foregroundStyle(.white)
        .padding(.horizontal, 20)
        .padding(.vertical, 8)
    }

    private var controls: some View {
        ScrollView {
            VStack(spacing: 12) {
                HStack {
                    Button(model.activity.userPaused ? "Resume" : "Pause",
                           systemImage: model.activity.userPaused ? "play.fill" : "pause.fill") { model.togglePause() }
                        .disabled(!model.running || model.closing)
                        .accessibilityIdentifier("player-pause")
                    Text(activityLabel).font(.caption)
                    Spacer()
                    Text(model.frameTimeP95 > 0 ? String(format: "work p95 %.1f ms", model.frameTimeP95) : "work p95 —")
                        .font(.caption.monospacedDigit())
                }
                Picker("Display", selection: $scaleSetting) {
                    ForEach(PlayerScaleMode.allCases) { Text($0.rawValue).tag($0.rawValue) }
                }.pickerStyle(.segmented)
                Picker("Filtering", selection: $filterSetting) {
                    ForEach(PlayerFilter.allCases) { Text($0.rawValue).tag($0.rawValue) }
                }.pickerStyle(.segmented)
                if let error = OrientationLock.shared.errorMessage {
                    Text("Display rotation: \(error)").font(.caption).foregroundStyle(.orange)
                }
                HStack {
                    Image(systemName: model.volume == 0 ? "speaker.slash.fill" : "speaker.wave.2.fill")
                    Slider(value: $model.volume, in: 0...1).accessibilityLabel("Game Volume")
                    Button(model.volume == 0 ? "Unmute" : "Mute") { model.volume = model.volume == 0 ? 1 : 0 }
                }
                Button("Save, Stop & Inspect Files", systemImage: "folder") { confirmInspect = true }
                    .disabled(model.closing)
                    .accessibilityIdentifier("player-inspect")
            }
            .padding()
        }
        .frame(maxHeight: 210)
        .background(Color(white: 0.1))
        .foregroundStyle(.white)
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
