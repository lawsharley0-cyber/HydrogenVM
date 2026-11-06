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

#if COLD_START_URL_TEST && targetEnvironment(simulator)
/// Mark the actual handoff stages for the simulator regression test.
enum ColdStartURLTest {
    static func record(_ stage: String, value: String) {
        let file = URL.documentsDirectory.appending(path: "cold-start-" + stage)
        do {
            try FileManager.default.createDirectory(at: file.deletingLastPathComponent(), withIntermediateDirectories: true)
            try value.write(to: file, atomically: true, encoding: .utf8)
        } catch {
            FileHandle.standardError.write(Data("Cold-start marker failed at \(file.path): \(error)\n".utf8))
        }
    }

    static func queued(_ url: URL) -> Bool {
        let file = URL.documentsDirectory.appending(path: "cold-start-queued")
        return (try? String(contentsOf: file, encoding: .utf8)) == url.absoluteString
    }
}
#endif
