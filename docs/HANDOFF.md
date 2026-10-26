# HydrogenVM — handoff (2026-10-20)

## Current priority: guest-owned standalone mod

Latest audit: seven earned trainers, King 2, nine cards, native name/deck swaps
and two real defeats without grants now persist. v27 repairs the saved arena
refresh using the original native routine; the actual earned save graduates
to Arena 1. Main 5968d88 is integrated at 5fbaa8d; fulltest 256/256, TestLab
17/17, complete compatibility gate and full report pass locally. Read
[UI_AUDIT.md](clash-royale/UI_AUDIT.md) for fresh/returning runs, audio captures,
Apple simulator warnings, hashes and exact remaining blockers. Arena matches,
economy/timers, unlocked controls and phone validation remain unfinished.

October 25 UI/crash continuation: read [UI_AUDIT.md](clash-royale/UI_AUDIT.md)
and the latest section in [GUEST_MOD_PROGRESS.md](clash-royale/GUEST_MOD_PROGRESS.md).
The phone's v19 home abort identifies unported native command 516, now handled
without bypassing original execution. v23 also fixes local profile and supplies
offline help/settings notices. Three real upgrades in a private earned-player
copy pass; repeated final-package launches, later training and device retests
remain required. Shared regex/locale fixes pass 253 runtime and 17 TestLab
checks on Linux; Apple adapter validation is still a CI gate. Stable guest
bundle/save identity is retained. Never replace the owner's failing phone save.

Later same-day evidence reaches six earned wins, nine cards, five real card
upgrades and King level 2 in the QA copy. Native initial-name command 201 is now
ported (v25/v26); a real Boris defeat persists one loss without a reward and
returns home. Ordinary main 6884670 cannot render the name field because it
lacks the shared CoreText caret API already on this branch (TL-N). Use the
updated runtime, with no Clash backend flags or package marker. Seventh win,
graduation and arena/economy port remain unfinished; see UI_AUDIT.

The owner requests the offline backend inside a separate Clash IPA, surviving
ordinary emulator updates without a Clash-specific host service. This explicitly
authorizes modifying an output copy of the game executable. Read
[GUEST_MOD_PROGRESS.md](clash-royale/GUEST_MOD_PROGRESS.md) before continuing.
The new guest-code prototype runs on unmodified main `6884670` with no Clash
service: three earned training victories, chest banking, guest-owned persistence,
native card upgrade and aborted-reward recovery are reproduced. A deliberate
SIGKILL after chest ID 3's durable journal recovers through the original command;
see the progress document for final no-duplicate checks. It is not fully functional yet. Earlier
host-backend seven-training/all-arena scene evidence does not qualify the guest
port. No proprietary/generated binaries, fixtures or player data are in Git.

## Codex Recovery & Continuation — October 25, 2026

For the owner's importable guest-IPA request, read
[OFFLINE_IPA.md](clash-royale/OFFLINE_IPA.md). The metadata package reaches the
original tutorial on a fresh local import without offline CLI flags. It still
requires the updated emulator. The owner confirms a PR #138 iPhone build gets
stuck at 50% / couldn't log in; neither packaging nor Linux tests resolve that
device report. Preserve it and obtain the exact build/log before claiming a
root cause. CI adds Apple-host offline socket/state and TL-O ABI checks.

See [CODEX_RECOVERY_HANDOFF.md](clash-royale/CODEX_RECOVERY_HANDOFF.md) for the complete recovery report from Codex session `01a15ad7-5c8b-7222-ae87-545437c2d162`.
Key verified additions:
- Package-marker auto-configuration (`HydrogenVMOfflineBackend: clash-2.1306.1` in `Info.plist`), allowing modded offline IPAs to be imported directly without CLI flags (`tools/package_clash_offline.py`).
- ARMv7 aggregate return ABI fix for `__sincosf_stret` (hidden `r0` pointer) and TestLab regression case `TL-O` (all 15 TestLab cases passing).
- Offline IPA tested and verified working in `hvm run` without `--offline-clash`.

