# BADLAND 1.0 — Linux bench investigation

Owner-selected first app, November 12, 2026. Tracking: [#423](https://github.com/lawsharley0-cyber/HydrogenVM/issues/423).
**In progress: Linux core playability reproduced; frame-time qualification remains open.**
Menus, controlled flight, an earned level completion, music/audio capture,
four-minute clean runs and saved progression across relaunch are reproduced.
One timing run without live frame export has p99 below 33 ms, but the final
post-review run did not reproduce that tail latency. Outliers, capture costs
and physical-device limits are reported separately below.

## Fixture and reproduction

- BADLAND 1.0, build 8; `com.frogmindgames.dodge`; ARMv7s slice, unencrypted.
- IPA SHA-256: `bd67dbc3af4c67190f5b87887773535619343c922101807e08df8d616c3866c1`.
- Private extraction: `~/bench/apps/BADLAND_1.0/Payload/BADLAND.app`.
- Ubuntu/WSL x86-64, 4,096-byte host pages, Clang 18 Release, Mesa EGL GLES.
- Initial reproduction was on `main` e1825f58. Work includes main through
  5f33889d, retaining the newer audio/image-scaling, internet-control and CPU changes.

```sh
build/hvm run "$HOME/bench/apps/BADLAND_1.0/Payload/BADLAND.app" \
  --gpu --home "$HOME/bench/homes/badland-repro-fresh" \
  --run-for 200 --graceful --tap 116,284@2 \
  --report build/badland.json --screenshot build/badland.png
```

Use a new home for a fresh launch; reuse that exact home for relaunch checks.
The tap dismisses the original Everyplay SDK expiration alert. The game binary,
clock, assets, saves and assertions have not been patched. The CLI input-file
bridge also drove holds at `(160,284)`, pause at `(310,8)` and resume at `(85,435)`.
Positions are portrait framebuffer points, before the root controller's rotation.
Recorded WAVs, frame sequences, contact sheets, logs and saves remain private
under `~/bench/runs/` and `~/bench/homes/`, outside the repository.

## Shared changes and fail-before coverage

| Observed failure | Shared correction | Runner / Test Lab |
|---|---|---|
| Crashlytics calls missing NSString range-to-byte conversion | Partial UTF-8/UTF-16/UTF-32/ASCII/Latin-1 conversion, UTF-16 ranges, sizing queries, external marks, bounds and byte limits | `fulltest` byte-conversion checks; TL-AA |
| Everyplay GL-view wrapper calls missing CGRect dictionary creation | Owned rectangle dictionaries and checked reconstruction, preserving signed fractional fields | `hvm_geometry_dict_tests`; TL-AB |
| Cocos2d prepends global declarations before an extension directive | Conservative GLES source translation shared by software and host backends; strict compiler validation is retained | `hvm_shader_source_tests`; TL-AC |
| First window appearance leaves scene/configuration data uninitialized | Synchronous initial layout between appearance callbacks | `hvm_window_layout_tests`; TL-AD |
| A landscape navigation controller retains a portrait-size custom GL child | Navigation layout resizes managed content even without a flexible autoresizing mask | `hvm_window_layout_tests`; TL-AD |
| Offline score cache throws during keyed archiving | Local GKScore category/date/value/context storage and NSCoding; network requests still fail as unauthenticated | `hvm_gamekit_coding_tests`; TL-AE |
| CocosBuilder struct-valued properties silently do nothing | Box/unbox NSValue structs through method signatures and NSInvocation | `hvm_kvc_struct_tests`; TL-AF |
| Graceful stop fails in Everyplay pauseRecording | Rational CMTime addition/subtraction with documented scale, rounding, overflow, epoch and nonnumeric rules | `hvm_media_time_tests`; TL-AG |
| Landscape readback spends ~44 ms copying a rotated opaque image | Preserve opacity metadata and copy pixel-aligned quarter-turns without per-pixel floating-point blending | `hvm_raster_rotation_tests` (including local benchmark) |
| Rotated solid backgrounds still cost ~9 ms in the general path rasterizer | Exact discrete UIKit interface rotations, and rectangle filling for quarter-turned solid backgrounds and host-layer holes | `hvm_raster_rotation_tests --bench`: normal root-controller layout, full-frame pixel check and local compositor budget |

Each runner regression was demonstrated failing before its corresponding fix.
The compiler's existing strict extension tests and all baselines remain intact.
Conditional, commented and in-function extension directives are not hoisted.
The fast raster path excludes fractional transforms, masks, tint, transparency
and other blend modes; those retain their existing paths.
Arbitrary-angle backgrounds and rounded corners retain path rendering. The solid
background benchmark uses normal UIWindow/root-controller layout, rather than a
hand-authored exact matrix: the latter initially missed the floating-point
cosine residual in UIKit's orientation transform. With that real layout the
bench failed at **8.75 ms** before the fix and passed at **0.93 ms** afterward
(median, 640x1136, local 5 ms budget). Timing budgets are opt-in bench checks;
pixel mapping is checked by the default runner as well.

Reference contracts: [NSString byte conversion](https://developer.apple.com/documentation/foundation/nsstring/getbytes(_:maxlength:usedlength:encoding:options:range:remaining:)),
[CoreMedia addition](https://developer.apple.com/documentation/coremedia/cmtimeadd(_:_:).md),
[CoreMedia subtraction](https://developer.apple.com/documentation/coremedia/cmtimesubtract(_:_:).md).
GKScore archive roundtrips are qualified for this implementation; compatibility
with an archive produced by Apple's private GameKit coder has not been established.

## Review hardening

Read-only output tests failed before the CGRect/NSString checked-write changes.
The rectangle API now fails without writing an invalid destination; NSString
uses the existing controlled guest memory-fault convention for invalid output
spans, including its byte count and remaining range. Rectangle tests also cover
the arena-end boundary and an unmapped span.

KVC errors and NSString range errors now reach guest NSException handlers.
TL-AF and TL-AA were interrupted before these corrections and pass afterward.
Reentrant disappearance during appearance/layout no longer receives a stale
didAppear callback (runner and TL-AD failed before). CMTime only carries rounding
from its operands and accepted result, not a discarded overflowing scale
(runner and TL-AG failed before). TL-AE now independently checks both objects
against the authored 64-bit constants.

The ASCII review identified a real literal-0xff output error, but replacing
every accented character with '?' would not reproduce Foundation's fallback.
Apple's [CF encoding implementation](https://github.com/apple-oss-distributions/CF/blob/main/CFStringEncodings.c)
and [NSString bridge](https://github.com/swiftlang/swift-corelibs-foundation/blob/main/Sources/Foundation/NSString.swift)
use 0xff as a control value for ASCII fallback, not an output byte. The accented
character regression expects `é -> e`. Apple builds use native CF; Linux uses
Unicode compatibility decomposition and '?' for other unsupported scalars.
This does not establish complete parity with Apple's legacy fallback tables.

## Gate results (November 13)

- `hvm_tests fulltest`: **795 passed, 0 failed**.
- Support registrations: **47 passed, 0 failed**.
- Seven focused runner executables: **all passed**.
- Test Lab: **33 passed, 0 failed/interrupted/skipped**.
- Python tools: **398 passed, 5 skipped**, 716 subtests passed.
- Stub audit against the unchanged baseline: **0 new/changed** findings.
- Full compatibility harness, including assembly/C guests and required private
  IPA fixtures: **summary.ok=true; zero baseline regressions**, no app skips.
- Must-launch: **13 passed, 0 failed, 21 skipped**. BADLAND was added with
  `--only BADLAND_1.0 --seconds 60 --record` (801-frame expectation, picture and
  sound required), then passed a separate check. All 33 prior entries are
  unchanged. The 21 missing fixtures are skips, not passes.

## Real-game evidence

All homes are private and separate from the extracted bundle. The scripted
fresh-home flight sequence dismisses the alert at 2 seconds, then holds
`(160,284)` for 1.2 seconds every 3 seconds, starting at 30 seconds (66 holds).
This drives actual guest input, physics, collisions, deaths and respawns.

| Private run | Duration / outcome | Frames | Average / p50 / p95 / p99 ms |
|---|---|---|---|
| `badland-qualified-fresh` (live frames + WAV + contact sheet) | 240 s, clean save/close, exit 0 | 7,935 | 28.32 / 25.17 / 35.13 / 142.61 |
| `badland-timing-fresh` (same scripted input, WAV, no live frame export) | 240 s, clean save/close, exit 0 | 9,892 | 22.48 / 22.54 / 27.00 / 30.41 |
| `badland-review-final-fresh` (post-review fixes, WAV, no live frame export) | 241 s, clean save/close, exit 0; earned completion with three clones | 8,878 | 26.40 / 20.45 / 33.55 / 285.21 |

These percentiles include startup; they do not claim every frame is under 33 ms.
The timing run's maximum was 1,219 ms during the complete launch/session.
Live-export numbers do not meet the same tail-latency target. Physical-device
speed is a separate qualification. The final run had no runtime errors or audio
transport drops, but its tail latency means the frame-time requirement remains
open. The faster earlier run alone does not qualify every gameplay scenario.

- Captured scenes show correctly sized landscape graphics, flying characters,
  scrolling/animated scenery, collision/death/respawn and working pause/resume.
- `badland-qualified-fresh` earned a level completion. Relaunching that exact
  home and opening Statistics showed **1/40 levels survived, one clone saved,
  50 screen touches and 1m46s flying time**. The original completed-level save
  entry remained unchanged across the clean second shutdown. No save was edited.
- WAV capture produced non-silent stereo PCM with zero dropped blocks/transport
  errors. Independent waveform comparisons against the supplied PCM16 banks
  found intro/menu music matches (~0.73/~0.75 normalized correlation) and a
  partial flight-effect match (~0.42 in the mixed output). These support playback
  of supplied music/effects; they are not a perceptual audit of every sound.
  The game exposes one Sounds slider; moving it and returning to Statistics
  worked. Background music remained audible at its minimum, so this is not
  claimed as a master-mute check. The mixer classifies FMOD's combined output as
  music; its zero separate-effect-voice count is not evidence of missing effects.
- Local 1136x640 opaque quarter-turn microbenchmark: **44.39 -> 2.63 ms** median.
  This is one compositor operation, not a whole-game FPS claim.

The requested `docs/IPA_PLAYABILITY_LOOP.md` was absent from main, agent/offline
and searched local checkouts. The explicit owner-supplied playability procedure
is being followed; the missing guide has not been fabricated.

Physical-phone limits remain GPU-driver behavior, microphone and headphone
routing, 16 KB memory pages and real device speed. No phone results are requested
or inferred from this bench.
