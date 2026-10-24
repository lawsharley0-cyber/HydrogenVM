import Foundation
import CoreGraphics
import QuartzCore

struct ExploreAction: Codable, Sendable {
    var timeOffset: Double
    var kind: String
    var details: String
}

struct ExploreProblem: Codable, Sendable {
    var problemText: String
    var triggeringActionIndex: Int?
}

struct ExploreResult: Codable, Sendable {
    var enabled: Bool
    var actions: [ExploreAction]
    var newProblems: [ExploreProblem]
    var crashed: Bool
    var reproSteps: [ExploreAction]?
    var screensVisited: Int
    var seed: UInt64
}

struct DeterministicRNG: RandomNumberGenerator {
    var state: UInt64
    init(seed: UInt64) {
        state = seed == 0 ? 1 : seed
    }
    mutating func next() -> UInt64 {
        state ^= state &<< 13
        state ^= state &>> 7
        state ^= state &<< 17
        return state
    }
}

@MainActor
final class ExplorePhase {
    let session: HVMSession
    let options: BatchTestOptions
    let bundleId: String
    let seed: UInt64

    var actions: [ExploreAction] = []
    var newProblems: [ExploreProblem] = []
    private var knownProblemHashes = Set<String>()
    var visitedFrameHashes = Set<String>()

    let startTime: Date
    private var currentFrameProvider: (() -> CGImage?)?

    init(session: HVMSession, options: BatchTestOptions, bundleId: String, currentFrameProvider: @escaping () -> CGImage?) {
        self.session = session
        self.options = options
        self.bundleId = bundleId
        self.currentFrameProvider = currentFrameProvider
        self.startTime = Date()

        var hasher = Hasher()
        hasher.combine(bundleId)
        self.seed = UInt64(bitPattern: Int64(hasher.finalize()))
    }

    func seedKnownProblems(from logs: [(String, HVMLogLevel)]) {
        for (raw, level) in logs {
            let text = raw.trimmingCharacters(in: .whitespacesAndNewlines)
            if ConsoleReport.rank(text, level) != nil {
                knownProblemHashes.insert(ConsoleReport.normalized(text))
            }
        }
    }

    func recordLogLine(_ raw: String, level: HVMLogLevel) {
        let text = raw.trimmingCharacters(in: .whitespacesAndNewlines)
        if let rank = ConsoleReport.rank(text, level), rank > 0 || rank == 0 {
            let key = ConsoleReport.normalized(text)
            if !knownProblemHashes.contains(key) {
                knownProblemHashes.insert(key)
                newProblems.append(ExploreProblem(problemText: String(text.prefix(600)), triggeringActionIndex: actions.isEmpty ? nil : actions.count - 1))
            }
        }
    }

    private func recordAction(kind: String, details: String) {
        actions.append(ExploreAction(timeOffset: Date().timeIntervalSince(startTime), kind: kind, details: details))
    }

    static func hashImage(_ image: CGImage?) -> String {
        guard let image = image else { return "none" }
        let width = image.width
        let height = image.height
        guard width > 0 && height > 0 else { return "empty" }

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
            return "error"
        }

        context.draw(image, in: CGRect(x: 0, y: 0, width: width, height: height))

        var hasher = Hasher()
        let stepX = max(1, width / 20)
        let stepY = max(1, height / 20)

