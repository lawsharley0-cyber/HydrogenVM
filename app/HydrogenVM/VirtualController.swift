import Foundation
import GameController
import SwiftUI
import UIKit

/// How the left thumbstick on `GCVirtualController` maps onto the guest game.
enum VirtualThumbstickMode: String, CaseIterable, Codable, Identifiable {
    /// Drags a virtual finger around a center point on the guest screen.
    case touchStick = "Touch Stick"
    /// Tilts the guest's accelerometer (CoreMotion / UIAcceleration / Android sensor).
    case tilt = "Accelerometer Tilt"

    var id: String { rawValue }
}

/// The four face buttons offered by `GCVirtualController`.
enum VirtualControllerButton: String, CaseIterable, Codable, Identifiable {
    case a = "Button A"
    case b = "Button B"
    case x = "Button X"
    case y = "Button Y"

    var id: String { rawValue }

    var shortLabel: String {
        switch self {
        case .a: "A"
        case .b: "B"
        case .x: "X"
        case .y: "Y"
        }
    }

    var gcElementName: String {
        switch self {
        case .a: GCInputButtonA
        case .b: GCInputButtonB
        case .x: GCInputButtonX
        case .y: GCInputButtonY
        }
    }

    /// Dedicated finger indices (6...9) so virtual controller presses never collide
    /// with direct multi-touch fingers (0...5) or the virtual thumbstick (10).
    var fingerIndex: Int {
        switch self {
        case .a: 6
        case .b: 7
        case .x: 8
        case .y: 9
        }
    }
}

/// Normalized position (0...1 in the upright guest viewport) where a virtual control
/// lands on the guest screen.
struct NormalizedTargetPoint: Codable, Equatable {
    var u: Double
    var v: Double

    init(u: Double, v: Double) {
        self.u = min(max(u, 0.04), 0.96)
        self.v = min(max(v, 0.04), 0.96)
    }

    /// Converts upright normalized `(u, v)` into the portrait framebuffer guest points
    /// expected by `HVMSession.touch(_:x:y:finger:)`, matching `PlayerGeometry.guestPoint`.
    func guestPoint(guestSize: CGSize, orientation: GuestOrientation) -> CGPoint {
        let clampedU = min(max(u, 0), 1)
        let clampedV = min(max(v, 0), 1)
        let normalized: CGPoint
        switch orientation {
        case .portrait: normalized = CGPoint(x: clampedU, y: clampedV)
        case .upsideDown: normalized = CGPoint(x: 1 - clampedU, y: 1 - clampedV)
        case .landscapeLeft: normalized = CGPoint(x: clampedV, y: 1 - clampedU)
        case .landscapeRight: normalized = CGPoint(x: 1 - clampedV, y: clampedU)
        }
        return CGPoint(
            x: min(normalized.x * guestSize.width, max(0, guestSize.width - 0.5)),
            y: min(normalized.y * guestSize.height, max(0, guestSize.height - 0.5))
        )
    }
}

/// Per-game on-screen controller layout and mapping, saved per bundle identifier.
struct VirtualControllerLayout: Codable, Equatable {
    var enabled: Bool = false
    var thumbstickMode: VirtualThumbstickMode = .touchStick
    /// Upright normalized center of the virtual touch thumbstick.
    var stickCenter: NormalizedTargetPoint = NormalizedTargetPoint(u: 0.18, v: 0.76)
    /// Maximum drag radius as a fraction of the shorter upright guest dimension.
    var stickRadiusFraction: Double = 0.14
    /// Tilt sensitivity in g-units at full thumbstick deflection when `thumbstickMode == .tilt`.
    var tiltSensitivity: Double = 0.65
    /// Which face buttons are shown and mapped.
    var enabledButtons: [VirtualControllerButton] = [.a, .b, .x, .y]
    /// Upright normalized touch targets for each face button.
    var buttonTargets: [String: NormalizedTargetPoint] = [
        VirtualControllerButton.a.rawValue: NormalizedTargetPoint(u: 0.84, v: 0.82),
        VirtualControllerButton.b.rawValue: NormalizedTargetPoint(u: 0.92, v: 0.70),
        VirtualControllerButton.x.rawValue: NormalizedTargetPoint(u: 0.76, v: 0.70),
        VirtualControllerButton.y.rawValue: NormalizedTargetPoint(u: 0.84, v: 0.58)
    ]

