import Foundation

/// Why a `hydrogenvm://run/<game-id>` link did not open a game.
enum GameLinkError: Error, Equatable {
    /// Not a game link at all (wrong scheme, host, path shape or characters).
    case malformed
    /// A well-formed link for a game that is not installed (deleted, renamed, or another phone).
    case unknownGame(String)

    var message: String {
        switch self {
        case .malformed:
            return "That link is not a HydrogenVM game link. A game icon uses hydrogenvm://run/<game-id>."
        case .unknownGame(let id):
            return "No installed game has the id “\(id)”. It may have been deleted, reinstalled under another id, or belong to another device."
        }
    }
}

/// The Home Screen link for one game (`hydrogenvm://run/<game-id>`).
///
/// Foundation only (no UIKit/SwiftUI) so `tools/tests/swift/PlayerTests.swift` can
/// cover the parser on the macOS runner.
enum GameLink {
    static let scheme = "hydrogenvm"
    static let runHost = "run"

    /// Game ids are bundle identifiers: letters, digits, `.`, `-` and `_`, up to 255
    /// characters. Anything else is rejected so a link can never carry a path, a
    /// query or a `%20`-encoded space into the launcher.
    static func isGameID(_ id: String) -> Bool {
        guard !id.isEmpty, id.count <= 255 else { return false }
        guard !id.hasPrefix("."), !id.hasPrefix("-"), !id.hasSuffix("."), !id.hasSuffix("-") else { return false }
        return id.allSatisfy { c in
            c.isASCII && (c.isLetter || c.isNumber || c == "." || c == "-" || c == "_")
        }
    }

    /// The game id in `hydrogenvm://run/<game-id>`, or nil when the URL is not one.
    static func gameID(from url: URL) -> String? {
        guard let components = URLComponents(url: url, resolvingAgainstBaseURL: false) else { return nil }
        // Compare the scheme and host case-insensitively; URLComponents keeps the spelling it was given.
        guard components.scheme?.lowercased() == scheme, components.host?.lowercased() == runHost,
              components.query == nil, components.fragment == nil else { return nil }
        let path = components.path
        guard path.hasPrefix("/") else { return nil }
        let id = String(path.dropFirst())
        guard !id.contains("/"), isGameID(id) else { return nil }
        return id
    }

    /// The deep link for a game id, or nil when the id cannot be put in a link.
    static func url(forGameID id: String) -> URL? {
        guard isGameID(id) else { return nil }
        var components = URLComponents()
        components.scheme = scheme
        components.host = runHost
        components.path = "/" + id
        return components.url
    }

    /// What a link for `wanted` should do while `running` (the bundle id of the game on screen,
    /// if any) is open. A second game must not start over a running one: its session stops
    /// gracefully only when its screen closes.
    enum StartDecision: Equatable { case start, alreadyRunning, busy }

    static func startDecision(wanted: String, running: String?) -> StartDecision {
        guard let running else { return .start }
        return running.caseInsensitiveCompare(wanted) == .orderedSame ? .alreadyRunning : .busy
    }

    /// Resolve a link against the installed bundle identifiers. Success carries the
    /// installed spelling of the id, so a matched game is always found by the caller.
    static func resolve(_ url: URL, installed: [String]) -> Result<String, GameLinkError> {
        guard let id = gameID(from: url) else { return .failure(.malformed) }
        guard let match = installed.first(where: { $0.caseInsensitiveCompare(id) == .orderedSame }) else {
            return .failure(.unknownGame(id))
        }
        return .success(match)
    }
}

/// A game as a Shortcuts action ("Play <game>"), with no configuration profile.
///
/// The library's Add to Home Screen button offers this activity in the system
/// Add to Siri sheet, which saves it to the player's Shortcuts library in one tap.
/// From Shortcuts, Add to Home Screen gives the game an icon; running it opens
/// HydrogenVM with this activity, which becomes the usual `hydrogenvm://run/<id>`
/// link. An app cannot put an icon on the Home Screen by itself, with or without
/// a profile, so that last step is always the player's.
///
/// Foundation only, so `tools/tests/swift/PlayerTests.swift` covers it on macOS.
enum GameShortcut {
    /// Declared in Info.plist (`NSUserActivityTypes`).
    static let activityType = "com.lfull.HydrogenVM.play-game"
    static let gameKey = "gameID"

    static func title(forGameNamed name: String) -> String {
        let trimmed = name.trimmingCharacters(in: .whitespacesAndNewlines)
        return "Play " + (trimmed.isEmpty ? "Game" : trimmed)
    }

    /// The activity's userInfo, or nil for an id a link cannot carry.
    static func userInfo(forGameID id: String) -> [String: String]? {
        GameLink.url(forGameID: id) == nil ? nil : [gameKey: id]
    }

    /// The game link for an incoming activity, or nil if it is not ours or is malformed.
    static func link(activityType type: String, userInfo: [AnyHashable: Any]?) -> URL? {
        guard type == activityType, let id = userInfo?[gameKey] as? String else { return nil }
        return GameLink.url(forGameID: id)
    }
}
