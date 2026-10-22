# User-reported app compatibility and performance

Recorded October 21, 2026 (America/Chicago), directly from the project owner. This is a persistent investigation queue for future contributors and AI agents.

**Evidence level: user reported; not yet reproduced by the recording agent.** The precise emulator commit/build, host/device, OS, app versions and IPA hashes were not supplied with this list. Do not assume these observations came from current-main Linux, the iPhone host, or a particular historical fixture. Preserve the observations even if a different fixture or short CI smoke passes.

## App observations

| ID | App, as named by user | Observed behavior | Follow-up scenario |
|---|---|---|---|
| UAR-001 | Angry Birds | Crashes at the loading screen. | Capture load through transition into the next scene; compare the exact app version with the existing fixture. |
| UAR-002 | Goat Sim | Begins the initial load, then shows a black screen without a crash. | Inspect live frames, guest execution and scene transition. Do not classify this as a crash or a successful launch. |
| UAR-003 | Jetpack Joyride | Does not crash, but performance is poor and in-game music is absent. | Measure a real gameplay segment and inspect the music path separately from frame rate. |
| UAR-004 | Mega Jump | Fantastic performance; sound effects work, but music is absent. | Preserve this performance and working effects; investigate music independently. Useful positive regression control. |
| UAR-005 | Minecraft 1.1 | Black screen. | Identify the exact app/edition/version and inspect live rendering and guest progress. Do not conflate with UAR-006. |
| UAR-006 | Minecraft PE | Loads the menu, then crashes when loading a world. | Exercise world load; a menu screenshot is insufficient verification. |
| UAR-007 | Pocket Ants | Initially works well, then freezes after about 10 seconds and never recovers; no error is shown. | Run well beyond 10 seconds, track execution/frame/audio/input liveness and capture the hang without inventing a cause. |
| UAR-008 | Retry | Mostly loads, but performance is terrible, about 4–15 FPS. | Profile the same scene/input on matching builds; record frame-time percentiles and sustained FPS. |
| UAR-009 | Subway Surfers | Crashes on startup. | Capture the first failure, guest PC/backtrace and reached runtime API. |
| UAR-010 | Talking Larry | Crashes when something is clicked. | Reproduce the interaction, not only idle startup; record exactly which control/action triggers it. |
| UAR-011 | SurvivalCraft | Crashes on startup. | Capture the first failure, guest PC/backtrace and reached runtime API. |
| UAR-012 | Terraria | Crashes on startup. | Capture the first failure, guest PC/backtrace and reached runtime API. |
| UAR-013 | Tiny Tower | Runs well with everything working and decent frame rate. After closing the instance, the next startup crashes; another launch succeeds. | Preserve one sandbox across launch, close, failing relaunch and subsequent launch. Inspect shutdown and saved state; do not clear state as a workaround. |

Clash Royale is also a demanding performance workload from the ongoing restoration effort. Historical local tutorial measurements were about 2.8 FPS, but that old build is not a current-main benchmark. See the Clash continuation documents and original handoff for provenance. Broad app stability and performance now take priority alongside that restoration.

## Instructions for the next AI or contributor

1. Read this file together with current `AGENTS.md`, `STATUS.md`, `BACKLOG.md` and `PERFORMANCE.md`. Fetch the current integration branch and inspect active work before editing. These rows are unresolved reports, not proven shared root causes.
2. Separate startup crashes, later interaction/world-load crashes, black output, hangs, poor frame rate and missing music. Do not group them under a successful process exit or a single generic compatibility result.
3. Record the host/device and OS, host page size, emulator commit and dirty state, build configuration, app version/IPA hash, sandbox state, reproduction actions and duration. Keep proprietary assets, saves and tokens out of Git.
4. Reproduce on the reported platform/build when available. Use available fixtures meanwhile, but say when the exact failing app/version/device is unavailable. A passing different version, Linux run or three-second startup smoke does not resolve the owner's report.
5. Run scenario-appropriate checks: actual world load, user interaction, gameplay music, more than 10 seconds for Pocket Ants, and repeated same-sandbox launches for Tiny Tower. Read live frames before shutdown if teardown can clear the screenshot.
6. Profile measured hot paths before claiming a speed improvement. Compare matching scenes, inputs, host/build flags and timing; report frame percentiles and sustained behavior, not only CoreMark or loading-screen FPS. Keep guest simulation speed correct.
7. Distinguish missing music from missing effects, decoder failures, queue/loop problems and host speaker delivery through evidence. Mega Jump demonstrates that working effects and fast rendering do not establish working music. Do not assume the cause is M4A just because Clash needed that decoder.
8. Fix shared runtime behavior where evidence supports it. Preserve assertions, guest bounds, audio and rendering correctness; do not add title-based shortcuts, successful no-ops or modify original game binaries/assets. Keep working games such as Mega Jump as regression controls.
9. Register investigated work in the backlog under the repository's ownership rules. Link each fix and verification to the relevant UAR ID. Append reproduction/resolution evidence below; retain the original report. Only mark resolved after the reported scenario passes reproducibly.
10. Keep the broad crash/performance work separate from unfinished offline game progression. Do not label a game playable because its startup probe, benchmark or CI suite is green.

## Evidence and resolution log

No matching-build reproductions or resolutions were attached when this report was recorded. Future entries should include UAR ID, date, exact build/app/host, reproduction command/actions, artifact location, result, and fix commit/PR where applicable.
