import Foundation

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
    case fit = "Aspect Fit", integer = "Integer Pixels", stretch = "Stretch"
    var id: String { rawValue }
}

enum PlayerFilter: String, CaseIterable, Identifiable {
    case linear = "Bilinear", nearest = "Nearest Neighbor"
    var id: String { rawValue }
}

/// One layout is shared by drawing and inverse input mapping. Coordinates sent
/// to the runtime always remain in the portrait framebuffer's guest points.
struct PlayerGeometry: Equatable {
    let guestSize: CGSize
    let orientation: GuestOrientation
    let rect: CGRect

    init(container: CGSize, guest: CGSize, pixels: CGSize, orientation: GuestOrientation,
         mode: PlayerScaleMode, displayScale: CGFloat) {
        guestSize = guest
        self.orientation = orientation
        guard [container.width, container.height, guest.width, guest.height,
               pixels.width, pixels.height, displayScale].allSatisfy({ $0.isFinite && $0 > 0 }) else {
            rect = .zero
            return
        }
        let upright = orientation.landscape ? CGSize(width: guest.height, height: guest.width) : guest
        var size = container
        if mode != .stretch {
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
        }
        rect = CGRect(x: (container.width - size.width) / 2, y: (container.height - size.height) / 2,
                      width: size.width, height: size.height)
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