    func target(for button: VirtualControllerButton) -> NormalizedTargetPoint {
        buttonTargets[button.rawValue] ?? NormalizedTargetPoint(u: 0.84, v: 0.75)
    }

    mutating func setTarget(_ point: NormalizedTargetPoint, for button: VirtualControllerButton) {
        buttonTargets[button.rawValue] = point
    }

    func isButtonEnabled(_ button: VirtualControllerButton) -> Bool {
        enabledButtons.contains(button)
    }

    mutating func setButton(_ button: VirtualControllerButton, enabled isOn: Bool) {
        if isOn {
            if !enabledButtons.contains(button) { enabledButtons.append(button) }
        } else {
            enabledButtons.removeAll { $0 == button }
        }
    }

    static let stickFingerIndex = 10
    static let storagePrefix = "player.virtualControllerLayout"
    static let enabledPrefix = "player.virtualControllerEnabled"

    static func enabledKey(_ bundleID: String) -> String {
        "\(enabledPrefix).\(bundleID)"
    }

    static func storageKey(_ bundleID: String) -> String {
        "\(storagePrefix).\(bundleID)"
    }

    static func load(bundleID: String, defaults: UserDefaults = .standard) -> VirtualControllerLayout {
        var layout = VirtualControllerLayout()
        if let data = defaults.data(forKey: storageKey(bundleID)),
           let decoded = try? JSONDecoder().decode(VirtualControllerLayout.self, from: data) {
            layout = decoded
        }
        if defaults.object(forKey: enabledKey(bundleID)) != nil {
            layout.enabled = defaults.bool(forKey: enabledKey(bundleID))
        }
        return layout
    }

    func save(bundleID: String, defaults: UserDefaults = .standard) {
        defaults.set(enabled, forKey: Self.enabledKey(bundleID))
        if let data = try? JSONEncoder().encode(self) {
            defaults.set(data, forKey: Self.storageKey(bundleID))
        }
    }

    static func setEnabled(_ isOn: Bool, bundleID: String, defaults: UserDefaults = .standard) {
        var current = load(bundleID: bundleID, defaults: defaults)
        current.enabled = isOn
        current.save(bundleID: bundleID, defaults: defaults)
    }
}

/// Manages Apple's `GCVirtualController` (iOS 15+) and translates its thumbstick and button
/// events into HydrogenVM's existing touch and accelerometer bridge.
@MainActor
final class VirtualControllerCoordinator {
    private var virtualController: GCVirtualController?
    private var connectedElements: Set<String> = []
    private var stickDown = false
    private var buttonDown: Set<VirtualControllerButton> = []
    private(set) var stickDeflection: CGPoint = .zero

    var layout: VirtualControllerLayout
    var guestSize: CGSize = CGSize(width: 320, height: 480)
    var orientation: GuestOrientation = .portrait
    var inputEnabled: Bool = true
    var onTouch: ((HVMTouchPhase, CGPoint, Int) -> Void)?
    var onTilt: ((Double, Double, Double) -> Void)?

    init(layout: VirtualControllerLayout) {
        self.layout = layout
    }

    var isDrivingTilt: Bool {
        layout.enabled && layout.thumbstickMode == .tilt && virtualController != nil
    }

    func syncState(enabled: Bool, layout newLayout: VirtualControllerLayout, guestSize: CGSize, orientation: GuestOrientation, inputEnabled: Bool) {
        let layoutChanged = self.layout != newLayout || self.orientation != orientation || self.guestSize != guestSize
        let elementsChanged = Set(newLayout.enabledButtons.map(\.gcElementName)) != connectedElements.subtracting([GCInputLeftThumbstick])

        if !enabled || !newLayout.enabled || !inputEnabled {
            releaseAllInputs()
        } else if layoutChanged {
            releaseAllInputs()
        }

        self.layout = newLayout
        self.guestSize = guestSize
        self.orientation = orientation
        self.inputEnabled = inputEnabled

        guard enabled && newLayout.enabled else {
            disconnect()
            return
        }

        if virtualController == nil || elementsChanged {
            connect(with: newLayout)
        }
    }

