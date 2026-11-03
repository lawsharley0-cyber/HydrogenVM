import Foundation
import CoreGraphics
import UIKit

enum BatchRendererMode: String, Codable, CaseIterable, Identifiable, Sendable {
    case software = "software"
    case gpu = "gpu"
    case both = "both"

    var id: String { rawValue }

    var displayName: String {
        switch self {
        case .software: return "Software"
        case .gpu: return "GPU"
        case .both: return "Both (Software & GPU)"
        }
    }
}

struct BatchTestOptions: Codable, Sendable {
    var secondsPerApp: Int = 10
    var rendererMode: BatchRendererMode = .both
    var keepGoing: Bool = true
    var resetSandbox: Bool = false
    var exploreEnabled: Bool = true
    var exploreTime: Int = 30
    /// Start the game a second time on the data the first run saved (catches save-file crashes).
    var relaunchEnabled: Bool = true
    var relaunchTime: Int = 12

    init(secondsPerApp: Int = 10, rendererMode: BatchRendererMode = .both, keepGoing: Bool = true, resetSandbox: Bool = false, exploreEnabled: Bool = true, exploreTime: Int = 30, relaunchEnabled: Bool = true) {
        self.secondsPerApp = secondsPerApp
        self.rendererMode = rendererMode
        self.keepGoing = keepGoing
        self.resetSandbox = resetSandbox
        self.exploreEnabled = exploreEnabled
        self.exploreTime = exploreTime
        self.relaunchEnabled = relaunchEnabled
    }

    // Saved sessions from older builds lack the newer keys.
    init(from decoder: Decoder) throws {
        let c = try decoder.container(keyedBy: CodingKeys.self)
        secondsPerApp = try c.decodeIfPresent(Int.self, forKey: .secondsPerApp) ?? 10
        rendererMode = try c.decodeIfPresent(BatchRendererMode.self, forKey: .rendererMode) ?? .both
        keepGoing = try c.decodeIfPresent(Bool.self, forKey: .keepGoing) ?? true
        resetSandbox = try c.decodeIfPresent(Bool.self, forKey: .resetSandbox) ?? false
        exploreEnabled = try c.decodeIfPresent(Bool.self, forKey: .exploreEnabled) ?? true
        exploreTime = try c.decodeIfPresent(Int.self, forKey: .exploreTime) ?? 30
        relaunchEnabled = try c.decodeIfPresent(Bool.self, forKey: .relaunchEnabled) ?? true
        relaunchTime = try c.decodeIfPresent(Int.self, forKey: .relaunchTime) ?? 12
    }
}

enum FrameSignal: String, Codable, Sendable {
    case ok = "ok"
    case allBlack = "all-black"
    case allWhite = "all-white"
    case noFrames = "no-frames"
}

struct BatchPerf: Codable, Sendable {
    var framesPresented: Int
    var fps: Int
    var frameTimeP95Ms: Double

    init(framesPresented: Int = 0, fps: Int = 0, frameTimeP95Ms: Double = 0.0) {
        self.framesPresented = framesPresented
        self.fps = fps
        self.frameTimeP95Ms = frameTimeP95Ms
    }
}

struct BatchFrameInfo: Codable, Sendable {
    var signal: String
    var thumbnailPath: String?

    init(signal: String = "no-frames", thumbnailPath: String? = nil) {
        self.signal = signal
        self.thumbnailPath = thumbnailPath
    }
}

struct BatchAppResult: Codable, Identifiable, Sendable {
    var id: String { "\(bundleId)_\(mode)" }
    var bundleId: String
    var name: String
    var version: String
    var minOS: String
    var mode: String // "software" or "gpu"
    var crashed: Bool
    var exitCode: Int
    var summary: String
    var signature: String?
    var exceptionKind: String?
    var crashReport: String?
    var perf: BatchPerf
    var frame: BatchFrameInfo
    var keepGoing: Bool
    var logs: String
    /// The last files the game opened (path, result, size, bytes read), JSON, from the crash bundle.
    var fileHistory: String? = nil
    var explore: ExploreResult?
    /// The checklist (launch, picture, music, sound effects, touch, resume, close, relaunch…).
    var checks: [BatchCheck]? = nil
    /// Last engine counters (audio mixer, off-screen frames, missing APIs).
    var diagnostics: BatchDiagnostics? = nil

    var worstCheck: BatchCheck.Status? {
        guard let checks else { return nil }
        if checks.contains(where: { $0.status == .fail }) { return .fail }
        if checks.contains(where: { $0.status == .warn }) { return .warn }
        return .pass
    }

    init(bundleId: String, name: String, version: String, minOS: String, mode: String, crashed: Bool, exitCode: Int, summary: String, signature: String? = nil, exceptionKind: String? = nil, crashReport: String? = nil, perf: BatchPerf = BatchPerf(), frame: BatchFrameInfo = BatchFrameInfo(), keepGoing: Bool = true, logs: String = "", explore: ExploreResult? = nil) {
        self.bundleId = bundleId
        self.name = name
        self.version = version
        self.minOS = minOS
        self.mode = mode
        self.crashed = crashed
        self.exitCode = exitCode
        self.summary = summary
        self.signature = signature
        self.exceptionKind = exceptionKind
        self.crashReport = crashReport
        self.perf = perf
        self.frame = frame
        self.keepGoing = keepGoing
        self.logs = logs
        self.explore = explore
    }
}

struct BatchSessionState: Codable, Sendable {
    var runId: String
    var timestamp: String
    var options: BatchTestOptions
    var completedResults: [BatchAppResult]
    var pendingBundleIds: [String]

