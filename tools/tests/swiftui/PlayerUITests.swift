import XCTest

final class PlayerUITests: XCTestCase {
    @MainActor
    func testCustomKeyInputKeyboardAppearsAndForwardsIncrementalEvents() {
        let app = XCUIApplication()
        app.launchEnvironment["PLAYER_KEYINPUT_TEST"] = "1"
        app.launch()
        app.buttons["Open Player"].tap()
        XCTAssertTrue(app.keyboards.firstMatch.waitForExistence(timeout: 10))
        app.typeText("abc")
        wait(app.staticTexts["probe-key-text"], label: "abc")
        app.typeText(XCUIKeyboardKey.delete.rawValue)
        wait(app.staticTexts["probe-key-text"], label: "ab")
        wait(app.staticTexts["probe-key-event"], label: "delete")
        app.typeText("\n")
        wait(app.staticTexts["probe-key-event"], label: "return")
        let screenshot = XCTAttachment(screenshot: app.screenshot())
        screenshot.name = "Custom UIKeyInput keyboard and Done accessory"
        screenshot.lifetime = .keepAlways
        add(screenshot)
        app.toolbars.buttons["Done"].tap()
        wait(app.staticTexts["probe-key-event"], label: "dismiss")
        let hidden = NSPredicate(format: "exists == false")
        expectation(for: hidden, evaluatedWith: app.keyboards.firstMatch)
        waitForExpectations(timeout: 10)
    }

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
    private func open(_ orientation: Int = 1, overlay: Bool = true) -> XCUIApplication {
        let app = XCUIApplication()
        // The debug overlay is off by default (#280); tests that drive the controls turn it on.
        if overlay { app.launchArguments += ["-player.showDebugOverlay", "YES"] }
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
    func testControlsAreNotDrawnByDefault() {
        let app = open(overlay: false)
        XCTAssertFalse(app.buttons["player-controls"].exists)
        XCTAssertFalse(app.buttons["player-pause"].exists)
        // The game still receives its touches with nothing drawn over it.
        app.otherElements["player-viewport"].coordinate(withNormalizedOffset: CGVector(dx: 0.5, dy: 0.5)).tap()
        wait(app.staticTexts["probe-touch"], label: "3:160,240:0")
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
        let app = open(4)   // UIInterfaceOrientationLandscapeLeft
        let screen = app.otherElements["player-viewport"]
        screen.coordinate(withNormalizedOffset: CGVector(dx: 0.5, dy: 0.5)).tap()
        wait(app.staticTexts["probe-touch"], label: "3:160,240:0")
        app.buttons["player-controls"].tap()
        app.buttons["Stretch"].tap()
        app.buttons["hud-minimize"].tap()
        screen.coordinate(withNormalizedOffset: CGVector(dx: 0.25, dy: 0.25)).tap()
        // With Stretch the entire container is the display, independent of safe areas.
        wait(app.staticTexts["probe-touch"], label: "3:80,360:0")
        app.buttons["probe-rotate"].tap()
        screen.coordinate(withNormalizedOffset: CGVector(dx: 0.25, dy: 0.25)).tap()
        wait(app.staticTexts["probe-touch"], label: "3:240,120:0")
        app.buttons["player-controls"].tap()
        app.buttons["Integer Pixels"].tap()
        app.buttons["Nearest Neighbor"].tap()
        app.buttons["hud-minimize"].tap()
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
        let scale = app.segmentedControls["player-scale-mode"]
        let useDefault = scale.buttons["Use Default"]
        func select(_ label: String) {
            let segment = scale.buttons[label]
            expectation(for: NSPredicate(format: "isHittable == true"), evaluatedWith: segment)
            waitForExpectations(timeout: 5)
            // Keep one deliberate finger gesture; do not retry a missed selection.
            segment.press(forDuration: 0.1)
            expectation(for: NSPredicate(format: "isSelected == true"), evaluatedWith: segment)
            waitForExpectations(timeout: 5)
        }
        for _ in 0..<3 {
            select("Fill (Crop)")
            XCTAssertTrue(app.segmentedControls["player-crop-focus"].waitForExistence(timeout: 5))
            select("Use Default")
            XCTAssertTrue(app.segmentedControls["player-crop-focus"].waitForNonExistence(timeout: 5))
            app.buttons["probe-default-extended"].tap()
            XCTAssertTrue(app.staticTexts["The bars show a blurred, dimmed copy of the frame."].waitForExistence(timeout: 5))
            app.buttons["probe-default-fill"].tap()
            XCTAssertTrue(app.segmentedControls["player-crop-focus"].waitForExistence(timeout: 5))
            select("Stretch")
            app.buttons["probe-default-extended"].tap()
            wait(app.staticTexts["probe-scale-default"], label: "Extended Bars")
            XCTAssertTrue(app.staticTexts["The bars show a blurred, dimmed copy of the frame."].waitForNonExistence(timeout: 5))
            select("Use Default")
            expectation(for: NSPredicate(format: "isSelected == true"), evaluatedWith: useDefault)
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
