import XCTest
@testable import HydrogenVM

final class SafeModeRetryTests: XCTestCase {
    func testRetryMustPresentAndOutliveTheOriginalLaunch() {
        XCTAssertFalse(RunModel.retrySucceeded(previousLifetime: 90, elapsed: 5, hasPicture: true))
        XCTAssertFalse(RunModel.retrySucceeded(previousLifetime: 90, elapsed: 90, hasPicture: true))
        XCTAssertFalse(RunModel.retrySucceeded(previousLifetime: 90, elapsed: 92, hasPicture: false))
        XCTAssertTrue(RunModel.retrySucceeded(previousLifetime: 90, elapsed: 92, hasPicture: true))
    }

    func testImmediateStartupCrashStillRequiresFiveSecondsOfRetry() {
        XCTAssertFalse(RunModel.retrySucceeded(previousLifetime: 0, elapsed: 1, hasPicture: true))
        XCTAssertTrue(RunModel.retrySucceeded(previousLifetime: 0, elapsed: 5, hasPicture: true))
    }
}