    init(runId: String = UUID().uuidString, timestamp: String = ISO8601DateFormatter().string(from: Date()), options: BatchTestOptions = BatchTestOptions(), completedResults: [BatchAppResult] = [], pendingBundleIds: [String] = []) {
        self.runId = runId
        self.timestamp = timestamp
        self.options = options
        self.completedResults = completedResults
        self.pendingBundleIds = pendingBundleIds
    }
}

enum BatchTestStatus: String, Codable, Sendable {
    case idle
    case running
    case paused
    case completed
    case cancelled
}

struct BatchTarget: Sendable {
    let app: InstalledApp
    let useGPU: Bool
    var modeName: String { useGPU ? "gpu" : "software" }
    var targetId: String { "\(app.info.bundleIdentifier)_\(modeName)" }
}

@MainActor
final class FrameAnalyzer {
    static func analyze(_ image: CGImage?) -> FrameSignal {
        guard let image else { return .noFrames }
        let width = image.width
        let height = image.height
        guard width > 0 && height > 0 else { return .noFrames }

        let bytesPerPixel = 4
        let bytesPerRow = width * bytesPerPixel
        var rawData = [UInt8](repeating: 0, count: height * bytesPerRow)

        guard let colorSpace = CGColorSpace(name: CGColorSpace.sRGB),
              let context = CGContext(data: &rawData,
                                      width: width,
                                      height: height,
                                      bitsPerComponent: 8,
                                      bytesPerRow: bytesPerRow,
                                      space: colorSpace,
                                      bitmapInfo: CGImageAlphaInfo.premultipliedLast.rawValue | CGBitmapInfo.byteOrder32Big.rawValue) else {
            return .ok
        }

        context.draw(image, in: CGRect(x: 0, y: 0, width: width, height: height))

        var darkPixels = 0
        var brightPixels = 0
        var sampledPixels = 0

        let stepX = max(1, width / 20)
        let stepY = max(1, height / 20)

        for y in stride(from: 0, to: height, by: stepY) {
            for x in stride(from: 0, to: width, by: stepX) {
                let offset = (y * width + x) * bytesPerPixel
                guard offset + 2 < rawData.count else { continue }
                let r = rawData[offset]
                let g = rawData[offset + 1]
                let b = rawData[offset + 2]

                sampledPixels += 1
                if r <= 15 && g <= 15 && b <= 15 {
                    darkPixels += 1
                }
                if r >= 240 && g >= 240 && b >= 240 {
                    brightPixels += 1
                }
            }
        }

        guard sampledPixels > 0 else { return .ok }

        if darkPixels == sampledPixels {
            return .allBlack
        }
        if brightPixels == sampledPixels {
            return .allWhite
        }

        return .ok
    }

    /// From "GPU: presented frame N directly (average rgb r,g,b, L/64 samples lit)":
    /// the latest sample decides (all dark = black screen).
    static func analyzeGPUSamples(_ lines: [String]) -> FrameSignal {
        guard let last = lines.last(where: { $0.contains("samples lit") }) else { return .ok }
        guard let range = last.range(of: #"(\d+)/64 samples lit"#, options: .regularExpression) else { return .ok }
        let lit = Int(last[range].split(separator: "/").first ?? "") ?? 1
        if lit == 0 { return .allBlack }
        if last.contains("average rgb 255,255,255") { return .allWhite }
        return .ok
    }

    static func savePNG(_ image: CGImage, to url: URL) -> Bool {
        let uiImage = UIImage(cgImage: image)
        guard let data = uiImage.pngData() else { return false }
        do {
            try FileManager.default.createDirectory(at: url.deletingLastPathComponent(), withIntermediateDirectories: true)
            try data.write(to: url, options: .atomic)
            return true
        } catch {
            return false
        }
    }
}

/// Exit state of the session under test, written by its onExit callback.
@MainActor
final class ExitBox {
    var exited = false
    var code: NSInteger = -1
    var crash: String?
    var frames = 0
}

@Observable
@MainActor
final class BatchTestRunner {
    var options = BatchTestOptions()
    private(set) var status: BatchTestStatus = .idle
    private(set) var results: [BatchAppResult] = []
    private(set) var targets: [BatchTarget] = []
    private(set) var currentIndex: Int = 0

    private(set) var currentApp: InstalledApp?
    private(set) var currentModeName: String = "software"
    private(set) var currentFrame: CGImage?
    private(set) var currentFPS: Int = 0
    private(set) var currentLogs: [String] = []
    /// The first lines of the session (loading, missing classes): the tail alone
    /// loses them when an app floods the log while it stops.
    private var headLogs: [String] = []
    var lastLog: String? { currentLogs.last ?? headLogs.last }
    private var logLineCount = 0
    private var lastLogLine = ""
    private var lastLogRepeats = 0

    private func keepLog(_ raw: String) {
        let line = raw.trimmingCharacters(in: .whitespacesAndNewlines)
        guard !line.isEmpty else { return }
        if line == lastLogLine {
            lastLogRepeats += 1
            let folded = "\(lastLogRepeats + 1)× " + line
            if let i = currentLogs.indices.last { currentLogs[i] = folded } else if let i = headLogs.indices.last { headLogs[i] = folded }
            return
        }
        lastLogLine = line
        lastLogRepeats = 0
        logLineCount += 1
        if let r = line.range(of: "Missing: ") {
            let what = String(line[r.upperBound...])
            if missingLines.count < 40 && !missingLines.contains(what) { missingLines.append(what) }
            if what.localizedCaseInsensitiveContains("audio format not supported") || what.localizedCaseInsensitiveContains("can't decode audio") {
                if audioDecodeFailures.count < 5 { audioDecodeFailures.append(what) }
            }
        }
        if headLogs.count < 60 { headLogs.append(line); return }
        currentLogs.append(line)
        if currentLogs.count > 500 { currentLogs.removeFirst() }
    }