        for y in stride(from: 0, to: height, by: stepY) {
            for x in stride(from: 0, to: width, by: stepX) {
                let offset = (y * width + x) * bytesPerPixel
                if offset + 2 < rawData.count {
                    hasher.combine(rawData[offset])
                    hasher.combine(rawData[offset+1])
                    hasher.combine(rawData[offset+2])
                }
            }
        }
        return String(hasher.finalize(), radix: 16)
    }

    private func captureFrameHash() -> String {
        let h = Self.hashImage(currentFrameProvider?())
        visitedFrameHashes.insert(h)
        return h
    }

    func run(deadline: Date, exitBox: ExitBox) async {
        var rng = DeterministicRNG(seed: self.seed)

        _ = captureFrameHash()

        let size = session.screenSize
        let w = size.width > 0 ? size.width : 320
        let h = size.height > 0 ? size.height : 480

        let cx = w / 2
        let cy = h / 2

        // 1. Grid of taps
        let cols = 6
        let rows = 10
        let stepX = w / CGFloat(cols)
        let stepY = h / CGFloat(rows)

        var gridPoints: [CGPoint] = []
        for r in 0..<rows {
            for c in 0..<cols {
                gridPoints.append(CGPoint(x: (CGFloat(c) + 0.5) * stepX, y: (CGFloat(r) + 0.5) * stepY))
            }
        }
        gridPoints.shuffle(using: &rng)

        for pt in gridPoints {
            if Date() > deadline || exitBox.exited { break }
            let hashBefore = captureFrameHash()
            await performTap(x: pt.x, y: pt.y, label: "grid tap")
            let hashAfter = captureFrameHash()

            if hashBefore != hashAfter {
                if Date() > deadline || exitBox.exited { break }
                let nx = pt.x + CGFloat.random(in: -20...20, using: &rng)
                let ny = pt.y + CGFloat.random(in: -20...20, using: &rng)
                await performTap(x: max(0, min(w, nx)), y: max(0, min(h, ny)), label: "smart tap nearby")
                _ = captureFrameHash()
            }
        }

        // 2. Corners (Back buttons)
        if Date() <= deadline && !exitBox.exited {
            let corners = [CGPoint(x: 20, y: 20), CGPoint(x: w - 20, y: 20), CGPoint(x: 20, y: h - 20), CGPoint(x: w - 20, y: h - 20)]
            for pt in corners {
                if Date() > deadline || exitBox.exited { break }
                await performTap(x: pt.x, y: pt.y, label: "corner tap")
                _ = captureFrameHash()
            }
        }

        // 3. Swipes
        if Date() <= deadline && !exitBox.exited {
            await performSwipe(from: CGPoint(x: cx, y: cy), to: CGPoint(x: 20, y: cy), label: "swipe left")
            await performSwipe(from: CGPoint(x: cx, y: cy), to: CGPoint(x: w - 20, y: cy), label: "swipe right")
            await performSwipe(from: CGPoint(x: cx, y: cy), to: CGPoint(x: cx, y: 20), label: "swipe up")
            await performSwipe(from: CGPoint(x: cx, y: cy), to: CGPoint(x: cx, y: h - 20), label: "swipe down")
            _ = captureFrameHash()
        }

        // 4. Long presses, two-finger tap, pinch
        if Date() <= deadline && !exitBox.exited {
            await performLongPress(x: cx, y: cy, label: "center long press")
            await performTwoFingerTap(x: cx, y: cy, label: "center two-finger tap")
            await performPinch(label: "pinch")
            _ = captureFrameHash()
        }

        // 5. Motion and lifecycle
        if Date() <= deadline && !exitBox.exited {
            recordAction(kind: "tilt", details: "accelerometer left")
            session.setAccelerationX(-0.5, y: -0.5, z: 0)
            try? await Task.sleep(for: .milliseconds(500))

            recordAction(kind: "tilt", details: "accelerometer right")
            session.setAccelerationX(0.5, y: -0.5, z: 0)
            try? await Task.sleep(for: .milliseconds(500))

            recordAction(kind: "tilt", details: "accelerometer forward")
            session.setAccelerationX(0, y: 0, z: -1)
            try? await Task.sleep(for: .milliseconds(500))

            recordAction(kind: "tilt", details: "accelerometer center")
            session.setAccelerationX(0, y: -1, z: 0)
            try? await Task.sleep(for: .milliseconds(500))

            recordAction(kind: "lifecycle", details: "enter background")
            session.enterBackground()
            try? await Task.sleep(for: .milliseconds(1000))

            recordAction(kind: "lifecycle", details: "enter foreground")
            session.enterForeground()
            try? await Task.sleep(for: .milliseconds(1000))
        }

        while Date() < deadline && !exitBox.exited {
            try? await Task.sleep(for: .milliseconds(250))
        }

        _ = captureFrameHash()
    }

    private func performTap(x: CGFloat, y: CGFloat, label: String) async {
        recordAction(kind: "tap", details: "\(label) at (\(Int(x)), \(Int(y)))")
        session.touch(.began, x: Float(x), y: Float(y), finger: 0)
        try? await Task.sleep(for: .milliseconds(50))
        session.touch(.ended, x: Float(x), y: Float(y), finger: 0)
        try? await Task.sleep(for: .milliseconds(250))
    }

    private func performSwipe(from: CGPoint, to: CGPoint, label: String) async {
        recordAction(kind: "swipe", details: label)
        session.touch(.began, x: Float(from.x), y: Float(from.y), finger: 0)
        try? await Task.sleep(for: .milliseconds(50))

        let steps = 5
        for i in 1...steps {
            let t = CGFloat(i) / CGFloat(steps)
            let mx = from.x + (to.x - from.x) * t
            let my = from.y + (to.y - from.y) * t
            session.touch(.moved, x: Float(mx), y: Float(my), finger: 0)
            try? await Task.sleep(for: .milliseconds(50))
        }

        session.touch(.ended, x: Float(to.x), y: Float(to.y), finger: 0)
        try? await Task.sleep(for: .milliseconds(250))
    }

    private func performLongPress(x: CGFloat, y: CGFloat, label: String) async {
        recordAction(kind: "long_press", details: "\(label) at (\(Int(x)), \(Int(y)))")
        session.touch(.began, x: Float(x), y: Float(y), finger: 0)
        try? await Task.sleep(for: .milliseconds(1000))
        session.touch(.ended, x: Float(x), y: Float(y), finger: 0)
        try? await Task.sleep(for: .milliseconds(250))
    }

    private func performTwoFingerTap(x: CGFloat, y: CGFloat, label: String) async {
        recordAction(kind: "two_finger_tap", details: label)
        session.touch(.began, x: Float(x - 10), y: Float(y), finger: 0)
        session.touch(.began, x: Float(x + 10), y: Float(y), finger: 1)
        try? await Task.sleep(for: .milliseconds(50))
        session.touch(.ended, x: Float(x - 10), y: Float(y), finger: 0)
        session.touch(.ended, x: Float(x + 10), y: Float(y), finger: 1)
        try? await Task.sleep(for: .milliseconds(250))
    }

    private func performPinch(label: String) async {
        let size = session.screenSize
        let w = size.width > 0 ? size.width : 320
        let h = size.height > 0 ? size.height : 480
        let cx = w / 2
        let cy = h / 2

        recordAction(kind: "pinch", details: label)
        session.touch(.began, x: Float(cx - 50), y: Float(cy), finger: 0)
        session.touch(.began, x: Float(cx + 50), y: Float(cy), finger: 1)
        try? await Task.sleep(for: .milliseconds(50))

        let steps = 5
        for i in 1...steps {
            let offset = 50.0 - (40.0 * (CGFloat(i) / CGFloat(steps)))
            session.touch(.moved, x: Float(cx - offset), y: Float(cy), finger: 0)
            session.touch(.moved, x: Float(cx + offset), y: Float(cy), finger: 1)
            try? await Task.sleep(for: .milliseconds(50))
        }

        session.touch(.ended, x: Float(cx - 10), y: Float(cy), finger: 0)
        session.touch(.ended, x: Float(cx + 10), y: Float(cy), finger: 1)
        try? await Task.sleep(for: .milliseconds(250))
    }

    func buildResult(crashed: Bool) -> ExploreResult {
        var reproSteps: [ExploreAction]? = nil
        if crashed {
            reproSteps = Array(actions.suffix(20))
        }

        return ExploreResult(
            enabled: true,
            actions: actions,
            newProblems: newProblems,
            crashed: crashed,
            reproSteps: reproSteps,
            screensVisited: visitedFrameHashes.count,
            seed: seed
        )
    }
}
