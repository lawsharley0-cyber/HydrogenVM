import XCTest
import UIKit
import CoreFoundation
@testable import SaveEditorHarness

final class PlayerViewportTests: XCTestCase {
    @MainActor
    func testAccessibilityActionUsesOverlayCallback() throws {
        let view = EmulatorScreen.ScreenView(frame: .zero)
        var calls = 0
        view.onOverlayGesture = { calls += 1 }
        let action = try XCTUnwrap(view.accessibilityCustomActions?.first)
        XCTAssertEqual(action.name, "Open Player Controls")
        let selector = try XCTUnwrap(action.selector)
        let target = try XCTUnwrap(action.target)
        XCTAssertTrue(UIApplication.shared.sendAction(selector, to: target, from: action, for: nil))
        XCTAssertEqual(calls, 1)
        view.onOverlayGesture = nil
        XCTAssertNil(view.accessibilityCustomActions)
    }

    @MainActor
    func testBackdropThrottlesDistinctFramesAndRedrawsAfterBeingDisabled() throws {
        let view = EmulatorScreen.ScreenView(frame: CGRect(x: 0, y: 0, width: 320, height: 480))
        var now: CFTimeInterval = 10
        view.backdropClock = { now }
        let backdrop = try XCTUnwrap(view.layer.sublayers?.first)

        func frame(_ color: UIColor) -> CGImage {
            UIGraphicsImageRenderer(size: CGSize(width: 4, height: 4)).image { context in
                color.setFill()
                context.fill(CGRect(x: 0, y: 0, width: 4, height: 4))
            }.cgImage!
        }
        func configure(_ image: CGImage?, _ mode: PlayerScaleMode) {
            view.configure(image: image, glLayer: nil, guest: CGSize(width: 320, height: 480),
                           orientation: .portrait, mode: mode, filter: .linear, enabled: true)
        }
        func backdropImage() -> CGImage? {
            guard let contents = backdrop.contents else { return nil }
            guard CFGetTypeID(contents as CFTypeRef) == CGImage.typeID else {
                XCTFail("Backdrop contents must be a CGImage")
                return nil
            }
            return (contents as! CGImage)
        }

        configure(frame(.red), .extended)
        XCTAssertFalse(backdrop.isHidden)
        let first = try XCTUnwrap(backdropImage())
        now = 10.1
        configure(frame(.green), .extended)
        XCTAssertTrue(first === (try XCTUnwrap(backdropImage())))
        now = 10.3
        configure(frame(.blue), .extended)
        let second = try XCTUnwrap(backdropImage())
        XCTAssertFalse(first === second)

        now = 10.31
        configure(frame(.red), .fit)
        XCTAssertTrue(backdrop.isHidden)
        now = 10.32
        configure(frame(.green), .extended)
        XCTAssertFalse(backdrop.isHidden)
        let afterReenable = try XCTUnwrap(backdropImage())
        XCTAssertFalse(second === afterReenable)
    }
}