    /// The session's first 60 lines and its last 120, repeats folded.
    private func makeLogExcerpt() -> String {
        let tail = Array(currentLogs.suffix(120))
        let skipped = logLineCount - headLogs.count - tail.count
        var lines = headLogs
        if skipped > 0 { lines.append("… \(skipped) lines …") }
        lines += tail
        return lines.joined(separator: "\n")
    }
    private(set) var progressText: String = ""

    @ObservationIgnored private var activeSession: HVMSession?
    @ObservationIgnored private var currentTestTask: Task<Void, Never>?
    @ObservationIgnored private var lastPresentedFrames: UInt64 = 0
    @ObservationIgnored private var frameCount: Int = 0
    @ObservationIgnored private var fpsTimer: Timer?
    @ObservationIgnored private var lastCapturedFrame: CGImage?
    @ObservationIgnored private var gpuSampleLines: [String] = []
    @ObservationIgnored private var lastP95: Double = 0
    @ObservationIgnored private var lastTotalPresented: UInt64 = 0
    /// Engine counters, copied every second while the game runs (they vanish when it stops).
    @ObservationIgnored private var lastDiag: BatchDiagnostics?
    @ObservationIgnored private var missingLines: [String] = []
    @ObservationIgnored private var audioDecodeFailures: [String] = []

    static var reportsDir: URL {
        URL.documentsDirectory.appending(path: "TestReports", directoryHint: .isDirectory)
    }

    static var pendingSessionURL: URL {
        reportsDir.appending(path: "batch_session.json")
    }

    static var thumbnailsDir: URL {
        reportsDir.appending(path: "thumbnails", directoryHint: .isDirectory)
    }

    init() {}

    static func hasPendingSession() -> Bool {
        FileManager.default.fileExists(atPath: pendingSessionURL.path)
    }

    func clearPendingSession() {
        try? FileManager.default.removeItem(at: Self.pendingSessionURL)
    }

    func startNewBatch(apps: [InstalledApp], options: BatchTestOptions) {
        self.options = options
        self.clearPendingSession()
        self.results = []

        var newTargets: [BatchTarget] = []
        for app in apps where app.info.runnable {
            switch options.rendererMode {
            case .software:
                newTargets.append(BatchTarget(app: app, useGPU: false))
            case .gpu:
                newTargets.append(BatchTarget(app: app, useGPU: true))
            case .both:
                newTargets.append(BatchTarget(app: app, useGPU: false))
                newTargets.append(BatchTarget(app: app, useGPU: true))
            }
        }

        self.targets = newTargets
        self.currentIndex = 0
        self.status = .running

        savePendingSession()
        runNextTarget()
    }

    func resumePendingSession(allApps: [InstalledApp]) {
        guard let data = try? Data(contentsOf: Self.pendingSessionURL),
              let saved = try? JSONDecoder().decode(BatchSessionState.self, from: data) else {
            return
        }

        self.options = saved.options
        self.results = saved.completedResults

        let completedIds = Set(saved.completedResults.map(\.id))

        var remainingTargets: [BatchTarget] = []
        for app in allApps where app.info.runnable {
            let swTarget = BatchTarget(app: app, useGPU: false)
            let gpuTarget = BatchTarget(app: app, useGPU: true)

            switch options.rendererMode {
            case .software:
                if !completedIds.contains(swTarget.targetId) { remainingTargets.append(swTarget) }
            case .gpu:
                if !completedIds.contains(gpuTarget.targetId) { remainingTargets.append(gpuTarget) }
            case .both:
                if !completedIds.contains(swTarget.targetId) { remainingTargets.append(swTarget) }
                if !completedIds.contains(gpuTarget.targetId) { remainingTargets.append(gpuTarget) }
            }
        }

        self.targets = remainingTargets
        self.currentIndex = 0
        self.status = .running

        runNextTarget()
    }

    func cancel() {
        guard status == .running || status == .paused else { return }
        status = .cancelled
        currentTestTask?.cancel()
        activeSession?.stopGracefully()
        activeSession = nil
        fpsTimer?.invalidate()
        fpsTimer = nil
        UIApplication.shared.isIdleTimerDisabled = false
    }

    private func savePendingSession() {
        try? FileManager.default.createDirectory(at: Self.reportsDir, withIntermediateDirectories: true)
        let state = BatchSessionState(
            options: options,
            completedResults: results,
            pendingBundleIds: targets.suffix(from: currentIndex).map(\.app.info.bundleIdentifier)
        )
        if let data = try? JSONEncoder().encode(state) {
            try? data.write(to: Self.pendingSessionURL, options: .atomic)
        }

        // Also update latest batch report files
        saveFinalReports()
    }

