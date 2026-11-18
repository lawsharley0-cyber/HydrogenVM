# HydrogenVM expansion plan (2026-11-17)

How we get from "three Android games and a strong iOS library" to "most games just work, bugs
are found without Lawson playing, and the old games feel better than they did". It covers the
feedback-loop tools, Android breadth, automation, the player experience, growth and a
self-improving loop.

This plan extends [PLAN.md](PLAN.md); it doesn't replace its rules. The hard rules in
[AGENTS.md](../AGENTS.md) apply to every card:
- no JIT;
- no FairPlay;
- no title-specific code;
- every fix comes with a test that fails first;
- no fake success.

Android architecture is in [research/android-support.md](research/android-support.md).

**Effort** figures are estimates for one focused agent, not measurements. **Done when** is the
bar for closing a card. Each card is one PR or a short series, with gates in the PR body. The
order of phases matters; cards *within* a phase can run in parallel when their files don't
overlap (see "Running it" at the end).

## Where we are (start of this plan)

- **iOS:**
  - Large library of games tested through Test All Games and per-game playability docs.
  - Test Lab, fulltest (846), stub lint and the compat harness gate every change.
- **Android:**
  - Dead Space plays on the phone.
  - Alto's Adventure boots, plays and takes touch on the bench; phone confirmation pending.
  - Rayman is blocked by Ubisoft's dead login server.
  - Drivers: EA Blast, Ubisoft UAF, Unity (Mono).
  - Signals, `/proc`, looper/input/sensors and framework services are in place.
