import XCTest

@MainActor
final class EditorUITests: XCTestCase {
    var app: XCUIApplication!

    override func setUpWithError() throws {
        continueAfterFailure = false
        XCUIDevice.shared.orientation = .portrait
        app = XCUIApplication()
        app.launch()
    }

    func openNestedJSON() {
        app.buttons["Open JSON"].tap()
        let nested = app.buttons["json-container-nested"]
        XCTAssertTrue(nested.waitForExistence(timeout: 10))
        nested.tap()
        XCTAssertTrue(app.switches["enabled"].waitForExistence(timeout: 5))
    }

    func testSaveAndCloseFromNestedJSONPersistsOnReopen() {
        openNestedJSON()
        app.switches["enabled"].tap()
        XCTAssertTrue(app.buttons["save-changes"].isEnabled)
        app.buttons["editor-close"].tap()
        app.buttons["Save & Close"].tap()
        XCTAssertTrue(app.staticTexts["disk-json"].waitForExistence(timeout: 5))
        XCTAssertEqual(app.staticTexts["disk-json"].label, "nested=true")
        openNestedJSON()
        XCTAssertEqual(app.switches["enabled"].value as? String, "1")
    }

    func testDiscardFromNestedJSONPreservesDisk() {
        openNestedJSON()
        app.switches["enabled"].tap()
        app.buttons["editor-close"].tap()
        app.buttons["Discard Changes"].tap()
        XCTAssertTrue(app.staticTexts["disk-json"].waitForExistence(timeout: 5))
        XCTAssertEqual(app.staticTexts["disk-json"].label, "nested=false")
    }

    func testReturningFromNestedJSONKeepsEdits() {
        openNestedJSON()
        app.switches["enabled"].tap()
        app.navigationBars.buttons.element(boundBy: 0).tap()
        // The root must retain the edited model and a reachable Save button.
        let save = app.buttons["save-changes"]
        XCTAssertTrue(save.isEnabled)
        save.tap()
        app.buttons["editor-close"].tap()
        XCTAssertTrue(app.staticTexts["disk-json"].waitForExistence(timeout: 5))
        XCTAssertEqual(app.staticTexts["disk-json"].label, "nested=true")
    }

    func testAutoSaveFromNestedJSON() {
        openNestedJSON()
        app.switches["Auto-save edits"].tap()
        app.switches["enabled"].tap()
        let saved = app.staticTexts.matching(NSPredicate(format: "label CONTAINS %@", "Saved ✓")).firstMatch
        XCTAssertTrue(saved.waitForExistence(timeout: 10))
        app.buttons["editor-close"].tap()
        XCTAssertTrue(app.staticTexts["disk-json"].waitForExistence(timeout: 5))
        XCTAssertEqual(app.staticTexts["disk-json"].label, "nested=true")
    }

    func testKeyboardSavePersistsText() {
        app.buttons["Open Text"].tap()
        let editor = app.textViews["plain-text-editor"]
        XCTAssertTrue(editor.waitForExistence(timeout: 10))
        editor.tap()
        editor.typeText("edited")
        let save = app.buttons["keyboard-save"]
        XCTAssertTrue(save.waitForExistence(timeout: 5))
        XCTAssertTrue(save.isHittable)
        save.tap()
        app.buttons["Done"].tap()
        app.buttons["editor-close"].tap()
        XCTAssertTrue(app.staticTexts["disk-text"].waitForExistence(timeout: 5))
        XCTAssertTrue(app.staticTexts["disk-text"].label.contains("edited"))
    }
}
