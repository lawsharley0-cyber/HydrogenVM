import Foundation
import CoreGraphics

@main
@MainActor
struct PlayerTests {
    static var checks = 0
    static func check(_ condition: @autoclosure () -> Bool, _ message: String) {
        checks += 1
        precondition(condition(), message)
    }
    static func near(_ a: CGFloat, _ b: CGFloat) -> Bool { abs(a - b) < 0.00001 }
    static func main() {
        let guest = CGSize(width: 320, height: 480)
        let pixels = CGSize(width: 640, height: 960)
        // Independent quarter-point expectations, not round trips through a
        // forward transform implemented by this same model.
        let expected = [CGPoint(x: 80, y: 120), CGPoint(x: 240, y: 360),
                        CGPoint(x: 80, y: 360), CGPoint(x: 240, y: 120)]
        for (i, orientation) in GuestOrientation.allCases.enumerated() {
            for mode in PlayerScaleMode.allCases {
                let g = PlayerGeometry(container: CGSize(width: 900, height: 700), guest: guest,
                                       pixels: pixels, orientation: orientation, mode: mode, displayScale: 3)
                let p = g.guestPoint(CGPoint(x: g.rect.minX + g.rect.width / 4,
                                             y: g.rect.minY + g.rect.height / 4))!
                check(near(p.x, expected[i].x) && near(p.y, expected[i].y), "orientation quarter point")
                let center = g.guestPoint(CGPoint(x: g.rect.midX, y: g.rect.midY))!
                check(near(center.x, 160) && near(center.y, 240), "center")
                check(g.guestPoint(CGPoint(x: -1, y: -1)) == nil, "outside begin ignored")
                let continued = g.guestPoint(CGPoint(x: -1, y: 10000), continuing: true)!
                check(continued.x >= 0 && continued.x < 320 && continued.y >= 0 && continued.y < 480, "continued touch clamped")
                if mode == .integer {
                    let widthPixels = (orientation.landscape ? pixels.height : pixels.width)
                    check(near(g.rect.width * 3 / widthPixels, floor(g.rect.width * 3 / widthPixels)), "whole source pixels")
                }
            }
        }
        let letterbox = PlayerGeometry(container: CGSize(width: 1000, height: 600), guest: guest,
                                       pixels: pixels, orientation: .portrait, mode: .fit, displayScale: 2)
        check(letterbox.rect == CGRect(x: 300, y: 0, width: 400, height: 600), "known aspect fit")
        check(letterbox.guestPoint(CGPoint(x: 20, y: 300)) == nil, "letterbox ignored")
        let small = PlayerGeometry(container: CGSize(width: 100, height: 100), guest: guest,
                                  pixels: pixels, orientation: .portrait, mode: .integer, displayScale: 2)
        check(small.rect.width <= 100 && small.rect.height <= 100, "integer fallback avoids crop")
        let invalid = PlayerGeometry(container: .zero, guest: guest, pixels: pixels,
                                    orientation: .portrait, mode: .fit, displayScale: 2)
        check(invalid.guestPoint(.zero) == nil, "zero layout")
        for orientation in GuestOrientation.allCases {
            let a = orientation.acceleration(x: 1, y: 2, z: 3, host: orientation)
            check(a.0 == 1 && a.1 == 2 && a.2 == 3, "matching host/guest axes stay device-fixed")
        }
        let left = GuestOrientation.landscapeLeft.acceleration(x: 1, y: 2, z: -1, host: .portrait)
        check(left.0 == -2 && left.1 == 1 && left.2 == -1, "left fallback axes")
        let right = GuestOrientation.landscapeRight.acceleration(x: 1, y: 2, z: -1, host: .portrait)
        check(right.0 == 2 && right.1 == -1 && right.2 == -1, "right fallback axes")

        var state = PlayerActivity()
        state.userPaused = true
        check(state.reconcile(ready: false, backgrounded: false, running: true) == nil, "wait for guest startup")
        check(state.reconcile(ready: true, backgrounded: false, running: true) == .background, "request pause")
        state.userPaused = false
        check(state.reconcile(ready: true, backgrounded: false, running: true) == nil, "fast resume waits for pause acknowledgement")
        check(!state.acceptsInput, "pending pause blocks input")
        check(state.reconcile(ready: true, backgrounded: true, running: true) == .foreground, "resume after acknowledgement")
        check(state.reconcile(ready: true, backgrounded: false, running: true) == nil && state.acceptsInput, "resumed")
        state.userPaused = true
        check(state.reconcile(ready: true, backgrounded: false, running: true) == .background, "pause again")
        state.hostBackgrounded = true
        check(state.reconcile(ready: true, backgrounded: true, running: true) == nil, "background acknowledgement")
        state.hostBackgrounded = false
        check(state.reconcile(ready: true, backgrounded: true, running: true) == nil && state.phase == .paused,
              "host foreground preserves user pause")
        state.userPaused = false
        state.hostBackgrounded = true
        check(state.reconcile(ready: true, backgrounded: true, running: true) == nil, "resume while host away stays paused")
        state.hostBackgrounded = false
        check(state.reconcile(ready: true, backgrounded: true, running: true) == .foreground, "foreground resumes")
        check(state.reconcile(ready: true, backgrounded: false, running: false) == nil && state.phase == .stopped,
              "exit resets status")
        // #281: fill and extended bars share this one layout with every other mode.
        let phone = CGSize(width: 390, height: 844)
        for orientation in GuestOrientation.allCases {
            let container = orientation.landscape ? CGSize(width: 844, height: 390) : phone
            for mode in PlayerScaleMode.allCases {
                let g = PlayerGeometry(container: container, guest: guest, pixels: pixels,
                                       orientation: orientation, mode: mode, displayScale: 3,
                                       focus: PlayerCropFocus.center.position, touchSafeArea: .zero)
                let center = g.guestPoint(CGPoint(x: container.width / 2, y: container.height / 2))!
                check(near(center.x, 160) && near(center.y, 240),
                      "display centre reaches the guest centre (\(mode.rawValue), \(orientation))")
                if mode.crops {
                    check(g.rect.minX <= 0.0001 && g.rect.minY <= 0.0001
                          && g.rect.maxX >= container.width - 0.0001 && g.rect.maxY >= container.height - 0.0001,
                          "fill covers the whole display (\(orientation))")
                    check(!g.hasBars, "fill has no bars (\(orientation))")
                } else {
                    check(g.rect.minX >= -0.0001 && g.rect.maxX <= container.width + 0.0001
                          && g.rect.minY >= -0.0001 && g.rect.maxY <= container.height + 0.0001,
                          "\(mode.rawValue) never crops (\(orientation))")
                }
            }
        }
        // The crop focus picks which edge survives; the guest sees the matching edge.
        let narrow = CGSize(width: 320, height: 800)
        let topFocus = PlayerGeometry(container: phone, guest: narrow, pixels: narrow, orientation: .portrait,
                                      mode: .fill, displayScale: 3, focus: PlayerCropFocus.top.position)
        check(near(topFocus.rect.minY, 0), "top focus keeps the top edge")
        check(near(topFocus.guestPoint(CGPoint(x: 195, y: 0))!.y, 0), "top of the display is the top of the guest")
        let bottomFocus = PlayerGeometry(container: phone, guest: narrow, pixels: narrow, orientation: .portrait,
                                         mode: .fill, displayScale: 3, focus: PlayerCropFocus.bottom.position)
        check(near(bottomFocus.rect.maxY, phone.height), "bottom focus keeps the bottom edge")
        check(near(bottomFocus.guestPoint(CGPoint(x: 195, y: phone.height - 0.001))!.y, 799.5),
              "bottom of the display is the end of the guest")
        // The notch and home indicator must stay over game pixels, for every focus.
        for orientation in GuestOrientation.allCases {
            let container = orientation.landscape ? CGSize(width: 844, height: 390) : phone
            let safeArea = orientation.landscape
                ? CGRect(x: 47, y: 21, width: 844 - 94, height: 390 - 42)
                : CGRect(x: 0, y: 59, width: 390, height: 844 - 93)
            for focus in PlayerCropFocus.allCases {
                let g = PlayerGeometry(container: container, guest: guest, pixels: pixels,
                                       orientation: orientation, mode: .fill, displayScale: 3,
                                       focus: focus.position, touchSafeArea: safeArea)
                check(g.rect.minX <= safeArea.minX + 0.0001 && g.rect.maxX >= safeArea.maxX - 0.0001
                      && g.rect.minY <= safeArea.minY + 0.0001 && g.rect.maxY >= safeArea.maxY - 0.0001,
                      "fill keeps the touch safe area covered (\(orientation), \(focus.rawValue))")
            }
        }
        // Extended bars: same picture as aspect fit, and the bars never swallow a touch.
        let extended = PlayerGeometry(container: phone, guest: guest, pixels: pixels, orientation: .portrait,
                                      mode: .extended, displayScale: 3)
        let fit = PlayerGeometry(container: phone, guest: guest, pixels: pixels, orientation: .portrait,
                                 mode: .fit, displayScale: 3)
        check(extended.rect == fit.rect && extended.hasBars, "extended bars keep the aspect-fit picture")
        check(extended.guestPoint(CGPoint(x: 195, y: 10)) == nil, "a tap in the bar does not reach the guest")
        check(extended.guestPoint(CGPoint(x: 195, y: 10), continuing: true) != nil,
              "a drag into the bar stays clamped on the guest edge")
        // Per-game setting with a global default.
        let suite = "hvm.player.settings.tests"
        let defaults = UserDefaults(suiteName: suite)!
        defaults.removePersistentDomain(forName: suite)
        check(PlayerSettings.scaleMode(bundleID: "com.one", defaults: defaults) == .fit, "no setting means aspect fit")
        defaults.set(PlayerScaleMode.fill.rawValue, forKey: PlayerSettings.scaleModeKey)
        check(PlayerSettings.scaleMode(bundleID: "com.one", defaults: defaults) == .fill, "the global default applies")
        PlayerSettings.setScaleMode(.extended, bundleID: "com.one", defaults: defaults)
        check(PlayerSettings.scaleMode(bundleID: "com.one", defaults: defaults) == .extended, "a game overrides the default")
        check(PlayerSettings.hasScaleOverride(bundleID: "com.one", defaults: defaults)
              && !PlayerSettings.hasScaleOverride(bundleID: "com.two", defaults: defaults),
              "only the game that chose a mode has an override")
        check(PlayerSettings.scaleMode(bundleID: "com.two", defaults: defaults) == .fill, "other games keep the default")
        PlayerSettings.clearScaleMode(bundleID: "com.one", defaults: defaults)
        check(PlayerSettings.scaleMode(bundleID: "com.one", defaults: defaults) == .fill, "clearing falls back to the default")
        check(!PlayerSettings.hasScaleOverride(bundleID: "com.one", defaults: defaults), "clearing removes the override")
        check(PlayerSettings.cropFocus(bundleID: "com.one", defaults: defaults) == .center, "crop focus defaults to centre")
        PlayerSettings.setCropFocus(.bottom, bundleID: "com.one", defaults: defaults)
        check(PlayerSettings.cropFocus(bundleID: "com.one", defaults: defaults) == .bottom, "crop focus is remembered per game")
        defaults.removePersistentDomain(forName: suite)
        // #281: a tap at a known screen point reaches the right guest point. The expected
        // values are worked out by hand from the cover and fit rules (480x960 and 960x480
        // displays, 320x480 guest), not read back from the model under test.
        func tap(_ g: PlayerGeometry, _ x: CGFloat, _ y: CGFloat, _ ex: CGFloat, _ ey: CGFloat, _ label: String) {
            if let p = g.guestPoint(CGPoint(x: x, y: y)) {
                check(near(p.x, ex) && near(p.y, ey), label)
            } else {
                check(false, label + " (tap ignored)")
            }
        }
        func fill(_ container: CGSize, _ orientation: GuestOrientation, _ focus: PlayerCropFocus) -> PlayerGeometry {
            PlayerGeometry(container: container, guest: guest, pixels: pixels, orientation: orientation,
                           mode: .fill, displayScale: 3, focus: focus.position)
        }
        let tall = CGSize(width: 480, height: 960), wide = CGSize(width: 960, height: 480)
        // Portrait on 480x960: scale 2, picture 640x960 placed at x = -80.
        tap(fill(tall, .portrait, .center), 0, 0, 40, 0, "fill portrait: display corner")
        tap(fill(tall, .portrait, .center), 120, 240, 100, 120, "fill portrait: interior point")
        tap(fill(tall, .upsideDown, .center), 120, 240, 220, 360, "fill upside down: interior point")
        // Landscape on 960x480: scale 2, picture 960x640. Centre focus puts it at y = -80.
        tap(fill(wide, .landscapeLeft, .center), 240, 80, 80, 360, "fill landscape left: interior point")
        tap(fill(wide, .landscapeRight, .center), 240, 80, 240, 120, "fill landscape right: interior point")
        // The crop focus moves the picture (y = 0, -80, -160), so the same tap reaches a different guest point.
        tap(fill(wide, .landscapeLeft, .top), 720, 240, 120, 120, "fill top focus: same tap, upper part of the guest")
        tap(fill(wide, .landscapeLeft, .center), 720, 240, 160, 120, "fill centre focus: same tap")
        tap(fill(wide, .landscapeLeft, .bottom), 720, 240, 200, 120, "fill bottom focus: same tap, lower part of the guest")
        // Extended bars on 480x960: aspect-fit picture 480x720 at y = 120.
        let barred = PlayerGeometry(container: tall, guest: guest, pixels: pixels, orientation: .portrait,
                                    mode: .extended, displayScale: 3)
        tap(barred, 120, 300, 80, 120, "extended bars: interior point")
        check(barred.guestPoint(CGPoint(x: 240, y: 60)) == nil, "extended bars: a tap in the top bar is ignored")
        // #281 modes 3 and 4: taller guest screen and safe-area insets.
        check(GuestScreenSettings.virtualHeight(display: CGSize(width: 390, height: 844)) == 692, "320 * 844 / 390 = 692.3")
        check(GuestScreenSettings.virtualHeight(display: CGSize(width: 844, height: 390)) == 692, "landscape display gives the same height")
        check(GuestScreenSettings.virtualHeight(display: CGSize(width: 320, height: 480)) == 480, "3:2 keeps 480")
        check(GuestScreenSettings.virtualHeight(display: CGSize(width: 100, height: 4000)) == nil, "absurd aspect is refused")
        check(GuestScreenSettings.virtualHeight(display: .zero) == nil, "zero display is refused")
        // Display top inset 47, bottom 34, left 3, right 5 at 390 wide: scale 320/390.
        let sc: CGFloat = 320 / 390
        func safe(_ o: GuestOrientation) -> String? {
            GuestScreenSettings.safeAreaValue(top: 47, left: 3, bottom: 34, right: 5, orientation: o, scale: sc)
        }
        check(safe(.portrait) == "39,2,28,4", "portrait insets (47,3,34,5) * 0.8205")
        check(safe(.upsideDown) == "28,4,39,2", "upside down swaps top/bottom and left/right")
        check(safe(.landscapeLeft) == "4,39,2,28", "landscape left: display right is guest top")
        check(safe(.landscapeRight) == "2,28,4,39", "landscape right: display left is guest top")
        check(GuestScreenSettings.safeAreaValue(top: -1, left: 0, bottom: 0, right: 0, orientation: .portrait, scale: 1) == nil,
              "negative inset refused")
        check(GuestScreenSettings.safeAreaValue(top: 500, left: 0, bottom: 0, right: 0, orientation: .portrait, scale: 1) == nil,
              "inset beyond the runtime limit refused")
        let suite = UserDefaults(suiteName: "hvm.player.tests.281b")!
        suite.removePersistentDomain(forName: "hvm.player.tests.281b")
        let gid = "com.example.game"
        var env: [String: String?] = [:]
        func launch() {
            GuestScreenSettings.apply(bundleID: gid, display: CGSize(width: 390, height: 844),
                                      insets: (47, 0, 34, 0), orientation: .portrait, defaults: suite) { env[$0] = $1 }
        }
        launch()
        check(env["HVM_VIRTUAL_SCREEN_HEIGHT"] == .some(nil) && env["HVM_SAFE_AREA"] == .some(nil), "off by default: both cleared")
        GuestScreenSettings.setVirtualScreen(true, bundleID: gid, defaults: suite)
        launch()
        check(env["HVM_VIRTUAL_SCREEN_HEIGHT"] == .some("692") && env["HVM_SAFE_AREA"] == .some("39,0,28,0"), "opt-in sets both")
        GuestScreenSettings.setSafeArea(false, bundleID: gid, defaults: suite)
        launch()
        check(env["HVM_VIRTUAL_SCREEN_HEIGHT"] == .some("692") && env["HVM_SAFE_AREA"] == .some(nil), "safe-area off switch")
        GuestScreenSettings.setRecord(.broken, bundleID: gid, defaults: suite)
        launch()
        check(env["HVM_VIRTUAL_SCREEN_HEIGHT"] == .some(nil), "a game recorded as broken falls back to the previous screen")
        check(!GuestScreenSettings.virtualScreen(bundleID: "com.example.other", defaults: suite), "settings are per game")
        print("Player geometry/activity: \(checks) checks passed")
    }
}
