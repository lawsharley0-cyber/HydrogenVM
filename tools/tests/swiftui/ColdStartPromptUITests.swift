import XCTest

/// Accept the OS confirmation, without launching the target application ourselves.
/// The shell driver independently requires the URL's queue, route and guest result.
final class ColdStartPromptUITests: XCTestCase {
    @MainActor
    func testAcceptSystemOpenConfirmation() {
        let springboard = XCUIApplication(bundleIdentifier: "com.apple.springboard")
        let confirmation = springboard.alerts.containing(.staticText, identifier: "Open in “HydrogenVM”?").firstMatch
        // Some simulator versions remember approval. In either case the actual
        // target must enter the foreground; this helper never calls launch/activate.
        if confirmation.waitForExistence(timeout: 10) {
            let open = confirmation.buttons["Open"]
            XCTAssertTrue(open.waitForExistence(timeout: 5))
            open.tap()
            XCTAssertTrue(confirmation.waitForNonExistence(timeout: 10))
        }
        let target = XCUIApplication(bundleIdentifier: "com.lfull.HydrogenVM")
        XCTAssertTrue(target.wait(for: .runningForeground, timeout: 10))
    }
}
