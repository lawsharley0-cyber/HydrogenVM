import Foundation

/// Whether the in-game controls (the FPS pill and the panel behind it) are drawn.
/// With the "Show debug overlay while playing" setting off, nothing of HydrogenVM is
/// on screen until the player holds three fingers down; that opens the same controls
/// once, and minimizing them hides them again.
struct PlayerOverlay {
    /// Three fingers held on the screen for this long. A game seldom holds three
    /// fingers still for most of a second, and the game still gets its touches.
    static let gestureTouches = 3
    static let gestureHoldSeconds = 0.8
    static let settingKey = "player.showDebugOverlay"

    var settingOn = false
    private(set) var summoned = false

    /// The pill is not drawn at all when this is false, so it cannot swallow touches.
    var visible: Bool { settingOn || summoned }
    /// The expanded controls show straight away after the gesture.
    var startsExpanded: Bool { summoned && !settingOn }

    mutating func summon() { summoned = true }
    /// Minimizing the controls puts the overlay away again when the setting is off.
    mutating func minimize() { summoned = false }
}
