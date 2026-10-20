# Clash Royale solo restoration investigation

Date: 2026-10-20. Work branch: `gemini-merge`.

## Local workstation tester

`tools/local_test.sh` and `tools/desktop.py` now provide a WSLg desktop window,
live frames, mouse touch/drag, background/foreground, screenshots, graceful
shutdown and persistent sandbox reuse. Windows launchers are in the task's
`outputs/` folder. See [`LOCAL_TESTING.md`](LOCAL_TESTING.md) for controls and paths.
The current local suite passes **57 checks**. Shutdown now distinguishes an
intentional Halt/ProcessExit from actual guest/HLE faults; the latter are still
reported. End-to-end viewer testing receives live frames, delivers touch down/up,
and exits cleanly. A Documents marker survives relaunch. This verifies host
sandbox retention, not offline avatar/home progression. The visible game limit
remains the connection-error dialog described below.

## Current status: Mach and custom-font startup support (2026-10-20)

Runtime commit: `095832c2c4362010d2da927412361b2b81b97ea2`.
The branch was fetched before editing and matched the supplied `b9c4701` head.
Local WSL/Linux Release build with Clang: **55 runtime checks passed, 0 failed**.
The checksum-pinned client is unchanged, including its bundled offline flag.

**Clash is not playable.** Strict startup now passes the original crash-reporter
and `DataLoaderFont::load` blockers, loads the logic tables and UI assets, and
continues until the probe time limit. The longer capture shows a visible UIKit
**Connection error / Try again** dialog over mostly black game output. This is
not a loading/menu screen or a working NPC match. The scanner's `runs` category
only means the timed launch continued; its frame-variation check can be satisfied
by this error dialog.

Implemented in the shared runtime:

- Per-process, synchronized synthetic Mach send-reference accounting for the
  task, a distinct host port, `mach_thread_self`, and `task_threads` exports.
  `mach_port_mod_refs`, `mach_port_get_refs`, and deallocation validate task,
  name, right, underflow and overflow. No receive rights/IPC were fabricated.
- `vm_read_overwrite` copies only mapped guest ranges with suitable access,
  rejects wrapping addresses, and reports the copied byte count. Guest page
  size/mask/shift data agree on 4 KiB pages. The next crash-reporter operation,
  `mach_make_memory_entry_64`, explicitly returns `KERN_NOT_SUPPORTED` and a null
  handle because there is no memory-object backend. The client recovers from
  that capability failure; this is not successful Mach memory-object support.
- CF allocator context copying, guest retain/release/allocation callbacks,
  thread defaults, and distinct null/system allocators. No-copy CFData retains
  the bytes' allocator and invokes its deallocator on destruction. Custom
  object-storage allocators, including `kCFAllocatorUseContext`, remain an
  explicitly reported unsupported case.
- Custom standalone TrueType fonts preserve provider bytes and supply their own
  cmap, names, metrics, traits, glyph bounds and outlines through CGFont/CTFont.
  The existing rasterizer draws custom outlines into guest bitmap contexts.
  Pinned, unmodified `stb_truetype` supplies decoding; the wrapper validates table,
  cmap and glyph ranges and rejects recursive/unsupported composites. This is
  not a claim of a fully hardened font parser or CFF/TTC support.
- A limited, uniform-attribute, horizontal CoreText typesetter/line/run path,
  including line breaking, owned run lifetimes and glyph extraction. Complex
  scripts, surrogate shaping, arbitrary multi-run text, frame layout and several
  other CoreText APIs remain unsupported. Font subpixel settings are tracked.
- CF mutable character-set creation/copy/range bridges. Fixed the existing
  `NSCharacterSet +new` ownership error (it incorrectly autoreleased its result),
  which otherwise caused a double release after the formatter's pool drain.
- `HVM_DUMP_SHADERS=<existing-or-new-directory>` writes original shader sources
  for diagnosis. The investigation workflow now triggers for all shared runtime
  and runner changes, rather than only the old directory fix.

### Current evidence and blockers

Local strict/exploratory/relaunch probes cover the pinned client plus the same
three older IPAs at fixture commit `ceba289b57cbda0e229e778966f195bcc1437fa0`.
Angry Birds and Art of Glow still produce varied output without new runtime API
failures; Tiny Tower's black frame remains its known baseline. Clash's reached
runtime miss is the unsupported audio format of `scroll_loading_01.wav`;
SCNetworkReachability continues to report offline by design. Static unreached
imports remain and must not be confused with reached failures.