    private func runNextTarget() {
        guard status == .running else { return }
        guard currentIndex < targets.count else {
            finishBatch()
            return
        }

        let target = targets[currentIndex]
        currentApp = target.app
        currentModeName = target.modeName
        currentLogs = []
        headLogs = []
        logLineCount = 0
        lastLogLine = ""
        lastLogRepeats = 0
        currentFrame = nil
        lastCapturedFrame = nil
        gpuSampleLines = []
        lastP95 = 0
        lastTotalPresented = 0
        lastDiag = nil
        missingLines = []
        audioDecodeFailures = []
        frameCount = 0
        lastPresentedFrames = 0
        progressText = "Testing \(currentIndex + 1)/\(targets.count): \(target.app.title) (\(target.modeName.uppercased()))"

        UIApplication.shared.isIdleTimerDisabled = true

        // Each run gets a throwaway copy of the app's data, so the Explore phase's taps
        // (sound/music toggles, settings, progress) never change the player's own saves.
        let testHome = Self.prepareTestHome(for: target.app, fresh: options.resetSandbox)

        currentTestTask = Task { @MainActor [weak self] in
            guard let self else { return }

            _ = await Self.waitForArenaRelease(8.0)
            guard !Task.isCancelled, self.status == .running else { return }

            var journalID: UUID?
            defer {
                if let journalID, HVMEngine.liveGuestArenaCount() == 0 { CrashReport.endSession(journalID) }
            }

            var s: HVMSession!
            var exitBox: ExitBox!
            var startRetries = 0

            while startRetries < 5 {
                let attemptJournalID: UUID
                do {
                    attemptJournalID = try CrashReport.beginSession(bundleID: target.app.info.bundleIdentifier,
                        version: target.app.info.version, renderer: target.modeName, phase: "batch-launch")
                } catch {
                    self.keepLog("[crash-journal] \(error.localizedDescription)")
                    self.status = .paused
                    UIApplication.shared.isIdleTimerDisabled = false
                    return
                }
                journalID = attemptJournalID
                exitBox = ExitBox()
                s = HVMSession(bundlePath: target.app.info.bundlePath, homePath: testHome.path(percentEncoded: false))
                s.keepGoing = self.options.keepGoing
                s.useGPU = target.useGPU
                self.activeSession = s

                // The bridge delivers every callback on the main queue (see RunModel).
                s.onFrame = { [weak self] img in
                    MainActor.assumeIsolated {
                        guard let self else { return }
                        self.currentFrame = img
                        self.lastCapturedFrame = img
                        self.frameCount += 1
                    }
                }

                s.onLog = { [weak self] line, _ in
                    MainActor.assumeIsolated {
                        guard let self else { return }
                        self.keepLog(line)
                        if line.contains("GPU: presented frame") { self.gpuSampleLines.append(line) }
                    }
                }

                s.onExit = { code, crash in
                    MainActor.assumeIsolated {
                        // Runs even if cancellation already unwound the task.
                        // Each startup retry creates a fresh, token-owned journal.
                        CrashReport.endSession(attemptJournalID)
                        exitBox.exited = true
                        exitBox.code = code
                        exitBox.crash = crash
                    }
                }

                self.fpsTimer?.invalidate()
                self.fpsTimer = Timer.scheduledTimer(withTimeInterval: 1.0, repeats: true) { [weak self] _ in
                    MainActor.assumeIsolated {
                        guard let self, let s = self.activeSession else { return }
                        let presented = s.framesPresented
                        self.currentFPS = presented >= self.lastPresentedFrames ? max(Int(presented - self.lastPresentedFrames), self.frameCount) : self.frameCount
                        self.lastPresentedFrames = presented
                        self.frameCount = 0
                        // Kept from the running session: once it stops, its counters are gone.
                        self.lastP95 = s.frameTimeP95Milliseconds
                        if presented > 0 { self.lastTotalPresented = presented }
                        if let d = BatchDiagnostics(s.diagnostics) { self.lastDiag = d }
                    }
                }

                s.start()

                let startWaitDeadline = Date().addingTimeInterval(5.5)
                while Date() < startWaitDeadline && !exitBox.exited && !Task.isCancelled && HVMEngine.liveGuestArenaCount() == 0 {
                    try? await Task.sleep(for: .milliseconds(50))
                }

                if exitBox.exited, let crash = exitBox.crash, crash.contains("failed to reserve") {
                    startRetries += 1
                    s.stop()
                    self.activeSession = nil
                    _ = await Self.waitForArenaRelease(5.0)
                    try? await Task.sleep(for: .milliseconds(1000))
                    guard !Task.isCancelled, self.status == .running else { return }
                    continue
                } else {
                    break
                }
            }

            guard self.status == .running else { return }
            let durationSeconds = Double(self.options.secondsPerApp)
            let deadline = Date().addingTimeInterval(durationSeconds)

            var launchDiag: BatchDiagnostics? = nil   // counters at the end of the launch wait
            // A let, so the log closure below can capture it under strict concurrency.
            let explorePhase: ExplorePhase? = self.options.exploreEnabled
                ? ExplorePhase(session: s, options: self.options, bundleId: target.app.info.bundleIdentifier) { [weak self] in self?.lastCapturedFrame }
                : nil
            if explorePhase != nil {

                let originalOnLog = s.onLog
                s.onLog = { [weak self] line, level in
                    MainActor.assumeIsolated {
                        guard let self else { return }
                        self.keepLog(line)
                        if line.contains("GPU: presented frame") { self.gpuSampleLines.append(line) }

                        explorePhase?.recordLogLine(line, level: level)
                    }
                }

                while Date() < deadline && !exitBox.exited && self.status == .running {
                    try? await Task.sleep(for: .milliseconds(250))
                }

                launchDiag = self.lastDiag
                if !exitBox.exited && self.status == .running {
                    let exploreDuration = Double(self.options.exploreTime)
                    let exploreDeadline = Date().addingTimeInterval(exploreDuration)

                    if let ep = explorePhase {
                        self.progressText = "Exploring \(self.currentIndex + 1)/\(self.targets.count): \(target.app.title) (\(target.modeName.uppercased()))"
                        await ep.run(deadline: exploreDeadline, exitBox: exitBox)
                    }
                }

                s.onLog = originalOnLog

            } else {
                while Date() < deadline && !exitBox.exited && self.status == .running {
                    try? await Task.sleep(for: .milliseconds(250))
                }
                launchDiag = self.lastDiag
            }

            let exitedOnItsOwn = exitBox.exited
            var closedGracefully: Bool? = nil
            if !exitBox.exited && self.status == .running {
                if let d = BatchDiagnostics(s.diagnostics) { self.lastDiag = d }
                self.activeSession?.stopGracefully()
                let gracefulDeadline = Date().addingTimeInterval(4.0)
                while Date() < gracefulDeadline && !exitBox.exited {
                    try? await Task.sleep(for: .milliseconds(100))
                }
                closedGracefully = exitBox.exited
                // A game that ignores the close request is stopped, so the next one
                // can have the memory (only one 4 GB guest arena fits on iOS).
                if !exitBox.exited {
                    self.activeSession?.stop()
                    let hardDeadline = Date().addingTimeInterval(8.0)
                    while Date() < hardDeadline && !exitBox.exited {
                        try? await Task.sleep(for: .milliseconds(100))
                    }
                }
            }
            
            let hardStopped = closedGracefully == false && !exitedOnItsOwn
            let engineTeardownTimedOut = !exitBox.exited && self.status == .running
            if engineTeardownTimedOut {
                self.keepLog("[teardown] Engine did not exit after the forced-stop deadline; stopping the batch before another game can reserve memory")
            }

            self.fpsTimer?.invalidate()
            self.fpsTimer = nil

            guard self.status == .running else { return }

            let presented = self.lastTotalPresented
            let p95Ms = self.lastP95
            let capturedCrash = exitBox.crash ?? (engineTeardownTimedOut
                ? "Engine teardown timed out after the forced stop. The batch stopped instead of launching another game into the live guest arena."
                : nil)
            let exitStatusCode = exitBox.code
            let isCrashed = capturedCrash != nil || (exitBox.exited && exitStatusCode != 0)

            // On the GPU path the delivered image is only the UIKit overlay (with a
            // hole where the GL layer shows), so judge the picture from the
            // engine's own samples of presented GL frames instead.
            let frameSig = target.useGPU && !self.gpuSampleLines.isEmpty
                ? FrameAnalyzer.analyzeGPUSamples(self.gpuSampleLines)
                : FrameAnalyzer.analyze(self.lastCapturedFrame)
            var thumbRelPath: String? = nil

            if let img = self.lastCapturedFrame {
                let thumbFileName = "\(target.app.info.bundleIdentifier)_\(target.modeName).png"
                let thumbURL = Self.thumbnailsDir.appending(path: thumbFileName)
                if FrameAnalyzer.savePNG(img, to: thumbURL) {
                    thumbRelPath = "thumbnails/\(thumbFileName)"
                }
            }

            let summaryText: String
            let signatureText: String
            if isCrashed {
                summaryText = capturedCrash?.components(separatedBy: "\n").first ?? "Crashed (status \(exitStatusCode))"
                signatureText = engineTeardownTimedOut ? "engine-teardown-timeout" : (capturedCrash != nil ? "crash-fault" : "exit-\(exitStatusCode)")
            } else if frameSig != .ok {
                summaryText = "Wrong output: \(frameSig.rawValue)"
                signatureText = "signal-\(frameSig.rawValue)"
            } else {
                summaryText = "OK - \(self.currentFPS) fps"
                signatureText = "ok"
            }

            let logExcerpt = self.makeLogExcerpt()
            let exploreResult = explorePhase?.buildResult(crashed: isCrashed)

            // Second launch on the data the first run left behind.
            var relaunch: (crashed: Bool, frames: Int, summary: String?)? = nil
            if self.options.relaunchEnabled && !isCrashed && exitBox.exited && !hardStopped && self.status == .running {
                self.progressText = "Relaunching \(self.currentIndex + 1)/\(self.targets.count): \(target.app.title) (\(target.modeName.uppercased()))"
                self.activeSession = nil
                _ = await Self.waitForArenaRelease(8.0)
                if let journalID, HVMEngine.liveGuestArenaCount() == 0 { CrashReport.endSession(journalID) }
                try? await Task.sleep(for: .milliseconds(500))
                relaunch = await self.relaunch(target: target, home: testHome)
            }
            guard self.status == .running else { return }

            var obs = BatchObservations()
            obs.crashed = isCrashed
            obs.crashSummary = isCrashed ? summaryText : nil
            obs.frameSignal = frameSig.rawValue
            obs.fps = self.currentFPS
            obs.launchDiag = launchDiag
            obs.finalDiag = self.lastDiag
            obs.exploreEnabled = explorePhase != nil
            obs.exploreCrashed = exploreResult?.crashed ?? false
            obs.screensVisited = exploreResult?.screensVisited ?? 0
            obs.backgroundTested = exploreResult?.backgroundTested ?? false
            obs.resumeFramesBefore = exploreResult?.resumeFramesBefore
            obs.resumeFramesAfter = exploreResult?.resumeFramesAfter
            obs.closedGracefully = closedGracefully
            obs.engineTeardownTimedOut = engineTeardownTimedOut
            obs.exitedOnItsOwn = exitedOnItsOwn && !isCrashed
            obs.relaunchEnabled = self.options.relaunchEnabled
            obs.relaunchCrashed = relaunch?.crashed
            obs.relaunchFrames = relaunch?.frames
            obs.relaunchSummary = relaunch?.summary
            obs.missingLines = self.missingLines
            obs.audioDecodeFailures = self.audioDecodeFailures
            let checks = BatchCheckBuilder.build(obs)

            let appResult = BatchAppResult(
                bundleId: target.app.info.bundleIdentifier,
                name: target.app.title,
                version: target.app.info.version,
                minOS: target.app.info.minimumOSVersion,
                mode: target.modeName,
                crashed: isCrashed,
                exitCode: Int(exitStatusCode),
                summary: summaryText,
                signature: signatureText,
                exceptionKind: engineTeardownTimedOut ? "engine-teardown-timeout" : (isCrashed ? "crash" : nil),
                crashReport: capturedCrash,
                perf: BatchPerf(framesPresented: Int(presented), fps: self.currentFPS, frameTimeP95Ms: p95Ms),
                frame: BatchFrameInfo(signal: frameSig.rawValue, thumbnailPath: thumbRelPath),
                keepGoing: self.options.keepGoing,
                logs: logExcerpt,
                explore: exploreResult
            )

            var finished = appResult
            finished.checks = checks
            finished.diagnostics = self.lastDiag
            if isCrashed, let bundle = self.activeSession?.crashBundle, let data = bundle.data(using: .utf8),
               let json = (try? JSONSerialization.jsonObject(with: data)) as? [String: Any], let files = json["fileHistory"],
               let filesData = try? JSONSerialization.data(withJSONObject: files) {
                finished.fileHistory = String(decoding: filesData, as: UTF8.self)
            }
            self.results.append(finished)
            self.currentIndex += 1
            self.activeSession = nil
            try? FileManager.default.removeItem(at: testHome)

            if engineTeardownTimedOut {
                // The old engine still owns its guest arena. Launching the next
                // game would only produce misleading reservation failures.
                self.status = .cancelled
                self.progressText = "Stopped: \(target.app.title) did not finish closing. See its teardown log."
                self.clearPendingSession()
                self.saveFinalReports()
                UIApplication.shared.isIdleTimerDisabled = false
                return
            }

            self.savePendingSession()

            // Let the finished game's threads and memory go before the next one starts.
            _ = await Self.waitForArenaRelease(8.0)
            try? await Task.sleep(for: .milliseconds(500))
            self.runNextTarget()
        }
    }