    private func connect(with layout: VirtualControllerLayout) {
        disconnect()
        let config = GCVirtualController.Configuration()
        var elements: Set<String> = [GCInputLeftThumbstick]
        for button in layout.enabledButtons {
            elements.insert(button.gcElementName)
        }
        config.elements = elements
        connectedElements = elements

        let controller = GCVirtualController(configuration: config)
        virtualController = controller
        controller.connect { [weak self] error in
            Task { @MainActor [weak self] in
                guard let self, error == nil else { return }
                self.bindHandlers()
            }
        }
    }

    func disconnect() {
        releaseAllInputs()
        virtualController?.disconnect()
        virtualController = nil
        connectedElements.removeAll()
    }

    func releaseAllInputs() {
        if stickDown {
            let center = layout.stickCenter.guestPoint(guestSize: guestSize, orientation: orientation)
            onTouch?(.cancelled, center, VirtualControllerLayout.stickFingerIndex)
            stickDown = false
        }
        for button in buttonDown {
            let pt = layout.target(for: button).guestPoint(guestSize: guestSize, orientation: orientation)
            onTouch?(.cancelled, pt, button.fingerIndex)
        }
        buttonDown.removeAll()
        stickDeflection = .zero
    }

    private func bindHandlers() {
        guard let gamepad = virtualController?.controller?.extendedGamepad else { return }

        gamepad.leftThumbstick.valueChangedHandler = { [weak self] _, xValue, yValue in
            Task { @MainActor [weak self] in
                self?.handleStick(x: Double(xValue), y: Double(yValue))
            }
        }

        let map: [(GCControllerButtonInput, VirtualControllerButton)] = [
            (gamepad.buttonA, .a),
            (gamepad.buttonB, .b),
            (gamepad.buttonX, .x),
            (gamepad.buttonY, .y)
        ]

        for (input, button) in map where layout.isButtonEnabled(button) {
            input.pressedChangedHandler = { [weak self] _, _, pressed in
                Task { @MainActor [weak self] in
                    self?.handleButton(button, pressed: pressed)
                }
            }
        }
    }

    private func handleStick(x: Double, y: Double) {
        guard layout.enabled, inputEnabled else {
            if stickDown { releaseAllInputs() }
            return
        }
        stickDeflection = CGPoint(x: x, y: y)
        let magnitude = hypot(x, y)
        let deadzone = 0.08

        switch layout.thumbstickMode {
        case .tilt:
            if stickDown {
                let center = layout.stickCenter.guestPoint(guestSize: guestSize, orientation: orientation)
                onTouch?(.cancelled, center, VirtualControllerLayout.stickFingerIndex)
                stickDown = false
            }
            let scale = layout.tiltSensitivity
            let ax = magnitude > deadzone ? x * scale : 0
            let ay = magnitude > deadzone ? y * scale : 0
            let az = -sqrt(max(0.05, 1.0 - min(0.95, ax * ax + ay * ay)))
            let mapped = orientation.acceleration(x: ax, y: ay, z: az, host: orientation)
            onTilt?(mapped.0, mapped.1, mapped.2)

        case .touchStick:
            let upright = orientation.landscape
                ? CGSize(width: guestSize.height, height: guestSize.width)
                : guestSize
            let shortSide = max(1, min(upright.width, upright.height))
            let radiusU = (shortSide * layout.stickRadiusFraction) / max(1, upright.width)
            let radiusV = (shortSide * layout.stickRadiusFraction) / max(1, upright.height)
            let centerPoint = layout.stickCenter.guestPoint(guestSize: guestSize, orientation: orientation)

            if magnitude <= deadzone {
                if stickDown {
                    onTouch?(.ended, centerPoint, VirtualControllerLayout.stickFingerIndex)
                    stickDown = false
                }
                return
            }

            // Thumbstick +Y is up on screen, which is -v in top-down normalized coordinates.
            let targetUV = NormalizedTargetPoint(
                u: layout.stickCenter.u + x * radiusU,
                v: layout.stickCenter.v - y * radiusV
            )
            let guestPt = targetUV.guestPoint(guestSize: guestSize, orientation: orientation)
            if !stickDown {
                stickDown = true
                onTouch?(.began, centerPoint, VirtualControllerLayout.stickFingerIndex)
                onTouch?(.moved, guestPt, VirtualControllerLayout.stickFingerIndex)
            } else {
                onTouch?(.moved, guestPt, VirtualControllerLayout.stickFingerIndex)
            }
        }
    }

