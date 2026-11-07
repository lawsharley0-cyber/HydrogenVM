import XCTest

final class PlayerUITests: XCTestCase {
    @MainActor
    func testSyntheticSessionCountsDeliveredFrames() {
        let app = XCUIApplication()
        app.launchEnvironment["PLAYER_COUNTER_TEST"] = "1"
        app.launch()
        let counter = app.staticTexts["frame-counter"]
        XCTAssertTrue(counter.waitForExistence(timeout: 10))
        XCTAssertEqual(counter.label, "0")
        app.buttons["Deliver Test Frame"].tap()
        wait(counter, label: "1")
        app.buttons["Deliver Test Frame"].tap()
        wait(counter, label: "2")
    }

    @MainActor
    private func open(_ orientation: Int = 1, overlay: Bool = true, controls: Bool = true) -> XCUIApplication {
        let app = XCUIApplication()
        // The debug overlay is off by default (#280); tests that drive the controls turn it on.
        if overlay { app.launchArguments += ["-player.showDebugOverlay", "YES"] }
        app.launchArguments += ["-player.showControlsButton", controls ? "YES" : "NO"]
        app.launchEnvironment["PLAYER_ORIENTATION"] = String(orientation)
        app.launch()
        app.buttons["Open Player"].tap()
        XCTAssertTrue(app.otherElements["player-viewport"].waitForExistence(timeout: 10))
        return app
    }

    @MainActor
    private func wait(_ element: XCUIElement, label: String) {
        let predicate = NSPredicate(format: "label == %@", label)
        expectation(for: predicate, evaluatedWith: element)
        waitForExpectations(timeout: 10)
    }

    @MainActor
    func testControlsCanBeHiddenWithoutBlockingTouches() {
        let app = open(overlay: false, controls: false)
        XCTAssertFalse(app.buttons["player-controls"].exists)
        XCTAssertFalse(app.buttons["player-pause"].exists)
        // The game still receives its touches with nothing drawn over it.
        app.otherElements["player-viewport"].coordinate(withNormalizedOffset: CGVector(dx: 0.5, dy: 0.5)).tap()
        wait(app.staticTexts["probe-touch"], label: "3:160,240:0")
    }

    @MainActor
    func testControlsButtonWorksWithoutPerformanceOverlay() {
        let app = open(overlay: false)
        XCTAssertTrue(app.buttons["player-controls"].isHittable)
        XCTAssertEqual(app.buttons["player-controls"].value as? String, "Playing")
        app.buttons["player-controls"].tap()
        app.buttons["player-pause"].tap()
        wait(app.staticTexts["probe-lifecycle"], label: "paused")
        app.buttons["hud-minimize"].tap()
        XCTAssertTrue(app.buttons["player-controls"].isHittable)
        XCTAssertFalse(app.buttons["player-pause"].exists)
        XCTAssertEqual(app.buttons["player-controls"].value as? String, "Paused")
    }

    @MainActor
    private func selectMenu(_ app: XCUIApplication, _ identifier: String, _ option: String) {
        let menu = app.buttons[identifier]
        if !menu.isHittable { app.scrollViews["player-controls-panel"].swipeUp() }
        XCTAssertTrue(menu.isHittable)
        menu.tap()
        app.buttons[option].tap()
    }

    @MainActor
    private func minimize(_ app: XCUIApplication) {
        let button = app.buttons["hud-minimize"]
        if !button.isHittable { app.scrollViews["player-controls-panel"].swipeDown(velocity: .fast) }
        XCTAssertTrue(button.isHittable)
        button.tap()
    }

