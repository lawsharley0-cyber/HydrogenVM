import Foundation

/// Real files in a temporary folder, with one kind of operation made to fail on demand.
class FaultyFiles: SnapshotFiles {
    let disk = DiskSnapshotFiles()
    var failCopy = false
    /// Moves whose destination's last path component is in this set fail.
    var failMovesTo: Set<String> = []
    func exists(_ url: URL) -> Bool { disk.exists(url) }
    func makeDirectory(_ url: URL) throws { try disk.makeDirectory(url) }
    func write(_ data: Data, to url: URL) throws { try disk.write(data, to: url) }
    func copy(_ from: URL, to: URL) throws {
        if failCopy { throw CocoaError(.fileWriteUnknown) }
        try disk.copy(from, to: to)
    }
    func move(_ from: URL, to: URL) throws {
        if failMovesTo.contains(to.lastPathComponent) { throw CocoaError(.fileWriteUnknown) }
        try disk.move(from, to: to)
    }
    func remove(_ url: URL) { disk.remove(url) }
}

/// Test All Games selection rules (app/HydrogenVM/BatchSelection.swift). Expected values are
/// worked out by hand from the rules in that file, never copied from a run.
@main
@MainActor
struct BatchSelectionTests {
    static var checks = 0

    static func check(_ condition: @autoclosure () -> Bool, _ message: String) {
        checks += 1
        precondition(condition(), message)
    }

    struct Game { let id: String; let runnable: Bool }

    static func pick(_ json: String) throws -> [String] {
        try JSONDecoder().decode(BatchSelection.Pick.self, from: Data(json.utf8)).onlyGames
    }

    static func chosen(_ library: [Game], _ pick: [String]) -> [String] {
        BatchSelection.games(library, pick: pick, id: \.id, runnable: \.runnable).map(\.id)
    }

    static func outcome(_ id: String, crashed: Bool = false, signal: String = "ok", failedCheck: Bool = false) -> BatchSelection.Outcome {
        BatchSelection.Outcome(bundleId: id, crashed: crashed, signal: signal, failedCheck: failedCheck)
    }

    static func report(partial: Bool?, apps: Int, kind: String = "hvm-compat-report", complete: Bool? = nil) -> Data {
        var options: [String: Any] = ["seconds": 10]
        if let complete { options["runComplete"] = complete }
        if let partial { options["selection"] = ["partial": partial, "selected": partial ? ["a"] : [], "libraryGames": 3] }
        let rows = (0..<apps).map { ["bundleId": "id\($0)"] }
        let root: [String: Any] = ["kind": kind, "options": options, "suites": ["apps": ["apps": rows]]]
        return try! JSONSerialization.data(withJSONObject: root)
    }

