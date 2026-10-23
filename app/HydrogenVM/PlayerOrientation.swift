import SwiftUI
import UIKit

/// Follows the guest's interface orientation while the player is presented.
@Observable
@MainActor
final class OrientationLock {
    static let shared = OrientationLock()
    private(set) var mask: UIInterfaceOrientationMask = .all
    private(set) var errorMessage: String?

    func lock(_ mask: UIInterfaceOrientationMask) {
        self.mask = mask
        errorMessage = nil
        for case let scene as UIWindowScene in UIApplication.shared.connectedScenes where scene.activationState == .foregroundActive {
            scene.keyWindow?.rootViewController?.setNeedsUpdateOfSupportedInterfaceOrientations()
            scene.requestGeometryUpdate(.iOS(interfaceOrientations: mask)) { [weak self] error in
                Task { @MainActor in self?.errorMessage = error.localizedDescription }
            }
        }
    }
}

