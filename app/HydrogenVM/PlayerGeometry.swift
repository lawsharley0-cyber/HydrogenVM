import Foundation
import CoreGraphics

/// Values match the guest's UIInterfaceOrientation, not UIDeviceOrientation.
enum GuestOrientation: Int, CaseIterable {
    case portrait = 1, upsideDown = 2, landscapeLeft = 3, landscapeRight = 4

    var landscape: Bool { self == .landscapeLeft || self == .landscapeRight }
    /// Clockwise quarter turns from the guest's upright UI to its portrait framebuffer.
    var quarterTurns: Int {
        switch self {
        case .portrait: 0
        case .upsideDown: 2
        case .landscapeLeft: 3
        case .landscapeRight: 1
        }
    }
    var displayAngle: Double { -Double(quarterTurns) * .pi / 2 }

    /// CoreMotion and UIAcceleration both use device-fixed, y-up axes. Only the
    /// difference between the actual host scene and guest orientation is applied.
    /// When the host follows the guest, raw acceleration is already correct.
    func acceleration(x: Double, y: Double, z: Double, host: GuestOrientation) -> (Double, Double, Double) {
        switch (quarterTurns - host.quarterTurns + 4) % 4 {
        case 1: (y, -x, z)
        case 2: (-x, -y, z)
        case 3: (-y, x, z)
        default: (x, y, z)
        }
    }
}

enum PlayerScaleMode: String, CaseIterable, Identifiable {
    case fit = "Aspect Fit"
    case integer = "Integer Pixels"
    case stretch = "Stretch"
    case fill = "Fill (Crop)"
    case extended = "Extended Bars"
    var id: String { rawValue }

    /// Covers the whole display and crops the overflow (issue #281, mode 1).
    var crops: Bool { self == .fill }
    /// Keeps the aspect-fit picture and paints the bars with the game's own pixels
    /// (issue #281, mode 2).
    var extendsBars: Bool { self == .extended }
}

enum PlayerFilter: String, CaseIterable, Identifiable {
    case linear = "Bilinear", nearest = "Nearest Neighbor"
    var id: String { rawValue }
}

/// Where the crop sits when the picture is larger than the display (issue #281, mode 1).
enum PlayerCropFocus: String, CaseIterable, Identifiable {
    case top = "Top", center = "Center", bottom = "Bottom"
    var id: String { rawValue }

    /// 0 keeps the picture's top edge, 1 its bottom edge.
    var vertical: CGFloat {
        switch self {
        case .top: 0
        case .center: 0.5
        case .bottom: 1
        }
    }

    var position: CGPoint { CGPoint(x: 0.5, y: vertical) }
}

/// One layout is shared by drawing and inverse input mapping. Coordinates sent
/// to the runtime always remain in the portrait framebuffer's guest points.
struct PlayerGeometry: Equatable {
    let guestSize: CGSize
    let orientation: GuestOrientation
    let mode: PlayerScaleMode
    let containerSize: CGSize
    let rect: CGRect
    let focus: CGPoint
    /// The part of the container that must stay visible for touches (notch, home
    /// indicator). Fill keeps it inside the picture whenever the picture is big enough.
    let touchSafeArea: CGRect

    init(container: CGSize, guest: CGSize, pixels: CGSize, orientation: GuestOrientation,
         mode: PlayerScaleMode, displayScale: CGFloat,
         focus: CGPoint = CGPoint(x: 0.5, y: 0.5), touchSafeArea: CGRect = .zero) {
        guestSize = guest
        self.orientation = orientation
        self.mode = mode
        containerSize = container
        self.focus = focus
        self.touchSafeArea = touchSafeArea
        guard [container.width, container.height, guest.width, guest.height,
               pixels.width, pixels.height, displayScale].allSatisfy({ $0.isFinite && $0 > 0 }) else {
            rect = .zero
            return
        }
        let upright = orientation.landscape ? CGSize(width: guest.height, height: guest.width) : guest
        var size = container
        var origin = CGPoint.zero
        switch mode {
        case .stretch:
            break
        case .fill:
            // Cover: the smaller scale that still reaches both edges, so the crop only
            // ever removes picture, never adds empty space.
            let cover = max(container.width / upright.width, container.height / upright.height)
            size = CGSize(width: upright.width * cover, height: upright.height * cover)
            let fx = min(max(focus.x, 0), 1), fy = min(max(focus.y, 0), 1)
            origin = CGPoint(x: (container.width - size.width) * fx,
                             y: (container.height - size.height) * fy)
            // Never crop the touch-safe area away when the picture is large enough to
            // keep it: clamp the placed picture so the safe area stays inside it.
            if touchSafeArea.width > 0, size.width >= touchSafeArea.width {
                origin.x = min(max(origin.x, touchSafeArea.maxX - size.width), touchSafeArea.minX)
            }
            if touchSafeArea.height > 0, size.height >= touchSafeArea.height {
                origin.y = min(max(origin.y, touchSafeArea.maxY - size.height), touchSafeArea.minY)
            }
        case .fit, .integer, .extended:
            // Extended bars keep the whole picture and paint behind the bars instead.
            let fit = min(container.width / upright.width, container.height / upright.height)
            size = CGSize(width: upright.width * fit, height: upright.height * fit)
            if mode == .integer {
                let source = orientation.landscape ? CGSize(width: pixels.height, height: pixels.width) : pixels
                let multiple = floor(min(container.width * displayScale / source.width,
                                         container.height * displayScale / source.height))
                // If even 1x source pixels cannot fit, preserve the full image.
                if multiple >= 1 {
                    size = CGSize(width: source.width * multiple / displayScale,
                                  height: source.height * multiple / displayScale)
                }
            }
            origin = CGPoint(x: (container.width - size.width) / 2,
                             y: (container.height - size.height) / 2)
        }
        rect = CGRect(origin: origin, size: size)
    }