- **Gap finder** (`tools/android_gaps.py`, PR #511): scans APKs, ranks missing native
  functions and Java methods across games, and writes labelled placeholders.
- **Phone-only bugs so far:** host `/proc` leaking into the bench; stale absolute links after a
  re-import. Both would have been caught by card A2 below.

---

## Phase A — Tighten the feedback loop (≈2 weeks)

> **Status (2026-11-17): built.** A1 one-tap reports (needs a GitHub token on the Diagnostics
> tab), A2 `--phone`, A3 engine badge and APK batch runs, A4 `tools/nightly/` (needs scheduling
> on the PC, see `docs/LOCAL_TESTING.md`), A5 `tools/android_testlab.py` in CI. The Test Lab's
> first run found and fixed a futex wake-count bug.

Everything later goes faster once bugs reproduce off the phone and reports arrive complete.

### A1. One-tap problem reports (2–3 days)
- **What:** a "Report problem" button in the in-game menu and the crash screen. It files a
  GitHub issue in one tap:
  - title: game, platform and symptom;
  - the full report JSON and the log as attachments, not a pasted excerpt;
  - device model, iOS version and app build;
  - the last screenshot;
  - the last 30 s of frames and audio levels.

  Works offline: the report is queued and sent later.
- **How:** the GitHub REST API with a fine-grained token entered once in Settings (never in the
  repo). Fallback: the share sheet with the files zipped. Reuse the sanitized report bundle
  from PR #490 / #491.
- **Files:** `app/HydrogenVM/` (report UI, uploader), `HVMEngine.mm` (bundle assembly).
- **Done when:** a crash on the phone becomes an issue with the full log in one tap; Lawson
  never has to trim a log.

### A2. Phone mode for the Linux bench (3–4 days)
- **What:** `hvm run --phone` (also on in the compat harness) makes Linux behave like the iPhone
  in the ways that have bitten us:
  - no host `/proc` (the stand-in only);
  - 16 KB host pages (`GuestArena(16384)`);
  - GLES1-over-ES2 emulation;
  - iOS-style `/var/mobile/Containers/…` paths whose container id **changes between runs**
    (catches stale absolute links);
  - a **case-insensitive file system view** (APFS default). Android games are case-sensitive,
    so two files differing only by case collide on the phone. Detect it, and report and resolve
    it deterministically.
  - the iPhone's memory budget: fail allocations past a configurable limit.
- **Files:** `tests/runner/HvmRun.cpp`, `src/runtime/Process.cpp` (path layer), `GuestArena`.
- **Done when:** the Dead Space #504 and Alto #507 crashes reproduce under `--phone` on a build
  without their fixes. Each gets a regression check.

### A3. Test All Games for APKs + engine label (3–4 days)
- **What:** the in-app batch tester runs Android games too. Each result shows:
  - the engine, and whether it is supported (from the gap finder's detection, moved into the
    engine so the app can call it);
  - whether it drew a picture and played sound;
  - placeholder hits and missing gaps.

  The library row shows an engine badge, e.g. "Unity 4 · supported" or "libGDX · needs Java".
- **Files:** `app/HydrogenVM/` (Test All Games, library row), `src/loader/Apk.cpp` (engine
  detection at import), `tools/android_gaps.py` (shared rules).
- **Done when:** one batch run covers Lawson's APKs and the report feeds the gap ledger directly.

### A4. Nightly corpus run on the test PC (3–5 days)
- **What:** Lawson's Windows PC, where the Desktop `test_ipa` folder already holds the private
  fixtures, runs every IPA and APK nightly under WSL, with `--phone`, scripted taps where they
  exist, screenshots and reports. It compares each game with the previous night: crash, no
  picture, silence, new missing APIs, or a screenshot that differs beyond a threshold. Then it
  publishes:
  - a status JSON (feeds G1);
  - a short summary issue only when something changed for the worse, with the bisect range.
- **Files:** `tools/nightly/` (runner, diff, publisher), a Windows Task Scheduler entry
  documented in `docs/LOCAL_TESTING.md`. Fixtures never leave the PC.
- **Done when:** a deliberately broken commit is flagged the next morning with the game and the
  commit range.

### A5. Android Test Lab (4–6 days)
- **What:** tiny Android test programs, one per bug class. Each fails before its fix and passes
  after, like the iOS Test Lab. They are built with clang targeting `armv7a-linux-androideabi`
  against a minimal bionic header set in `tests/android/` (no NDK download needed).
- **First cases**, one per class already fixed:
  - signal numbers and flags;
  - mutex types;
  - `/proc/self/maps` and `auxv`;
  - cacheflush;
  - futex;
  - JNI reflection;
  - MotionEvent.obtain;
  - pointer ids;
  - relative data links.
- **Files:** `tests/android/`, `tools/hvm_testlab.py` (an `--android` suite), CI workflow.
- **Done when:** the suite runs in CI and each case is shown to fail on a build without its fix.

---

## Phase B — Android breadth: the quick unlocks (≈3–4 weeks)

Ordered by the gap ledger. Re-run `android_gaps.py scan` on the corpus after each card and let
the ranking pick the next fix.

### B1. NativeActivity driver (1–2 days)
`ANativeActivity_onCreate`, the callbacks struct, window and input-queue creation, focus,
pause and resume. `Looper.cpp` already has the input queue and `pushTouchEvent`. This unlocks
`android_native_app_glue` games, many Cocos2d-x, Unreal, GameMaker and ported titles.
- **Done when:** a NativeActivity game (Geometry Wars if it is one, or an A5 test app) draws and
  takes touch.

### B2. Package formats (2–3 days)
- Split APK bundles (`.xapk`, `.apks`, `.apkm`): merge the base and config splits at import.
- Expansion files: `main.*.obb` / `patch.*.obb`, whether inside the APK's assets (Rayman ships
  one) or alongside it. Place them where the game looks
  (`Android/obb/<package>/` on the stand-in sdcard).
- **Done when:** each format imports from the phone's Files app and the game finds its data.

### B3. System library gaps (1–2 weeks, a series)
- **Sockets:**
  - translate between Linux and BSD socket formats (`sockaddr`, `SOL_SOCKET` options, flags);
  - map `getaddrinfo` error codes;
  - stay within the per-game "Allow internet" switch;
  - when the switch is off, fail fast with `ENETUNREACH` so games fall back to offline.
- **Event plumbing:**
  - `epoll`, `eventfd`, `pipe2`, `poll` over descriptors, `timerfd`;
  - `ALooper_addFd` for real descriptors.
- **Memory-mapped files** for assets; `pread` / `pwrite`.
- **Done when:** each item has an A5 case, and the ledger's native list for the corpus is empty
  or placeholders only.

### B4. Graphics breadth (1 week)
- GLES 3.0 contexts: chosen by the `libGLESv3` DT_NEEDED entry or the requested EGL config.
- Texture formats Android games ship:
  - ETC2/EAC;
  - ATC (Adreno builds);
  - PVRTC (PowerVR builds);
  - DXT/S3TC (Tegra builds).

  Each is decoded on load if the iPhone's GPU can't sample it.
