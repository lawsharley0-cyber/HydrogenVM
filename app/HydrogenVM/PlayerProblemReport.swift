import Foundation

/// A local report snapshot. Construction has no clipboard, file or network effects.
/// Raw guest output is untrusted: only diagnostic categories, numeric machine
/// state and recognizable runtime symbols are exported. Account-bearing lines,
/// URLs, paths and free-form guest messages are omitted rather than guessed safe.
struct PlayerProblemReport: Identifiable {
    let id = UUID()
    let text: String
    let issueURL: URL

    init(bundleID: String, gameVersion: String, device: String, osVersion: String,
         build: String, log: [String], crashJSON: String?) {
        let metadata = ["gameBundleID": Self.identifier(bundleID),
                        "gameVersion": Self.identifier(gameVersion),
                        "device": Self.identifier(device), "iOSVersion": Self.identifier(osVersion),
                        "hydrogenBuild": Self.identifier(build)]
        var fields: [String: Any] = ["schema": "hvm-player-problem/v1", "metadata": metadata,
                                     "runLog": Self.diagnosticLog(log),
                                     "privacy": "Free-form guest output, account data, URLs and paths are omitted. Review before sharing."]
        if let crashJSON {
            fields["crash"] = Self.machineCrash(crashJSON)
        }
        let data = (try? JSONSerialization.data(withJSONObject: fields, options: [.prettyPrinted, .sortedKeys])) ?? Data()
        text = String(decoding: data, as: UTF8.self)
        var url = URLComponents(string: "https://github.com/lawsharley0-cyber/HydrogenVM/issues/new")!
        let title = "[problem] \(metadata["gameBundleID"] ?? "game")"
        let body = "Game: \(metadata["gameBundleID"] ?? "") \(metadata["gameVersion"] ?? "")\n"
            + "HydrogenVM: \(metadata["hydrogenBuild"] ?? "")\n"
            + "Device: \(metadata["device"] ?? ""), iOS \(metadata["iOSVersion"] ?? "")\n\n"
            + "What happened, and how can it be reproduced?\n\n"
            + "Attach or paste the reviewed report using Copy report or Share report in the player.\n"
        url.queryItems = [URLQueryItem(name: "title", value: title), URLQueryItem(name: "body", value: body)]
        issueURL = url.url!
    }

    private static func identifier(_ value: String) -> String {
        String(value.prefix(160).filter { $0.isASCII && ($0.isLetter || $0.isNumber || ".,_+-() ".contains($0)) })
    }

    private static func matches(_ pattern: String, _ text: String) -> [String] {
        guard let regex = try? NSRegularExpression(pattern: pattern) else { return [] }
        let range = NSRange(text.startIndex..., in: text)
        return regex.matches(in: text, range: range).compactMap {
            Range($0.range, in: text).map { String(text[$0]) }
        }
    }

    static func diagnosticLog(_ lines: [String]) -> [String] {
        // Output is deliberately constructed from an allowlist, never a lightly
        // redacted copy of arbitrary NSLog/network/response/account contents.
        let categories = ["Crash:", "Stall:", "Snapshot:", "Hitch:", "Missing function:",
                          "Missing selector:", "Missing class:", "Unimplemented:",
                          "Defaulted:", "Loaded ", "GPU:", "Compatibility profile:",
                          "bad memory access", "Undefined instruction", "Stopped:"]
        var output: [String] = []
        var omitted = 0
        for raw in lines.suffix(4000) {
            let line = String(raw.prefix(4096))
            // Discard the entire line if it may carry account/credential data.
            if !matches("(?i)(account|user.?name|user.?id|password|passwd|token|secret|authorization|cookie|bearer|email|login|session.?id|receipt|purchase|keychain|https?://|[^ ]+@[^ ]+)", line).isEmpty {
                omitted += 1; continue
            }
            let unprefixed = line.replacingOccurrences(of: "^\\[hvm(?::[^]]+)?\\]\\s*", with: "", options: .regularExpression)
            guard let category = categories.first(where: { unprefixed.hasPrefix($0) }) else {
                omitted += 1; continue
            }
            // Paths and quoted guest values are excluded before extracting state.
            let machine = unprefixed.replacingOccurrences(of: "(?:[A-Za-z]:[\\\\/]|/)[^\\s,;]+|\"[^\"]*\"|'[^']*'",
                                                           with: " ", options: .regularExpression)
            let addresses = matches("\\b0x[0-9a-fA-F]{1,16}\\b", machine)
            let symbols = matches("(?<![A-Za-z0-9])_[A-Za-z][A-Za-z0-9_]{1,100}\\b|[-+]\\[[A-Za-z_][A-Za-z0-9_]* [A-Za-z_][A-Za-z0-9_:]*\\]", machine)
            output.append(category.trimmingCharacters(in: .whitespaces) + " " + (addresses + symbols).prefix(24).joined(separator: " "))
        }
        output.append("[\(omitted) free-form or sensitive lines omitted]")
        return output
    }

    static func machineCrash(_ json: String) -> [String: Any] {
        guard let data = json.data(using: .utf8),
              let object = (try? JSONSerialization.jsonObject(with: data)) as? [String: Any] else {
            return ["available": true, "note": "Crash text omitted; not structured JSON"]
        }
        var result: [String: Any] = ["available": true]
        // Reject arbitrary strings and nested dictionaries (logs, account
        // state, paths, request payloads). Only machine numbers are exported.
        for key in ["pc", "lr", "sp", "faultAddress", "address", "elapsedSeconds", "guestInstructions"] {
            if let number = object[key] as? NSNumber { result[key] = number }
        }
        if let registers = object["registers"] as? [String: Any] {
            var safe: [String: NSNumber] = [:]
            for key in (0...15).map({ "r\($0)" }) + ["pc", "lr", "sp", "cpsr"] {
                if let number = registers[key] as? NSNumber { safe[key] = number }
            }
            result["registers"] = safe
        } else if let registers = object["registers"] as? [NSNumber] {
            result["registers"] = Array(registers.prefix(16))
        }
        if let signature = object["signature"] as? String,
           signature.range(of: "^[0-9a-fA-F]{8,64}$", options: .regularExpression) != nil { result["signature"] = signature }
        if let trace = object["backtrace"] as? [String] {
            result["backtrace"] = trace.prefix(32).map { diagnosticLog(["Crash: " + $0]).first ?? "[omitted]" }
        }
        return result
    }
}