    /// True when something other than the picture is visible: the bars.
    var hasBars: Bool {
        guard rect.width > 0, rect.height > 0, containerSize.width > 0, containerSize.height > 0 else { return false }
        return rect.minX > 0.0001 || rect.minY > 0.0001
            || rect.maxX < containerSize.width - 0.0001 || rect.maxY < containerSize.height - 0.0001
    }

    func guestPoint(_ point: CGPoint, continuing: Bool = false) -> CGPoint? {
        guard rect.width > 0, rect.height > 0, point.x.isFinite, point.y.isFinite,
              continuing || rect.contains(point) else { return nil }
        let u = min(max((point.x - rect.minX) / rect.width, 0), 1)
        let v = min(max((point.y - rect.minY) / rect.height, 0), 1)
        let normalized: CGPoint
        switch orientation {
        case .portrait: normalized = CGPoint(x: u, y: v)
        case .upsideDown: normalized = CGPoint(x: 1 - u, y: 1 - v)
        case .landscapeLeft: normalized = CGPoint(x: v, y: 1 - u)
        case .landscapeRight: normalized = CGPoint(x: 1 - v, y: u)
        }
        return CGPoint(x: min(normalized.x * guestSize.width, max(0, guestSize.width - 0.5)),
                       y: min(normalized.y * guestSize.height, max(0, guestSize.height - 0.5)))
    }
}

/// Per-game player settings with a global default. The game's bundle identifier
/// selects the override; no title checks and no per-game code paths.
enum PlayerSettings {
    static let scaleModeKey = "player.scaleMode"
    static let cropFocusKey = "player.cropFocus"

    static func perGameKey(_ key: String, bundleID: String) -> String { "\(key).\(bundleID)" }

    static func scaleMode(bundleID: String, defaults: UserDefaults = .standard) -> PlayerScaleMode {
        if let raw = defaults.string(forKey: perGameKey(scaleModeKey, bundleID: bundleID)),
           let mode = PlayerScaleMode(rawValue: raw) {
            return mode
        }
        guard let raw = defaults.string(forKey: scaleModeKey), let mode = PlayerScaleMode(rawValue: raw) else {
            return .fit
        }
        return mode
    }

    static func setScaleMode(_ mode: PlayerScaleMode, bundleID: String, defaults: UserDefaults = .standard) {
        defaults.set(mode.rawValue, forKey: perGameKey(scaleModeKey, bundleID: bundleID))
    }

    /// Back to the global default.
    static func clearScaleMode(bundleID: String, defaults: UserDefaults = .standard) {
        defaults.removeObject(forKey: perGameKey(scaleModeKey, bundleID: bundleID))
    }

    /// True when this game has chosen its own mode, so a later change of the global default does not reach it.
    static func hasScaleOverride(bundleID: String, defaults: UserDefaults = .standard) -> Bool {
        defaults.string(forKey: perGameKey(scaleModeKey, bundleID: bundleID)) != nil
    }

    static func cropFocus(bundleID: String, defaults: UserDefaults = .standard) -> PlayerCropFocus {
        guard let raw = defaults.string(forKey: perGameKey(cropFocusKey, bundleID: bundleID)),
              let focus = PlayerCropFocus(rawValue: raw) else {
            return .center
        }
        return focus
    }

    static func setCropFocus(_ focus: PlayerCropFocus, bundleID: String, defaults: UserDefaults = .standard) {
        defaults.set(focus.rawValue, forKey: perGameKey(cropFocusKey, bundleID: bundleID))
    }
}

/// How a game copes with the taller virtual screen (issue #281, mode 3).
enum GuestScreenRecord: String, CaseIterable, Identifiable {
    case unknown = "Not tried", works = "Works", broken = "Broken"
    var id: String { rawValue }
}

