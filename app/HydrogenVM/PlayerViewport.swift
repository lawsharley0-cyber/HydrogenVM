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
    let guestSize: CGSize
    var orientation: GuestOrientation = .portrait
    var mode: PlayerScaleMode = .fit
    var filter: PlayerFilter = .linear
    var inputEnabled = true
    let onTouch: (HVMTouchPhase, CGPoint, Int) -> Void

    func makeUIView(context: Context) -> ScreenView { ScreenView() }

    func updateUIView(_ view: ScreenView, context: Context) {
        view.onTouch = onTouch
        view.configure(image: image, guest: guestSize, orientation: orientation,
                       mode: mode, filter: filter, enabled: inputEnabled)
    }

    static func dismantleUIView(_ view: ScreenView, coordinator: ()) { view.cancelTouches() }

    final class ScreenView: UIView {
        var onTouch: ((HVMTouchPhase, CGPoint, Int) -> Void)?
        private let picture = CALayer()
        private var guestSize = CGSize(width: 320, height: 480)
        private var pixelSize = CGSize(width: 640, height: 960)
        private var orientation = GuestOrientation.portrait
        private var mode = PlayerScaleMode.fit
        private var enabled = true
        private var geometry: PlayerGeometry?
        private struct Finger { let id: Int; var point: CGPoint }
        private var fingers: [ObjectIdentifier: Finger] = [:]

        override init(frame: CGRect) {
            super.init(frame: frame)
            isMultipleTouchEnabled = true
            backgroundColor = .black
            clipsToBounds = true
            layer.addSublayer(picture)
            accessibilityIdentifier = "player-viewport"
        }
        required init?(coder: NSCoder) { fatalError() }

        func configure(image: CGImage?, guest: CGSize, orientation: GuestOrientation,
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
            picture.contents = image
            picture.magnificationFilter = filter == .nearest ? .nearest : .linear
            picture.minificationFilter = filter == .nearest ? .nearest : .linear
            CATransaction.commit()
            setNeedsLayout()
        }

        override func layoutSubviews() {
            super.layoutSubviews()
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
