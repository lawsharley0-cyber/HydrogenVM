import SwiftUI
import UIKit

struct HarnessAppInfo { let bundlePath: String }
enum HVMLogLevel { case error, stderr, warning, debug, stdout, info }
enum HVMTouchPhase: Int { case began = 0, moved = 1, ended = 3, cancelled = 4 }

@Observable @MainActor
final class PlayerProbe {
    static let shared = PlayerProbe()
    var touch = "none"
    var lifecycle = "active"
    var volume: Float = 1
    var stopCount = 0
    var lastOrientation = 1
    var maximumTouches = 0
    var cancellations = 0
    weak var session: HVMSession?
}

/// Replaces only the emulator bridge. The actual RunModel, RunView, UIKit
/// viewport and geometry are compiled into this synthetic UI fixture.
@MainActor
final class HVMSession {
    var keepGoing = false
    var onFrame: ((CGImage) -> Void)?
    var onLog: ((String, HVMLogLevel) -> Void)?
    var onExit: ((Int, String?) -> Void)?
    var screenSize = CGSize(width: 320, height: 480)
    var interfaceOrientation = 1
    var isBackgrounded = false
    var frameTimeP95Milliseconds = 4.2
    var outputVolume: Float = 1 { didSet { PlayerProbe.shared.volume = outputVolume } }
    private var activeFingers: Set<UInt> = []
    init(bundlePath: String, homePath: String) { PlayerProbe.shared.session = self }
    func start() {
        interfaceOrientation = Int(ProcessInfo.processInfo.environment["PLAYER_ORIENTATION"] ?? "1") ?? 1
        Task { @MainActor [weak self] in
            try? await Task.sleep(for: .milliseconds(100))
            self?.deliverFrame()
        }
    }
    func deliverFrame() {
        // Distinct framebuffer corners expose reversed orientation visually.
        let image = UIGraphicsImageRenderer(size: CGSize(width: 320, height: 480)).image { context in
            UIColor.red.setFill(); context.fill(CGRect(x: 0, y: 0, width: 160, height: 240))
            UIColor.green.setFill(); context.fill(CGRect(x: 160, y: 0, width: 160, height: 240))
            UIColor.blue.setFill(); context.fill(CGRect(x: 0, y: 240, width: 160, height: 240))
            UIColor.yellow.setFill(); context.fill(CGRect(x: 160, y: 240, width: 160, height: 240))
        }
        PlayerProbe.shared.lastOrientation = interfaceOrientation
        onFrame?(image.cgImage!)
    }
    func stopGracefully() {
        PlayerProbe.shared.stopCount += 1
        PlayerProbe.shared.lifecycle = "stopped"
        Task { @MainActor [weak self] in
            try? await Task.sleep(for: .milliseconds(200))
            self?.onExit?(0, nil)
        }
    }
    func enterBackground() {
        PlayerProbe.shared.lifecycle = "pausing"
        Task { @MainActor [weak self] in
            try? await Task.sleep(for: .milliseconds(200))
            self?.isBackgrounded = true
            PlayerProbe.shared.lifecycle = "paused"
        }
    }
    func enterForeground() { isBackgrounded = false; PlayerProbe.shared.lifecycle = "active" }
    func touch(_ phase: HVMTouchPhase, x: Float, y: Float, finger: UInt) {
        if phase == .began { activeFingers.insert(finger) }
        PlayerProbe.shared.maximumTouches = max(PlayerProbe.shared.maximumTouches, activeFingers.count)
        if phase == .cancelled { PlayerProbe.shared.cancellations += 1 }
        if phase == .ended || phase == .cancelled { activeFingers.remove(finger) }
        PlayerProbe.shared.touch = "\(phase.rawValue):\(Int(x.rounded())),\(Int(y.rounded())):\(finger)"
    }
    func setAccelerationX(_ x: Double, y: Double, z: Double) {}
}

struct PlayerHarness: View {
    let app: InstalledApp
    var body: some View {
        RunView(app: app, keepGoing: false)
            .overlay(alignment: .topTrailing) {
                VStack {
                    Text(PlayerProbe.shared.touch).accessibilityIdentifier("probe-touch")
                    Text(PlayerProbe.shared.lifecycle).accessibilityIdentifier("probe-lifecycle")
                    Text("\(PlayerProbe.shared.volume)").accessibilityIdentifier("probe-volume")
                    Text("\(PlayerProbe.shared.maximumTouches)").accessibilityIdentifier("probe-max-touches")
                    Text("\(PlayerProbe.shared.cancellations)").accessibilityIdentifier("probe-cancellations")
                    Button("Change Guest Orientation") {
                        guard let session = PlayerProbe.shared.session else { return }
                        session.interfaceOrientation = session.interfaceOrientation == 3 ? 4 : 3
                        session.deliverFrame()
                    }.accessibilityIdentifier("probe-rotate")
                    Button("Rotate During Touch") {
                        Task { @MainActor in
                            try? await Task.sleep(for: .seconds(2))
                            guard let session = PlayerProbe.shared.session else { return }
                            session.interfaceOrientation = session.interfaceOrientation == 3 ? 4 : 3
                            session.deliverFrame()
                        }
                    }.accessibilityIdentifier("probe-delayed-rotate")
                }
                .font(.system(size: 9))
                .foregroundStyle(.white)
                .padding(.top, 45)
            }
    }
}