/// The taller guest screen (mode 3) and the safe-area insets (mode 4) are opt-in per game and
/// reach the runtime as environment variables set for one launch. The runtime ignores values
/// it cannot use, so a game falls back to the previous screen rather than failing.
enum GuestScreenSettings {
    static let virtualKey = "player.virtualScreen"
    static let safeAreaKey = "player.safeArea"
    static let recordKey = "player.virtualScreenRecord"
    static let heightVariable = "HVM_VIRTUAL_SCREEN_HEIGHT"
    static let safeAreaVariable = "HVM_SAFE_AREA"

    static func virtualScreen(bundleID: String, defaults: UserDefaults = .standard) -> Bool {
        defaults.bool(forKey: PlayerSettings.perGameKey(virtualKey, bundleID: bundleID))
    }

    static func setVirtualScreen(_ on: Bool, bundleID: String, defaults: UserDefaults = .standard) {
        defaults.set(on, forKey: PlayerSettings.perGameKey(virtualKey, bundleID: bundleID))
    }

    /// Safe-area insets follow the taller screen unless the player switched them off.
    static func safeArea(bundleID: String, defaults: UserDefaults = .standard) -> Bool {
        let key = PlayerSettings.perGameKey(safeAreaKey, bundleID: bundleID)
        return defaults.object(forKey: key) as? Bool ?? virtualScreen(bundleID: bundleID, defaults: defaults)
    }

    static func setSafeArea(_ on: Bool, bundleID: String, defaults: UserDefaults = .standard) {
        defaults.set(on, forKey: PlayerSettings.perGameKey(safeAreaKey, bundleID: bundleID))
    }

    static func record(bundleID: String, defaults: UserDefaults = .standard) -> GuestScreenRecord {
        defaults.string(forKey: PlayerSettings.perGameKey(recordKey, bundleID: bundleID))
            .flatMap(GuestScreenRecord.init(rawValue:)) ?? .unknown
    }

    static func setRecord(_ record: GuestScreenRecord, bundleID: String, defaults: UserDefaults = .standard) {
        defaults.set(record.rawValue, forKey: PlayerSettings.perGameKey(recordKey, bundleID: bundleID))
    }

    /// A game recorded as broken goes back to the previous screen even if the switch is still on.
    static func effectiveVirtualScreen(bundleID: String, defaults: UserDefaults = .standard) -> Bool {
        virtualScreen(bundleID: bundleID, defaults: defaults) && record(bundleID: bundleID, defaults: defaults) != .broken
    }

    /// Portrait guest points (width 320) that match the display's aspect, or nil when the
    /// result is outside what the runtime accepts (480...1024).
    static func virtualHeight(display: CGSize, guestWidth: CGFloat = 320) -> Int? {
        let short = min(display.width, display.height), long = max(display.width, display.height)
        guard short.isFinite, long.isFinite, short > 0 else { return nil }
        let height = Int((guestWidth * long / short).rounded(.down))
        return (480...1024).contains(height) ? height : nil
    }

    /// Display-edge insets expressed as "top,left,bottom,right" in the portrait framebuffer's
    /// points. `scale` is guest points per display point. Which display edge is which guest
    /// edge follows the same quarter turns as `PlayerGeometry.guestPoint`.
    static func safeAreaValue(top: CGFloat, left: CGFloat, bottom: CGFloat, right: CGFloat,
                              orientation: GuestOrientation, scale: CGFloat) -> String? {
        guard scale.isFinite, scale > 0, [top, left, bottom, right].allSatisfy({ $0.isFinite && $0 >= 0 }) else { return nil }
        let guest: (CGFloat, CGFloat, CGFloat, CGFloat)
        switch orientation {
        case .portrait: guest = (top, left, bottom, right)
        case .upsideDown: guest = (bottom, right, top, left)
        case .landscapeLeft: guest = (right, top, left, bottom)
        case .landscapeRight: guest = (left, bottom, right, top)
        }
        let values = [guest.0, guest.1, guest.2, guest.3].map { Int(($0 * scale).rounded()) }
        guard values.allSatisfy({ $0 <= 200 }) else { return nil }
        return values.map(String.init).joined(separator: ",")
    }

    /// Sets or clears the variables for the launch about to start.
    static func apply(bundleID: String, display: CGSize, insets: (top: CGFloat, left: CGFloat, bottom: CGFloat, right: CGFloat),
                      orientation: GuestOrientation, defaults: UserDefaults = .standard,
                      environment: (String, String?) -> Void) {
        let height = effectiveVirtualScreen(bundleID: bundleID, defaults: defaults)
            ? virtualHeight(display: display) : nil
        environment(heightVariable, height.map(String.init))
        var safe: String?
        if safeArea(bundleID: bundleID, defaults: defaults), height != nil {
            let portraitWidth = min(display.width, display.height)
            safe = safeAreaValue(top: insets.top, left: insets.left, bottom: insets.bottom, right: insets.right,
                                 orientation: orientation, scale: portraitWidth > 0 ? 320 / portraitWidth : 0)
        }
        environment(safeAreaVariable, safe)
    }
}