    @MainActor
    func testPortraitCenterTouchAndPauseResume() {
        let app = open()
        let screen = app.otherElements["player-viewport"]
        screen.coordinate(withNormalizedOffset: CGVector(dx: 0.5, dy: 0.5)).tap()
        wait(app.staticTexts["probe-touch"], label: "3:160,240:0")
        app.buttons["player-controls"].tap()
        app.buttons["player-pause"].tap()
        wait(app.staticTexts["probe-lifecycle"], label: "paused")
        app.buttons["player-pause"].tap()
        wait(app.staticTexts["probe-lifecycle"], label: "active")
        app.scrollViews["player-controls-panel"].swipeUp(velocity: .fast)
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
        let screen = app.otherElements["player-viewport"]
        screen.coordinate(withNormalizedOffset: CGVector(dx: 0.5, dy: 0.5)).tap()
        wait(app.staticTexts["probe-touch"], label: "3:160,240:0")
        app.buttons["player-controls"].tap()
        selectMenu(app, "player-scale-mode", "Stretch")
        minimize(app)
        screen.coordinate(withNormalizedOffset: CGVector(dx: 0.25, dy: 0.25)).tap()
        // With Stretch the entire container is the display, independent of safe areas.
        wait(app.staticTexts["probe-touch"], label: "3:80,360:0")
        app.buttons["probe-rotate"].tap()
        screen.coordinate(withNormalizedOffset: CGVector(dx: 0.25, dy: 0.25)).tap()
        wait(app.staticTexts["probe-touch"], label: "3:240,120:0")
        app.buttons["player-controls"].tap()
        selectMenu(app, "player-scale-mode", "Integer Pixels")
        selectMenu(app, "player-filter", "Nearest Neighbor")
        minimize(app)
        screen.coordinate(withNormalizedOffset: CGVector(dx: 0.5, dy: 0.5)).tap()
        wait(app.staticTexts["probe-touch"], label: "3:160,240:0")
        let screenshot = XCTAttachment(screenshot: app.screenshot())
        screenshot.name = "Player controls landscape"
        screenshot.lifetime = .keepAlways
        add(screenshot)
    }

    @MainActor
    func testScaleModeReturnsToLiveGlobalDefault() {
        let app = open()
        app.buttons["player-controls"].tap()
        let scale = app.buttons["player-scale-mode"]
        func select(_ label: String) {
            selectMenu(app, "player-scale-mode", label)
            expectation(for: NSPredicate(format: "value == %@", label), evaluatedWith: scale)
            waitForExpectations(timeout: 5)
        }
        for _ in 0..<3 {
            select("Fill (Crop)")
            XCTAssertTrue(app.buttons["player-crop-focus"].waitForExistence(timeout: 5))
            select("Use Default")
            XCTAssertTrue(app.buttons["player-crop-focus"].waitForNonExistence(timeout: 5))
            app.buttons["probe-default-extended"].tap()
            XCTAssertTrue(app.staticTexts["The bars show a blurred, dimmed copy of the frame."].waitForExistence(timeout: 5))
            app.buttons["probe-default-fill"].tap()
            XCTAssertTrue(app.buttons["player-crop-focus"].waitForExistence(timeout: 5))
            select("Stretch")
            app.buttons["probe-default-extended"].tap()
            wait(app.staticTexts["probe-scale-default"], label: "Extended Bars")
            XCTAssertTrue(app.staticTexts["The bars show a blurred, dimmed copy of the frame."].waitForNonExistence(timeout: 5))
            select("Use Default")
            expectation(for: NSPredicate(format: "value == 'Use Default'"), evaluatedWith: scale)
            waitForExpectations(timeout: 5)
            XCTAssertTrue(app.staticTexts["The bars show a blurred, dimmed copy of the frame."].waitForExistence(timeout: 5))
        }
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
        let screen = app.otherElements["player-viewport"]
        screen.pinch(withScale: 1.3, velocity: 1)
        wait(app.staticTexts["probe-max-touches"], label: "2")
        app.buttons["probe-delayed-rotate"].tap()
        screen.coordinate(withNormalizedOffset: CGVector(dx: 0.5, dy: 0.5)).press(forDuration: 4)
        let predicate = NSPredicate(format: "label != '0'")
        expectation(for: predicate, evaluatedWith: app.staticTexts["probe-cancellations"])
        waitForExpectations(timeout: 10)
    }
}