    /// Starts the game again on `home` for `relaunchTime` seconds and reports what happened.
    private func relaunch(target: BatchTarget, home: URL) async -> (crashed: Bool, frames: Int, summary: String?) {
        var journalID: UUID?
        defer {
            if let journalID, HVMEngine.liveGuestArenaCount() == 0 { CrashReport.endSession(journalID) }
        }
        var s: HVMSession!
        var box: ExitBox!
        var startRetries = 0
        
        while startRetries < 5 {
            _ = await Self.waitForArenaRelease(8.0)
            if Task.isCancelled || status != .running { return (false, 0, nil) }
            let attemptJournalID: UUID
            do {
                attemptJournalID = try CrashReport.beginSession(bundleID: target.app.info.bundleIdentifier,
                    version: target.app.info.version, renderer: target.modeName, phase: "batch-relaunch")
            } catch {
                return (true, 0, "Crash journal: \(error.localizedDescription)")
            }
            journalID = attemptJournalID
            box = ExitBox()
            s = HVMSession(bundlePath: target.app.info.bundlePath, homePath: home.path(percentEncoded: false))
            s.keepGoing = options.keepGoing
            s.useGPU = target.useGPU
            activeSession = s
            s.onFrame = { [weak self] img in
                MainActor.assumeIsolated {
                    box.frames += 1
                    self?.currentFrame = img
                }
            }
            s.onLog = { [weak self] line, _ in
                MainActor.assumeIsolated { self?.keepLog(line) }
            }
            s.onExit = { code, crash in
                MainActor.assumeIsolated {
                    CrashReport.endSession(attemptJournalID)
                    box.exited = true
                    box.code = code
                    box.crash = crash
                }
            }
            s.start()
            
            let startWaitDeadline = Date().addingTimeInterval(5.5)
            while Date() < startWaitDeadline && !box.exited && !Task.isCancelled && HVMEngine.liveGuestArenaCount() == 0 {
                try? await Task.sleep(for: .milliseconds(50))
            }
            if box.exited, let crash = box.crash, crash.contains("failed to reserve") {
                startRetries += 1
                s.stop()
                activeSession = nil
                _ = await Self.waitForArenaRelease(5.0)
                try? await Task.sleep(for: .milliseconds(1000))
                if Task.isCancelled || status != .running { return (false, 0, nil) }
                continue
            } else {
                break
            }
        }

        let deadline = Date().addingTimeInterval(Double(options.relaunchTime))
        var presented = 0
        while Date() < deadline && !box.exited && status == .running {
            try? await Task.sleep(for: .milliseconds(250))
            presented = max(presented, Int(s.framesPresented))
        }
        if !box.exited {
            s.stopGracefully()
            let graceful = Date().addingTimeInterval(4.0)
            while Date() < graceful && !box.exited { try? await Task.sleep(for: .milliseconds(100)) }
            if !box.exited {
                s.stop()
                let hard = Date().addingTimeInterval(8.0)
                while Date() < hard && !box.exited { try? await Task.sleep(for: .milliseconds(100)) }
            }
        }
        let crashed = box.crash != nil || (box.exited && box.code != 0 && Date() < deadline)
        let summary = box.crash?.components(separatedBy: "\n").first ?? (crashed ? "Exited with status \(box.code) on the second launch" : nil)
        activeSession = nil
        return (crashed, max(presented, box.frames), summary)
    }