- **Done when:** each format has a decode test with independently derived expected pixels.

### B5. Audio breadth (3–5 days)
- `android.media.MediaPlayer` and `SoundPool` in the Java layer, which many games use for music
  and effects. Decode through the host (AudioToolbox on the phone, a bundled decoder on Linux).
- AAudio for newer games.
- **Done when:** a MediaPlayer game plays music and an A5 case checks the sample output.

### B6. Input and lifecycle (3–4 days)
- Android back button: a gesture or button in the in-game menu → `KEYCODE_BACK`.
- Game controllers → `AKeyEvent` / `AMotionEvent` joystick axes (shares C2's mapping).
- Real accelerometer from CoreMotion.
- Full multitouch.
- Pause, resume and saving on app switch (`onPause` / `onSaveInstanceState` per driver).
- **Done when:** a tilt game steers with the phone, and leaving and returning keeps the game's
  progress.

### B7. More engine drivers (each 2–5 days, picked by the ledger)
- **Cocos2d-x:** mostly native; the Cocos2dxActivity / Renderer / Helper Java glue.
- **Unity IL2CPP:** like Mono, minus the JIT work.
- **GameMaker:** `RunnerJNILib`.
- **Unreal 3/4:** the GameActivity glue.
- **Adobe AIR.**

Each driver follows the Unity pattern: its Java side is in host code, and its plugin SDKs get
placeholders.

---

## Phase C — Automation: find bugs without Lawson (≈3–4 weeks)

### C1. Record on the phone, replay on the bench (1–2 weeks; was PLAN.md T7)
- **Record** (opt-in per session): touches with timestamps, timer and clock reads, file reads
  (path, offset, length, and a hash; the bytes only for files outside the bundle), RNG seeds,
  thread scheduling points at locks and waits, and network answers when "Allow internet" is on.
  The recording is attached to A1 reports.
- **Replay** on the bench under `--phone`, deterministic to the extent the record captures.
  Divergence is detected by comparing frame hashes and the call ring.
- **Done when:** a recorded phone session that crashes after minutes of play crashes at the
  same point on replay.

### C2. Auto-player bot (1–2 weeks)
- **What:** a bench harness that plays: screenshot → decide → tap or drag → repeat.
  - Level 1: a random "monkey" with menu heuristics (tap text-like regions, the screen center,
    common button corners).
  - Level 2: an AI vision model (Claude) given the screenshot and a goal ("start a game", "get
    past the tutorial", "open settings"). It returns the tap.

  It records every step as a replayable script (C1 format), so any crash it finds reproduces
  without the bot.
- **Cost guard:** a vision call only when the screen changed and the cheap heuristics stalled,
  with a per-game budget.
- **Done when:** the bot gets Alto from launch to gameplay and Dead Space through its first
  room, and a planted crash deep in a game is found and saved as a replay.

### C3. Safe-mode retry (2–3 days)
- **What:** when a game crashes in the first minutes, the app offers (or, if the player chose
  "always", silently does) one relaunch with fallbacks, one at a time, ordered by what the
  crash looks like:
  - the GLES1 emulation path;
  - the frame-rate cap;
  - "Allow internet" off;
  - reduced texture resolution;
  - extra memory headroom.

  A fallback that works is saved as that game's profile (the existing compatibility-profile
  mechanism from PR #488/#494) and reported, so the root cause still gets fixed.
- **Done when:** a game that crashes only with one setting launches on the second try, and the
  report names the fallback.

---

## Phase D — Java (DEX) interpreter (≈5–8 weeks)

The single biggest unlock for Android. It runs the game's own Java code instead of
hand-writing each engine's Java side.

- **D1. Interpreter core (2–3 weeks).** A Dalvik bytecode interpreter in C++: all 218
  instructions, exceptions, monitors, static init order, interfaces, arrays and strings. Classes
  load lazily from `classes*.dex`. No JIT: it interprets.
  - **Done when:** the Android Test Lab gets a Java suite (arithmetic, objects, exceptions,
    threads), checked against expected values worked out by hand.
- **D2. Bridge to the existing host classes (1 week).** Unknown framework classes resolve to
  the C++ implementations already in `Java.cpp` / `JavaServices.cpp`. Game classes run
  interpreted. JNI calls from native code into game Java classes run interpreted too, so the
  hand-written per-engine glue shrinks over time.
- **D3. Core library (1–2 weeks).** The parts of `java.lang`, `java.util`, `java.io` and
  `java.nio` games use (collections, StringBuilder, formatting, streams, threads,
  `ByteBuffer`), prioritized by the gap finder's framework-reference counts.
- **D4. libGDX and pure-Java games (1–2 weeks).** The first engine that is mostly Java.
  - **Done when:** one libGDX game and one Java-only game reach gameplay.

**Risk:** this is the largest card. The fallback is staying with hand-written drivers, which
already cover the native-heavy engines (B7). Re-decide with the corpus data after Phase B.

---

## Phase E — Make old games better than they were (≈3–4 weeks)

### E1. Faster launches (1 week)
- Persist decoded blocks per game and build: guest address, ARM/Thumb, a hash of the source
  bytes, and the Op records, but no host pointers. They load lazily and are checked against the
  hash on first use. These are data, not machine code, so this is within the no-JIT rule.
- Android: extract `assets/` once at import instead of reading from the APK on every launch
  (keeping `base.apk` for engines that read it directly).
- Unity Mono: keep the JIT'd-code caches warm. Profile first.
- **Done when:** Alto's cold start time is measured before and after, and is lower.

### E2. HD and widescreen modes (1–2 weeks)
- **Resolution:** render the game's framebuffer at a multiple of its original size where it
  draws to framebuffer 0, with better texture filtering (anisotropic) as an option.
- **Widescreen:** fill the screen by adjusting the viewport and projection where the engine
  allows it; otherwise "stretch" and "fit" choices, per game.
- **Done when:** before and after screenshots for three games are in the PR; it is off by
  default, per-game toggles.

### E3. Controllers and on-screen controls (1 week)
- MFi, Xbox and PlayStation controllers through GameController.framework. iOS games that used
  the old GameController API get it directly. For other iOS games, buttons map to touches at
  chosen screen spots; Android games get them through B6.
- An on-screen controls editor: drag virtual buttons and sticks over the game and save a
  per-game layout. It is shareable as a small file.
- **Done when:** a controller plays a touch-only game through a mapping, and a layout survives
  relaunch.

### E4. Quick save and resume (2–3 weeks; research spike first)
- **The hard part is solved:** safepoints (from the Alto work) can pause every guest thread at
  a known point.
- **Snapshot:**
  - the guest arena (dirty pages);
  - thread register files;
  - the host-side runtime state (the services);
  - the GL objects the game created, replayed from a recorded object log on restore;
  - audio state.
- **Spike first (3 days):** snapshot and restore a simple 2D game. Measure the size and the
  restore fidelity. Only then commit to the general version.
- **Done when:** one game saves mid-level and resumes there after an app kill.

---

## Phase F — Bring dead online games back (≈2–3 weeks)

### F1. Server-replacement toolkit (2–3 weeks)
- **What:** one framework for local stand-in servers, instead of a bespoke one per game
  (Clash Royale, Clash of Clans):
  - **capture:** with "Allow internet" on and the real server still alive, or from public
    protocol documentation, record the conversations;
  - **describe:** a small rules file per game: endpoints, message shapes, canned and computed
    answers, simple persistent state;
  - **serve:** inside the app, on loopback. The TLS bridge already terminates the game's old
    TLS locally.
- **Rayman Adventures** is the first target (its Ubisoft login handshake), then the "offline
  fallback for online features" Lawson asked for (2026-11-12).
- **Rules:** only for games the player owns; no circumventing purchases or DRM. A server pack
  is data, so it can be shared without game files.
- **Done when:** Rayman passes "Connecting" to its menu offline.

---

## Phase G — Growth (≈1–2 weeks, after A4)

### G1. Public compatibility list (1 week)
- A static page (GitHub Pages) generated from the nightly status JSON (A4). For each game:
  - status: Plays / In-game / Menus / Boots / Crashes;
  - platform and engine;
  - last-tested build and date;
  - one screenshot (where publishing it is fine; otherwise none);
  - known issues linked to GitHub issues.

  Filterable by platform and engine. Players can submit their own results through the A1
  reporter (marked "community").
- **Done when:** the page builds from real nightly data and updates on its own.

### G2. Feeds the publicity and monetization decisions
The compatibility list, before and after HD screenshots (E2), and short clips are what a
launch post, a donation page or a publisher pitch (the licensing pilot) need. This card is
Lawson's call on timing; the tooling above produces the material.

---

---

## Phase H — Self-improving loop (≈2–3 weeks, after A and C)

The emulator gets better continuously without a person writing each fix. Fixes are made
**off the phone**. iOS can't compile or load code the app writes for itself (the same rule
that makes HydrogenVM JIT-less), and small on-device models are not reliable enough for
emulator code. Everything is checked against real games before it ships.

### H1. Automated fix agent (1–2 weeks)
- **Input:** the gap ledger (`tools/android_gaps.py`), the nightly status (A4), new A1
  reports with their C1 replays, and the auto-player's crash scripts (C2).
- **Loop**, on the test machine or CI on a schedule:
  1. Pick the top-ranked gap or regression that no open PR covers.
  2. Hand it to a strong model (Claude or Gemini, through its CLI or API) with the head-engineer
     prompt (`docs/HEAD_ENGINEER_PROMPT.md`) and the evidence.
  3. Require a test that fails first, the fix, and the gates.
  4. Re-run the affected games under `--phone`, plus the original replay.
  5. Open a PR with the evidence.
- **Guards:**
  - a per-run budget;
  - one area per PR;
  - hotspot files are never edited by two runs at once;
  - no merge without green gates and the game re-check;
  - a policy setting for whether green fix PRs auto-merge or wait for Lawson.
- **Done when:** a planted gap, such as a removed JNI method a game uses, is found, fixed,
  tested and proposed with no person involved.

### H2. Downloadable improvement packs (1 week)
- **What:** the parts of the runtime that are data become signed packs the app downloads, so
  games improve without reinstalling:
  - placeholder tables (`placeholders/*.inc` as data);
  - per-game compatibility profiles;
  - Java-method stand-ins described as data (return values, fields);
  - fake-server rules (F1);
  - control layouts (E3).
- **How:** packs are versioned and signed with a project key (the app holds only the public
  key) and served from GitHub Releases. They are checked on launch and cached; one switch in
  Settings rolls back to the built-in tables. Anything that needs new code still ships in an
  IPA.
- **Done when:** a pack published from CI changes a game's behaviour on the phone on next
  launch, and a pack with a bad signature is refused.

### H3. On-device helpers (3–5 days)
- Rules first:
  - match a crash against known patterns to pick the C3 safe-mode fallback;
  - suggest a control layout from the game's engine and input APIs;
  - flag "this game needs Allow internet".
- A tiny on-device model is an option only if the rules stop scaling; it never writes or loads
  code.
- **Done when:** the safe-mode fallback for the known crash classes is chosen automatically,
  and the choice is visible in the report.

## Phase I — Correct by construction, then fast (≈10–14 weeks, starts after A)

The goal is to stop guessing what the CPU should do and to stop interpreting code we have
already seen run. Every step is checked against Arm's own definition of the instruction set.

### I1. Reference CPU generated from Arm's specification (1–2 weeks)
- **What:** Arm publishes the A-profile architecture as machine-readable JSON
  (`Instructions.json`: A64, A32 and T32 encodings, decode rules and ASL pseudocode;
  `Registers.json`). A generator in `tools/armspec/` turns the A32, T32, VFP and NEON entries
  into a slow, independent reference interpreter (`src/core/refcpu/`, test builds only) and a
  decode table.
- **Licence:** read Arm's `notice` file before importing anything; the spec is vendored by
  path and version, not edited.
- **Done when:** the reference CPU runs the existing CPU unit tests and the Test Lab apps with
  the same results as the fast CPU, and every A32/T32 encoding has a generated decode test.

### I2. Differential fuzzing: fast CPU against the reference (1 week)
- **What:** random and corpus-derived instruction sequences (taken from real game code) run on
  both CPUs from the same state; any difference in registers, flags or memory is minimized to
  one instruction and filed. Runs in CI on a time budget and longer in the nightly run (A4).
- **Optional:** the same harness against a real AArch32 device (an ARM board or old Android
  phone) as a third opinion where the spec leaves behaviour implementation-defined.
- **Done when:** a deliberately broken flag in one handler is caught and minimized within the
  CI budget.

### I3. Profile-guided interpreter speedups (2–3 weeks)
- **What:** measure which instruction pairs and short sequences the nightly corpus actually
  runs most, and fuse the top ones into single handlers; link blocks directly where the target
  is fixed; tighten the dispatch path. Every change runs under I2 before merging.
- **Reference point:** Podish, a JIT-less x86 interpreter on iPhone, reports about 2× over iSH
  from pre-decoded blocks, tail-call dispatch, block linking and ~256 profile-chosen fused pairs.
- **Done when:** guest MIPS on the bench corpus is up by at least 1.5× with I2 clean.

### I4. Recompiled Packs: ahead-of-time translation, interpreter as the safety net (6–10 weeks)
- **What:** iOS forbids creating code at runtime but runs code that was compiled ahead of time
  and signed into the app. The bench records which blocks a game runs (nightly runs, replays,
  the auto-player), translates those blocks from ARMv7 to native ARM64 C/LLVM on the PC, and
  builds a per-game framework that is signed into HydrogenVM at the next sideload.
- **At runtime:** a block with a native version runs natively; anything else — code the pack
  doesn't cover, indirect jumps to unknown targets, code the game writes itself (Mono's JIT on
  Android) — falls back to the interpreter. Code retirement (`retireRange`) and `cacheflush`
  already make the handoff safe; a pack is ignored if the game binary's hash doesn't match.
