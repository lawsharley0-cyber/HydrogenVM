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
    /// Three fingers held down (PlayerOverlay): asks for the controls while the overlay is off.
    var onOverlayGesture: (() -> Void)? = nil
    let onTouch: (HVMTouchPhase, CGPoint, Int) -> Void

    func makeUIView(context: Context) -> ScreenView { ScreenView() }

    func updateUIView(_ view: ScreenView, context: Context) {
        view.onTouch = onTouch
        view.onOverlayGesture = onOverlayGesture
        view.configure(image: image, glLayer: glLayer, guest: guestSize, orientation: orientation,
                       mode: mode, filter: filter, enabled: inputEnabled)
    }

    static func dismantleUIView(_ view: ScreenView, coordinator: ()) { view.cancelTouches() }

    final class ScreenView: UIView, UIGestureRecognizerDelegate {
        var onTouch: ((HVMTouchPhase, CGPoint, Int) -> Void)?
        var onOverlayGesture: (() -> Void)? {
            didSet { holdRecognizer.isEnabled = onOverlayGesture != nil }
        }
        // Observes only: it never cancels or delays the touches the game receives.
        private lazy var holdRecognizer: UILongPressGestureRecognizer = {
            let r = UILongPressGestureRecognizer(target: self, action: #selector(overlayHeld(_:)))
            r.numberOfTouchesRequired = PlayerOverlay.gestureTouches
            r.minimumPressDuration = PlayerOverlay.gestureHoldSeconds
            r.cancelsTouchesInView = false
            r.delaysTouchesBegan = false
            r.delaysTouchesEnded = false   // the default would withhold the game's touch-end once this recognizes
            r.delegate = self
            r.isEnabled = false
            return r
        }()

        @objc private func overlayHeld(_ r: UILongPressGestureRecognizer) {
            if r.state == .began { onOverlayGesture?() }
        }

        func gestureRecognizer(_ g: UIGestureRecognizer, shouldRecognizeSimultaneouslyWith other: UIGestureRecognizer) -> Bool { true }
        /// Container scaled and rotated to the viewport: the GL layer (if any)
        /// underneath, the composited UIKit frame on top.
        private let picture = CALayer()
        private let overlay = CALayer()
        private var glLayer: CALayer?
        private var guestSize = CGSize(width: 320, height: 480)
        private var pixelSize = CGSize(width: 640, height: 960)
        private var orientation = GuestOrientation.portrait
        private var mode = PlayerScaleMode.fit
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
            addGestureRecognizer(holdRecognizer)
            backgroundColor = .black
            clipsToBounds = true
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
                       mode: PlayerScaleMode, filter: PlayerFilter, enabled: Bool) {
            let pixels = image.map { CGSize(width: $0.width, height: $0.height) } ?? pixelSize
            if guestSize != guest || self.orientation != orientation || self.mode != mode || pixelSize != pixels || !enabled {
                cancelTouches()
            }
            guestSize = guest
            pixelSize = pixels
            self.orientation = orientation
            self.mode = mode
            self.enabled = enabled
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
            CATransaction.commit()
            setNeedsLayout()
        }

        override func layoutSubviews() {
            super.layoutSubviews()
            // The picture's turns depend only on the guest's orientation: the window
            // already turns with the phone between the two landscapes, so the
            // picture stays upright without changing them.
            let next = PlayerGeometry(container: bounds.size, guest: guestSize, pixels: pixelSize,
                                      orientation: orientation, mode: mode,
                                      displayScale: window?.screen.scale ?? traitCollection.displayScale)
            if let geometry, geometry != next { cancelTouches() }
            geometry = next
            let rect = next.rect
            CATransaction.begin()
            CATransaction.setDisableActions(true)
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
