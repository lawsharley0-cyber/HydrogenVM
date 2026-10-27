# HydrogenVM: handoff for Grok Bot (2026-10-24)

Newest continuation after `5040611`: [Arena 2 progression](clash-royale/ARENA_PROGRESSION.md)
adds original Bone Pit sectors, legal Arena-2 card pools, native thresholds and
entry/result/return checks; 38 focused tests and 43 private sector comparisons.
Boundary trophies are component fixtures, not an earned climb; Arenas 3–6,
draw/partial-crown accounting and full audits remain open. The owner's new
[random clan-event request](clash-royale/CLAN_EVENTS_PLAN.md) is recorded as planned
multi-bot social work after core arena/results. Training remains original and
advanced battle-AI refinement remains deferred. No shared src or device pass.

Newest October 27 continuation after `f8505aa`: [STORE_PURCHASES.md](clash-royale/STORE_PURCHASES.md)
supersedes the shop chest/currency backlog below. Original Silver/Golden/Magical
chests and all three Gem-for-Gold packs now have scoped native purchase/reveal/
relaunch evidence. Controlled before/after checkpoint interruption preserves
payment/reward accounting; three invalid pending records reject unchanged.
37 focused tests, actual fresh original training and retained clan checks pass.
Higher-cost component fixtures were explicitly funded; original earned and
phone-failing saves remain intact. All seven original trainers stay unchanged.
The owner defers advanced opponent AI refinement until every other feature and
bug fix is ready. Next: later-arena progression/results/profile accounting,
then full button/save/device/visual/audio/performance audits. This guest-only
stage changes no shared engine code and establishes no new iPhone or audio-
quality/performance verdict. Earlier chronological evidence follows.

October 27 Clash-specific continuation after `45f5a97`, on
`codex/clash-offline-continuation` / draft PR #138: read
[DAILY_SHOP.md](clash-royale/DAILY_SHOP.md) and the superseding banner in
[the complete agent handoff](clash-royale/AGENT_HANDOFF_2026-10-26.md).
Daily card offers/native purchases/retained counts/calendar rotation now have
scoped UI/save evidence and 36 focused tests. Shop chest/currency paths and
later arenas/full audits remain open. The owner requires all seven original
training names, decks, AI and tutorial behavior unchanged; new features stay
post-training. This guest-only work does not change main's shared alpha engine
or establish a new phone/audio-quality/performance pass.

This hands over the current work on HydrogenVM, a JIT-less 32-bit iOS `.ipa` emulator that runs on modern iPhones. The repo is `lawsharley0-cyber/HydrogenVM` and the integration branch is `main`. Read `AGENTS.md` (the working agreement) and this file. Open other docs only when a task names them.

The owner is Lawson. He tests on an iPhone with the app built by CI. He sends **batch reports** (`latest_batch_report.json` / `.md` from the in-app *Test All Games*) and occasionally run logs. He wants every game in his library to work. Talk to him in plain language, and give him a link to the new `.ipa` whenever a meaningful batch of fixes lands.

October 25 Clash continuation update (scoped to the owner-authorized PR #138
work): main 5968d88 is integrated into that draft at 5fbaa8d; it is not merged
into main. The current stat implementation retains the 108-byte INODE64 layout.
The separate guest IPA contains its own offline transport/persistence, and now
has seven earned training wins, native name, King 2/deck upgrades and two real
losses with no reward. v27 uses the original arena refresh to graduate that
earned save to Arena 1. Arena matchmaking/economy and later arenas still need
their guest port; earlier host-backend evidence is separate. Read
[UI_AUDIT.md](clash-royale/UI_AUDIT.md) and GUEST_MOD_PROGRESS for current hashes,
exact tests, device gaps and startup-helper timing correction. Shared runtime
compatibility is still required; no Clash-specific host service is used.

---

## 1. Ground rules (these are not optional)

- **No FairPlay decryption.** Never implement ciphers. Crypto and codecs go to host libraries (AudioToolbox, CommonCrypto, iconv, ffmpeg on Linux).
- **Never commit** IPAs, certificates, provisioning profiles, secrets, or game assets. Fixtures stay outside the repo.
- Treat downloaded and extracted files as untrusted. Use their own directory, run Python on them with `python3 -I`, and pass paths as arguments.
- Don't post on PRs or issues on the owner's behalf, and don't delete anything, unless asked. Exception: you may close the issue/PR you just merged, and open new issues that describe work for an agent.
- Don't touch **PR #138** (`codex/clash-offline-continuation`, Clash Royale; the owner runs it) or **#118** (an old failed Jules task).
- Commit trailers that this project uses:
  ```
  Co-Authored-By: <your model> <noreply@...>
  ```
  End commit messages with the attribution your platform requires. Write commit messages in plain sentences about behaviour: what now works and why.
