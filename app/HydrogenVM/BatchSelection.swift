import Foundation

/// The rules for which games a Test All Games run covers, kept free of UIKit and the
/// engine so tools/tests/swift/BatchSelectionTests.swift can compile and check them alone.
enum BatchSelection {
    /// The saved game pick inside batch options. Options saved before the pick existed have
    /// no `onlyGames` key; they decode to an empty pick, which means every game.
    struct Pick: Decodable, Equatable {
        var onlyGames: [String]
        private enum CodingKeys: String, CodingKey { case onlyGames }
        init(onlyGames: [String] = []) { self.onlyGames = onlyGames }
        init(from decoder: Decoder) throws {
            let c = try decoder.container(keyedBy: CodingKeys.self)
            onlyGames = try c.decodeIfPresent([String].self, forKey: .onlyGames) ?? []
        }
    }

    /// The runnable games a run covers, in library order. An empty pick means every runnable
    /// game; picked ids that are no longer installed are skipped.
    static func games<T>(_ library: [T], pick: [String], id: (T) -> String, runnable: (T) -> Bool) -> [T] {
        let chosen = Set(pick)
        return library.filter { runnable($0) && (chosen.isEmpty || chosen.contains(id($0))) }
    }

    /// What one result needs for the retest decision.
    struct Outcome: Equatable {
        var bundleId: String
        var crashed: Bool
        var signal: String
        var failedCheck: Bool
    }

    /// Bundle ids (first-seen order, each once) of the games that crashed, did not show a
    /// good picture or failed a check. A game with only warnings is not a problem here.
    static func gamesWithProblems(_ outcomes: [Outcome]) -> [String] {
        var seen = Set<String>()
        return outcomes.filter { $0.crashed || $0.signal != "ok" || $0.failedCheck }
            .map(\.bundleId).filter { seen.insert($0).inserted }
    }

    /// "Retest the game with problems" or "Retest the 3 games with problems".
    static func retestTitle(_ count: Int) -> String {
        count == 1 ? "Retest the game with problems" : "Retest the \(count) games with problems"
    }

    /// The `options.selection` entry of report.json: whether only some games were picked,
    /// which ones, and how many runnable games the library had when the run started.
    static func reportEntry(pick: [String], libraryGames: Int) -> [String: Any] {
        ["partial": !pick.isEmpty, "selected": pick, "libraryGames": libraryGames]
    }

    /// The "Games" line of report.md.
    static func markdownLine(pick: [String], libraryGames: Int) -> String {
        if pick.isEmpty { return "**Games:** all \(libraryGames) (full run)" }
        let ids = pick.map { "`\($0)`" }.joined(separator: ", ")
        return "**Games:** \(pick.count) of \(libraryGames) selected (partial run): \(ids)"
    }

    /// True when a saved report.json is a finished full run with at least one result, i.e.
    /// worth keeping when a later run replaces the latest report. Reports written before the
    /// selection existed have no `selection` entry and were always full runs; reports without
    /// `runComplete` count as finished. A cancelled or stopped run says `runComplete: false`.
    static func isFullRunWithResults(_ reportJSON: Data) -> Bool {
        guard let root = (try? JSONSerialization.jsonObject(with: reportJSON)) as? [String: Any],
              root["kind"] as? String == "hvm-compat-report" else { return false }
        if (root["options"] as? [String: Any])?["runComplete"] as? Bool == false { return false }
        let selection = (root["options"] as? [String: Any])?["selection"] as? [String: Any]
        if selection?["partial"] as? Bool == true { return false }
        let apps = ((root["suites"] as? [String: Any])?["apps"] as? [String: Any])?["apps"] as? [Any]
        return !(apps ?? []).isEmpty
    }

    /// The thumbnailPath of every row of a saved report.json (empty for anything else).
    static func thumbnailPaths(inReport reportJSON: Data) -> [String?] {
        guard let root = (try? JSONSerialization.jsonObject(with: reportJSON)) as? [String: Any],
              let apps = ((root["suites"] as? [String: Any])?["apps"] as? [String: Any])?["apps"] as? [[String: Any]] else { return [] }
        return apps.map { ($0["frame"] as? [String: Any])?["thumbnailPath"] as? String }
    }

    /// The thumbnail files to export: the ones this run's results point at ("thumbnails/x.png"),
    /// each once, never a path that leaves the thumbnails folder.
    static func thumbnailFiles(_ paths: [String?]) -> [String] {
        var seen = Set<String>()
        return paths.compactMap { path -> String? in
            guard let path, path.hasPrefix("thumbnails/") else { return nil }
            let name = String(path.dropFirst("thumbnails/".count))
            guard !name.isEmpty, !name.contains("/"), name != ".", name != ".." else { return nil }
            return seen.insert(name).inserted ? name : nil
        }
    }
}
