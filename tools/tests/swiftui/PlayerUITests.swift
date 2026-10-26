import XCTest

final class PlayerUITests: XCTestCase {
    @MainActor
    func testGuestKeyboardInsertionDeletionAndReturn() {
        let app = open()
        app.buttons["probe-keyboard-open"].tap()
        XCTAssertTrue(app.keyboards.firstMatch.waitForExistence(timeout: 10))
        app.typeText("Clans123")
        wait(app.staticTexts["probe-keyboard-text"], label: "Clans123")
        app.typeText(XCUIKeyboardKey.delete.rawValue)
        wait(app.staticTexts["probe-keyboard-text"], label: "Clans12")
        app.typeText("\n")
        wait(app.staticTexts["probe-keyboard-returns"], label: "1")
        let hidden = NSPredicate(format: "exists == false")
        expectation(for: hidden, evaluatedWith: app.keyboards.firstMatch)
        waitForExpectations(timeout: 10)
        XCTAssertTrue(app.descendants(matching: .any)["player-viewport"].exists)
    }

    @MainActor
    private func open(_ orientation: Int = 1) -> XCUIApplication {
        let app = XCUIApplication()
        app.launchEnvironment["PLAYER_ORIENTATION"] = String(orientation)
        app.launch()
        app.buttons["Open Player"].tap()
        XCTAssertTrue(app.descendants(matching: .any)["player-viewport"].waitForExistence(timeout: 10))
        return app
    }

    @MainActor
    private func wait(_ element: XCUIElement, label: String) {
        let predicate = NSPredicate(format: "label == %@", label)
        expectation(for: predicate, evaluatedWith: element)
        waitForExpectations(timeout: 10)
    }

    @MainActor
    func testPortraitCenterTouchAndPauseResume() {
        let app = open()
        let screen = app.descendants(matching: .any)["player-viewport"]
        screen.coordinate(withNormalizedOffset: CGVector(dx: 0.5, dy: 0.5)).tap()
        wait(app.staticTexts["probe-touch"], label: "3:160,240:0")
        app.buttons["player-controls"].tap()
        app.buttons["player-pause"].tap()
        wait(app.staticTexts["probe-lifecycle"], label: "paused")
        app.buttons["player-pause"].tap()
        wait(app.staticTexts["probe-lifecycle"], label: "active")
        app.buttons["Mute"].tap()
        wait(app.staticTexts["probe-volume"], label: "0.0")
        app.buttons["Unmute"].tap()
        wait(app.staticTexts["probe-volume"], label: "1.0")
        let screenshot = XCTAttachment(screenshot: app.screenshot())
        screenshot.name = "Player controls portrait"
        screenshot.lifetime = .keepAlways
        add(screenshot)
    }

    @MainActor
    func testBothLandscapeDirectionsAndDisplayModes() {
        let app = open(3)
        let screen = app.descendants(matching: .any)["player-viewport"]
        screen.coordinate(withNormalizedOffset: CGVector(dx: 0.5, dy: 0.5)).tap()
        wait(app.staticTexts["probe-touch"], label: "3:160,240:0")
        app.buttons["player-controls"].tap()
        app.buttons["Stretch"].tap()
        screen.coordinate(withNormalizedOffset: CGVector(dx: 0.25, dy: 0.25)).tap()
        // With Stretch the entire container is the display, independent of safe areas.
        wait(app.staticTexts["probe-touch"], label: "3:80,360:0")
        app.buttons["probe-rotate"].tap()
        screen.coordinate(withNormalizedOffset: CGVector(dx: 0.25, dy: 0.25)).tap()
        wait(app.staticTexts["probe-touch"], label: "3:240,120:0")
        app.buttons["Integer Pixels"].tap()
        app.buttons["Nearest Neighbor"].tap()
        screen.coordinate(withNormalizedOffset: CGVector(dx: 0.5, dy: 0.5)).tap()
        wait(app.staticTexts["probe-touch"], label: "3:160,240:0")
        let screenshot = XCTAttachment(screenshot: app.screenshot())
        screenshot.name = "Player controls landscape"
        screenshot.lifetime = .keepAlways
        add(screenshot)
    }

    @MainActor
    func testInspectWaitsForStopAndOpensExistingBrowser() {
        let app = open()
        app.buttons["player-controls"].tap()
        app.buttons["player-inspect"].tap()
        app.buttons["Save, Stop & Inspect"].tap()
        XCTAssertTrue(app.buttons["Done"].firstMatch.waitForExistence(timeout: 10))
        XCTAssertFalse(app.buttons["player-pause"].isHittable)
    }

    @MainActor
    func testMultitouchAndRotationCancelsHeldFinger() {
        let app = open()
        let screen = app.descendants(matching: .any)["player-viewport"]
        screen.pinch(withScale: 1.3, velocity: 1)
        wait(app.staticTexts["probe-max-touches"], label: "2")
        app.buttons["probe-delayed-rotate"].tap()
        screen.coordinate(withNormalizedOffset: CGVector(dx: 0.5, dy: 0.5)).press(forDuration: 4)
        let predicate = NSPredicate(format: "label != '0'")
        expectation(for: predicate, evaluatedWith: app.staticTexts["probe-cancellations"])
        waitForExpectations(timeout: 10)
    }
}