- **Never fake success.** Don't return success from a stub when the real API would fail. Don't special-case a game by name. Fix the general behaviour so it matches iOS, and cite the evidence (the trace or disassembly) in the commit.

## 2. Build and test (run all of this before every push)

### Build (Linux)
```sh
cmake -S . -B build -G Ninja -DCMAKE_CXX_COMPILER=clang++ -DCMAKE_BUILD_TYPE=Release   # once
cmake --build build -j16                     # builds hvm, hvm_tests, ...
```
`ld64` for building the iOS test apps: `tools/build_cctools.sh` produces `/opt/cctools/bin/arm-apple-darwin11-ld`.

### The three gates (all must pass)
```sh
./build/hvm_tests fulltest                      # currently 253 passed, 0 failed
python3 tools/hvm_testlab.py run                # 13/13 (Test Lab: deterministic guest app, see docs/TEST_LAB.md)
HVM_LD64=/opt/cctools/bin/arm-apple-darwin11-ld python3 tools/hvm_report.py --out build/report-check
#   -> build 1/0, cpu 15/0, ios 6/0, bench 5/0; "problems: 1 perf" (CoreMark below the iPhone goal) is normal
```
The full report **builds and runs the iOS test apps** (CTest, ObjCTest, FoundationTest, UIKitTest...). A broken test app crashes Linux CI and **skips the IPA build**. That has happened before (PR #139), so always run the report locally.

### Add a test for every fix
- Runtime behaviour gets a check in `tests/runner/TestRunner.cpp`, in the `fulltest` block. Follow the existing examples: call HLE symbols with `proc.callGuest(cpu, *proc.lookupSymbol("_name"), {...})`, send ObjC messages with `r.send(cpu, obj, "sel:", {...})` and use `check(cond, "sentence")`. `r.send` returns `uint64_t`, so mask BOOL results with `& 0xFF` and ints with `uint32_t(...)`.
- Guest-level behaviour gets a check in the iOS test apps under `tests/ios/apps/*/main.{c,m}`. The test SDK headers live in `tests/ios/include` and are minimal. Declare what you need: see the Darwin signal structs in `ctest/main.c` and `object_getInstanceVariable` in `include/objc/runtime.h`.
- A real-app bug class gets a Test Lab case (`TL-X`), which must fail before the fix (docs/TEST_LAB.md).

### Run real games locally (the most important check)
The owner's IPAs are attachments in the conversation. Extract each one into a fixture directory `<fixtures>/apps/<bundleId>/` that holds the `.app` and acts as the home. Names must not end in spaces. Then:
```sh
build/hvm run <dir>/<Name>.app --home <dir> --run-for 40 --keep-going --screenshot out.png > out.log 2>&1; echo $?
#  exit 0 = still running when --run-for ended; 70 = crash/stop (the reason is in the log)
```
Useful flags: `--tap X,Y@SECONDS` (taps in **portrait screen points**, e.g. 320x568; for a landscape game take the screenshot pixel /2 *before* rotation), `--drag x1,y1,x2,y2@t`, `--gpu` (host GL path, closer to the device), `--trace` (logs every HLE call with caller and symbol), `--report r.json` (includes `audio.nonSilentFrames`), `--lifecycle`, `--graceful`.
Environment switches: `HVM_ZOMBIES=1` (names freed objects that get messaged, with free/alloc backtraces), `HVM_TRACE_BLOCKS=lo-hi` (hex; prints registers and `lr` at every block start in that range, single-steps, about 7 s per game, so it's cheap), `HVM_GL_DEBUG=1`.
Run them all in parallel, six at a time, with a loop like:
```sh
for d in fixtures/apps/*/; do app=$(ls -d "$d"*.app|head -1); id=$(basename "$d")
  ( timeout 100 build/hvm run "$app" --home "$d" --run-for 40 --keep-going --screenshot runs/$id.png > runs/$id.log 2>&1; echo "exit=$?" >> runs/$id.log ) &
  while [ $(jobs -r | wc -l) -ge 6 ]; do sleep 1; done; done; wait
for f in runs/*.log; do echo "$f $(tail -n1 $f) $(grep -m1 -E 'error\]|Crash|Stopped' $f)"; done
```
**Always look at the screenshot.** Exit 0 with a black or white screen is not a pass.

### Debugging recipe (this is how every fix today was found)
1. Read the crash block: pc, lr, registers and backtrace with symbol names.
2. Run with `--trace` and look at the last 30–60 HLE calls before the crash. These are usually the API we got wrong.
3. Disassemble around the pc. A small script that reads the Mach-O (fat slices: pick cputype 12 with subtype 9 or 11) and runs `llvm-mc --disassemble -triple thumbv7-apple-ios` or `armv7` works well. With `otool`, use `-arch armv7 -ov` for ObjC metadata, because otool otherwise prints the armv6 slice first and its addresses differ.
4. `HVM_TRACE_BLOCKS` around the function entry gives you its arguments and `lr` (its caller).
5. Mono and Unity: managed code turns faults into `NullReferenceException` through our signal delivery. The `[hvm] Signal 11 to the app's handler...` line and the `fault backtrace` that follows show where the fault happened.
6. Find out what iOS really does (objc4/CF/dyld source and documented behaviour) and do exactly that. Add a test, then rerun the game.

### Ship a device build
Put **`[mac]`** in the commit subject. CI (`.github/workflows`, "CI" run) then builds the IPA. A push to `main` **cancels an in-progress run**, so wait for the current run (~25–30 min) before pushing again, or push without `[mac]`. Get the artifact:
```sh
gh api "repos/lawsharley0-cyber/HydrogenVM/actions/runs?per_page=5&branch=main" --jq '.workflow_runs[]|select(.name=="CI")|"\(.id) \(.status) \(.conclusion) \(.head_sha[0:7])"'
gh api repos/lawsharley0-cyber/HydrogenVM/actions/runs/<RUN>/artifacts --jq '.artifacts[]|"\(.id) \(.name)"'
# link: https://github.com/lawsharley0-cyber/HydrogenVM/actions/runs/<RUN>/artifacts/<ID>   (HydrogenVM-device-ipa)
```
GitHub GraphQL (`gh pr list`) is blocked in some sandboxes. Use REST: `gh api repos/.../pulls?state=open`.

---

## 3. State at handoff

`main` = `2b60a06`. The latest device IPA is from `a086db5`: run 40126147019, artifact 12572933279.

Today's fixes:

| Commit | What |
|---|---|
| `01b1164` | SIGSEGV/SIGBUS delivered to the guest `sigaction` handler (Engine fault hook + `deliverGuestFault` in `src/runtime/libc/System.cpp`, Darwin siginfo/ucontext/mcontext). `object_getInstanceVariable` clears `*outValue` when the ivar is missing (MonoTouch). `dlopen(NULL)` returns `RTLD_DEFAULT` (-2), which Unity's `mono_dl_register_symbol` table needs. |
| `a086db5` | `std::filebuf` vtable (`LibStdcxx.cpp`); string-stream `__ZTV` symbols use GCC's layout (`LibStdcxxStreams.cpp`, address points +12 / +32 / ios); AVAssetWriter, CVPixelBuffer and CVOpenGLESTextureCache (`src/runtime/uikit/AVWriter.cpp`, new; frames are accepted and discarded); AVVideo* constants; `en0` + `sysctl(NET_RT_IFLIST)` MAC 02:00:00:00:00:00; `stringWithString:` of a constant returns the constant; software `glGetVertexAttrib*`. |
| `6bcfbee` | The first three guest-handled faults log a backtrace. |
| `2b60a06` | `-[NSKeyedUnarchiver initForReadingWithData:nil]` returns an empty unarchiver (iOS 6 behaviour); rejected data logs its first bytes. |

Local results (Linux, 40 s):

| Game | Result |
|---|---|
| Survivalcraft | Menu → New World → Play reaches the 3D world. Taps at (209,400), (247,260), (290,260). |
| Bad Piggies | Menu. Locally the picture isn't rotated because the accelerometer defaults to lying flat, so check it on the device. |
| Hungry Shark | Menu (the "Everyplay build expired" alert is the game's own). |
| Terraria | Menu → Play → Create Character. |
| The Sandbox | Name entry. |
| Talking Larry | Record → stop → share → YouTube form opens. Its buttons are invisible until `UIRoundedRectButton` exists. |
| Minion Rush | No crash, but still a white screen at 180 s. **Needs investigation.** |
| Goat Simulator | Black at 90 s. One run hit SIGSEGV → UE3 handler → abort (a race?). The device crash was in a TSparseArray/FES2FrameBuffer lookup. |
| Other games in the library | Mega Jump, Jetpack, Subway Surfers, Minecraft PE, Clumsy Ninja, Talking John, Retry, Army of Darkness and Pocket Ants still launch. |

---

## 4. Open tasks, in priority order, with how to finish each

### T1/T2. #140 and #141: done on main (commit after `910dc70`)
Implemented directly and verified with the real games:
- Loopback-only UDP: `Net.cpp` (`n_bind`/`n_sendto`/`n_recvfrom`/`n_getsockname`, `udpDestination`), `ioctl` FIONBIO/FIONREAD in `System.cpp`. Minecraft PE: Start Game reaches the world, and Join Game shows "Scanning for WiFi Games".
- `UIRoundedRectButton` (`UIControls.cpp`). Talking Larry's YouTube form shows Cancel / Upload.
- GKAchievement keeps identifier, percentComplete and completed (`GameKit.cpp`). Game Center still reports not signed in.
- Legacy lifecycle (`UIApplication.cpp` `legacyLifecycle`): `DTSDKName` < 4 or `UIApplicationExitsOnSuspend` get only resign/become active. Mega Jump is SDK 3.0.

Jules sessions for #140 (16888253082826464357) and #141 may still open PRs. Close them as superseded, or merge only a part that is genuinely additional (for example a same-frame touch test). Rebase it first, then rerun the gates.

### T3. Mega Jump has no sound on the device (owner report)
On Linux the SFX work. Tap 60,442@15 (decline OpenFeint), then 160,430@22 (PLAY). Measure with `--audio-fd` on a datagram socketpair and `--report`: nonSilentFrames is about 200k. The music is .m4a, which needs AudioToolbox, so it is silent on Linux without ffmpeg.

Most likely cause on the device: the Explore phase toggled the game's own music and speaker buttons, and that setting was saved in the real home. `c29ed49` now runs batch tests in a throwaway copy of the home (`BatchTest.prepareTestHome`). Ask the owner to check those two buttons. If it's still silent, get a run log and look for `AudioSession`/`AVAudioPlayer`/`OpenAL` warnings.

### T4. Minion Rush: white screen
The game draws and presents (436 `presentRenderbuffer:` calls in 75 s), but `HVM_DUMP_VIEWS=1` shows the window holds only its white root UIView: the `EAGLView` it created is never added to any view. Find out what normally adds it:
- `AppController initDisplay` / `showWelcomeScreen` / FreemiumIGP; disassemble them (`otool -arch armv7 -ov` gives the IMPs).
- Possibly a callback we never send, such as `viewDidAppear:` on EAGLViewController, a notification, or a network or IGP completion.

Dismiss the "No Internet connection" alert with `--tap 160,327@65`.

Newer finding: the game never calls `renderbufferStorage:fromDrawable:`, so its frames never reach a layer at all (the `--report` `display.offscreenPresents` stays 0 because nothing is presented to a drawable). Trace why the EAGLView setup path that would call it is skipped.

### Test All Games checklist (how the owner's batch reports read now)
Every game gets a checklist (`BatchChecks.swift`, `BatchCheckBuilder.build`): Launches, Shows a picture, Game view on screen, Audio output, Music, Sound effects, Responds to touch, Background & resume, Closes cleanly, Relaunch, Missing APIs, Handled faults, Speed. Each is pass / warn / fail / skip with a one-line detail, in the app, in `latest_batch_report.md` (a checklist table plus per-game lines) and in the JSON (`checks`, `diagnostics`).
- The numbers come from `HVMSession.diagnostics` (bridge) = `Mixer::stats()` + `Process::Perf` (`offscreenPresents`, `faultsToApp`) + `missingCount()`. The runner copies them every second, because they vanish when the game stops.
- Music = audible stream / looping / ≥8 s voice; Sound effects = the other audible voices. Audible means peak above 1e-3 at the app's own gain (before the player's volume). "Music: fail" means a `Missing: AVAudioPlayer: audio format not supported` line was logged; a "warn" means nothing was heard (often the game's own music setting).
- "Game view on screen: fail" means GL frames went to a layer not inside a visible window (`layerOnScreen`).
- Relaunch starts the game a second time on the data the first run saved (12 s).
- Headless: `hvm --report` now includes `mixer` and `display` objects; with no `--audio-fd`, a silent sink still runs the mixer so these numbers are real.

### T5. Goat Simulator (UE3, #125)
It's slow under the interpreter. Run `--run-for 300 --gpu`. The device crash was a NULL inside `TSparseArray<...FES2FrameBuffer...>` (a GL framebuffer map, called from `UOnlineSuppliedUIGameCenter`/`UInAppMessageIPhone`), possibly a GameCenter/UI-thread race. The 12 missing `glBeginQuery`/`glGenQueries` (EXT occlusion query) functions could make it take a different path. Implement them in the software renderer and in `tools/hostgl_spec.txt` honestly (queries report "any samples passed" = 1).

### T6. Clumsy Ninja FMOD trap (device only, #65/#109)
It passes on Linux. On the device it traps inside FMOD. Compare audio formats and rates between the device and Linux: AudioUnit/AUGraph callbacks and buffer frame counts. The 98-missing-function list includes `AUGraphGetIndNode`, `AudioUnitAddRenderNotify` and friends. Implement them truthfully.

### T7. Bad Piggies orientation and the rest of #121
Check on the device that it rotates (Unity 3.5 derives orientation from the accelerometer). If not, compare what `UIDevice orientation` and the accelerometer gravity report against the real phone.

### T8. Triage new device batch reports
The JSON layout is `d['suites']['apps']['apps'][i]` with fields `name`, `crashReport`, `exceptionKind`, `explore`, `fileHistory` and `logs`. For each crash, reproduce it locally with the recipe in §2, fix it, test it, and record it in `docs/dev-system/USER_APP_REPORTS.md` when relevant. Delegate well-scoped items to Jules by opening a GitHub issue with the evidence, files, tests and acceptance criteria (see #140/#141 as templates), then starting a Jules task that names the issue.

---

## 4b. Review status of branches and PRs (2026-10-25)
- **PR #138** (codex, Clash Royale offline) is a draft and still in progress; its own description says arena trophies are wrong. It also changes the "iPhone stat ABI", which conflicts with the reverted legacy-stat change that broke Unity, Mono and Clash at launch. **Not merged.** Merge only when the owner asks and the PR is no longer a draft. Rebase it first, keep the INODE64 stat layout, then run all gates and the full game sweep.
- **PR #118** (failed Jules task, Jetpack termination): its changes are already in main. It should be closed as superseded (needs the owner's go-ahead).
- **Old topic branches**: not merged into main by git, but checked with `git merge-tree`. They are either fully contained or were merged earlier in reviewed or edited form; what's left is superseded code, scratch files or duplicates.
  - `issue-34` (swaps VNMLA/VNMLS) contradicts the ARM ARM opc1=x01 table, and main passes `tests/asm/vfp_neon.S`, so it was rejected.
  - `fix-issue-124` was superseded by the zombie implementation.
- **Jules sessions for #140/#141**: the work is done on main. Stop or archive them, and close any PR they open unless it adds a genuinely new test.

## 5. Architecture notes that cost time to learn

- **Short guest paths:** apps see `/var/mobile/Applications/<UUID>/...`. Every host file access goes through `Process::hostPath`/`guestPath`. Homes come with a trailing `/`, which is trimmed, and `/private/var` is accepted.
- **`struct stat`:** plain `_stat`/`_fstat` fill the 108-byte INODE64 layout. A "legacy 96-byte" change broke Unity, Mono and Clash on the device and was reverted. Don't redo it.
- **Missing imports:** functions become stubs that report and return 0 (`--keep-going`). Cocoa-style data names become zero blocks. C++ vtables/VTTs become per-slot trap stubs that name the class and slot. **Weak** data imports that nothing provides are 0, except string constants matched by the resolver in `src/runtime/foundation/Register.cpp`. When an app does `ldr r0,[r0]` on a weak constant and faults at 0, add the constant there with its real iOS value.
- **Signal delivery:** `Engine::setFaultHook`. The PC is imprecise (block start), so handlers that redirect the PC work, and resuming at the same PC re-executes the block.
- **ObjC from host code:** `rt(x).send(...)`. Objects that need CFRetain/CFRelease must be real ObjC objects with a `HostObject` (see `AVWriter.cpp`).
- **libstdc++ streams:** our own layout with ios_base at +0x40 inside file streams. The vtables apps link against (`__ZTV...`) must have GCC's address points, because app code inlines constructors.
- **Networking is offline by design.** Reachability reports no network. TCP works only with `--loopback-network`. UDP is coming in #141.
- **App shell (Swift, `app/HydrogenVM/`):** Linux can't compile it. Only the `[mac]` CI proves it builds. Keep SwiftUI bodies small, because the type checker times out (see the FileBrowser sections).

## 6. When the owner asks for a build
Push with `[mac]`, wait for the `HydrogenVM-device-ipa` artifact, and send him the artifact link with a short summary: what now works, what to try on the phone, and what's still broken.