    private func finishBatch() {
        status = .completed
        clearPendingSession()
        saveFinalReports()
        UIApplication.shared.isIdleTimerDisabled = false
    }

    static let testHomesDir = FileManager.default.temporaryDirectory.appending(path: "BatchTestHomes", directoryHint: .isDirectory)

    /// Waits (off the main actor) for the previous session's guest arena to be unmapped, so the main thread keeps
    /// serving the UI and exit callbacks while the wait runs.
    nonisolated static func waitForArenaRelease(_ timeout: TimeInterval) async -> Bool {
        await Task.detached(priority: .utility) { HVMEngine.wait(forGuestArenaRelease: timeout) }.value
    }

    /// A scratch home for one test run: a copy of the app's Documents and Library
    /// (or empty ones when `fresh`). The app's real data is never written by a test.
    static func prepareTestHome(for app: InstalledApp, fresh: Bool) -> URL {
        let fm = FileManager.default
        let home = testHomesDir.appending(path: app.info.bundleIdentifier, directoryHint: .isDirectory)
        try? fm.removeItem(at: home)
        try? fm.createDirectory(at: home, withIntermediateDirectories: true)
        for sub in ["Documents", "Library", "tmp"] {
            let src = app.homeURL.appending(path: sub), dst = home.appending(path: sub)
            if !fresh, sub != "tmp", fm.fileExists(atPath: src.path(percentEncoded: false)) {
                if (try? fm.copyItem(at: src, to: dst)) != nil { continue }
            }
            try? fm.createDirectory(at: dst, withIntermediateDirectories: true)
        }
        return home
    }

