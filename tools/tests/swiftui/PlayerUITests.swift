import XCTest

final class PlayerUITests: XCTestCase {
    @MainActor
    func testCustomKeyInputKeyboardAppearsAndForwardsIncrementalEvents() throws {
        try XCTSkipIf(true, "Fails in CI's simulator for a reason the job log does not show (#384)")
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
    private func open(_ orientation: Int = 1, overlay: Bool = true, controls: Bool = false) -> XCUIApplication {
        let app = XCUIApplication()
        // The debug overlay is off by default (#280); tests that drive the controls turn it on.
        app.launchArguments += ["-player.showDebugOverlay", overlay ? "YES" : "NO",
                                "-player.showControlsButton", controls ? "YES" : "NO"]
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
    func testControlsButtonWorksWithoutPerformanceOverlay() {
        let app = open(overlay: false, controls: true)
        let pill = app.buttons["player-controls"]
        XCTAssertTrue(pill.waitForExistence(timeout: 5))
        XCTAssertEqual(pill.value as? String, "Running")
        pill.tap()
        app.buttons["player-pause"].tap()
        wait(app.staticTexts["probe-lifecycle"], label: "paused")
        app.buttons["hud-minimize"].tap()
        XCTAssertEqual(pill.value as? String, "Paused")
    }

    @MainActor
    func testDebugPillStaysCompactAndExpandedControlsStayOnScreen() {
        for orientation in [1, 4] {
            let app = open(orientation)
            let viewport = app.otherElements["player-viewport"]
            let pill = app.buttons["player-controls"]
            XCTAssertTrue(pill.waitForExistence(timeout: 5))
            XCTAssertLessThan(pill.frame.width, viewport.frame.width / 2)
            XCTAssertLessThan(pill.frame.height, 64)
            pill.tap()

            let window = app.windows.firstMatch.frame.insetBy(dx: -1, dy: -1)
            for name in ["player-pause", "player-inspect", "hud-minimize", "player-quit"] {
                let control = app.buttons[name]
                XCTAssertTrue(control.isHittable, "\(name) is outside the usable player area")
                XCTAssertTrue(window.contains(control.frame))
            }
            app.buttons["hud-minimize"].tap()
            XCTAssertTrue(pill.isHittable)
            app.terminate()
        }
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
    private func selectScaleMode(_ mode: String, in app: XCUIApplication) {
        let menu = app.buttons["player-scale-mode"]
        XCTAssertTrue(menu.isHittable)
        menu.tap()
        let option = app.buttons[mode]
        XCTAssertTrue(option.waitForExistence(timeout: 5))
        option.tap()
        expectation(for: NSPredicate(format: "value == %@", mode), evaluatedWith: menu)
        waitForExpectations(timeout: 5)
    }

    @MainActor
    func testBothLandscapeDirectionsAndDisplayModes() {
        let app = open(4)   // UIInterfaceOrientationLandscapeLeft
        let screen = app.otherElements["player-viewport"]
        screen.coordinate(withNormalizedOffset: CGVector(dx: 0.5, dy: 0.5)).tap()
        wait(app.staticTexts["probe-touch"], label: "3:160,240:0")
        app.buttons["player-controls"].tap()
        selectScaleMode("Stretch", in: app)
        app.buttons["hud-minimize"].tap()
        screen.coordinate(withNormalizedOffset: CGVector(dx: 0.25, dy: 0.25)).tap()
        // With Stretch the entire container is the display, independent of safe areas.
        wait(app.staticTexts["probe-touch"], label: "3:80,360:0")
        app.buttons["probe-rotate"].tap()
        screen.coordinate(withNormalizedOffset: CGVector(dx: 0.25, dy: 0.25)).tap()
        wait(app.staticTexts["probe-touch"], label: "3:240,120:0")
        app.buttons["player-controls"].tap()
        selectScaleMode("Integer Pixels", in: app)
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
        for _ in 0..<3 {
            selectScaleMode("Fill (Crop)", in: app)
            XCTAssertTrue(app.segmentedControls["player-crop-focus"].waitForExistence(timeout: 5))
            selectScaleMode("Use Default", in: app)
            XCTAssertTrue(app.segmentedControls["player-crop-focus"].waitForNonExistence(timeout: 5))
            app.buttons["probe-default-extended"].tap()
            XCTAssertTrue(app.staticTexts["The bars show a blurred, dimmed copy of the frame."].waitForExistence(timeout: 5))
            app.buttons["probe-default-fill"].tap()
            XCTAssertTrue(app.segmentedControls["player-crop-focus"].waitForExistence(timeout: 5))
            selectScaleMode("Stretch", in: app)
            app.buttons["probe-default-extended"].tap()
            wait(app.staticTexts["probe-scale-default"], label: "Extended Bars")
            XCTAssertTrue(app.staticTexts["The bars show a blurred, dimmed copy of the frame."].waitForNonExistence(timeout: 5))
            selectScaleMode("Use Default", in: app)
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
