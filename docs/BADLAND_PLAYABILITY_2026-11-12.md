# BADLAND 1.0 — Linux bench investigation

Owner-selected first app, November 12, 2026. Tracking: [#423](https://github.com/lawsharley0-cyber/HydrogenVM/issues/423).
**In progress, not yet qualified as playable.** Main menu, intro, scene rendering,
music signal, pause/resume and a 200-second clean run are reproduced. Sustained
gameplay/progression and the <33 ms whole-frame target remain under investigation.

## Fixture and reproduction

- BADLAND 1.0, build 8; `com.frogmindgames.dodge`; ARMv7s slice, unencrypted.
- IPA SHA-256: `bd67dbc3af4c67190f5b87887773535619343c922101807e08df8d616c3866c1`.
- Private extraction: `~/bench/apps/BADLAND_1.0/Payload/BADLAND.app`.
- Ubuntu/WSL x86-64, 4,096-byte host pages, Clang 18 Release, Mesa EGL GLES.
- Initial reproduction was on `main` e1825f58. Work has been rebased onto
  fa5db02f, retaining the newer audio/image-scaling and internet-control changes.

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

Each runner regression was demonstrated failing before its corresponding fix.
The compiler's existing strict extension tests and all baselines remain intact.
Conditional, commented and in-function extension directives are not hoisted.
The fast raster path excludes fractional transforms, masks, tint, transparency
and other blend modes; those retain their existing paths.

Reference contracts: [NSString byte conversion](https://developer.apple.com/documentation/foundation/nsstring/getbytes(_:maxlength:usedlength:encoding:options:range:remaining:)),
[CoreMedia addition](https://developer.apple.com/documentation/coremedia/cmtimeadd(_:_:).md),
[CoreMedia subtraction](https://developer.apple.com/documentation/coremedia/cmtimesubtract(_:_:).md).
GKScore archive roundtrips are qualified for this implementation; compatibility
with an archive produced by Apple's private GameKit coder has not been established.

## Current evidence and remaining gates

- `hvm_tests fulltest`: **790 passed, 0 failed**.
- Support registrations: **47 passed, 0 failed**.
- Seven focused runner executables: **all passed**.
- Test Lab: **33 passed, 0 failed/interrupted/skipped**.
- Python tools: **398 passed, 5 skipped**, 716 subtests passed.
- Stub audit against the unchanged baseline: **0 new/changed** findings.
- 200-second fresh-home session: clean save/close, exit 0, 4,921 presented frames,
  music signal and pause/resume. Median whole-frame **35.13 ms**, p95 **61.34 ms**;
  this includes startup, live capture and time in the pause menu. It does **not**
  meet the <33 ms playability target.
- Local 1136x640 opaque quarter-turn microbenchmark: **44.39 -> 2.63 ms** median.
  This is one compositor operation, not a whole-game FPS claim.
- Relaunch uses the original saved home and reaches the main menu. Actual earned
  level progression and exact audio-effect correctness still need qualification.
- Must-launch is running against available private controls; only **12 of 33**
  listed fixtures are present. Missing fixtures are skips, not passes.

The requested `docs/IPA_PLAYABILITY_LOOP.md` was absent from main, agent/offline
and searched local checkouts. The explicit owner-supplied playability procedure
is being followed; the missing guide has not been fabricated.

Physical-phone limits remain GPU-driver behavior, microphone and headphone
routing, 16 KB memory pages and real device speed. No phone results are requested
or inferred from this bench.
