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
                self.stopSensors()
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
        s.start()
        observeHostLifecycle()
        UIApplication.shared.isIdleTimerDisabled = true
        startSensors()
        fpsTimer = Timer.scheduledTimer(withTimeInterval: 1, repeats: true) { [weak self] _ in
            MainActor.assumeIsolated {
                guard let self else { return }
                self.fps = self.frameCount
                self.frameCount = 0
            }
        }
    }

    /// Close the app the way iOS would: it gets resign-active / background / terminate and
    /// saves, then stops (onExit follows). The session force-stops a hung app after a few seconds.
    func stop() {
        guard running, !closing else { return }
        userStopped = true
        closing = true
        session?.stopGracefully()
        stopSensors()
    }

    func teardown() {
        stop()
        fpsTimer?.invalidate()
        fpsTimer = nil
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
                self.session?.enterBackground()
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
                self.session?.enterForeground()
                self.startSensors()
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
        guard motion.isAccelerometerAvailable else { return }
        motion.accelerometerUpdateInterval = 1.0 / 60
        motion.startAccelerometerUpdates(to: .main) { [weak self] data, _ in
            guard let a = data?.acceleration else { return }
            MainActor.assumeIsolated { self?.session?.setAccelerationX(a.x, y: a.y, z: a.z) }
        }
    }

    private func stopSensors() { motion.stopAccelerometerUpdates() }
}

struct RunView: View {
    let app: InstalledApp
    let keepGoing: Bool
    @State private var model: RunModel
    @State private var showLog = false
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
                    let fit = fittedRect(in: geo.size)
                    ZStack {
                        EmulatorScreen(image: model.frame, guestSize: model.screenSize) { phase, point, finger in
                            model.touch(phase, at: point, finger: finger)
                        }
                        .frame(width: fit.width, height: fit.height)
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
                if showLog { LogPanel(lines: model.log, copyText: { model.logText }).frame(height: 220) }
            }
        }
        .statusBarHidden(true)
        .persistentSystemOverlays(.hidden)
        .onAppear {
            OrientationLock.shared.lock(.portrait)
            model.start(keepGoing: keepGoing)
        }
        .onChange(of: model.finished) { _, done in if done { dismiss() } }
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
            if model.running { Text("\(model.fps) fps").font(.caption.monospacedDigit()).foregroundStyle(.secondary) }
            Spacer()
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

    private func fittedRect(in size: CGSize) -> CGSize {
        let g = model.screenSize
        guard g.width > 0, g.height > 0, size.width > 0, size.height > 0 else { return size }
        let s = min(size.width / g.width, size.height / g.height)
        return CGSize(width: (g.width * s).rounded(.down), height: (g.height * s).rounded(.down))
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

/// UIKit view that shows frames and forwards multi-touch in guest points.
struct EmulatorScreen: UIViewRepresentable {
    let image: CGImage?
    let guestSize: CGSize
    let onTouch: (HVMTouchPhase, CGPoint, Int) -> Void

    func makeUIView(context: Context) -> ScreenView {
        let v = ScreenView()
        v.onTouch = onTouch
        return v
    }

    func updateUIView(_ v: ScreenView, context: Context) {
        v.onTouch = onTouch
        v.guestSize = guestSize
        v.layer.contents = image
    }

    final class ScreenView: UIView {
        var onTouch: ((HVMTouchPhase, CGPoint, Int) -> Void)?
        var guestSize = CGSize(width: 320, height: 480)
        private var fingers: [ObjectIdentifier: Int] = [:]

        override init(frame: CGRect) {
            super.init(frame: frame)
            isMultipleTouchEnabled = true
            backgroundColor = .black
            layer.contentsGravity = .resize
            layer.magnificationFilter = .linear
            layer.minificationFilter = .trilinear
        }
        required init?(coder: NSCoder) { fatalError() }

        private func guestPoint(_ t: UITouch) -> CGPoint {
            let p = t.location(in: self)
            guard bounds.width > 0, bounds.height > 0 else { return .zero }
            return CGPoint(x: min(max(p.x / bounds.width * guestSize.width, 0), guestSize.width - 0.5),
                           y: min(max(p.y / bounds.height * guestSize.height, 0), guestSize.height - 0.5))
        }

        private func send(_ touches: Set<UITouch>, _ phase: HVMTouchPhase) {
            for t in touches {
                let key = ObjectIdentifier(t)
                let finger: Int
                if let f = fingers[key] { finger = f } else {
                    finger = (0...10).first { !fingers.values.contains($0) } ?? fingers.count
                    fingers[key] = finger
                }
                onTouch?(phase, guestPoint(t), finger)
                if phase == .ended || phase == .cancelled { fingers[key] = nil }
            }
        }

        override func touchesBegan(_ touches: Set<UITouch>, with event: UIEvent?) { send(touches, .began) }
        override func touchesMoved(_ touches: Set<UITouch>, with event: UIEvent?) { send(touches, .moved) }
        override func touchesEnded(_ touches: Set<UITouch>, with event: UIEvent?) { send(touches, .ended) }
        override func touchesCancelled(_ touches: Set<UITouch>, with event: UIEvent?) { send(touches, .cancelled) }
    }
}
