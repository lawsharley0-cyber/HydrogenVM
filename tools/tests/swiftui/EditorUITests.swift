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

    func turnOn(_ control: XCUIElement) {
        print("Switch before tap: \(control.debugDescription)")
        // SwiftUI can expose the label and thumb as one accessibility element.
        // Tap the actual thumb rather than the center of the label's row.
        control.coordinate(withNormalizedOffset: CGVector(dx: 0.92, dy: 0.5)).tap()
        XCTAssertEqual(control.value as? String, "1", "The switch must change before testing persistence.")
    }

    func testSaveAndCloseFromNestedJSONPersistsOnReopen() {
        openNestedJSON()
        turnOn(app.switches["enabled"])
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
        turnOn(app.switches["enabled"])
        app.buttons["editor-close"].tap()
        app.buttons["Discard Changes"].tap()
        XCTAssertTrue(app.staticTexts["disk-json"].waitForExistence(timeout: 5))
        XCTAssertEqual(app.staticTexts["disk-json"].label, "nested=false")
    }

    func testReturningFromNestedJSONKeepsEdits() {
        openNestedJSON()
        turnOn(app.switches["enabled"])
        app.buttons["BackButton"].tap()
        // The root must retain the edited model and a reachable Save button.
        let save = app.buttons["save-changes"]
        XCTAssertTrue(save.isEnabled)
        save.tap()
        let warning = app.alerts["Game May Be Running"]
        XCTAssertTrue(warning.waitForExistence(timeout: 5))
        warning.buttons["Save Anyway"].tap()
        let saved = app.staticTexts.matching(NSPredicate(format: "label CONTAINS %@", "Saved ✓")).firstMatch
        XCTAssertTrue(saved.waitForExistence(timeout: 10))
        app.buttons["editor-close"].tap()
        XCTAssertTrue(app.staticTexts["disk-json"].waitForExistence(timeout: 5))
        XCTAssertEqual(app.staticTexts["disk-json"].label, "nested=true")
    }

    func testAutoSaveFromNestedJSON() {
        openNestedJSON()
        turnOn(app.switches["Auto-save edits"])
        turnOn(app.switches["enabled"])
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
        editor.coordinate(withNormalizedOffset: CGVector(dx: 0.15, dy: 0.3)).tap()
        editor.typeText("edited")
        XCTAssertTrue((editor.value as? String)?.contains("edited") == true, "Native text input must arrive before saving.")
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