## Clash continuation — October 24, 2026

Latest continuation adds original crown collection command 523/source 3, with
native reward execution and durable recovery. An isolated ten-crown starting
fixture recovers a real killed 58-byte journal without backup restoration;
72 gold, 2 gems and cards apply once across two relaunches. Normal chest reveal
also returns cleanly to home. This does not prove earned ten-crown progression:
the earned player still has three crowns. Read PROGRESS for exact runs and
remaining gameplay/device gates. The owner's latest report specifically calls
out wrong princess angles and King cannon movement away from its anchor;
initial-facing correction alone does not close either phone observation.

The owner explicitly prioritizes core offline Clash functionality. Draft
[PR #138](https://github.com/lawsharley0-cyber/HydrogenVM/pull/138), branch
`codex/clash-offline-continuation`, checkpoints the embedded backend and local
Gemini work. Read [current evidence](clash-royale/PROGRESS.md) first. All seven
native training wins and the normal Arena 1 home transition were reproduced on
Linux. Knight, Bomber and Arrows upgrades, a full eight-card deck, distinct
chest identities, and actual interrupted-reward recovery are verified. The
native name dialog before NPC7 now completes using command 201. Returning home
after NPC6 exposed missing CTLineGetOffsetForStringIndex; the shared runtime
implementation uses actual glyph positions, with a failing-before/passing-after
TL-N guest test. This is not a newly signed iPhone/device validation.

Main `2b60a06` is integrated. A plain stat/fstat ABI regression from earlier main was
corrected using Apple's iPhone ABI: 64-bit inodes, st_size at byte 60; new TL-H v2
fails before the fix and passes after. Check this independently before changing
stat for another app; unsuffixed iPhone symbols do not imply legacy Mac ABI.

The first Arena 1 native NPC win completes after retaining sector delimiter 12.
Native ELO is now used: the bot receives its CSV score and active arena, and a
later real win produced 34 trophies in both native avatar and visible home,
retained across two relaunches. Fixed +30/-20 guesses and the old live-score
write were removed. A short-int
name-command error from `33d82ad` is corrected and its exact state is repaired
using the original command, with no direct save edit.

Further local fixes restore original tower scaling and neutral buff percentages:
damage/size must be 100%, not 0. The incorrect 1% size caused invalid maximum HP,
stuck health-bar frames and missing tower occupants. Four occupants now render,
and a damaged 1363/1526-HP tower selects the correct 11/99 animation frame.
The full sector now retains static decoration IDs instead of deleting native
CSV-created objects: original trees, rocks, fences and arena edges render again.
Earlier wins exercised the old tower initialization and do not prove current
balance. All this still needs phone validation.

Native free-chest collection now grants gold, gems and Goblins, survives an
actual interrupted journal/relaunch, and does not grant again on two relaunches.
A Goblins-for-Arrows deck swap also persists. All six bundled arena scenes and
attacking bots load in isolated synthetic profiles; earned later-arena promotion
and wins remain unverified. See PROGRESS for exact runs and their limits.

Corrected-combat three-crown victory is now verified, with native score 34 to
65 retained across two relaunches. The victory screen's Wooden Chest differs
from the saved cycle's Gold Chest, and crowns remain 0/10: native training-style
NPC battles skip crown credit. This is still a core accounting task.

Newer accounting work reconciles the native earned chest index and transfers
actual destroyed-tower crowns through original home logic. Native crown count
3 persists across two relaunches. Combined gem unlock/claim now journals the
native deduction with the reward; an actual killed isolated process recovered
gold 214 to 236 while retaining gems 72, without duplicate grants. The earned
player reached level 2 through Archers/Goblins upgrades. A new win/reward-label
comparison and full crown-chest collection are still pending.

Measured real elapsed home time is now applied through the original native API,
with timestamps tied to native ticks and reward completion. An initial seven-
second recovery shortfall was reproduced and corrected; later ordinary and paid
recovery comparisons differ by less than one second. Old untimestamped saves
start prospectively. Original opposing tower directions replace zero vectors;
initial facing is corrected locally, while cannon motion/device appearance remain open.

Arena entry now uses each CSV match cost and original Gold/FreeGold policy,
committed before sector delivery with duplicate/old-home guards. The real
Arena 1 test spent 204 to 203 gold, retained across two relaunches; the test
deliberately abandoned the battle and retains that fee without a fake result.
Apple checkpoint a92eb84 (before entry fees) compiled and packaged an unsigned
iOS device IPA; see PROGRESS/CI 40157432144. It has no phone validation.

Next: victory reward-display comparison, full crown collection, long Silver
unlocks, earned later-arena coverage and the remaining sound/cannon/text reports. The
owner explicitly requests continuing through the bug-fix phase, not stopping
at intermediate milestones. Paid renames and old half-claimed/duplicate-ID saves
need separate work; do not reset flags or progress. No assets/private
saves are in the PR. Do not merge the draft or equate its unit tests with a
complete playable game.

Owner clarification: princess orientation is wrong and the King cannon moves
away from its anchor. Keep both unresolved after restoring their visibility.

## Current continuation — October 22, 2026

Integration is `main`. Use a topic branch and a draft PR; the owner integrates
after independent review. The notes below the historical heading describe older
`gemini-merge` checkpoints and are not a request to change integration branches.
Current task ordering and ownership are in `dev-system/BACKLOG.md` and
`dev-system/STATUS.md`; the original architecture and client-decoder evidence
remain useful, but historical counts are not current validation.

The owner's immediate priority is filesystem/save-editor reliability (B-211).
Draft PR #18 adds a shared revision-aware store, exact original-byte rolling
backups, encoding/compression preservation, buffered SQLite changes, staged
snapshot restoration, visible Save controls and guarded dismissal. See
`dev-system/FILE_EDITOR_HANDOFF.md` for validation, device scenarios and the
ordered continuation. Do not describe a compiled UI as a device interaction pass.

The current shell is `app/HydrogenVM/HydrogenVMApp.swift`, `LibraryView.swift`,
`RunView.swift`, `DiagnosticsView.swift`, `FileBrowser.swift`, and
`Bridge/HVMEngine.h`/`Bridge/HVMEngine.mm`. Save persistence helpers are adjacent
Swift files. The C++ runtime lives under `src/` and is shared with the CLI.

Merged main already includes homogeneous clipping (PR #4), POSIX regex and
`tmpnam` (PR #5), the GLSL preprocessor/functions and bounded execution (PRs
#8/#9), and the unsigned Apple build workflow (PR #7). The software GLES/GLSL
implementation is a tested subset, not full ES 1.00 conformance. Unsupported
syntax must fail compilation with diagnostics; do not guess sampler roles.
No performance or full-gameplay claim follows from a short smoke or a shader
compile check.

Group B remains owned by open PRs #11/#12; Clash planning/tools by #13.
PRs #15–#17 cover allocator, PCM and ZIP safety. Do not duplicate or merge those
patches from this save-editor branch. The next graphics limit correction (issue
#14) and point-size work wait for Group B integration. Native 16 KiB-page
permissions/precise faults, signed-device gameplay and earned Clash progression
remain separate unresolved tasks. `CCCrypt` ciphers remain unimplemented and
outbound networking remains disabled by default.

## Historical handoff (superseded task ordering)

## Active Clash Royale continuation (updated after runtime fixes)

**Latest home-state checkpoint:** the decoder-derived 184-byte initial home/avatar
is accepted by the unchanged pinned client. It reaches the 65% loading scene and
requests NPC 18000000 through message 14104 (`12 00`). Two 75-second launches
return exit 0 with home acceptance, retained login on the second launch, no errors/
crash and non-silent audio. Shared Foundation fixes cover immutable regex copying/
lifetime, dictionary subclass subscript/KVC primitives and an inherited allKeys
recursion that previously exhausted the host stack. **79 runtime checks, 12 Python
tests, 18 shader checks and 107 IMA WAV comparisons pass.**

Use `tools/clash_home_probe.py` for the reproducible two-launch experiment. Its
peer validates the pinned IPA, loads the original starter template and atomically
stores a separate initial home-state JSON. This is not progression persistence.
The exact next blocker is **21903 SectorStateMessage** for Npc1/FirstMatch/BasicDeck1
(seed 2): mode markers 11/12, battle/tower/object/component/deck/avatar/tutorial
state. Decoder addresses and limits are recorded at the top of the restoration
document. The peer does not yet reply to mission requests. **No playable battle or
working menu is verified.** This checkpoint supersedes login-only notes below.

For the current user-requested Clash Royale work, read
[`CLASH_ROYALE_RESTORATION.md`](CLASH_ROYALE_RESTORATION.md) first.
Local desktop testing is available through `tools/local_test.sh` and
`tools/desktop.py`; see [`LOCAL_TESTING.md`](LOCAL_TESTING.md). The task output
folder includes Windows launchers. The viewer preserves the sandbox by running
an installed `.app` instead of re-importing its IPA on every launch.

**Latest offline investigation (`de7f95b`):** native saveOffline at 0x925b4
is `bx lr; nop`, not implemented save logic. The CSV flag/filenames alone are
insufficient evidence. The client resolves game.clashroyaleapp.com. A new explicit
loopback TCP capability plus hostname alias captures its RC4 login, wire version
(2,0,1306). A decoder-derived local LoginOk is accepted: the client reports
accountId (0,1) and retains the diagnostic identity/token on relaunch. This is
identity persistence only. The peer does not provide home state; the diagnostic
capture remains black, with no menu or battle. Next implement pinned
OwnHomeDataMessage/LogicClientHome/LogicClientAvatar state, then NPC sector state.
See the restoration document for exact addresses, protocol limitations and
commands. The native game binary/assets are unchanged; networking defaults off.

Audio checkpoint: shared IMA WAV decoding now covers all 107 pinned-client IMA
assets, compared sample-for-sample against libsndfile after `fact` trimming.
The desktop viewer streams actual mixer PCM to SDL/PulseAudio; install libpulse0
on WSLg. It records device underruns/errors and mixer signal/drop statistics.
Clash produces non-silent loading audio. Startup priming reduced frequent
underruns to one in 45 seconds; speaker audibility, combat and iPhone audio still
need checking. The pinned executable is armv7 only, so re-signing it does not
provide a native iOS 11+ sideload. Read the audio checkpoint in the restoration doc.

The fresh local suite passes **79 runtime checks**, **12 Python protocol/home tests**,
and **18 shader-source checks**. Windows-mounted checkouts now build through a
checksum-synchronized ext4 source mirror (`1201143`) to improve compilation and
avoid timestamp-related stale objects. The measured compiler-front-end change
is not a gameplay FPS result. The default local launchers retain offline policy.

**Latest rendering milestone (`ff2d4fd`):** Clash now displays the original loading artwork,
logo, training-camp hint, 50% bar and connection-error panel. The generic GLES2
path executes a bounded straight-line GLSL subset, including vec4 transforms,
multiple color varyings, channel arithmetic, texture units, column-major matrix
transforms, fixed varying arrays and perspective-correct interpolation. It has
real compile/link failures and info logs; there is no fallback based on names.
All **62 local runtime checks pass**, including five new shader/pixel/diagnostic
checks. All **18 captured Clash shader sources compile**. The four-app local
strict/exploratory/relaunch scan still reaches its limits, with varied Angry
Birds/Art of Glow frames and Tiny Tower's known black frame. No iOS device build
was tested in this continuation. See the restoration document for subset limits.

**Clash is not playable.** It still attempts server connections despite bundled
`OfflineMode=true`; no offline_avatar/home save files appear. The empty offline-
save entry and local login findings above supersede the earlier flag
investigation; next supply home/avatar and NPC state. `titan.checkpoint` and a
timed scanner `runs` result are not evidence of offline progression. Prior blank-scene/“UIKit dialog” notes
below describe the earlier renderer; the native styled panel is now visible.

The earlier Mach/allocator/TrueType fixes remain intact. GitHub Actions jobs
previously failed before starting because of account billing/spending limits;
validation is local until remote steps actually execute. The investigation
workflow now dumps and compile-checks reached shaders; run
`hvm_tests shaders <dump-directory>` locally. That historical checkpoint used `gemini-merge`;
no game binary or assets were changed. These goals take precedence over the
older roadmap below.


JIT-less emulator that runs **32-bit iOS apps (.ipa, armv6/armv7) on modern iPhones**.
Sideload-only iPhone app, bundle id `com.lfull.HydrogenVM`.
Repo `github.com/lawsharley0-cyber/HydrogenVM`, **integration branch `main`; work on topic branches**.
The owner (Lawson) tests sideloaded builds on device and sends logs and IPAs. He wants
**systematic fixes**: find every issue per IPA, fix it once in the shared runtime, and
verify the fix across all apps.

## Build, test, CI

```sh
cmake -S . -B build -G Ninja && ninja -C build hvm hvm_tests   # host build (Linux/macOS); needs zlib, sqlite3
build/hvm_tests fulltest          # 79 runtime checks, all must pass
build/hvm run <App.app> --home <dir> --run-for 30 [--keep-going] [--screenshot f.png] [--report r.json]
      [--graceful] [--lifecycle bg@8,fg@13] [--launches 2] [--trace] [--tap x,y@t] [--drag ...]
tools/compat.py scan <ipas|apps|dirs> --out compat --seconds 30 [--relaunch] [--only a,b]   # see docs/COMPAT.md
```

* **CI** is in `.github/workflows/ci.yml`. The Linux job runs on every push. The macOS job
  (Release device app, **unsigned IPA artifact `HydrogenVM-device-ipa` → `HydrogenVM-unsigned.ipa`**,
  simulator run) runs only when the commit message contains **`[mac]`** or on
  workflow_dispatch. A new push cancels the in-progress run. Reports go to the `ci-reports`
  branch. Artifact downloads 403 from the Claude sandbox, so the user downloads them himself.
  The device artifact now requires successful package checks and the simulator
  test; `macos-results` holds diagnostics even on failure. See
  [`DEVELOPMENT_AND_RELEASE.md`](DEVELOPMENT_AND_RELEASE.md). This updated Apple
  build path has not been executed from the Windows workstation; Actions remains
  subject to the account billing block.
* The iOS app is in `app/` (XcodeGen `project.yml`, Swift 6, iOS 26 target). Swift can only be
  compiled by the macOS CI job; there is no local Swift toolchain on Linux.
* Commit trailers the user's tooling expects: `Co-Authored-By: …` and `Claude-Session: …`.

## Architecture (where things are)

* `src/core/cpu/`: threaded-IR interpreter (ARM/Thumb/VFP/NEON decoders, `Engine.cpp`).
  * Blocks are decoded lazily into ops that tail-call each other; there is no JIT.
  * Supports `fusePairs`/`fuseOps` superinstructions.
  * Per-Cpu `suspend_count`/`in_guest` safepoints back `Engine::suspend/resume` (used by
    `thread_suspend`).
* `src/loader/`: Mach-O loader (fat, dyld-info and classic relocations, **weak binds now
  applied**), IPA/zip, plist.
* `src/runtime/Process.*`: one guest process.
  * Symbol registry: `p.def<&fn>("_sym")` registers typed HLE functions;
    `defineFunction(name, raw)` registers raw ones; `defineDataBlock`.
  * Imports are bound to HLE functions or to "missing" stubs. The heuristics for whether a
    missing import is data or a function are in `makeMissingStub` and `looksLikeData`/
    `looksLikeFunction`. Missing-symbol resolvers provide string constants.
  * Threads, lifecycle API, `processSlot`/`threadSlot`.
* `src/runtime/libc/`: libc/POSIX, pthread + Mach threads, `Unwind.cpp` (SJLJ C++/ObjC
  exceptions), `LibStdcxx*.cpp` (GCC 4.2 libstdc++: COW string, rb-tree, list, file streams,
  string streams/ostream formatting), `Zlib.cpp`, `Sqlite.cpp` (host sqlite), `Net.cpp`
  (offline sockets), `CommonCrypto.cpp`, `Posix.cpp`, `Stdlib.cpp` (malloc zones).
* `src/runtime/objc/`: ObjC runtime. `ClassBuilder` (`r.defineClass`, `r.builder(cls)`,
  `.m<&fn>(sel, types)`, `.cm<>`); placeholder "stub" classes return nil; `forwardHook`
  handles `-forwardInvocation:`.
* `src/runtime/foundation/`: Foundation/CF.
  * Strings, collections, defaults, run loops, dispatch, formatters, `Calendar.cpp`.
  * `Network.cpp`: CFNetwork/NSStream, offline.
  * `UrlLoading.cpp`: NSURLConnection/Request, offline (fails with -1009).
  * `Invocation.cpp`: NSInvocation and forwarding.
  * `Extras.cpp`: NSScanner, NSXMLParser, NSURLResponse, NSHTTPCookie(Storage), NSCache,
    NSDecimalNumber.
  * `Security.cpp`: keychain.
* `src/runtime/uikit/`: UIKit, nibs, widgets, QuartzCore, GameKit, and audio stubs.
  * `OpenGLES.cpp`: GLES1 fixed-function plus a bounded GLES2 interpreter; see current restoration notes.
  * `PlatformStubs.cpp`: audio (OpenAL, AudioQueue, AVAudioPlayer, ExtAudioFile, AudioFile)
    and StoreKit.
  * `MiscFrameworks.cpp`: UIWebView, CLLocationManager (denied), MPMoviePlayer (finishes
    immediately), MPMusicPlayer, UIImagePicker (unavailable), UTType, CoreTelephony,
    CaptiveNetwork.
  * `UIApplication.cpp`: the lifecycle handler.
* `src/runtime/audio/`: software mixer (AVAudioSourceNode pulls it on device), decoders for
  WAV/CAF/AIFF/MP3 (minimp3).
* `src/runtime/graphics/`: PNG, PVRTC decode, 2D raster, text.
* `app/HydrogenVM/`:
  * `Bridge/HVMEngine.mm`: ObjC bridge; HVMSession start/stop/**stopGracefully/
    enterBackground/enterForeground**, audio, per-run logs in Documents/Logs.
  * `RunView.swift`: runs a game; Stop = save then close, with HydrogenVM's background state
    forwarded to the game.
  * `LibraryView.swift`: apps in `Documents/Apps/<bundle id>/<Name>.app`, with the app's
    sandbox beside the bundle.
  * **`FileBrowser.swift`** (new): the per-app file explorer.

## Conventions and gotchas (learned the hard way)

* **32-bit iOS calling convention is APCS.** 64-bit arguments are **not** moved to an even
  register: `f(int a, long long b)` → a=r0, b=r1:r2. Structs larger than 4 bytes are returned
  through r0 (sret). Floats travel in core registers (softfp). `ArgReader`/`p.def` already do
  this. Hand-decoded raw functions must too.
* **Never cache guest addresses in C++ statics.** The iOS app runs one `Process` per launch,
  so a static leaks into the next launch; this caused the crash when reopening a game. Use
  `static const char key=0; uint32_t& v = x.proc.processSlot(&key);` (or `threadSlot`).
* GCC libstdc++ layouts are documented at the top of `LibStdcxx.cpp`,
  `LibStdcxxContainers.cpp` and `LibStdcxxStreams.cpp`.
* Classic Mach-O external relocations are relative to the first segment.
* Weak binds: override only when the symbol is registered; otherwise keep the app's own
  definition.
* Missing data symbols become zero blocks. String-constant names (…Key, …Notification, k…)
  resolve to constant NSStrings, including for weak imports.
* The network is deliberately **offline** everywhere (sockets, CFNetwork, NSURLConnection,
  reachability).
* `CCCrypt` / `CCCryptor*` forward to the host crypto library (owner decision 2026-10-23); the emulator contains no cipher code.
* **FairPlay-encrypted IPAs are unsupported.** We do not help decrypt.
* Debugging aids:
  * `HVM_TRACE_BLOCKS=lo-hi` logs blocks with registers (slow).
  * `--trace` logs HLE calls.
  * `HVM_TRACE_DRAW`, `HVM_TRACE_TRI`, `HVM_DUMP_TEX`, `HVM_DUMP_RB`.
  * Disassembly: llvm-objdump is too slow on the fat binaries, so extract bytes and use
    `llvm-mc --disassemble -triple=thumbv7-apple-ios`. A helper script for this was in the
    scratchpad and is easy to rewrite.

## Status (compat scan, 30 s, strict and relaunch)

| App | Graphics | Status |
|---|---|---|
| Angry Birds 1.0 | GLES1 | runs, title screen OK |
| Tiny Tower 1.0 | GLES1 | runs; saves on close; relaunch OK |
| Art of Glow 1.1.2 | GLES1 | runs |
| Jetpack Joyride 1.3.5 | GLES1 | runs to its OpenFeint opt-in screen (not tapped through yet), ~3 fps on Linux |
| Minecraft PE Lite 0.1.2 | GLES1 | runs, main menu renders |
| Subway Surfers 1.41 | **GLES2 / Unity** | runs, but **renders almost nothing** (frame check flags 93 % white) |

All six apps have zero runtime misses in strict mode. The remaining load-time gaps are listed
in `compat/REPORT.md` after a scan (single-app: EventKit, NSPredicate, NSRegularExpression,
NSSortDescriptor, NSMapTable/HashTable, NSAttributedString, UIBezierPath, UIPasteboard,
AddressBook, …).

## This session's features (all committed; last commit `1a0787b`)

1. **Compatibility scanner** `tools/compat.py` (see `docs/COMPAT.md`).
   * Per app: strict run (first blocker + screenshot), keep-going run (all runtime misses),
     static gaps, frame check (blank or one-colour frames), GL/engine detection.
   * Fixes are ranked by apps affected; regressions are diffed against the previous scan.
   * `--relaunch` opens each app twice in one process.
2. **Save on exit and background pause.**
   * `Process::requestGracefulStop` delivers resign-active → enter-background →
     will-terminate, flushes NSUserDefaults and stdio, then stops; a watchdog force-stops
     after 5 s.
   * `enterBackground` delivers the background events, then parks the main thread, suspends
     the other guest threads and silences the mixer. `enterForeground` reverses it.
   * The iOS Run screen uses these (shows "Saving…"); `didEnterBackground` uses a background
     task.
3. **Crash when reopening a game: fixed** (the stale statics above).
4. **File explorer** (Library → swipe or long-press → Files).
   * Browses the bundle plus Documents/Library/tmp.
   * Structured plist editor (keeps binary or XML format), text editor, image view, hex view
     with byte patch.
   * Import (replaces same-named files), share, rename, duplicate, delete, new folder/file.
5. Many runtime additions:
   * Libraries: zlib, sqlite, string streams, thread_suspend/get_state/task_threads, malloc
     zones, xattr stubs, asl.
   * Foundation and system frameworks: NSCalendar, NSInvocation, CFNetwork, NSURLConnection,
     SKProductsRequest, NSScanner, NSXMLParser, and the rest of the Extras.cpp classes.
   * Graphics and media: GL multisample resolve, AudioFileOpenWithCallbacks, AudioQueue
     extras.
   * Misc: UTType, CCHmac, objc_getClassList, and std::string accessors.

## Historical first-check item

* **CI run `39379622776` (commit `1a0787b`, `[mac]`) was still running when this handoff was
  written.** It is the **first compile of `FileBrowser.swift` and the RunView lifecycle
  Swift**, which were written blind. If the macOS job fails, fix the Swift errors, most
  likely Swift 6 concurrency or SwiftUI API mismatches. Then tell the user to grab the
  unsigned IPA.

## Historical next steps (superseded by current continuation)

1. **GLSL ES 1.00 interpreter (main task).** Today `gl_linkProgram` only regex-scans
   declarations and guesses roles: first mat4 = MVP, first sampler = texture, attribute names
   containing pos/col/tex. `shadeVertex`/`rasterizeTriangle` then draw as fixed-function.
   Unity shaders break this. Planned design, all in a new `src/runtime/graphics/glsl/`:
   * **Preprocessor:**
     * `#define`, with and without parameters; `#undef`.
     * `#if`/`#ifdef`/`#ifndef`/`#elif`/`#else`/`#endif`, with expression evaluation and
       `defined()`.
     * `#version`, `#extension`, `#pragma` and `#line` are accepted.
     * Predefined `GL_ES` and `__VERSION__ 100`; line continuations.
   * **Lexer and parser → AST:**
     * Precision, storage and parameter qualifiers.
     * Structs, arrays, and function prototypes with overloading.
     * All statements and expressions, including swizzles, constructors and indexing.
   * **Semantic pass:**
     * Types: float/int/bool scalars and vectors, mat2–4, sampler2D/Cube, structs, arrays.
     * GLSL ES 1.00 has no implicit conversions.
     * Overload resolution and the builtin functions.
   * **Codegen to an IR** with float4 registers (ints and bools as floats; a matrix is N
     registers):
     * Each source operand has a swizzle and negate; each destination has a write mask.
     * **Inline all functions** (GLSL ES has no recursion).
     * Dynamic array indexing ops (skinning bones).
   * **Lane-parallel interpreter**:
     * Runs 16 vertices or fragments per dispatch in SoA layout.
     * Execution masks handle if/loops/break/continue/discard.
     * Ops: arithmetic, the full builtin math set, matrix ops expanded to dp/mad, and
       texture2D/Proj/Lod/Cube through a batched sampler callback.
   * **GL integration:**
     * Real compile and link status and info logs.
     * Active uniform and attribute introspection: glGetActiveUniform/Attrib,
       glGetProgramiv counts, and glGetUniformLocation for `a[i]` and `s.f`. Unity queries
       these.
     * Uniform storage per location, including arrays and samplers.
     * Generic vertex attributes.
     * gl_Position, varyings, gl_FragCoord, gl_FrontFacing, gl_PointCoord and
       gl_PointSize.
   * **Fix the raster stage for everyone:**
     * Homogeneous **near-plane clipping**.
     * **Perspective-correct** interpolation (the `invW` value is computed but unused
       today).
     * Early depth test with late write when the shader may `discard`.
     * Mipmaps and derivatives could come later via 2×2 quads.
   * **Do this first:** add a shader-source dump (env `HVM_DUMP_SHADERS=<dir>` in
     `gl_shaderSource`; available now). Collect Subway Surfers' shaders as a
     corpus and make the compiler accept all of them before integrating. Then judge it with
     the frame check and screenshots in `compat.py`.
2. **Minecraft Story Mode** (Telltale) is too large for the user to upload (ask for a zip of
   the `.app` without its big asset folders). It uses **libc++ (`std::__1::…`)**, a second C++
   runtime ABI next to GCC libstdc++: strings, ostream/streambuf, and typeinfo for
   `std::__1` types. Its first blocker, `malloc_create_zone`, is already done.
3. Jetpack: tap through the OpenFeint opt-in (likely fine on device). Performance: Jetpack
   runs at ~3 fps and Subway at ~5 fps on the Linux CI box; device numbers will differ.
4. Work down the remaining load-time gaps from the scanner (start with the ones shared by
   several apps).
5. The user asked earlier about **APK support**; it was discussed only conceptually and
   nothing is started.

Scratch data from this session (IPAs, compat output, screenshots) lived in the Claude sandbox
and is **not** in the repo. The user's IPAs: Angry Birds 1.0, Tiny Tower 1.0, Art of Glow
1.1.2, Jetpack Joyride, Minecraft PE Lite 0.1.2, Subway Surfers 1.41. Ask the user to
re-upload them if needed.
