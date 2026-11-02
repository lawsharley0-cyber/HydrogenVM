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

/// The Home Screen link for one game, and the Web Clip profile that installs its icon.
///
/// Foundation only (no UIKit/SwiftUI) so `tools/tests/swift/PlayerTests.swift` can
/// cover the parser and the generated profile on the macOS runner.
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

/// One Web Clip configuration profile (`.mobileconfig`) that adds a game to the Home
/// Screen with the game's own name and icon, opening `hydrogenvm://run/<game-id>`.
///
/// iOS installs it only after the user approves the profile in Settings; an app cannot
/// put an icon on the Home Screen by itself. The payload identifiers are derived from
/// the game id, so installing a second clip for the same game updates the first one
/// instead of adding a duplicate.
struct WebClipProfile {
    let gameID: String
    let title: String
    let link: URL
    /// The game's icon as PNG, or nil when the bundle's icon could not be decoded;
    /// iOS then draws its own placeholder and the clip still works.
    let iconPNG: Data?

    var payloadUUID: UUID { Self.uuid(seed: "webclip-payload:" + gameID) }
    var configUUID: UUID { Self.uuid(seed: "webclip-config:" + gameID) }
    var identifier: String { "com.lfull.HydrogenVM.webclip." + gameID }
    var configIdentifier: String { "com.lfull.HydrogenVM.webclip.config." + gameID }

    /// A file name the user sees in Files; the game id stays in the directory name.
    var filename: String {
        let mapped = title.map { $0.isLetter || $0.isNumber ? $0 : "-" }
        let base = String(mapped).trimmingCharacters(in: CharacterSet(charactersIn: "-"))
        return (base.isEmpty ? "Game" : base) + ".mobileconfig"
    }

    /// The `.mobileconfig` document. Deterministic for the same game and icon.
    func payload() -> String {
        let name = Self.xml(title)
        let url = Self.xml(link.absoluteString)
        let description = Self.xml("Opens \(title) in HydrogenVM.")
        var lines: [String] = [
            "<?xml version=\"1.0\" encoding=\"UTF-8\"?>",
            "<!DOCTYPE plist PUBLIC \"-//Apple//DTD PLIST 1.0//EN\" \"http://www.apple.com/DTDs/PropertyList-1.0.dtd\">",
            "<plist version=\"1.0\">",
            "<dict>",
            "\t<key>PayloadContent</key>",
            "\t<array>",
            "\t\t<dict>",
            "\t\t\t<key>FullScreen</key>",
            "\t\t\t<true/>",
        ]
        if let icon = iconPNG?.base64EncodedString() {
            lines.append("\t\t\t<key>Icon</key>")
            lines.append("\t\t\t<data>\(icon)</data>")
        }
        lines += [
            "\t\t\t<key>IsRemovable</key>",
            "\t\t\t<true/>",
            "\t\t\t<key>Label</key>",
            "\t\t\t<string>\(name)</string>",
            "\t\t\t<key>PayloadDescription</key>",
            "\t\t\t<string>\(description)</string>",
            "\t\t\t<key>PayloadDisplayName</key>",
            "\t\t\t<string>\(name)</string>",
            "\t\t\t<key>PayloadIdentifier</key>",
            "\t\t\t<string>\(Self.xml(identifier))</string>",
            "\t\t\t<key>PayloadType</key>",
            "\t\t\t<string>com.apple.webClip.managed</string>",
            "\t\t\t<key>PayloadUUID</key>",
            "\t\t\t<string>\(payloadUUID.uuidString)</string>",
            "\t\t\t<key>PayloadVersion</key>",
            "\t\t\t<integer>1</integer>",
            "\t\t\t<key>URL</key>",
            "\t\t\t<string>\(url)</string>",
            "\t\t</dict>",
            "\t</array>",
            "\t<key>PayloadDescription</key>",
            "\t<string>\(description)</string>",
            "\t<key>PayloadDisplayName</key>",
            "\t<string>\(name) on the Home Screen</string>",
            "\t<key>PayloadIdentifier</key>",
            "\t<string>\(Self.xml(configIdentifier))</string>",
            "\t<key>PayloadOrganization</key>",
            "\t<string>HydrogenVM</string>",
            "\t<key>PayloadRemovalDisallowed</key>",
            "\t<false/>",
            "\t<key>PayloadType</key>",
            "\t<string>Configuration</string>",
            "\t<key>PayloadUUID</key>",
            "\t<string>\(configUUID.uuidString)</string>",
            "\t<key>PayloadVersion</key>",
            "\t<integer>1</integer>",
            "</dict>",
            "</plist>",
        ]
        return lines.joined(separator: "\n") + "\n"
    }

    private static func xml(_ text: String) -> String {
        var out = ""
        for c in text {
            switch c {
            case "&": out += "&amp;"
            case "<": out += "&lt;"
            case ">": out += "&gt;"
            case "\"": out += "&quot;"
            case "'": out += "&apos;"
            default: out.append(c)
            }
        }
        return out
    }

    /// A stable UUID in version-4 layout, so the same game always replaces its own clip.
    static func uuid(seed: String) -> UUID {
        let high = hash64(seed, offset: 0xcbf2_9ce4_8422_2325)
        let low = hash64(seed + "#2", offset: 0x8422_2325_cbf2_9ce4)
        var bytes = [UInt8](repeating: 0, count: 16)
        for i in 0..<8 { bytes[i] = UInt8(truncatingIfNeeded: high >> (56 - 8 * UInt64(i))) }
        for i in 0..<8 { bytes[8 + i] = UInt8(truncatingIfNeeded: low >> (56 - 8 * UInt64(i))) }
        bytes[6] = (bytes[6] & 0x0f) | 0x40
        bytes[8] = (bytes[8] & 0x3f) | 0x80
        return UUID(uuid: (bytes[0], bytes[1], bytes[2], bytes[3], bytes[4], bytes[5], bytes[6], bytes[7],
                           bytes[8], bytes[9], bytes[10], bytes[11], bytes[12], bytes[13], bytes[14], bytes[15]))
    }

    /// FNV-1a, 64-bit: small, dependency-free and identical on every platform.
    private static func hash64(_ text: String, offset: UInt64) -> UInt64 {
        var h = offset
        for byte in text.utf8 {
            h = (h ^ UInt64(byte)) &* 0x0000_0100_0000_01b3
        }
        return h
    }
}
