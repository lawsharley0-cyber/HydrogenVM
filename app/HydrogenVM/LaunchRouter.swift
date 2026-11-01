import Foundation
import Observation

/// One deep link waiting to be opened, shared by the app delegate (cold start) and
/// SwiftUI's `.onOpenURL` (already running). Only `hydrogenvm://` links are kept:
/// `.ipa` imports keep going through the library's own `.onOpenURL`.
@Observable
@MainActor
final class LaunchRouter {
    static let shared = LaunchRouter()

    /// Set as soon as a game link arrives, cleared when the library has opened it.
    var pending: URL?
    /// A link that could not open a game, shown to the player once.
    var message: String?

    func open(_ url: URL) {
        guard GameLink.gameID(from: url) != nil || url.scheme?.lowercased() == GameLink.scheme else { return }
        pending = url
    }
}
