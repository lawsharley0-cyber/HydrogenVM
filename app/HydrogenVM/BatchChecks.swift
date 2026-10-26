import Foundation

/// One line of the Test All Games checklist: everything we look at when testing an IPA by hand.
struct BatchCheck: Codable, Sendable, Identifiable, Hashable {
    enum Status: String, Codable, Sendable { case pass, warn, fail, skip }
    var id: String { name }
    var name: String
    var status: Status
    var detail: String

    var icon: String {
        switch status {
        case .pass: return "checkmark.circle.fill"
        case .warn: return "exclamationmark.triangle.fill"
        case .fail: return "xmark.octagon.fill"
        case .skip: return "minus.circle"
        }
    }
    var emoji: String {
        switch status {
        case .pass: return "✅"
        case .warn: return "⚠️"
        case .fail: return "❌"
        case .skip: return "➖"
        }
    }
}

/// A snapshot of the engine's counters (HVMSession.diagnostics). The counters belong to the
/// running game, so the runner copies them every second and keeps the last one.
struct BatchDiagnostics: Codable, Sendable, Equatable {
    var audioFramesRendered: Int = 0
    var audibleFrames: Int = 0
    var musicFrames: Int = 0
    var effectFrames: Int = 0
    var musicVoices: Int = 0
    var effectVoices: Int = 0
    var audioPeak: Double = 0
    var offscreenPresents: Int = 0
    var faultsToApp: Int = 0
    var missingCount: Int = 0
    var framesPresented: Int = 0
    var timeToFirstFrameMs: Double = -1

    init() {}
    init?(_ d: [String: NSNumber]) {
        guard !d.isEmpty else { return nil }
        func i(_ k: String) -> Int { d[k]?.intValue ?? 0 }
        audioFramesRendered = i("audioFramesRendered")
        audibleFrames = i("audibleFrames")
        musicFrames = i("musicFrames")
        effectFrames = i("effectFrames")
        musicVoices = i("musicVoices")
        effectVoices = i("effectVoices")
        audioPeak = d["audioPeak"]?.doubleValue ?? 0
        offscreenPresents = i("offscreenPresents")
        faultsToApp = i("faultsToApp")
        missingCount = i("missingCount")
        framesPresented = i("framesPresented")
        timeToFirstFrameMs = d["timeToFirstFrameMs"]?.doubleValue ?? -1
    }

    /// Mixer output runs at 44.1 kHz.
    static func seconds(_ frames: Int) -> String { String(format: "%.1f s", Double(frames) / 44_100.0) }
}

/// What the runner saw for one game, fed to `BatchCheckBuilder`.
struct BatchObservations: Sendable {
    var crashed = false
    var crashSummary: String?
    var frameSignal = "no-frames"
    var fps = 0
    var launchDiag: BatchDiagnostics?      // at the end of the launch wait
    var finalDiag: BatchDiagnostics?       // last snapshot before the game stopped
    var exploreEnabled = false
    var exploreCrashed = false
    var screensVisited = 0
    var resumeFramesBefore: Int?
    var resumeFramesAfter: Int?
    var backgroundTested = false
    var closedGracefully: Bool?            // nil: the game had already exited
    var engineTeardownTimedOut = false
    var exitedOnItsOwn = false
    var relaunchEnabled = false
    var relaunchCrashed: Bool?
    var relaunchFrames: Int?
    var relaunchSummary: String?
    var missingLines: [String] = []
    var audioDecodeFailures: [String] = []
}

