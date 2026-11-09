import Foundation

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

    static func report(partial: Bool?, apps: Int, kind: String = "hvm-compat-report") -> Data {
        var options: [String: Any] = ["seconds": 10]
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
        check(!BatchSelection.isFullRunWithResults(report(partial: false, apps: 2, kind: "other")), "other JSON is not a report")
        check(!BatchSelection.isFullRunWithResults(Data("not json".utf8)), "garbage is not a report")

        check(BatchSelection.thumbnailFiles(["thumbnails/a_gpu.png", nil, "", "thumbnails/a_gpu.png", "thumbnails/c_software.png"])
              == ["a_gpu.png", "c_software.png"], "only this run's thumbnails, each once")
        check(BatchSelection.thumbnailFiles(["thumbnails/../x.png", "thumbnails/", "other/a.png", "thumbnails/.."]) == [],
              "nothing outside the thumbnails folder")

        print("BatchSelectionTests: \(checks) checks passed")
    }
}
