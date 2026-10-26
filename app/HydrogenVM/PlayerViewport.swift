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

struct GuestKeyboardState: Equatable {
    var revision: UInt64 = 0
    var focusToken: UInt64 = 0
    var active = false
    var hasText = false
    var secure = false
    var multiline = false
    var type = 0
    var returnKey = 0
    var capitalization = 0
    var correction = 0

    init(_ value: [String: NSNumber] = [:]) {
        revision = value["revision"]?.uint64Value ?? 0
        focusToken = value["focusToken"]?.uint64Value ?? 0
        active = value["active"]?.boolValue ?? false
        hasText = value["hasText"]?.boolValue ?? false
        secure = value["secure"]?.boolValue ?? false
        multiline = value["multiline"]?.boolValue ?? false
        type = value["type"]?.intValue ?? 0
        returnKey = value["returnKey"]?.intValue ?? 0
        capitalization = value["capitalization"]?.intValue ?? 0
        correction = value["correction"]?.intValue ?? 0
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
    var keyboard = GuestKeyboardState()
    var onInsertText: (@MainActor (String) -> Void)? = nil
    var onDeleteBackward: (@MainActor () -> Void)? = nil
    var onTextReturn: (@MainActor () -> Void)? = nil
    let onTouch: (HVMTouchPhase, CGPoint, Int) -> Void

    func makeUIView(context: Context) -> ScreenView { ScreenView() }

    func updateUIView(_ view: ScreenView, context: Context) {
        view.onTouch = onTouch
        view.onInsertText = onInsertText
        view.onDeleteBackward = onDeleteBackward
        view.onTextReturn = onTextReturn
        view.configure(image: image, glLayer: glLayer, guest: guestSize, orientation: orientation,
                       mode: mode, filter: filter, enabled: inputEnabled)
        view.configureKeyboard(keyboard)
    }

    static func dismantleUIView(_ view: ScreenView, coordinator: ()) {
        view.cancelTouches()
        view.resignFirstResponder()
    }

    final class ScreenView: UIView, UIKeyInput {
        var onTouch: ((HVMTouchPhase, CGPoint, Int) -> Void)?
        var onInsertText: (@MainActor (String) -> Void)?
        var onDeleteBackward: (@MainActor () -> Void)?
        var onTextReturn: (@MainActor () -> Void)?
        private var keyboardState = GuestKeyboardState()
        private var keyboardAllowed = false
        var keyboardType: UIKeyboardType = .default
        var returnKeyType: UIReturnKeyType = .default
        var autocapitalizationType: UITextAutocapitalizationType = .none
        var autocorrectionType: UITextAutocorrectionType = .default
        var isSecureTextEntry = false
        var hasText: Bool { keyboardState.hasText }
        override var canBecomeFirstResponder: Bool { enabled && keyboardState.active }

        func insertText(_ text: String) {
            guard enabled, keyboardState.active else { return }
            if text == "\n", !keyboardState.multiline { onTextReturn?() }
            else { onInsertText?(text) }
        }

        func deleteBackward() {
            guard enabled, keyboardState.active else { return }
            onDeleteBackward?()
        }

        func configureKeyboard(_ state: GuestKeyboardState, attached: Bool = false) {
            let previous = keyboardState
            let wasAllowed = keyboardAllowed
            keyboardState = state
            keyboardAllowed = enabled && state.active
            let traitsChanged = previous.type != state.type || previous.returnKey != state.returnKey ||
                previous.capitalization != state.capitalization || previous.correction != state.correction || previous.secure != state.secure
            keyboardType = UIKeyboardType(rawValue: state.type) ?? .default
            returnKeyType = UIReturnKeyType(rawValue: state.returnKey) ?? .default
            autocapitalizationType = UITextAutocapitalizationType(rawValue: state.capitalization) ?? .none
            autocorrectionType = UITextAutocorrectionType(rawValue: state.correction) ?? .default
            isSecureTextEntry = state.secure
            guard keyboardAllowed, window != nil else {
                if isFirstResponder { resignFirstResponder() }
                return
            }
            // Re-rendering a frame must not steal focus from Hydrogen's own
            // inspector or another native control. Activate only on guest
            // focus, input resume or attachment to the player window.
            if attached || !wasAllowed || previous.focusToken != state.focusToken {
                becomeFirstResponder()
            } else if traitsChanged, isFirstResponder { reloadInputViews() }
        }
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
            configureKeyboard(keyboardState, attached: window != nil)
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
