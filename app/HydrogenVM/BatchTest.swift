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

    init(secondsPerApp: Int = 10, rendererMode: BatchRendererMode = .both, keepGoing: Bool = true, resetSandbox: Bool = false) {
        self.secondsPerApp = secondsPerApp
        self.rendererMode = rendererMode
        self.keepGoing = keepGoing
        self.resetSandbox = resetSandbox
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

    init(bundleId: String, name: String, version: String, minOS: String, mode: String, crashed: Bool, exitCode: Int, summary: String, signature: String? = nil, exceptionKind: String? = nil, crashReport: String? = nil, perf: BatchPerf = BatchPerf(), frame: BatchFrameInfo = BatchFrameInfo(), keepGoing: Bool = true, logs: String = "") {
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
    private(set) var progressText: String = ""

    @ObservationIgnored private var activeSession: HVMSession?
    @ObservationIgnored private var currentTestTask: Task<Void, Never>?
    @ObservationIgnored private var lastPresentedFrames: UInt64 = 0
    @ObservationIgnored private var frameCount: Int = 0
    @ObservationIgnored private var fpsTimer: Timer?
    @ObservationIgnored private var lastCapturedFrame: CGImage?

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
        currentFrame = nil
        lastCapturedFrame = nil
        frameCount = 0
        lastPresentedFrames = 0
        progressText = "Testing \(currentIndex + 1)/\(targets.count): \(target.app.title) (\(target.modeName.uppercased()))"

        UIApplication.shared.isIdleTimerDisabled = true

        if options.resetSandbox {
            resetAppData(target.app)
        }

        let s = HVMSession(bundlePath: target.app.info.bundlePath, homePath: target.app.homeURL.path(percentEncoded: false))
        s.keepGoing = options.keepGoing
        s.useGPU = target.useGPU
        activeSession = s

        var capturedCrash: String?
        var exitStatusCode: NSInteger = -1
        var didExit = false

        s.onFrame = { [weak self] img in
            Task { @MainActor [weak self] in
                guard let self else { return }
                self.currentFrame = img
                self.lastCapturedFrame = img
                self.frameCount += 1
            }
        }

        s.onLog = { [weak self] line, _ in
            Task { @MainActor [weak self] in
                guard let self else { return }
                self.currentLogs.append(line)
                if self.currentLogs.count > 500 { self.currentLogs.removeFirst() }
            }
        }

        s.onExit = { [weak self] code, crash in
            Task { @MainActor [weak self] in
                guard self != nil else { return }
                didExit = true
                exitStatusCode = code
                capturedCrash = crash
            }
        }

        fpsTimer = Timer.scheduledTimer(withTimeInterval: 1.0, repeats: true) { [weak self] _ in
            Task { @MainActor [weak self] in
                guard let self, let s = self.activeSession else { return }
                let presented = s.framesPresented
                self.currentFPS = presented >= self.lastPresentedFrames ? max(Int(presented - self.lastPresentedFrames), self.frameCount) : self.frameCount
                self.lastPresentedFrames = presented
                self.frameCount = 0
            }
        }

        s.start()

        currentTestTask = Task { @MainActor [weak self] in
            guard let self else { return }
            let durationSeconds = Double(self.options.secondsPerApp)
            let deadline = Date().addingTimeInterval(durationSeconds)

            while Date() < deadline && !didExit && self.status == .running {
                try? await Task.sleep(for: .milliseconds(250))
            }

            if !didExit && self.status == .running {
                self.activeSession?.stopGracefully()
                let gracefulDeadline = Date().addingTimeInterval(2.0)
                while Date() < gracefulDeadline && !didExit {
                    try? await Task.sleep(for: .milliseconds(100))
                }
            }

            self.fpsTimer?.invalidate()
            self.fpsTimer = nil

            guard self.status == .running else { return }

            let presented = s.framesPresented
            let p95Ms = s.frameTimeP95Milliseconds
            let isCrashed = capturedCrash != nil || (didExit && exitStatusCode != 0)

            let frameSig = FrameAnalyzer.analyze(self.lastCapturedFrame)
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
                signatureText = capturedCrash != nil ? "crash-fault" : "exit-\(exitStatusCode)"
            } else if frameSig != .ok {
                summaryText = "Wrong output: \(frameSig.rawValue)"
                signatureText = "signal-\(frameSig.rawValue)"
            } else {
                summaryText = "OK - \(self.currentFPS) fps"
                signatureText = "ok"
            }

            let logExcerpt = self.currentLogs.suffix(100).joined(separator: "\n")

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
                exceptionKind: isCrashed ? "crash" : nil,
                crashReport: capturedCrash,
                perf: BatchPerf(framesPresented: Int(presented), fps: self.currentFPS, frameTimeP95Ms: p95Ms),
                frame: BatchFrameInfo(signal: frameSig.rawValue, thumbnailPath: thumbRelPath),
                keepGoing: self.options.keepGoing,
                logs: logExcerpt
            )

            self.results.append(appResult)
            self.currentIndex += 1
            self.activeSession = nil

            self.savePendingSession()

            // Short pause before next game
            try? await Task.sleep(for: .milliseconds(500))
            self.runNextTarget()
        }
    }

    private func finishBatch() {
        status = .completed
        clearPendingSession()
        saveFinalReports()
        UIApplication.shared.isIdleTimerDisabled = false
    }

    private func resetAppData(_ app: InstalledApp) {
        for sub in ["Documents", "Library", "tmp"] {
            try? FileManager.default.removeItem(at: app.homeURL.appending(path: sub))
        }
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
            if let ex = r.exceptionKind { dict["exceptionKind"] = ex }
            return dict
        }

        return [
            "kind": "hvm-compat-report",
            "timestamp": ISO8601DateFormatter().string(from: Date()),
            "device": [
                "model": UIDevice.current.model,
                "systemVersion": UIDevice.current.systemVersion
            ],
            "options": [
                "seconds": options.secondsPerApp,
                "renderer": options.rendererMode.rawValue,
                "keepGoing": options.keepGoing
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
        md += "**Options:** \(options.secondsPerApp)s/app · Renderer \(options.rendererMode.displayName) · KeepGoing \(options.keepGoing)\n\n"

        let total = results.count
        let crashed = results.filter(\.crashed).count
        let wrongSignal = results.filter { !$0.crashed && $0.frame.signal != "ok" }.count
        let passed = total - crashed - wrongSignal

        md += "### Summary\n\n"
        md += "- **Total Tests Run:** \(total)\n"
        md += "- **Passed:** \(passed)\n"
        md += "- **Crashed:** \(crashed)\n"
        md += "- **Wrong Output:** \(wrongSignal)\n\n"

        md += "### App Results\n\n"
        md += "| App | Mode | Status | FPS | P95 (ms) | Signal | Summary |\n"
        md += "|---|---|---|---:|---:|---|---|\n"

        for r in results {
            let statusBadge = r.crashed ? "❌ Crashed" : (r.frame.signal != "ok" ? "⚠️ Output Error" : "✅ Passed")
            md += "| \(r.name) (`\(r.bundleId)`) | \(r.mode.uppercased()) | \(statusBadge) | \(r.perf.fps) | \(String(format: "%.1f", r.perf.frameTimeP95Ms)) | `\(r.frame.signal)` | \(r.summary.replacingOccurrences(of: "|", with: "\\|")) |\n"
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
