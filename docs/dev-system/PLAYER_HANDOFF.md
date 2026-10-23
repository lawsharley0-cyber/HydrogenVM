# Player continuation after save overhaul

Branch `codex/player-viewport-hud` is based on `codex/file-save-overhaul`
(`56b2a72`, draft #18). Keep #18 as its PR base until the owner integrates it.
This continuation owns Group E plus focused harness/docs; it does not change
the shared C++ runtime or compete with B-109's Group D font work.

## Implemented

- Guest interface orientation is captured on the guest render thread with each
  delivered image. The host never reads mutable UIKit state on its main queue.
  Frame dimensions are captured with the image, avoiding mismatched metadata
  when another guest frame is dropped.
- The player requests the corresponding host scene orientation and undoes the
  guest's root-view rotation inside its portrait framebuffer. Both landscape
  directions and upside-down portrait are represented. Rotation request errors
  are visible in player controls. This is guest-driven orientation; it does not
  synthesize guest device-orientation notifications from physical rotation.
- Aspect Fit, Integer Pixels and Stretch; Bilinear and Nearest Neighbor.
  Integer mode uses whole framebuffer-pixel multiples on the host's physical
  screen, falling back to aspect fit when 1x cannot fit without cropping.
- Drawing and touch mapping share one geometry model. New touches in letterbox
  bars are ignored; accepted drags clamp to guest edges. Finger IDs persist
  through a gesture. Layout/orientation changes, pause and host background
  cancel active touches. Unknown moved/ended touches cannot create fingers.
- CoreMotion's device-fixed acceleration is transformed only by the difference
  between the actual host scene and guest orientation. Matching orientations
  preserve the raw axes; applying another guest rotation there would be wrong.
- Collapsible controls expose lifecycle Pause/Resume, output volume/mute,
  existing log access, FPS and lifetime frame-hook work p95 (not display latency).
  User pause survives host background/foreground. A fast Resume waits until the
  outstanding background request acknowledges, preventing a queued pause race.
- Save, Stop & Inspect opens the existing browser only after the session's exit
  callback. Its existing editor and snapshot manager are then available.

## Acceptance and limits

Apple model tests, synthetic player UI tests, all existing editor UI tests and
the actual unsigned simulator shell build must pass on the exact source SHA.
Current runs are pending until evidence is appended below. The harness compiles
the actual RunView/RunModel/viewport and substitutes only the emulator bridge.
Its colored frames and lifecycle delays prove host behavior, not real gameplay.

PR #18 has no submitted reviews or inline review threads at this continuation's
initial inspection. Independent review and its signed-device scenarios remain
open. No physical iPhone or signed guest session was available here.

Two roadmap requirements remain deliberately open:

1. **Runtime-driven relayout:** source inspection found that
   `app_setStatusBarOrientation` calls `setRootViewController:` with the existing
   controller, while `w_setRoot` returns early for that same controller. This
   may leave the guest root transform stale after an explicit orientation
   change. Reproduce and fix under one Group D owner, with native expected
   geometry and runtime regression coverage. The host reflects delivered guest
   orientation; the synthetic host test does not close this runtime issue.
2. **Live snapshot/restore and file editing while paused:** `backgrounded()` is
   set before all worker suspensions finish, and is not a filesystem barrier.
   Defaults/stdio flushing exists, but arbitrary SQLite connections, guest
   background writers and in-memory save caches need an explicit contract.
   Never expose live replacement based merely on that Boolean. Implement an
   acknowledged quiescence/flush API and safe restart/cache invalidation first.

## Next owner/device checks

1. Review and integrate #18, then retarget this stacked PR to main.
2. On a signed iPhone, run portrait and both landscape games. Check safe areas,
   corner/center touches, simultaneous fingers, rotated held touches, controls
   expanded/collapsed, all scaling/filter modes and actual tilt behavior.
3. Pause/resume repeatedly; background while paused and while starting; return
   without unexpectedly resuming. Stop while paused and verify preferences and
   game saves survive same-sandbox relaunch. Test slow/hung lifecycle handlers.
4. Inspect only after exit. Create/restore a snapshot, then relaunch from Library
   and verify the guest consumes that revision. No live restore is claimed.
5. Coordinate the two runtime requirements above with Group D/A ownership.
   B-109 font work, Group B shader PRs #11/#12 and Clash #13 stay separate.