    private func handleButton(_ button: VirtualControllerButton, pressed: Bool) {
        guard layout.enabled, inputEnabled else { return }
        let pt = layout.target(for: button).guestPoint(guestSize: guestSize, orientation: orientation)
        if pressed {
            guard !buttonDown.contains(button) else { return }
            buttonDown.insert(button)
            onTouch?(.began, pt, button.fingerIndex)
        } else {
            guard buttonDown.contains(button) else { return }
            buttonDown.remove(button)
            onTouch?(.ended, pt, button.fingerIndex)
        }
    }
}

/// Interactive overlay shown when the player edits where virtual controller buttons
/// and the touch-stick center land on the guest screen.
struct VirtualControllerTargetEditorOverlay: View {
    @Binding var layout: VirtualControllerLayout
    let viewportRect: CGRect
    let onDone: () -> Void

    var body: some View {
        ZStack {
            if viewportRect.width > 0, viewportRect.height > 0 {
                // Touch-stick center marker when in Touch Stick mode
                if layout.thumbstickMode == .touchStick {
                    targetMarker(
                        label: "L-Stick",
                        systemImage: "l.joystick.fill",
                        color: .cyan,
                        point: layout.stickCenter
                    ) { updated in
                        layout.stickCenter = updated
                    }
                }

                // Face button target markers
                ForEach(layout.enabledButtons) { button in
                    targetMarker(
                        label: button.shortLabel,
                        systemImage: "circle.grid.cross.fill",
                        color: buttonColor(button),
                        point: layout.target(for: button)
                    ) { updated in
                        layout.setTarget(updated, for: button)
                    }
                }
            }

            VStack {
                HStack(spacing: 12) {
                    Image(systemName: "hand.draw.fill")
                        .foregroundStyle(.cyan)
                    Text("Drag A/B/X/Y and Stick targets onto the game's buttons")
                        .font(.caption.weight(.semibold))
                        .foregroundStyle(.white)
                    Button("Done") {
                        onDone()
                    }
                    .buttonStyle(.borderedProminent)
                    .tint(.blue)
                    .controlSize(.small)
                    .accessibilityIdentifier("virtual-controller-done-editing")
                }
                .padding(.horizontal, 14)
                .padding(.vertical, 8)
                .background(.ultraThinMaterial, in: Capsule())
                .overlay(Capsule().stroke(.white.opacity(0.25), lineWidth: 1))
                .padding(.top, 16)
                Spacer()
            }
        }
    }

    private func buttonColor(_ button: VirtualControllerButton) -> Color {
        switch button {
        case .a: .green
        case .b: .red
        case .x: .blue
        case .y: .orange
        }
    }

    private func targetMarker(
        label: String,
        systemImage: String,
        color: Color,
        point: NormalizedTargetPoint,
        onMove: @escaping (NormalizedTargetPoint) -> Void
    ) -> some View {
        let x = viewportRect.minX + CGFloat(point.u) * viewportRect.width
        let y = viewportRect.minY + CGFloat(point.v) * viewportRect.height

        return ZStack {
            Circle()
                .fill(color.opacity(0.32))
                .frame(width: 48, height: 48)
            Circle()
                .strokeBorder(color, lineWidth: 2)
                .frame(width: 48, height: 48)
            Text(label)
                .font(.system(size: 13, weight: .bold, design: .rounded))
                .foregroundStyle(.white)
                .shadow(color: .black, radius: 2, x: 0, y: 1)
        }
        .position(x: x, y: y)
        .gesture(
            DragGesture(minimumDistance: 0)
                .onChanged { value in
                    let u = Double((value.location.x - viewportRect.minX) / max(1, viewportRect.width))
                    let v = Double((value.location.y - viewportRect.minY) / max(1, viewportRect.height))
                    onMove(NormalizedTargetPoint(u: u, v: v))
                }
        )
        .accessibilityLabel("Target \(label)")
    }
}