    func buildCompatReportJSON() -> [String: Any] {
        let rows = results.map { r -> [String: Any] in
            var dict: [String: Any] = [
                "bundleId": r.bundleId,
                "name": r.name,
                "version": r.version,
                "renderer": r.mode,
                "crashed": r.crashed,
                "exitCode": r.exitCode,
                "summary": r.summary,
                "signature": r.signature ?? "unknown",
                "perf": [
                    "framesPresented": r.perf.framesPresented,
                    "fps": r.perf.fps,
                    "frameTimeP95Ms": r.perf.frameTimeP95Ms
                ],
                "frame": [
                    "signal": r.frame.signal,
                    "thumbnailPath": r.frame.thumbnailPath ?? ""
                ],
                "keepGoing": r.keepGoing,
                "logs": r.logs
            ]
            if let crash = r.crashReport { dict["crashReport"] = crash }
            if let files = r.fileHistory, let data = files.data(using: .utf8),
               let parsed = try? JSONSerialization.jsonObject(with: data) { dict["fileHistory"] = parsed }
            if let ex = r.exceptionKind { dict["exceptionKind"] = ex }
            if let checks = r.checks, let data = try? JSONEncoder().encode(checks),
               let parsed = try? JSONSerialization.jsonObject(with: data) { dict["checks"] = parsed }
            if let diag = r.diagnostics, let data = try? JSONEncoder().encode(diag),
               let parsed = try? JSONSerialization.jsonObject(with: data) { dict["diagnostics"] = parsed }

            if let explore = r.explore {
                if let expData = try? JSONEncoder().encode(explore),
                   let expParsed = try? JSONSerialization.jsonObject(with: expData) {
                    dict["explore"] = expParsed
                }
            }
            return dict
        }

        return [
            "kind": "hvm-compat-report",
            "timestamp": ISO8601DateFormatter().string(from: Date()),
            "build": [
                "commit": HVMSession.emulatorCommit,
                "configuration": "device",
                "binaryMatch": "verified"
            ],
            "host": [
                "kind": "iphone",
                "model": UIDevice.current.model,
                "os": "iOS \(UIDevice.current.systemVersion)",
                "pageSize": getpagesize()
            ],
            "device": [
                "model": UIDevice.current.model,
                "systemVersion": UIDevice.current.systemVersion
            ],
            "options": [
                "seconds": options.secondsPerApp,
                "renderer": options.rendererMode.rawValue,
                "keepGoing": options.keepGoing,
                "explore": options.exploreEnabled,
                "exploreSeconds": options.exploreTime,
                "relaunch": options.relaunchEnabled
            ],
            "suites": [
                "apps": [
                    "apps": rows
                ]
            ]
        ]
    }

