import SwiftUI
import UIKit

extension GuestOrientation {
    var mask: UIInterfaceOrientationMask {
        switch self {
        case .portrait: .portrait
        case .upsideDown: .portraitUpsideDown
        case .landscapeLeft: .landscapeLeft
        case .landscapeRight: .landscapeRight
        }
    }
}

/// Shows the guest's portrait framebuffer upright, undoing its root rotation.
struct EmulatorScreen: UIViewRepresentable {
    let image: CGImage?
    /// GPU-rendered GL drawable (HVMSession.glLayer); shown under `image`.
    var glLayer: CALayer? = nil
    let guestSize: CGSize
    var orientation: GuestOrientation = .portrait
    var mode: PlayerScaleMode = .fit
    var filter: PlayerFilter = .linear
    var inputEnabled = true
    /// Where the crop sits in fill mode (PlayerCropFocus).
    var focus: CGPoint = CGPoint(x: 0.5, y: 0.5)
    let onTouch: (HVMTouchPhase, CGPoint, Int) -> Void

    func makeUIView(context: Context) -> ScreenView { ScreenView() }

    func updateUIView(_ view: ScreenView, context: Context) {
        view.onTouch = onTouch
        view.configure(image: image, glLayer: glLayer, guest: guestSize, orientation: orientation,
                       mode: mode, filter: filter, enabled: inputEnabled, focus: focus)
    }

    static func dismantleUIView(_ view: ScreenView, coordinator: ()) { view.cancelTouches() }

    final class ScreenView: UIView {
        var onTouch: ((HVMTouchPhase, CGPoint, Int) -> Void)?
        /// Container scaled and rotated to the viewport: the GL layer (if any)
        /// underneath, the composited UIKit frame on top.
        private let picture = CALayer()
        private let overlay = CALayer()
        /// Extended bars: a blurred, dimmed copy of the frame behind the picture.
        private let backdrop = CALayer()
        private let backdropDim = CALayer()
        private var backdropSource: CGImage?
        private var backdropTime: CFTimeInterval = 0
        private var glLayer: CALayer?
        private var guestSize = CGSize(width: 320, height: 480)
        private var pixelSize = CGSize(width: 640, height: 960)
        private var orientation = GuestOrientation.portrait
        private var mode = PlayerScaleMode.fit
        private var focus = CGPoint(x: 0.5, y: 0.5)
        private var enabled = true
        private var geometry: PlayerGeometry?
        private struct Finger { let id: Int; var point: CGPoint }
        private var fingers: [ObjectIdentifier: Finger] = [:]

        @objc private func deviceTurned(_ note: Notification) {
            setNeedsLayout()
            // The scene's interface orientation updates during the rotation animation.
            Task { @MainActor [weak self] in
                try? await Task.sleep(for: .milliseconds(350))
                self?.setNeedsLayout()
            }
        }

        @objc private func glGeometryChanged(_ note: Notification) {
            guard let gl = glLayer, (note.object as AnyObject?) === gl else { return }
            setNeedsLayout()
        }

        override init(frame: CGRect) {
            super.init(frame: frame)
            isMultipleTouchEnabled = true
            backgroundColor = .black
            clipsToBounds = true
            backdrop.isHidden = true
            backdrop.magnificationFilter = .linear
            backdrop.minificationFilter = .linear
            backdropDim.backgroundColor = UIColor.black.cgColor
            backdropDim.opacity = 0.45
            backdrop.addSublayer(backdropDim)
            layer.addSublayer(backdrop)
            layer.addSublayer(picture)
            picture.addSublayer(overlay)
            // A 180° turn between the two landscapes doesn't change the view's size, so
            // layout isn't triggered by it; re-lay out when the device turns.
            UIDevice.current.beginGeneratingDeviceOrientationNotifications()
            NotificationCenter.default.addObserver(self, selector: #selector(deviceTurned(_:)),
                                                   name: UIDevice.orientationDidChangeNotification, object: nil)
            // Selector observers are removed automatically when the view goes away.
            NotificationCenter.default.addObserver(self, selector: #selector(glGeometryChanged(_:)),
                                                   name: Notification.Name("HVMGLLayerGeometryDidChange"),
                                                   object: nil)
            isAccessibilityElement = true
            accessibilityLabel = "Game Screen"
            accessibilityIdentifier = "player-viewport"
        }
        required init?(coder: NSCoder) { fatalError() }

        func configure(image: CGImage?, glLayer newGL: CALayer?, guest: CGSize, orientation: GuestOrientation,
                       mode: PlayerScaleMode, filter: PlayerFilter, enabled: Bool,
                       focus: CGPoint = CGPoint(x: 0.5, y: 0.5)) {
            let pixels = image.map { CGSize(width: $0.width, height: $0.height) } ?? pixelSize
            if guestSize != guest || self.orientation != orientation || self.mode != mode || pixelSize != pixels
                || !enabled || self.focus != focus {
                cancelTouches()
            }
            guestSize = guest
            pixelSize = pixels
            self.orientation = orientation
            self.mode = mode
            self.enabled = enabled
            self.focus = focus
            CATransaction.begin()
            CATransaction.setDisableActions(true)
            overlay.contents = image
            overlay.magnificationFilter = filter == .nearest ? .nearest : .linear
            overlay.minificationFilter = filter == .nearest ? .nearest : .linear
            if glLayer !== newGL {
                glLayer?.removeFromSuperlayer()
                glLayer = newGL
                if let gl = newGL { picture.insertSublayer(gl, below: overlay) }
            }
            glLayer?.magnificationFilter = overlay.magnificationFilter
            glLayer?.minificationFilter = overlay.minificationFilter
            updateBackdrop(image)
            CATransaction.commit()
            setNeedsLayout()
        }

