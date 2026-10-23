import Foundation

@main
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
        print("Player geometry/activity: \(checks) checks passed")
    }
}
