import Foundation
import XCTest
@testable import HydrogenVM

final class ProblemReporterTests: XCTestCase {
    func testDedupeKeyExtractsCrashSignatureAndConsoleGameMessage() {
        XCTAssertEqual(
            ProblemReporter.dedupeKey(forTitle: "[3a0007225e51] com.example.game: SIGSEGV at 0x0"),
            .signature("3a0007225e51")
        )
        XCTAssertEqual(
            ProblemReporter.dedupeKey(forTitle: "[console] com.example.game: missing symbol foo"),
            .console(game: "com.example.game", message: "missing symbol foo")
        )
        XCTAssertNil(ProblemReporter.dedupeKey(forTitle: "untagged title"))
        XCTAssertNil(ProblemReporter.dedupeKey(forTitle: "[] empty tag"))
        XCTAssertNil(ProblemReporter.dedupeKey(forTitle: "[console] com.example.game"))
    }

    func testMatchingIssueFindsSameSignatureOrSameConsoleMessageOnly() throws {
        let crashKey = try XCTUnwrap(ProblemReporter.dedupeKey(
            forTitle: "[3a0007225e51] com.example.game: crash A"
        ))
        let items: [[String: Any]] = [
            ["number": 10, "title": "[deadbeef0000] com.example.game: other crash",
             "html_url": "https://github.com/lawsharley0-cyber/HydrogenVM/issues/10"],
            ["number": 42, "title": "[3a0007225e51] com.example.game: crash B",
             "html_url": "https://github.com/lawsharley0-cyber/HydrogenVM/issues/42"]
        ]
        let crashMatch = try XCTUnwrap(ProblemReporter.matchingIssue(in: items, for: crashKey))
        XCTAssertEqual(crashMatch.number, 42)
        XCTAssertEqual(crashMatch.url.absoluteString, "https://github.com/lawsharley0-cyber/HydrogenVM/issues/42")

        let consoleKey = try XCTUnwrap(ProblemReporter.dedupeKey(
            forTitle: "[console] com.example.game: missing symbol foo"
        ))
        let consoleItems: [[String: Any]] = [
            ["number": 51, "title": "[console] com.example.game: different problem",
             "html_url": "https://github.com/lawsharley0-cyber/HydrogenVM/issues/51"],
            ["number": 52, "title": "[console] com.other.game: missing symbol foo",
             "html_url": "https://github.com/lawsharley0-cyber/HydrogenVM/issues/52"]
        ]
        XCTAssertNil(ProblemReporter.matchingIssue(in: consoleItems, for: consoleKey))

        let exactConsoleItems = consoleItems + [[
            "number": 53,
            "title": "[console] com.example.game: missing symbol foo",
            "html_url": "https://github.com/lawsharley0-cyber/HydrogenVM/issues/53"
        ]]
        let consoleMatch = try XCTUnwrap(ProblemReporter.matchingIssue(in: exactConsoleItems, for: consoleKey))
        XCTAssertEqual(consoleMatch.number, 53)
    }

    func testSearchQueryIncludesOpenIssuesInTitle() {
        let q = ProblemReporter.searchQuery(for: .signature("3a0007225e51"))
        XCTAssertTrue(q.contains("repo:\(ProblemReporter.repository)"))
        XCTAssertTrue(q.contains("is:issue"))
        XCTAssertTrue(q.contains("is:open"))
        XCTAssertTrue(q.contains("in:title 3a0007225e51"))
    }

    func testRenderSeenAgainIncludesSummaryAndAttachments() {
        let report = ProblemReport(
            title: "[3a0007225e51] com.example.game: SIGSEGV",
            summary: "**Device:** iPhone16,1, iOS 18.0",
            parts: [
                .init(name: "log.txt", text: "line 1\nline 2"),
                .init(name: "session.hvmrec", text: "HVMREC 1 seed=0123 wall=1 app=Test.app")
            ]
        )
        let comments = ProblemReporter.renderSeenAgain(report)
        XCTAssertEqual(comments.count, 3)
        XCTAssertTrue(comments[0].hasPrefix("**Seen again**"))
        XCTAssertTrue(comments[0].contains("iPhone16,1"))
        XCTAssertTrue(comments[1].contains("**log.txt**"))
        XCTAssertTrue(comments[2].contains("**session.hvmrec**"))
    }
}
