# Clash Royale solo restoration investigation

Date: 2026-10-20. Work branch: `gemini-merge`.

This is a tested compatibility baseline and restoration design, not a playable
offline mod. No client bytes or runtime behavior were changed.

## Reproduce and review

- Tested commit: `0a447062cc1d049b4980ac70054514cfe3177e75`.
- [Completed four-app investigation](https://github.com/lawsharley0-cyber/HydrogenVM/actions/runs/39458108530).
- [Diagnostics and screenshots artifact](https://github.com/lawsharley0-cyber/HydrogenVM/actions/runs/39458108530/artifacts/12282535281).
- Workflow: `.github/workflows/clash-royale.yml`.
- Client preparation/inspection: `python3 tools/clash_royale_probe.py --out clash-report`.
- Compatibility runs: `tools/compat.py scan`, strict and exploratory modes,
  15 seconds per launch, strict relaunch enabled, one app at a time.
- The workflow asserts that all four apps were scanned. Fixtures are fetched
  from pinned main commit `ceba289b57cbda0e229e778966f195bcc1437fa0`.
- A green investigation job means the inspection/probes completed; it does not
  mean Clash Royale passed or is playable.

## Verified client

The archive's release notes identify this as iOS 1.0; the actual IPA metadata
calls both its version and build **2.1306.1**.

- Bundle ID: `com.supercell.scroll`.
- ARMv7, minimum iOS 7.0, GLES2 imports.
- Executable encryption check: unencrypted.
- IPA size: 90,361,183 bytes.
- SHA-256: `29269ede437f39ffa62fb4234a1c323303ef417e7eb6ddc5c86f9be76e08fb3f`.
- [Source release](https://github.com/astralsc/clashroyale_archive/releases/tag/Clients).
- The probe verifies the checksum before inspecting/running the app.

## Results

The host engine built successfully. `build-clash/hvm_tests fulltest`:
**28 passed, 0 failed**.

| App | Strict/relaunch probe | Rendering evidence |
|---|---|---|
| Clash Royale 2.1306.1 | Startup crash; same failure in exploratory mode | No frames |
| Angry Birds 1.0 | Runs; no runtime misses | Varied frame, 298 presented frames |
| Tiny Tower 1.0 | Runs; no runtime misses | 363 presented frames, but final frame entirely black |
| Art of Glow 1.1.2 | Runs; reports network unavailable | Varied frame, 264 presented frames |

These are short Linux host smoke probes, not end-to-end gameplay or device
performance results. The frame counts are the scanner's reported counters.
The Tiny Tower black frame is unresolved; this investigation made no runtime
change, so it is not evidence of a newly introduced regression.

## First Clash Royale blocker

The traced sequence near the end of
`-[AppController application:didFinishLaunchingWithOptions:]` is:

1. `NSSearchPathForDirectoriesInDomains` at caller return address `0x0016d2a1`.
2. `NSArray objectAtIndex:` succeeds, followed by `NSString UTF8String`.
3. A second `NSSearchPathForDirectoriesInDomains` call at `0x0016d2e9`.
4. `NSArray objectAtIndex:` at `0x0016d2f3` throws:
   `index 0 beyond bounds for empty array`.

Trace exit status: 70. Both strict and exploratory probes reproduce the same
launch crash. This is the first observed blocker, before a server-check failure
has been established.

The relevant emulator implementation is `searchPaths` in
`src/runtime/foundation/Bundle.cpp`. It returns an empty vector for unsupported
directory enum values or masks without `NSUserDomainMask`.

**Still needed:** capture the directory enum, domain mask and expansion flag
at the second call, then verify the appropriate iOS directory behavior.
The existing trace identifies the API and caller but does not print those
argument values. Do not guess the requested folder, suppress NSArray bounds
checks, or fabricate a successful server login to conceal this crash.

## Restoration material in the original client

The probe enumerated 54 CSV files. Relevant bundled tables include:

- `res/csv_logic/arenas.csv`.
- `npcs.csv`, `predefined_decks.csv`, `tutorials_npc.csv`,
  `tutorials_home.csv`.
- Character, building, spell, projectile, buff, and progression definitions.
- Arena location data under `res/locations/`, including training, goblin,
  bone, barbarian, spell, royal, dark and pvp layouts.

CSV row counts include headers/type rows, so they are not arena or NPC counts.
These files establish that restoration assets/configuration exist locally.
They do not establish that all simulation logic is present, callable offline,
or compatible with a particular community server.

## Network and rendering constraints

On the tested branch:

- `src/runtime/libc/Net.cpp` makes socket creation fail with Darwin
  `ENETDOWN`; other socket operations are unavailable.
- Foundation stream/URL-loading and reachability implementations also report
  offline behavior.
- The handoff documents a partial/fake GLES2 shader implementation. Starting
  the app does not establish correct rendering of this GLES2 title.

Changing the game's server address alone therefore cannot provide a working
local backend in the current runtime.

## Protocol references checked

- [RetroRoyale](https://github.com/retroroyale/ClashRoyale) targets 1.9,
  not this client.
- [CR-1.2.3](https://github.com/Magich03/CR-1.2.3) targets a nearer 2016 version
  and explicitly describes itself as incomplete.
- In that 1.2.3 source, `StartMissionMessage` responds with
  `NpcSectorStateMessage`, whose encoder contains a fixed compressed snapshot.
  This is a lead for investigating NPC battle bootstrapping, not a validated
  1.0 protocol implementation or complete bot simulation.

No external server code was copied into HydrogenVM.

## Implementation sequence

1. Fix the verified directory lookup compatibility gap after recording its
   actual arguments; add a focused runtime check and rerun this baseline.
2. Reach a correctly rendered loading/menu screen and inventory the next real
   blockers. Keep strict and exploratory results separate.
3. Locate this client's NPC/tutorial battle entry points, message dispatch,
   snapshot format, random seed/tick control and result handling. Confirm what
   the client simulates rather than inferring it from filenames.
4. Add an opt-in per-process local transport/backend. Integrate descriptor
   lifetime, read/write, close, readiness and Darwin errors consistently with
   the shared runtime. Route restoration traffic locally.
5. Restore a single complete NPC match with original combat logic where
   available; verify troop deployment, elixir, towers, win/loss and replayability.
6. Add local profile/deck/progression saves and a save/relaunch test.
7. Expand to each included arena, with original NPC logic or a measured bot
   controller. Do not claim all arenas until each one has a complete match test.
8. Package a restoration profile keyed to the exact client hash, with explicit
   prerequisites and reversible patches. Keep game-specific state separate
   from the generic emulator.

A useful first proof of HydrogenVM as a restoration platform is one reproducible,
fully playable offline match with local persistence. That milestone is not yet
implemented.
