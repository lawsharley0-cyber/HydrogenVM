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

        // #279: Home Screen links (hydrogenvm://run/<id>) and the Web Clip profile.
        check(GameLink.gameID(from: URL(string: "hydrogenvm://run/com.example.game")!) == "com.example.game",
              "game link id")
        check(GameLink.gameID(from: URL(string: "HYDROGENVM://RUN/com.example.game")!) == "com.example.game",
              "scheme and host are case-insensitive")
        for bad in ["https://example.com/run/com.example.game",
                    "hydrogenvm://open/com.example.game",
                    "hydrogenvm://run",
                    "hydrogenvm://run/",
                    "hydrogenvm://run/com.example.game/extra",
                    "hydrogenvm://run/com.example%20game",
                    "hydrogenvm://run/com.example.game?source=clip"] {
            check(GameLink.gameID(from: URL(string: bad)!) == nil, "malformed link: \(bad)")
        }
        let installed = ["com.example.other", "com.example.game"]
        if case .success(let launched) = GameLink.resolve(URL(string: "hydrogenvm://run/COM.Example.Game")!,
                                                         installed: installed) {
            check(launched == "com.example.game", "resolves to the installed spelling of the id")
        } else {
            check(false, "installed game resolves")
        }
        if case .failure(let error) = GameLink.resolve(URL(string: "hydrogenvm://run/com.gone.game")!,
                                                       installed: installed) {
            check(error == .unknownGame("com.gone.game") && error.message.contains("com.gone.game"),
                  "a deleted or unknown game is reported by id")
        } else {
            check(false, "unknown game fails")
        }
        if case .failure(let error) = GameLink.resolve(URL(string: "https://example.com")!, installed: installed) {
            check(error == .malformed, "a foreign link is malformed, not an unknown game")
        } else {
            check(false, "malformed link fails")
        }
        let generated = GameLink.url(forGameID: "com.example.game")
        check(generated?.absoluteString == "hydrogenvm://run/com.example.game", "generated link")
        check(generated.flatMap(GameLink.gameID(from:)) == "com.example.game", "link round trip")
        check(GameLink.url(forGameID: "com.example game") == nil, "no link for an id with a space")

        let icon = Data([0x89, 0x50, 0x4E, 0x47])
        let clip = WebClipProfile(gameID: "com.example.game", title: "Tom & Jerry <HD>",
                                 link: generated!, iconPNG: icon)
        let profile = clip.payload()
        check(profile.contains("<string>hydrogenvm://run/com.example.game</string>"), "clip opens the game")
        check(profile.contains("Tom &amp; Jerry &lt;HD&gt;"), "clip label is escaped")
        check(profile.contains("com.apple.webClip.managed") && profile.contains("<key>FullScreen</key>"),
              "web clip payload")
        check(profile.contains("<string>Configuration</string>"), "configuration payload")
        check(profile.contains("<data>iVBORw==</data>"), "game icon is embedded")
        check(clip.payloadUUID != clip.configUUID, "payload identifiers differ")
        check(clip.filename == "Tom---Jerry--HD.mobileconfig", "file name is safe")
        check(WebClipProfile(gameID: "com.example.game", title: "Tom & Jerry <HD>",
                            link: generated!, iconPNG: icon).payload() == profile,
              "the same game gives the same profile")
        check(WebClipProfile(gameID: "com.example.other", title: "Tom & Jerry <HD>",
                            link: generated!, iconPNG: icon).payloadUUID != clip.payloadUUID,
              "a second clip for one game replaces it instead of piling up")
        let plain = WebClipProfile(gameID: "com.example.game", title: "", link: generated!, iconPNG: nil)
        check(!plain.payload().contains("<key>Icon</key>"), "no icon key when the game icon is missing")
        check(plain.filename == "Game.mobileconfig", "empty name falls back to a file name")
        // The document must be a real property list, not merely plausible text.
        let parsed: [String: Any]? = try? PropertyListSerialization.propertyList(
            from: Data(profile.utf8), options: [], format: nil) as? [String: Any]
        if let plist = parsed, let payload = (plist["PayloadContent"] as? [[String: Any]])?.first {
            check(plist["PayloadType"] as? String == "Configuration", "profile is a configuration")
            check(payload["PayloadType"] as? String == "com.apple.webClip.managed", "web clip payload type")
            check(payload["URL"] as? String == "hydrogenvm://run/com.example.game", "web clip URL")
            check(payload["FullScreen"] as? Bool == true && payload["Label"] as? String == "Tom & Jerry <HD>",
                  "label from the game name")
        } else {
            check(false, "profile parses as a property list")
        }
        print("Player geometry/activity: \(checks) checks passed")
    }
}
