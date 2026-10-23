import Foundation

/// User pause and host background are independent reasons to stay paused.
/// A foreground request waits for an outstanding background request to finish,
/// otherwise a fast Pause/Resume could leave a queued background callback parked.
struct PlayerActivity {
    enum Phase: String { case active, pausing, paused, resuming, stopped }
    enum Command { case background, foreground }
    var userPaused = false
    var hostBackgrounded = false
    private(set) var phase = Phase.active
    var wantsBackground: Bool { userPaused || hostBackgrounded }
    var acceptsInput: Bool { !wantsBackground && phase == .active }

    mutating func reconcile(ready: Bool, backgrounded: Bool, running: Bool) -> Command? {
        guard running else { phase = .stopped; return nil }
        guard ready else { return nil }
        if wantsBackground {
            if backgrounded { phase = .paused; return nil }
            if phase == .pausing { return nil }
            phase = .pausing
            return .background
        }
        if phase == .pausing && !backgrounded { return nil }
        if backgrounded {
            if phase == .resuming { return nil }
            phase = .resuming
            return .foreground
        }
        phase = .active
        return nil
    }
}