- **Checked by:** I2 per block (native result against the reference CPU) and C1 replays
  (a whole session must produce the same frame hashes with and without the pack).
- **First target:** Dead Space (heavy, already working, good replay candidate).
- **Done when:** Dead Space runs with a pack on the phone, replays match, and the hot path is
  measurably faster.

### I5. Proof-checked fixes (folds into H1)
- Every change from the automated fix agent (H1) must pass I2 for CPU work and the C1 replay of
  the game it targets. A model's fix that can't be checked doesn't merge.

### I6. From Argon's blueprint (2026-11-17): what we adopted
Reviewed `HydrogenVM_Next-Gen_Engine__Agentic_Architecture__Ranked_Research_Blueprint.pdf`
against the current code. Several of its gaps were already closed (the Android dynamic linker,
Bionic, EGL, OpenSL ES and AAssetManager are in `src/runtime/android/`).
- **Adopted, next in Group A — 16 KB page protection (its #6, closes B-101/B-102):**
  host-page `mprotect` where all four 4 KB guest pages share one permission (zero cost on the
  hot path; today the CPU enforces no guest permissions at all), a small shadow check only for the
  few split pages, and the exact faulting instruction read from the signal context, verified to
  point into the current block, with today's `block_pc` as the fallback. Checked under `--phone`
  and by I2.
- **Adopted, narrowed — native bundled libraries (its #3, second half):** recognise statically
  linked zlib (inflate), libpng row filters and Tremor/Vorbis in a game binary by byte signatures
  at import and run host versions of those pure buffer-in/buffer-out functions. Not Box2D or
  other libraries whose structs and float results differ by version. Every swap is checked by
  replaying the game with and without it.
- **Already planned, confirmed:** AOT packs (its #4 = I4), record/replay (its #7 = C1, built
  first as seed + clock + frame-keyed input, then T7's deterministic scheduler), differential
  fuzzing against Unicorn (its #10 = I2), fix agent with proof (its #1 = H1/I5, using tests and
  replays as the proof rather than Z3), dead-server peers (its #11 = F1, taint tracking later).
- **Later, measured first:** per-flag liveness, trace blocks and verified superhandlers (its #8,
  #9) go into I3 only where the corpus profile shows a win.
- **Not adopted as written:** running DEX through the ARM `Op` pipeline (its #2). Dalvik has up
  to 65,535 registers per frame, objects, garbage collection and exceptions, so it gets its own
  interpreter (Phase D); our JNI is already host code, so there is no cross-VM boundary to remove.
  Metal presentation (its #5) waits for a profile from the phone showing presentation, not
  emulation, is the bottleneck.
- **Agreed anti-patterns (its #12):** no whole-binary superset translation, no models in the
  instruction or frame loop.

### I7. From Argon's blueprint v2 (2026-11-17): verified against the code
- **Doing first — floating-point fast path (its #1):** confirmed. iOS threads start in RunFast
  (`Process.cpp`, `fpscr = 0x03000000`), so every VFP/NEON op takes the "unlikely" flush path and
  classifies operands with `std::fpclassify`. Make the RunFast path the fast one (branchless
  exponent test), then consider VCMP+VMRS+branch fusion. Checked by `vfp_neon.S`, the float C
  tests and cpudiff; kept only if VFP-heavy benchmarks gain at least 25%.
- **Next, measured first:** objc_msgSend inline caching in the call op (its #2); qsort/bsearch
  without a host round trip per comparison (its #6, confirmed in `Stdlib.cpp`); streaming music
  instead of decoding whole tracks up front (its #8: confirmed full-track decode, ~42 MB per 4-minute
  stereo track; but AAC already decodes through the host, so only streaming and Vorbis are new).
- **Corpus-driven:** offline paths for dead ad/social SDKs (its #7) only where the nightly corpus
  or a report shows a game stuck on one, using each SDK's documented "offline/declined" path.
- **Phone app (Phase E):** GCVirtualController for on-screen controls, BGContinuedProcessingTask for
  large imports, IOSurface for UIKit-only frames (its #10). GL games already present on the GPU.
- **Into I4's design:** register promotion to locals and hot/cold layout from replay traces (its #9).
- **Not taken — already exists:** 64-bit argument alignment for Android (its #3: `ArgReader::dword`
  already aligns under `align64Args()` for Android processes); ARM EHABI table lookup (its #5:
  `dl_unwind_find_exidx` and `__gnu_Unwind_Find_exidx` are implemented from `PT_ARM_EXIDX`).
- **Not taken — wrong target:** 4-wide software shaders (its #4). On the iPhone, GL games run on the
  GPU through EAGL (`HVMEngine.mm`); the software rasterizer is the bench fallback.

## Order and timeline

| Weeks | Main track | In parallel |
|---|---|---|
| 1–2 | A1 reports, A2 phone mode | A3 APK Test All Games |
| 2–3 | A4 nightly run, A5 Android Test Lab | B1 NativeActivity, B2 packages |
| 3–6 | B3 system libraries, B4 graphics | B5 audio, B6 input/lifecycle, B7 engines (ledger-driven) |
| 5–8 | C1 record/replay, C2 auto-player | C3 safe mode, E1 faster launches |
| 8–15 | D DEX interpreter | E2 HD/widescreen, E3 controllers, G1 compatibility list |
| 12–16 | F1 server toolkit (Rayman) | E4 quick-save spike → build |
| 10–16 | H1 automated fix agent | H2 improvement packs, H3 on-device helpers |
| 3–5 | I1 reference CPU, I2 differential fuzzing | C1 record/replay (moved up: I4 and H1 depend on it) |
| 6–9 | I3 interpreter speedups | |
| 9–19 | I4 Recompiled Packs (Dead Space first) | I5 with H1 |

About four months of focused work in total, faster with several agents on the parallel
column. Phase A comes first because every later card is cheaper with it in place.

## Running it

- **Ownership by area:**
  - **Engine and Android runtime** (`src/runtime/android`, `src/core`): one agent at a time
    per file. `Process.cpp/.hpp`, `Engine.*`, `libc/System.cpp` and `Jni.cpp` are hotspots.
  - **App** (`app/`): one agent; Swift is verified by dispatching `ipa.yml` on the branch.
  - **Tools and CI** (`tools/`, `.github/`): parallel with either.
- **Every card:**
  - Branch from `main`.
  - Run the gates: fulltest, lint, tools/tests and Test Lab, plus the Android Test Lab once A5
    exists.
  - In the PR, say what was verified on the bench and what needs the phone.
  - Merge when green, build the IPA, send Lawson the link once the build succeeded.
- **The ledger drives Android order:** re-scan the corpus after each B card and pick the next
  fix by "games unblocked".
- **What Lawson provides:**
  - a broad APK corpus (15–30 games across engines, 2012–2018);
  - the Windows PC for A4;
  - the GitHub token for A1;
  - decisions on G2.