    static func main() throws {
        // A session saved before the pick existed: options without onlyGames mean every game.
        let oldSession = #"{"secondsPerApp":10,"rendererMode":"both","keepGoing":true,"resetSandbox":false,"exploreEnabled":true,"exploreTime":30,"relaunchEnabled":true}"#
        let legacy = try pick(oldSession), saved = try pick(#"{"onlyGames":["b","a"]}"#), empty = try pick(#"{"onlyGames":[]}"#)
        check(legacy == [], "an old session decodes to an empty pick")
        check(saved == ["b", "a"], "a saved pick keeps its ids and order")
        check(empty == [], "an explicit empty pick stays empty")

        // Library order a, b (not runnable), c, d.
        let library = [Game(id: "a", runnable: true), Game(id: "b", runnable: false),
                       Game(id: "c", runnable: true), Game(id: "d", runnable: true)]
        check(chosen(library, []) == ["a", "c", "d"], "an empty pick tests every runnable game")
        check(chosen(library, ["d", "a"]) == ["a", "d"], "a pick runs in library order, not pick order")
        check(chosen(library, ["b", "c"]) == ["c"], "a picked game that cannot run is skipped")
        check(chosen(library, ["gone", "c"]) == ["c"], "a picked game no longer installed is skipped")
        check(chosen(library, ["gone"]) == [], "a pick of only removed games tests nothing, not every game")

        // Problems: crash, bad picture, failed check. Warnings alone are not a problem.
        let results = [
            outcome("ok"),
            outcome("warnOnly"),                                  // warn checks give failedCheck false
            outcome("crash", crashed: true, signal: "no-frames"),
            outcome("black", signal: "all-black"),
            outcome("check", failedCheck: true),
            outcome("crash", crashed: true),                      // its second renderer crashed too: listed once
            outcome("late", signal: "ok"),
            outcome("late", crashed: true),                        // only its second renderer crashed
        ]
        check(BatchSelection.gamesWithProblems(results) == ["crash", "black", "check", "late"],
              "crashed, bad-picture and failed-check games, each once, first-seen order")
        check(BatchSelection.gamesWithProblems([outcome("x"), outcome("y")]) == [], "all ok gives nothing to retest")

        check(BatchSelection.retestTitle(1) == "Retest the game with problems", "one game is singular")
        check(BatchSelection.retestTitle(2) == "Retest the 2 games with problems", "two games are plural")
        check(BatchSelection.retestTitle(38) == "Retest the 38 games with problems", "many games are plural")

        let full = BatchSelection.reportEntry(pick: [], libraryGames: 38)
        check(full["partial"] as? Bool == false && (full["selected"] as? [String]) == [] && full["libraryGames"] as? Int == 38,
              "a full run is not partial")
        let part = BatchSelection.reportEntry(pick: ["a", "c"], libraryGames: 38)
        check(part["partial"] as? Bool == true && (part["selected"] as? [String]) == ["a", "c"] && part["libraryGames"] as? Int == 38,
              "a partial run lists its picked ids")
        check(BatchSelection.markdownLine(pick: [], libraryGames: 38) == "**Games:** all 38 (full run)", "markdown full run")
        check(BatchSelection.markdownLine(pick: ["a", "c"], libraryGames: 38) == "**Games:** 2 of 38 selected (partial run): `a`, `c`",
              "markdown partial run")

        check(BatchSelection.isFullRunWithResults(report(partial: false, apps: 2)), "a full run with results is kept")
        check(BatchSelection.isFullRunWithResults(report(partial: nil, apps: 2)), "an older report without a selection was a full run")
        check(!BatchSelection.isFullRunWithResults(report(partial: true, apps: 2)), "a partial run never replaces the kept full run")
        check(!BatchSelection.isFullRunWithResults(report(partial: false, apps: 0)), "an empty report (run just started) is not kept")
        check(!BatchSelection.isFullRunWithResults(report(partial: false, apps: 2, complete: false)), "a cancelled or unfinished full run is not kept")
        check(BatchSelection.isFullRunWithResults(report(partial: false, apps: 2, complete: true)), "a finished full run is kept")
        check(!BatchSelection.isFullRunWithResults(report(partial: false, apps: 2, kind: "other")), "other JSON is not a report")
        check(!BatchSelection.isFullRunWithResults(Data("not json".utf8)), "garbage is not a report")

        check(BatchSelection.thumbnailFiles(["thumbnails/a_gpu.png", nil, "", "thumbnails/a_gpu.png", "thumbnails/c_software.png"])
              == ["a_gpu.png", "c_software.png"], "only this run's thumbnails, each once")
        check(BatchSelection.thumbnailFiles(["thumbnails/../x.png", "thumbnails/", "other/a.png", "thumbnails/.."]) == [],
              "nothing outside the thumbnails folder")

        // The kept full report's pictures: every row's thumbnailPath, then the same filter as an export.
        let keptReport: [String: Any] = ["kind": "hvm-compat-report", "suites": ["apps": ["apps": [
            ["bundleId": "a", "frame": ["signal": "ok", "thumbnailPath": "thumbnails/a_gpu.png"]],
            ["bundleId": "b", "frame": ["signal": "no-frames", "thumbnailPath": ""]],
            ["bundleId": "c", "frame": ["signal": "ok"]],
            ["bundleId": "d", "frame": ["signal": "ok", "thumbnailPath": "thumbnails/d_software.png"]]]]]]
        let savedPaths = BatchSelection.thumbnailPaths(inReport: try JSONSerialization.data(withJSONObject: keptReport))
        check(savedPaths == ["thumbnails/a_gpu.png", "", nil, "thumbnails/d_software.png"], "a saved report's thumbnail paths, row by row")
        check(BatchSelection.thumbnailFiles(savedPaths) == ["a_gpu.png", "d_software.png"], "the kept full report keeps its own pictures")
        check(BatchSelection.thumbnailPaths(inReport: Data("[]".utf8)).isEmpty, "not a report: no pictures")

        try snapshotChecks()

        print("BatchSelectionTests: \(checks) checks passed")
    }

    // MARK: last_full_run snapshot (BatchSnapshot.keepLastFullRun)

    static func text(_ url: URL) -> String? { (try? Data(contentsOf: url)).map { String(decoding: $0, as: UTF8.self) } }

    /// latest_batch_report.json with one row per (bundle id, thumbnail path).
    static func writeLatest(_ reports: URL, partial: Bool, rows: [(String, String)]) throws {
        let apps = rows.map { ["bundleId": $0.0, "frame": ["signal": "ok", "thumbnailPath": $0.1]] as [String: Any] }
        let root: [String: Any] = ["kind": "hvm-compat-report", "options": ["selection": ["partial": partial]],
                                   "suites": ["apps": ["apps": apps]]]
        try JSONSerialization.data(withJSONObject: root).write(to: reports.appending(path: "latest_batch_report.json"))
        try Data((partial ? "partial" : "full").utf8).write(to: reports.appending(path: "latest_batch_report.md"))
    }

    static func snapshotChecks() throws {
        let fm = FileManager.default
        let reports = fm.temporaryDirectory.appending(path: "batch-snapshot-\(UUID().uuidString)", directoryHint: .isDirectory)
        defer { try? fm.removeItem(at: reports) }
        let thumbs = reports.appending(path: "thumbnails", directoryHint: .isDirectory)
        try fm.createDirectory(at: thumbs, withIntermediateDirectories: true)
        let final = BatchSnapshot.folder(in: reports)
        let kept = final.appending(path: "thumbnails/a_gpu.png")
        let staging = reports.appending(path: "last_full_run.staging")
        let aside = reports.appending(path: "last_full_run.old")
        let picture = thumbs.appending(path: "a_gpu.png")
        let files = FaultyFiles()
        func keep() -> BatchSnapshot.Outcome { BatchSnapshot.keepLastFullRun(reports: reports, thumbnails: thumbs, files: files) }

        check(keep() == .nothingToKeep, "no latest report: nothing to keep")

        // 1. A full run is kept with its own copy of its picture.
        try Data("first".utf8).write(to: picture)
        try writeLatest(reports, partial: false, rows: [("a", "thumbnails/a_gpu.png")])
        check(keep() == .kept, "a full run is kept")
        check(text(kept) == "first" && text(final.appending(path: "report.md")) == "full", "the kept folder has the report and its picture")
        check(files.exists(final.appending(path: "report.json")), "the kept folder has report.json")

        // 2. A retest overwrites the picture and writes a partial report: the kept copy is unchanged.
        try Data("retest".utf8).write(to: picture)
        try writeLatest(reports, partial: true, rows: [("a", "thumbnails/a_gpu.png")])
        check(keep() == .nothingToKeep && text(kept) == "first", "a partial run never replaces the kept full run")
        // An empty full report (a run that has only just started) leaves the snapshot too.
        try writeLatest(reports, partial: false, rows: [])
        check(keep() == .nothingToKeep && text(kept) == "first" && text(final.appending(path: "report.md")) == "full",
              "an empty report leaves the kept full run unchanged")
        // A finished-run marker of false (a cancelled full run) leaves it too.
        let cancelled: [String: Any] = ["kind": "hvm-compat-report", "options": ["runComplete": false, "selection": ["partial": false]],
                                        "suites": ["apps": ["apps": [["bundleId": "a", "frame": ["signal": "ok", "thumbnailPath": "thumbnails/a_gpu.png"]]]]]]
        try JSONSerialization.data(withJSONObject: cancelled).write(to: reports.appending(path: "latest_batch_report.json"))
        check(keep() == .nothingToKeep && text(kept) == "first", "a cancelled full run leaves the kept full run unchanged")

        // 3. A full run naming a picture that is missing fails and keeps the previous snapshot.
        try writeLatest(reports, partial: false, rows: [("a", "thumbnails/a_gpu.png"), ("b", "thumbnails/b_gpu.png")])
        check(keep() == .failed, "a missing named picture fails the snapshot")
        check(text(kept) == "first" && !files.exists(final.appending(path: "thumbnails/b_gpu.png")), "the previous snapshot is untouched")
        check(!files.exists(staging), "no staging folder is left behind")

        // 4. A copy failure fails and keeps the previous snapshot.
        try writeLatest(reports, partial: false, rows: [("a", "thumbnails/a_gpu.png")])
        files.failCopy = true
        check(keep() == .failed && text(kept) == "first" && !files.exists(staging), "a copy failure keeps the previous snapshot")
        files.failCopy = false

        // 5. Publishing fails and so does the rollback: the previous snapshot waits in .old,
        //    and the next call restores it before anything else.
        files.failMovesTo = ["last_full_run"]
        check(keep() == .failed, "a failed publish reports failure")
        check(!files.exists(final) && text(aside.appending(path: "thumbnails/a_gpu.png")) == "first", "the previous snapshot survives in .old")
        files.failMovesTo = []
        try writeLatest(reports, partial: true, rows: [("a", "thumbnails/a_gpu.png")])
        check(keep() == .nothingToKeep && text(kept) == "first" && !files.exists(aside), "the next call restores the previous snapshot")

        // 6. Publishing fails but the rollback works: the previous snapshot is back in place.
        try writeLatest(reports, partial: false, rows: [("a", "thumbnails/a_gpu.png")])
        // (Fail only the staging -> final move: the first move to .old must succeed.)
        final class OnceFaulty: FaultyFiles {
            var failedOnce = false
            override func move(_ from: URL, to: URL) throws {
                if !failedOnce, from.lastPathComponent == "last_full_run.staging" { failedOnce = true; throw CocoaError(.fileWriteUnknown) }
                try super.move(from, to: to)
            }
        }
        let once = OnceFaulty()
        check(BatchSnapshot.keepLastFullRun(reports: reports, thumbnails: thumbs, files: once) == .failed, "a failed publish with a good rollback fails")
        check(text(kept) == "first" && !files.exists(aside) && !files.exists(staging), "the rollback put the previous snapshot back")

        // 7. Then a full run replaces the snapshot, picture and all.
        check(keep() == .kept && text(kept) == "retest", "a later full run replaces the snapshot")
        check(!files.exists(aside) && !files.exists(staging), "no .old or staging folder is left after a swap")
    }
}
