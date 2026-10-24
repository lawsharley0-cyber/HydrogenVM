import SwiftUI
import UIKit

/// Follows the guest's interface orientation while the player is presented.
@Observable
@MainActor
final class OrientationLock {
    static let shared = OrientationLock()
    private(set) var mask: UIInterfaceOrientationMask = .all
    private(set) var errorMessage: String?

    private var widenTask: Task<Void, Never>?

    /// Turns the window to `preferred` first (the game's own orientation, so it
    /// starts the way the game expects even with rotation lock on), then allows
    /// `mask`, so the phone can turn it to the other landscape afterwards.
    func lock(_ mask: UIInterfaceOrientationMask, preferring preferred: UIInterfaceOrientationMask? = nil) {
        widenTask?.cancel()
        errorMessage = nil
        let first = preferred ?? mask
        self.mask = first
        for case let scene as UIWindowScene in UIApplication.shared.connectedScenes where scene.activationState == .foregroundActive {
            scene.keyWindow?.rootViewController?.setNeedsUpdateOfSupportedInterfaceOrientations()
            scene.requestGeometryUpdate(.iOS(interfaceOrientations: first)) { [weak self] error in
                Task { @MainActor in self?.errorMessage = error.localizedDescription }
            }
        }
        guard first != mask else { return }
        widenTask = Task { @MainActor [weak self] in
            try? await Task.sleep(for: .milliseconds(700))
            guard let self, !Task.isCancelled else { return }
            self.mask = mask
            for case let scene as UIWindowScene in UIApplication.shared.connectedScenes where scene.activationState == .foregroundActive {
                scene.keyWindow?.rootViewController?.setNeedsUpdateOfSupportedInterfaceOrientations()
            }
        }
    }
}

