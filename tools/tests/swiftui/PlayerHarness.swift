import SwiftUI
import UIKit
import os

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
    var keyboardText = ""
    var keyboardReturns = 0
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
    var onKeyboardState: (([String: NSNumber]) -> Void)?
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
    private func publishKeyboard(active: Bool) {
        onKeyboardState?(["active": NSNumber(value: active), "hasText": NSNumber(value: !PlayerProbe.shared.keyboardText.isEmpty),
                          "focusToken": NSNumber(value: active ? 1 : 2), "returnKey": NSNumber(value: UIReturnKeyType.done.rawValue)])
    }
    func requestKeyboard() { publishKeyboard(active: true) }
    func insertText(_ text: String) {
        PlayerProbe.shared.keyboardText += text
        publishKeyboard(active: true)
    }
    func deleteBackward() {
        if !PlayerProbe.shared.keyboardText.isEmpty { PlayerProbe.shared.keyboardText.removeLast() }
        publishKeyboard(active: true)
    }
    func submitTextReturn() {
        PlayerProbe.shared.keyboardReturns += 1
        publishKeyboard(active: false)
    }
}

/// The synthetic UI app has no CPU engine or signal-handler chain. It uses a
/// real, closable file for the snapshot interface; this does not qualify the
/// production engine's host-fault handler or simulate a successful crash save.
@MainActor enum HVMCrashJournal {
    private static var file: FileHandle?
    static func openSignalFile(_ path: String) {
        closeSignalFile()
        do {
            let url = URL(fileURLWithPath: path)
            try FileManager.default.createDirectory(at: url.deletingLastPathComponent(), withIntermediateDirectories: true)
            try Data().write(to: url)
            file = try FileHandle(forWritingTo: url)
        } catch { preconditionFailure("Could not open synthetic journal: \(error)") }
    }
    static func closeSignalFile() {
        do { try file?.close(); file = nil }
        catch { preconditionFailure("Could not close synthetic journal: \(error)") }
    }
    static func availableMemory() -> UInt64 { UInt64(os_proc_available_memory()) }
}

struct PlayerHarness: View {
    let app: InstalledApp
    var body: some View {
        RunView(app: app, keepGoing: false)
            .overlay(alignment: .topTrailing) {
                VStack {
                    Text(PlayerProbe.shared.touch).accessibilityIdentifier("probe-touch")
                    Text(PlayerProbe.shared.lifecycle).accessibilityIdentifier("probe-lifecycle")
                    Text(verbatim: String(PlayerProbe.shared.volume)).accessibilityIdentifier("probe-volume")
                    Text("\(PlayerProbe.shared.maximumTouches)").accessibilityIdentifier("probe-max-touches")
                    Text("\(PlayerProbe.shared.cancellations)").accessibilityIdentifier("probe-cancellations")
                    Text(PlayerProbe.shared.keyboardText).accessibilityIdentifier("probe-keyboard-text")
                    Text("\(PlayerProbe.shared.keyboardReturns)").accessibilityIdentifier("probe-keyboard-returns")
                    Button("Open Guest Keyboard") { PlayerProbe.shared.session?.requestKeyboard() }
                        .accessibilityIdentifier("probe-keyboard-open")
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