    func buildMarkdownReport() -> String {
        var md = "# HydrogenVM — Test All Games Report\n\n"
        md += "**Date:** \(ISO8601DateFormatter().string(from: Date()))  \n"
        md += "**Device:** \(UIDevice.current.model), iOS \(UIDevice.current.systemVersion)  \n"
        md += "**Emulator Commit:** `\(HVMSession.emulatorCommit.prefix(12))`  \n"
        md += "**Options:** \(options.secondsPerApp)s/app · Renderer \(options.rendererMode.displayName) · KeepGoing \(options.keepGoing) · Explore \(options.exploreEnabled ? "\(options.exploreTime)s" : "off") · Relaunch \(options.relaunchEnabled)\n\n"

        let total = results.count
        let crashed = results.filter(\.crashed).count
        let wrongSignal = results.filter { !$0.crashed && $0.frame.signal != "ok" }.count
        let passed = total - crashed - wrongSignal
        let withMusic = results.filter { ($0.diagnostics?.musicFrames ?? 0) > 0 }.count
        let withEffects = results.filter { ($0.diagnostics?.effectFrames ?? 0) > 0 }.count
        let failedChecks = results.filter { $0.worstCheck == .fail }.count

        let explored = results.filter { $0.explore?.enabled == true }.count
        let exploreCrashed = results.filter { $0.explore?.enabled == true && $0.explore?.crashed == true }.count
        let exploreProblems = results.reduce(0) { $0 + ($1.explore?.newProblems.count ?? 0) }

        md += "### Summary\n\n"
        md += "- **Total Tests Run:** \(total)\n"
        md += "- **Passed:** \(passed)\n"
        md += "- **Crashed:** \(crashed)\n"
        md += "- **Wrong Output:** \(wrongSignal)\n"
        md += "- **Music heard:** \(withMusic) · **Sound effects heard:** \(withEffects)\n"
        md += "- **Games with a failed check:** \(failedChecks)\n"
        if explored > 0 {
            md += "- **Explored:** \(explored)\n"
            md += "- **Explore Crashes:** \(exploreCrashed)\n"
            md += "- **Explore Problems Discovered:** \(exploreProblems)\n"
        }
        md += "\n"

        md += "### App Results\n\n"
        md += "| App | Mode | Status | FPS | P95 (ms) | Signal | Summary |\n"
        md += "|---|---|---|---:|---:|---|---|\n"

        for r in results {
            let statusBadge = r.crashed ? "❌ Crashed" : (r.frame.signal != "ok" ? "⚠️ Output Error" : "✅ Passed")
            md += "| \(r.name) (`\(r.bundleId)`) | \(r.mode.uppercased()) | \(statusBadge) | \(r.perf.fps) | \(String(format: "%.1f", r.perf.frameTimeP95Ms)) | `\(r.frame.signal)` | \(r.summary.replacingOccurrences(of: "|", with: "\\|")) |\n"
        }

        // The checklist: one table of all games × checks, then each game's details.
        let checked = results.filter { $0.checks != nil }
        if let first = checked.first?.checks {
            let names = first.map(\.name)
            md += "\n### Checklist\n\n"
            md += "| Check | ✅ | ⚠️ | ❌ | ➖ |\n|---|---:|---:|---:|---:|\n"
            for n in names {
                let all = checked.compactMap { $0.checks?.first(where: { $0.name == n }) }
                func k(_ st: BatchCheck.Status) -> Int { all.filter { $0.status == st }.count }
                md += "| \(n) | \(k(.pass)) | \(k(.warn)) | \(k(.fail)) | \(k(.skip)) |\n"
            }
            md += "\n| App | Mode | " + names.joined(separator: " | ") + " |\n"
            md += "|---|---|" + names.map { _ in ":-:" }.joined(separator: "|") + "|\n"
            for r in checked {
                let cells = names.map { n in r.checks?.first(where: { $0.name == n })?.emoji ?? "" }
                md += "| \(r.name) | \(r.mode.uppercased()) | " + cells.joined(separator: " | ") + " |\n"
            }
            for r in checked {
                md += "\n#### \(r.name) (\(r.mode.uppercased()))\n\n"
                for c in r.checks ?? [] {
                    md += "- \(c.emoji) **\(c.name):** \(c.detail.replacingOccurrences(of: "|", with: "\\|"))\n"
                }
            }
        }

        md += "\n"
        return md
    }

    private func saveFinalReports() {
        let jsonDict = buildCompatReportJSON()
        if let jsonData = try? JSONSerialization.data(withJSONObject: jsonDict, options: [.prettyPrinted, .sortedKeys]) {
            let jsonURL = Self.reportsDir.appending(path: "latest_batch_report.json")
            try? jsonData.write(to: jsonURL, options: .atomic)
        }

        let mdText = buildMarkdownReport()
        let mdURL = Self.reportsDir.appending(path: "latest_batch_report.md")
        try? mdText.write(to: mdURL, atomically: true, encoding: .utf8)
    }

    func consoleIssueURL(for result: BatchAppResult) -> URL {
        let title = "[test-all] \(result.bundleId) (\(result.mode)): \(result.summary)"
        var body = "**App:** \(result.name) \(result.version) (`\(result.bundleId)`)\n"
        body += "**Emulator:** `\(HVMSession.emulatorCommit.prefix(12))` · renderer \(result.mode)\n"
        body += "**Performance:** \(result.perf.fps) fps · p95 \(String(format: "%.1f", result.perf.frameTimeP95Ms)) ms · signal `\(result.frame.signal)`\n"
        body += "**Summary:** \(result.summary)\n\n"
        if let checks = result.checks {
            for c in checks where c.status != .pass && c.status != .skip {
                body += "- \(c.emoji) \(c.name): \(c.detail)\n"
            }
            body += "\n"
        }
        body += "```\n"
        body += String(result.logs.suffix(2000))
        body += "\n```\n"

        var url = URLComponents(string: "https://github.com/lawsharley0-cyber/HydrogenVM/issues/new")!
        url.queryItems = [
            URLQueryItem(name: "title", value: String(title.prefix(150))),
            URLQueryItem(name: "body", value: body)
        ]
        return url.url!
    }
}