The native game scene is still not rendered correctly. Captured vertex shaders
use `uniform vec4 myPMVMatrix` and
`gl_Position = myPMVMatrix * a_position + vec4(-1, 1, 0, 0)`. The current GLES2
heuristic only applies a selected `mat4`; it does not execute this component-wise
transform. Fragment shaders also use two color varyings, channel masks and
additive color expressions that the heuristic does not implement. Fix this with
shared shader semantics, not game-specific attribute-name guesses. The dump
contains nine vertex/fragment pairs, including a blur shader with varying arrays.

The client actively attempts ports 9339, 1863, 3724, 30000 and 843, then shows its
connection error despite `OfflineMode=true`. No `offline_avatar.json` or
`offline_home.json` was created in the tested fresh or reused homes. This
establishes that this launch has not initialized native offline play; it does
not establish why the flag is insufficient or that a replacement server is
required. A scripted `--tap 160,337@18` reached UIKit touch handling and was
followed by another error dialog. The delegate retry callback and in-game input
have not been independently verified.

GitHub [investigation run 39508176438](https://github.com/lawsharley0-cyber/HydrogenVM/actions/runs/39508176438)
and CI run 39508176627 did **not start any test steps**. Their check annotation
says account payments failed or the spending limit must be increased. Those
failed badges are infrastructure failures, not remote test results. All results
above are local; no green Actions result is claimed for this revision.

### Reproduce this revision locally

Use the commands in `.github/workflows/clash-royale.yml`: prepare and hash-check
the client with `tools/clash_royale_probe.py`, build `hvm`, `hvm_tests` and
`hvm_machodump`, run `hvm_tests fulltest`, then `tools/compat.py scan` with the
four IPAs, `--seconds 15 --jobs 1 --relaunch`. The local environment had
`llvm-objdump-18`; a PATH shim named `llvm-objdump` allowed the unchanged probe
to run. Builds and IPA/sandbox copies used Linux storage for consistent I/O.

For the longer diagnostic/input capture:

```sh
HVM_DUMP_SHADERS=clash-shaders build-clash/hvm run clash-report/client.ipa \
  --home clash-home --run-for 30 --tap 160,337@18 \
  --screenshot clash-error.png --report clash-run.json
```

Next: implement/validate the required GLES2 shader semantics; trace where the
client actually consumes the settings offline flag and enters its offline
state; verify profile creation, one NPC battle and save/relaunch before arena
expansion. Keep the executable/assets unchanged until specific evidence calls
for an explicit, hash-verified restoration profile.

## Previous verified state: directory and timezone fixes

Tested engine commit: `239a0ab818b2ddb3a7f146dbbef2cae98e7aba5c` on
`gemini-merge`. [Completed run](https://github.com/lawsharley0-cyber/HydrogenVM/actions/runs/39484502121)
and [diagnostics artifact](https://github.com/lawsharley0-cyber/HydrogenVM/actions/runs/39484502121/artifacts/12292748070).

**38 runtime checks passed, 0 failed. Clash Royale is not playable yet.**
The two original startup blockers are fixed in the shared runtime:

1. **Directory domains.** The pinned binary's instructions establish
   `NSSearchPathForDirectoriesInDomains(9, 0xFFFF, true)`, followed by
   `NSSearchPathForDirectoriesInDomains(13, 2, true)`. The latter means
   Caches in the local domain. Previously, every domain mask without the user
   bit returned an empty array.
   `Bundle.cpp` now supplies supported local-domain locations under
   `<app home>/LocalDomain/`, with user locations first for combined masks.
   There is no shared guest filesystem mounted; these paths deliberately use
   a separate per-app root rather than host `/Library`. Searches do not create
   directories. Six checks cover cache paths, ordering, tilde expansion,
   invalid/unmounted domains and NSFileManager URL parity.
2. **Fixed-offset time zones.** `Misc.cpp` implements
   `+[NSTimeZone timeZoneForSecondsFromGMT:]`, stable GMT names and offsets,
   date-independent offset queries and no DST for fixed zones. Explicit
   GMT/UTC names no longer resolve to the host's local zone. Four additional
   checks cover zero/positive/negative offsets, names, date independence and
   named UTC. This does not expand NSDateFormatter's existing support for
   arbitrary nonzero timezone formatting.

Clash Royale now reaches `GameMain::init` and its bundled HockeyApp/PLCrashReporter
startup. The **next strict blocker** is:
`mach_port_mod_refs` at return address `0x00149863` in
`+[BITPLCrashReporter initialize] + 0x62`. Recorded arguments:
task `0x103`, port `0x103`, right `0` (send), delta `1`.

The exploratory run advances to `DataLoaderFont::load` and an original
Debugger assertion. Reached gaps include `CFAllocatorCreate`,
`CGContextSetAllowsFontSubpixelPositioning`,
`CGContextSetShouldSubpixelQuantizeFonts`,
`CGFontCreateWithDataProvider`, and `CTFontCreateWithGraphicsFont`.
It also encounters crash-reporting helpers `backtrace_symbols_fd` and
`vm_read_overwrite`. These exploratory results do not establish the next
strict call order after the Mach API is fixed.

All three older-game probes still run without new runtime misses.
Angry Birds and Art of Glow produce varied frames; Tiny Tower's final frame
is still black, as in the initial baseline. These are short Linux probes,
not device or full-gameplay validation.

### Native offline mode discovered

Static inspection of the exact pinned IPA establishes:

- `res/settings.csv`: ServerURL `localhost`, port `9339`,
  **OfflineMode `true`**, placeholder asset/security URLs `http://asdf`.
- Binary references `../offline_avatar.json`, `../offline_home.json`,
  `GameMode::saveOffline(String const&)` and an offline selected-deck variable.
- The only bundled JSON assets are `res/fingerprint.json` (version
  `2.1306.1`) and `res/level/starting_home.json`.
- The starting-home template has decks, collection, selected-deck and chest
  state. The offline avatar/home save files are **not bundled**.

See [static diagnosis](https://github.com/lawsharley0-cyber/HydrogenVM/actions/runs/39483383480)
and `.github/workflows/clash-static.yml` for the reproducible inspection.

This describes the archived package, not verified pristine retail settings.
No one in this investigation changed its settings or patched its executable.
The flag and symbols are promising, but do not prove native offline mode can
initialize a profile, launch NPC battles, or save/reload successfully.

### Immediate continuation goals

**Historical list from the previous handoff.** The Mach and font startup work
below is now implemented to the extent described in the current section. The
current blockers are rendering and offline-state entry, not the original
`mach_port_mod_refs` miss or `DataLoaderFont::load` assertion.

1. Resolve the observed Mach send-right reference operation consistently with
   HydrogenVM's existing synthetic task/thread ports. Inspect
   `src/runtime/libc/System.cpp` and `Pthread.cpp`; do not register blanket
   successful no-ops for unimplemented IPC or disable array/assert checks.
   Alternatively, a deliberately opt-in, hash-verified restoration profile
   could disable optional crash reporting; that would be a game-specific
   change and must be tested and documented as such.
2. Re-run strict mode, then implement the actual allocator/font prerequisites
   as reached. `CGFontCreateWithDataProvider` currently reports a missing
   function and uses a system-font fallback, while CoreText support is largely
   absent. Replacing missing calls with zero is not a working font renderer.
3. Reach a rendered loading/menu screen, then test whether this client's
   **already-enabled native offline mode** creates the missing saves from its
   bundled starting template. Prefer this verified path over assuming that a
   replacement network backend is necessary.
4. Validate one NPC match and save/relaunch, then expand arenas. If the native
   offline path is insufficient, investigate the specific missing protocol or
   state handling before choosing an in-process backend.

The previous baseline and research follow for context.


The initial investigation below preceded the runtime fixes described above;
at that baseline, no client bytes or runtime behavior had been changed.

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

## Initial baseline results (before fixes)

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

## Initial directory blocker (now resolved)

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

**Resolved:** static disassembly identified the second call as `(13, 2, true)`.
The shared directory-domain implementation and its tests are described in the
current-status section above. NSArray bounds checks remain intact.

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

1. Directory lookup, fixed-offset timezone, synthetic Mach references and the
   reached custom-font startup path are fixed. See the current section for the
   deliberately unsupported Mach and CoreText operations.
2. Reach a correctly rendered loading/menu screen and inventory the next real
   blockers. Keep strict and exploratory results separate.
3. Locate this client's NPC/tutorial battle entry points, message dispatch,
   snapshot format, random seed/tick control and result handling. Confirm what
   the client simulates rather than inferring it from filenames.
4. If native offline mode is insufficient, add an opt-in per-process local
   transport/backend. Integrate descriptor
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
