import Foundation

/// Tracks the performance overlay and temporary three-finger summons.
/// RunView also provides an independent controls button (on by default), and
/// always exposes that entry while VoiceOver is enabled.
struct PlayerOverlay {
    /// Three fingers held on the screen for this long. A game seldom holds three
    /// fingers still for most of a second, and the game still gets its touches.
    static let gestureTouches = 3
    static let gestureHoldSeconds = 0.8
    static let settingKey = "player.showDebugOverlay"
    /// Performance metrics stay off by default; the controls button is separate.
    static let settingDefault = false

    var settingOn = settingDefault
    private(set) var summoned = false

    /// The pill is not drawn at all when this is false, so it cannot swallow touches.
    var visible: Bool { settingOn || summoned }
    /// The expanded controls show straight away after the gesture.
    var startsExpanded: Bool { summoned && !settingOn }

    mutating func summon() { summoned = true }
    /// Minimizing the controls puts the overlay away again when the setting is off.
    mutating func minimize() { summoned = false }
}