enum BatchCheckBuilder {
    static func build(_ o: BatchObservations) -> [BatchCheck] {
        var c: [BatchCheck] = []
        let d = o.finalDiag ?? BatchDiagnostics()

        // 1. Launch
        if o.crashed {
            c.append(.init(name: "Launches", status: .fail, detail: o.crashSummary ?? "The game crashed"))
        } else if o.exitedOnItsOwn {
            c.append(.init(name: "Launches", status: .warn, detail: "The game quit by itself during the test"))
        } else {
            var detail = "Ran without crashing"
            if d.timeToFirstFrameMs >= 0 { detail += String(format: " · first frame after %.1f s", d.timeToFirstFrameMs / 1000) }
            c.append(.init(name: "Launches", status: .pass, detail: detail))
        }

        // 2. Picture
        switch o.frameSignal {
        case "ok": c.append(.init(name: "Shows a picture", status: .pass, detail: "\(d.framesPresented) frames drawn"))
        case "no-frames": c.append(.init(name: "Shows a picture", status: .fail, detail: "Nothing was ever drawn"))
        default: c.append(.init(name: "Shows a picture", status: .fail, detail: "The screen stayed \(o.frameSignal)"))
        }

        // 3. Game view on screen: GL frames drawn into a view that isn't in the window.
        if o.finalDiag == nil {
            c.append(.init(name: "Game view on screen", status: .skip, detail: "No engine counters (the game stopped early)"))
        } else if d.offscreenPresents > 0 {
            c.append(.init(name: "Game view on screen", status: .fail, detail: "\(d.offscreenPresents) frames went to a view that isn't in the window"))
        } else {
            c.append(.init(name: "Game view on screen", status: .pass, detail: "Every frame reached the window"))
        }

        // 4. Audio output
        if o.finalDiag == nil {
            c.append(.init(name: "Audio output", status: .skip, detail: "No engine counters"))
        } else if d.audioFramesRendered == 0 {
            c.append(.init(name: "Audio output", status: .fail, detail: "The sound mixer never ran"))
        } else {
            c.append(.init(name: "Audio output", status: .pass, detail: "Mixer ran for \(BatchDiagnostics.seconds(d.audioFramesRendered)), peak \(String(format: "%.2f", d.audioPeak))"))
        }

        // 5. Music
        if o.finalDiag == nil {
            c.append(.init(name: "Music", status: .skip, detail: "No engine counters"))
        } else if d.musicFrames > 0 {
            c.append(.init(name: "Music", status: .pass, detail: "Heard \(BatchDiagnostics.seconds(d.musicFrames)) of music (\(d.musicVoices) track\(d.musicVoices == 1 ? "" : "s"))"))
        } else if let bad = o.audioDecodeFailures.first {
            c.append(.init(name: "Music", status: .fail, detail: "No music: a sound file couldn't be decoded (\(bad))"))
        } else {
            c.append(.init(name: "Music", status: .warn, detail: "No music heard. It may be off in the game's settings, or this screen has none"))
        }

        // 6. Sound effects
        if o.finalDiag == nil {
            c.append(.init(name: "Sound effects", status: .skip, detail: "No engine counters"))
        } else if d.effectFrames > 0 {
            let afterTaps = d.effectFrames - (o.launchDiag?.effectFrames ?? 0)
            var detail = "Heard \(d.effectVoices) effect\(d.effectVoices == 1 ? "" : "s") (\(BatchDiagnostics.seconds(d.effectFrames)))"
            if o.exploreEnabled && afterTaps > 0 { detail += ", some after taps" }
            c.append(.init(name: "Sound effects", status: .pass, detail: detail))
        } else if let bad = o.audioDecodeFailures.first {
            c.append(.init(name: "Sound effects", status: .fail, detail: "No effects: a sound file couldn't be decoded (\(bad))"))
        } else {
            c.append(.init(name: "Sound effects", status: .warn, detail: o.exploreEnabled ? "No sound effects, even after taps" : "No sound effects (turn on Explore to tap around)"))
        }

        // 7. Touch
        if !o.exploreEnabled {
            c.append(.init(name: "Responds to touch", status: .skip, detail: "Explore is off"))
        } else if o.exploreCrashed {
            c.append(.init(name: "Responds to touch", status: .fail, detail: "Crashed while tapping around"))
        } else if o.screensVisited >= 2 {
            c.append(.init(name: "Responds to touch", status: .pass, detail: "Reached \(o.screensVisited) different screens"))
        } else {
            c.append(.init(name: "Responds to touch", status: .warn, detail: "The picture never changed after taps"))
        }

        // 8. Background / resume
        if !o.backgroundTested {
            c.append(.init(name: "Background & resume", status: .skip, detail: o.exploreEnabled ? "The game stopped before this step" : "Explore is off"))
        } else if o.exploreCrashed && o.resumeFramesAfter == nil {
            c.append(.init(name: "Background & resume", status: .fail, detail: "Crashed going to the background"))
        } else if let a = o.resumeFramesAfter, let b = o.resumeFramesBefore, a > b {
            c.append(.init(name: "Background & resume", status: .pass, detail: "Drew \(a - b) frames after coming back"))
        } else {
            c.append(.init(name: "Background & resume", status: .warn, detail: "No new frames after coming back (may be paused on a static screen)"))
        }

        // 9. Close
        if o.engineTeardownTimedOut {
            c.append(.init(name: "Closes cleanly", status: .fail, detail: "The engine stayed alive after the forced stop; the batch stopped to protect later games"))
        } else {
            switch o.closedGracefully {
            case .some(true): c.append(.init(name: "Closes cleanly", status: .pass, detail: "Quit within 4 s when asked"))
            case .some(false): c.append(.init(name: "Closes cleanly", status: .warn, detail: "Ignored the quit request and had to be stopped"))
            case .none: c.append(.init(name: "Closes cleanly", status: .skip, detail: "The game had already stopped"))
            }
        }

        // 10. Relaunch with the saved data from the first run
        if !o.relaunchEnabled {
            c.append(.init(name: "Relaunch", status: .skip, detail: "Relaunch is off"))
        } else if let crashed = o.relaunchCrashed {
            if crashed {
                c.append(.init(name: "Relaunch", status: .fail, detail: o.relaunchSummary ?? "Crashed on the second launch"))
            } else if (o.relaunchFrames ?? 0) > 0 {
                c.append(.init(name: "Relaunch", status: .pass, detail: "Started again with its saved data (\(o.relaunchFrames ?? 0) frames)"))
            } else {
                c.append(.init(name: "Relaunch", status: .warn, detail: "Started again but drew nothing"))
            }
        } else {
            c.append(.init(name: "Relaunch", status: .skip, detail: "Skipped because the first run didn't finish cleanly"))
        }

        // 11. Missing APIs
        let missing = max(d.missingCount, o.missingLines.count)
        if missing == 0 {
            c.append(.init(name: "Missing APIs", status: .pass, detail: "None reported"))
        } else {
            let names = o.missingLines.prefix(3).joined(separator: "; ")
            c.append(.init(name: "Missing APIs", status: .warn, detail: "\(missing) reported" + (names.isEmpty ? "" : ": \(names)")))
        }

        // 12. Faults the game handled itself (its own crash handlers)
        if d.faultsToApp > 0 {
            c.append(.init(name: "Handled faults", status: .warn, detail: "\(d.faultsToApp) memory fault\(d.faultsToApp == 1 ? "" : "s") passed to the game's own handler"))
        } else if o.finalDiag != nil {
            c.append(.init(name: "Handled faults", status: .pass, detail: "None"))
        }

        // 13. Speed
        if o.crashed || o.frameSignal == "no-frames" {
            c.append(.init(name: "Speed", status: .skip, detail: "No frames to time"))
        } else if o.fps >= 20 {
            c.append(.init(name: "Speed", status: .pass, detail: "\(o.fps) fps"))
        } else if o.fps >= 10 {
            c.append(.init(name: "Speed", status: .warn, detail: "\(o.fps) fps (slow)"))
        } else {
            c.append(.init(name: "Speed", status: .warn, detail: "\(o.fps) fps (very slow, or a static screen)"))
        }
        return c
    }
}