        /// Extended bars: a heavily downscaled (box-blurred) copy of the frame, dimmed,
        /// filling the whole display behind the picture. Once the GPU path can read its
        /// drawable back, the same layer takes that image.
        private func updateBackdrop(_ image: CGImage?) {
            let wants = mode == .extended && image != nil
            backdrop.isHidden = !wants
            guard wants, let image else { return }
            let now = CACurrentMediaTime()
            // A few updates a second is enough for a blurred backdrop and much cheaper
            // than regenerating a bitmap for every frame.
            guard backdropSource !== image || now - backdropTime > 0.25 else { return }
            backdropSource = image
            backdropTime = now
            backdrop.contents = Self.blurredCopy(image)
        }

        private static func blurredCopy(_ image: CGImage) -> CGImage? {
            let width = 32, height = 32
            guard let ctx = CGContext(data: nil, width: width, height: height, bitsPerComponent: 8,
                                      bytesPerRow: 0, space: CGColorSpaceCreateDeviceRGB(),
                                      bitmapInfo: CGImageAlphaInfo.premultipliedLast.rawValue) else { return nil }
            ctx.interpolationQuality = .medium
            ctx.draw(image, in: CGRect(x: 0, y: 0, width: width, height: height))
            return ctx.makeImage()
        }

        override func layoutSubviews() {
            super.layoutSubviews()
            // The picture's turns depend only on the guest's orientation: the window
            // already turns with the phone between the two landscapes, so the
            // picture stays upright without changing them.
            // The notch and home indicator must keep showing game pixels in fill mode,
            // so pass the window's safe area in with the layout.
            let insets = window?.safeAreaInsets ?? .zero
            let safeArea = CGRect(x: insets.left, y: insets.top,
                                  width: max(bounds.width - insets.left - insets.right, 0),
                                  height: max(bounds.height - insets.top - insets.bottom, 0))
            let next = PlayerGeometry(container: bounds.size, guest: guestSize, pixels: pixelSize,
                                      orientation: orientation, mode: mode,
                                      displayScale: window?.screen.scale ?? traitCollection.displayScale,
                                      focus: focus, touchSafeArea: safeArea)
            if let geometry, geometry != next { cancelTouches() }
            geometry = next
            let rect = next.rect
            CATransaction.begin()
            CATransaction.setDisableActions(true)
            backdrop.frame = bounds
            backdropDim.frame = backdrop.bounds
            picture.bounds = CGRect(origin: .zero, size: orientation.landscape
                ? CGSize(width: rect.height, height: rect.width) : rect.size)
            picture.position = CGPoint(x: rect.midX, y: rect.midY)
            picture.setAffineTransform(CGAffineTransform(rotationAngle: orientation.displayAngle))
            overlay.frame = picture.bounds
            if let gl = glLayer {
                if RunModel.automation {
                    RunModel.console("[gpu] layout picture \(picture.bounds.size) gl \(gl.bounds.size) super \(gl.superlayer === picture)")
                }
                // The GL layer's bounds are its drawable in pixels. The emulator says where
                // that drawable sits on the guest screen (rotated for landscape games);
                // until it does, scale the layer to fill the picture.
                if let unit = (gl.value(forKey: HVMGLLayerUnitTransformKey) as? NSValue)?.cgAffineTransformValue {
                    gl.anchorPoint = .zero
                    gl.position = .zero
                    gl.setAffineTransform(unit.concatenating(CGAffineTransform(scaleX: picture.bounds.width,
                                                                               y: picture.bounds.height)))
                } else {
                    let src = gl.bounds.size
                    gl.anchorPoint = CGPoint(x: 0.5, y: 0.5)
                    gl.position = CGPoint(x: picture.bounds.midX, y: picture.bounds.midY)
                    if src.width > 0, src.height > 0 {
                        gl.setAffineTransform(CGAffineTransform(scaleX: picture.bounds.width / src.width,
                                                                y: picture.bounds.height / src.height))
                    }
                }
            }
            CATransaction.commit()
        }

        override func didMoveToWindow() {
            super.didMoveToWindow()
            if window == nil { cancelTouches() }
            setNeedsLayout()
        }

        func cancelTouches() {
            let active = fingers.values.sorted { $0.id < $1.id }
            fingers.removeAll()
            for finger in active { onTouch?(.cancelled, finger.point, finger.id) }
        }

        private func send(_ touches: Set<UITouch>, _ phase: HVMTouchPhase) {
            layoutIfNeeded()
            guard enabled, let geometry else { return }
            for touch in touches {
                let key = ObjectIdentifier(touch)
                if phase == .began {
                    guard fingers[key] == nil,
                          let point = geometry.guestPoint(touch.location(in: self)),
                          let id = (0..<11).first(where: { id in !fingers.values.contains(where: { $0.id == id }) }) else { continue }
                    fingers[key] = Finger(id: id, point: point)
                }
                guard var finger = fingers[key],
                      let point = geometry.guestPoint(touch.location(in: self), continuing: true) else { continue }
                finger.point = point
                fingers[key] = finger
                onTouch?(phase, point, finger.id)
                if phase == .ended || phase == .cancelled { fingers[key] = nil }
            }
        }

        override func touchesBegan(_ touches: Set<UITouch>, with event: UIEvent?) { send(touches, .began) }
        override func touchesMoved(_ touches: Set<UITouch>, with event: UIEvent?) { send(touches, .moved) }
        override func touchesEnded(_ touches: Set<UITouch>, with event: UIEvent?) { send(touches, .ended) }
        override func touchesCancelled(_ touches: Set<UITouch>, with event: UIEvent?) { send(touches, .cancelled) }
    }
}