/// Sheet in `GameDetailView` for configuring the per-game virtual controller layout before or between sessions.
struct VirtualControllerConfigSheet: View {
    let bundleID: String
    let gameTitle: String
    @Environment(\.dismiss) private var dismiss
    @State private var layout: VirtualControllerLayout

    init(bundleID: String, gameTitle: String) {
        self.bundleID = bundleID
        self.gameTitle = gameTitle
        _layout = State(initialValue: VirtualControllerLayout.load(bundleID: bundleID))
    }

    var body: some View {
        NavigationStack {
            ScrollView {
                VStack(spacing: 0) {
                    GlossSectionLabel("On-Screen Controller")
                    GlossCard {
                        GlossToggleRow(
                            title: "Enable virtual controller",
                            hint: "Shows Apple's GCVirtualController thumbstick and buttons over \(gameTitle).",
                            isOn: $layout.enabled
                        )
                    }

                    GlossSectionLabel("Left Thumbstick")
                    GlossCard {
                        VStack(alignment: .leading, spacing: 10) {
                            Text("Thumbstick mapping")
                                .font(.system(size: 15, weight: .semibold))
                                .foregroundStyle(Gloss.ink)
                            Picker("Thumbstick Mode", selection: $layout.thumbstickMode) {
                                ForEach(VirtualThumbstickMode.allCases) { mode in
                                    Text(mode.rawValue).tag(mode)
                                }
                            }
                            .pickerStyle(.segmented)

                            if layout.thumbstickMode == .touchStick {
                                Text("Drags a virtual finger around (\(Int(layout.stickCenter.u * 100))%, \(Int(layout.stickCenter.v * 100))%) on the game screen. You can drag the target directly over the game from the in-game controls.")
                                    .font(.system(size: 12))
                                    .foregroundStyle(Gloss.inkSoft)
                                HStack {
                                    Text("Stick Range").font(.system(size: 13, weight: .medium)).foregroundStyle(Gloss.ink)
                                    Slider(value: $layout.stickRadiusFraction, in: 0.06...0.28)
                                }
                            } else {
                                Text("Maps the left thumbstick to accelerometer tilt for racing and tilt-controlled games.")
                                    .font(.system(size: 12))
                                    .foregroundStyle(Gloss.inkSoft)
                                HStack {
                                    Text("Tilt Sensitivity").font(.system(size: 13, weight: .medium)).foregroundStyle(Gloss.ink)
                                    Slider(value: $layout.tiltSensitivity, in: 0.25...1.2)
                                }
                            }
                        }
                        .padding(14)
                    }

                    GlossSectionLabel("Buttons")
                    GlossCard {
                        ForEach(Array(VirtualControllerButton.allCases.enumerated()), id: \.element.id) { index, button in
                            if index > 0 { GlossRule() }
                            let target = layout.target(for: button)
                            GlossToggleRow(
                                title: button.rawValue,
                                hint: "Touches (\(Int(target.u * 100))%, \(Int(target.v * 100))%) on the game screen.",
                                isOn: Binding(
                                    get: { layout.isButtonEnabled(button) },
                                    set: { layout.setButton(button, enabled: $0) }
                                )
                            )
                        }
                    }

                    Button {
                        let enabled = layout.enabled
                        layout = VirtualControllerLayout(enabled: enabled)
                    } label: {
                        Text("Reset Layout to Defaults")
                    }
                    .buttonStyle(GlossButtonStyle(kind: .blue))
                    .padding(.horizontal, 16)
                    .padding(.top, 18)
                    .padding(.bottom, 28)
                }
            }
            .background(LinenBackground())
            .navigationTitle("Controller Layout")
            .navigationBarTitleDisplayMode(.inline)
            .toolbar {
                ToolbarItem(placement: .confirmationAction) {
                    Button("Done") {
                        layout.save(bundleID: bundleID)
                        dismiss()
                    }
                }
            }
            .onChange(of: layout) { _, updated in
                updated.save(bundleID: bundleID)
            }
        }
    }
}
