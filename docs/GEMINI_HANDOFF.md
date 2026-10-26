# HydrogenVM: handoff for Gemini CLI (2026-10-25)

## Latest continuation — 2026-10-26

- **Audio:** RemoteIO callbacks now run on a guest audio thread every 10 ms, with
  format conversion and mixer output; normal pthread mutexes support the old-iOS
  creator/worker handoff. OpenAL gain queries now return the specified defaults.
  Hungry Shark produced 36,864 audible/music frames and Angry Birds 983,040;
  neither had audible mixer output before. Sound effects, clean close and
  relaunch remain unverified. TL-F/O/Q and fulltest cover the fixed behavior.
- **Minecraft Story Mode 1.1:** GLSL ES 3 named fragment outputs and texture
  aliases compile, program deletion follows GLES lifetime rules, and shared
  program state is locked against concurrent bind/create/delete/draw calls.
  Red-first fulltest cases cover all three bug classes. A 90 s software run
  rendered the Telltale legal screen and produced 3,289,088 audible/music frames
  instead of stopping in `glDrawArrays`. This is opening-screen evidence, not
  playable-game verification; one timed-stop host crash remained timing-dependent.
- **Still open:** Minion Rush crashes before its EAGL view is installed. Goat
  Simulator still lacks usable picture/report evidence. Do not call either fixed.
- **Verified gates:** fulltest 268/0, Test Lab 17/17, report build 1/0, cpu 15/0,
  ios 6/0, bench 5/0; only known warnings and the CoreMark perf notice remained.

- **Clumsy Ninja EAGL regression:** HostGL no longer applies the software
  renderer's cross-thread affinity rejection after creating a shared worker
  context. TL-N is the permanent guest regression: red before (`workerBound=0`),
  green after. `fulltest` also covers both software rejection and HostGL success.
- **Real-game evidence:** software showed the loading screen at 10 s with no
  handled faults. GPU ran 30 s, presented 240 frames, and had no old affinity
  warnings, but Linux readback captured a black screenshot. Do not call the
  device's picture or FMOD issue fixed until Lawson's next report confirms it.
- **Batch teardown:** the latest device symptom was a game that never delivered
  `onExit`, followed by unrelated games failing to reserve the 4 GB arena.
  `HVMEngine.mm` now logs each teardown stage and live arena count, and explicitly
  destroys the process outside `core->mu`. Test All Games reports an
  `engine-teardown-timeout`, saves a partial report, and stops instead of
  contaminating later results. The actual shutdown hang is **not fixed**; use
  the new `[teardown]` lines from the next device report to locate it (B-116).
- **Verified gates:** `fulltest` 260/0, Test Lab 14/14, `hvm_report.py` build 1/0,
  cpu 15/0, ios 6/0, bench 5/0; only the known CoreMark perf notice remained.
  On this Windows/WSL checkout ld64 was built as `build/cctools/bin/arm-apple-darwin11-ld`.

You are taking over engineering on **HydrogenVM**, a JIT-less emulator that runs 32-bit iOS `.ipa` games on modern iPhones. The repo is `lawsharley0-cyber/HydrogenVM`, branch `main`. The owner is **Lawson**. He tests on his iPhone with the `.ipa` that GitHub CI builds, runs the in-app **Test All Games**, and sends you the report. He wants **every game in his library to work reliably**. Talk to him in plain, short language. He is not a C++ developer.

Read this file first, then `AGENTS.md` (working agreement) and `docs/GROK_HANDOFF.md` (older, longer notes: architecture, debugging recipes, history). `docs/TEST_LAB.md` ("Agent guide") when you touch Test Lab. This file wins where they disagree.

---

## 0. Your machine (Lawson's Google Cloud VM)

- Debian 13, 2 CPUs, 8 GB RAM. Disk was 10 GB and **ran out once**; it is being enlarged to 50 GB. Check `df -h /` before big jobs. If it's above ~85 %, clean up (`~/apps/*/` copies you don't need, `/tmp/hvm*`, old `runs/`), never by deleting `~/ipas`.
- Repo: `~/HydrogenVM`. Built already: `build/hvm`, `build/hvm_tests`. Rebuild with `ninja -C build` (slow on 2 CPUs: a header change can take 20+ min; run it in `tmux`).
- `ld64` for the iOS test apps: `/opt/cctools/bin/arm-apple-darwin11-ld` (`HVM_LD64` is exported in `~/.bashrc`).
- `ffmpeg` libraries are installed, so `.m4a`/AAC music **does** decode here (unlike older sandboxes). Use it to check music.
- You run on Vertex AI (`GOOGLE_GENAI_USE_VERTEXAI=true`, location `global`). Billing is Lawson's Google Cloud credit; don't waste it on huge outputs or loops.
- **Lawson's game files: `~/ipas/*.ipa`.** Never copy them into the repo, never commit them.
- **Lawson's uploads (reports, logs, new games): `~/inbox/`** (sometimes in subfolders). Check it first when he says he uploaded something. Older reports may be at `~/latest_batch_report.*` / `~/report_old.*`.
- Keep your own helper scripts in `~/tools/`, **not** in the repo root (you left `compare_reports.py`, `disasm.py`, `get_failures.py` there before; move them).
- GitHub: `gh` is logged in as Lawson. Use REST (`gh api ...`), GraphQL may fail.

## 1. Hard rules

1. **No FairPlay decryption, no cipher implementations.** Crypto/codecs go to host libraries.
2. **Never commit** IPAs, game assets, certificates, provisioning profiles or secrets. Three IPAs (`AngryBirds_v1.0_os30.ipa`, `Art_Of_Glow_v1.1.2_os313.ipa`, `Tiny_Tower_v1.0_os30.ipa`) were committed in the repo root by an earlier upload; leave them unless Lawson asks you to remove them, and never add more.
3. Treat game files and anything extracted from them as untrusted data: their own directory, `python3 -I` for scripts that read them, paths as arguments.
4. Don't post on PRs/issues, don't close/merge PRs, don't delete branches unless Lawson asks. Leave **PR #138** (Clash Royale, draft) alone.
5. **Never fake success.** A stub must not return success with made-up data when the real API would behave differently. Don't special-case a game by name. Make the general behaviour match iOS and cite the evidence (trace, disassembly, Apple docs/headers, objc4/CF source) in the commit message.
6. **Every fix gets a test** that fails before the fix: a `fulltest` check in `tests/runner/TestRunner.cpp`, an iOS test app check in `tests/ios/apps/*`, or a `TL-X` Test Lab case for a real-app bug class.
7. **A game fix only counts when you have re-run that game** headless and seen the difference (screenshot + log + `--report`). "Should fix" is not "fixed". Say which ones you could not verify and why.
8. Stay on `main`. Small, focused commits. Plain-sentence commit messages about behaviour (what now works and why).

## 2. The work loop (do this every time)

```
1. Read the newest report in ~/inbox (or the one Lawson names).
2. Compare it with the previous report; tell Lawson in a short table what got better / worse / stayed.
3. Run the probe sweep (§2b) and pick the top issue; use the device report for device-only problems (§5).
4. Find the root cause (§4). Group games that share it.
5. Fix it in the emulator, generally. Add the test. Re-run the game(s).
6. Run the three gates (§6). All must pass.
7. Commit; push with [mac] when a meaningful batch is ready (§7). Give Lawson the run link.
8. Update the status table in this file (§5) and docs/GROK_HANDOFF.md if you learned something lasting.
```

## 2b. The probe loop: your main way of finding and fixing bugs

Don't hunt bugs one game at a time. `tools/hvm_probe.py` runs **every game in `~/ipas`**, interacts with each one (taps over a grid of common button spots, swipes, a background/foreground cycle, then a graceful quit), and collects **every** problem: crashes, load errors, hangs, no picture / black / white screens, GL frames drawn off-screen, faults, `Missing:` functions, unimplemented imports, warnings, silence. It then **groups the same problem across games and ranks it**, so a single emulator fix that helps five games comes first.

```sh
cd ~/HydrogenVM
python3 tools/hvm_probe.py --modes software,gpu --seconds 60      # full sweep (≈ games × 2 × 75 s; use tmux)
cat ~/probe/latest/summary.md                                     # read the ranked issue list
```
Each run's evidence is in `~/probe/<time>/<bundleId>-<mode>/`: `run.log`, `report.json`, `shot.png`, `script.txt` (the exact actions, so you can replay them with `build/hvm run ... <same args>`).

**The loop (repeat until the stop condition below):**
1. **Sweep:** `python3 tools/hvm_probe.py --modes software,gpu --baseline ~/probe/latest/summary.json` (the `--baseline` makes the summary list ✅ fixed / 🆕 new / still present against the previous sweep). First sweep: leave out `--baseline`.
2. **Take the top issue** in `summary.md` (highest severity, then most games). Severity: 100 crash/load error, 90 hang, 80 no picture/black/white, 70 GL off-screen, 60 logged error, 50 faults to the game's handler, 45 didn't finish quitting, 40 missing function, 30 unimplemented import, 20 warning, 10 silent.
3. **Reproduce it on one game** from the "Games" column: replay the `script.txt` arguments, or `python3 tools/hvm_probe.py --only <bundle or file name> --modes <mode>`. Add `--trace` / `HVM_TRACE_BLOCKS` / `HVM_ZOMBIES=1` as in §4.
4. **Fix the root cause in the emulator**, generally (rule 5). Add the test (rule 6).
5. **Re-probe just the affected games** (`--only ...`), confirm the issue is gone from their `summary.md`, and check nothing new appeared.
6. When you have a batch of fixes (or every ~2–3 fixes): run the **gates** (§6), commit, then the **full sweep with `--baseline`**. Any 🆕 new issue at severity ≥ 45 is a regression from your change: fix or revert it before pushing.
7. Push with `[mac]` (§7) and give Lawson the link plus the fixed/new/still counts.
8. Go back to 1.

**Stop condition ("all major bugs fixed"):** a full `--modes software,gpu` sweep has **no issue with severity ≥ 45 in any game**, and Lawson's next device report agrees (no failed checks). Then work down severity 40/30 (missing functions on code paths games actually use), then sound, then speed.

**Rules for the probe:**
- Never "fix" an issue by weakening the probe, hiding a warning, or making a stub claim success. The probe is the measuring stick; change it only to measure better (and say so in the commit).
- Run with `--jobs 1` on this 2-CPU VM when timing matters (quit watchdog, hangs, speed); heavier parallelism makes slow games look stuck. Re-run a severity-45 "did not finish quitting" issue alone before acting on it.
- The probe's taps are blind. When a game needs specific taps to get past a menu (to reach gameplay where the deeper bugs are), add them to `tools/probe_scripts/<bundleId>.txt` as `hvm run` arguments (`--tap X,Y@SECONDS`, `--drag x1,y1,x2,y2@T`). Mega Jump, Survivalcraft and Minion Rush already have one. Minion Rush needs `--seconds 90` (its alert tap is at 65 s). Commit these scripts.
- Use `--launches 2` now and then: it starts each game a second time on its saved data (save-file bugs).
- Linux ≠ the phone. Issues that only happen on the device (Lawson's report) still matter; reproduce them with the probe in `--modes gpu` first, and if you can't, say so.
- First probe finding (2026-10-25, 3 games): Angry Birds, Minecraft PE Lite and Talking Larry all hit "did not finish quitting" after the probe's taps and background/foreground cycle, but quit fine without the taps. That's a shared bug in the quit/lifecycle path: start there.

## 3. Running a game headless

`hvm` does **not** take an `.ipa` directly. Import it once (unzips into an app folder that doubles as the game's home), then run the `.app`:

```sh
mkdir -p ~/apps
build/hvm import ~/ipas/Bad_Piggies_1_0_0.ipa ~/apps          # prints bundle: ~/apps/<bundleId>/<Name>.app
APP=$(ls -d ~/apps/com.rovio.BadPiggies/*.app)
build/hvm run "$APP" --home ~/apps/com.rovio.BadPiggies --run-for 60 --keep-going \
    --screenshot ~/runs/badpiggies.png --report ~/runs/badpiggies.json > ~/runs/badpiggies.log 2>&1; echo "exit=$?"
# exit 0 = still running when --run-for ended; 70 = crash/stop (reason in the log)
```
- Imported apps take disk space (each ≈ the unzipped game). Import only what you're working on; `rm -rf ~/apps/<bundleId>` when done (that also resets its saved data).
- **Always look at the screenshot** (`--screenshot`). Exit 0 with a black/white picture is a failure.
- `--report` JSON has `mixer` (framesRendered, musicFrames, effectFrames, peak), `display` (offscreenPresents, faultsToApp, missingCount) and audio counts. This mirrors the device checklist.
- Flags: `--gpu` (host GL path, closer to the phone; use it for GPU-mode failures), `--tap X,Y@SECONDS` (portrait points, 320x480 / 320x568), `--drag x1,y1,x2,y2@t`, `--trace` (every HLE call with caller), `--lifecycle`, `--graceful`, `--audio-fd`.
- Env: `HVM_ZOMBIES=1` (messages to freed objects), `HVM_TRACE_BLOCKS=lo-hi` (hex range; registers + lr at each block), `HVM_DUMP_VIEWS=1` (view hierarchy), `HVM_GL_DEBUG=1`.
- With 2 CPUs, run at most 2 games at once. Use `timeout` so a hung game can't stall you.

## 4. Debugging recipe

1. Read the crash block in the log: pc, lr, registers, backtrace with symbol names. The report's `crashReport` / `logs` / `fileHistory` have the device side.
2. `--trace` and read the last 30–60 HLE calls before the failure; usually the API we got wrong is there.
3. Disassemble around the pc (`llvm-mc --disassemble -triple thumbv7-apple-ios` on the armv7 slice; with otool use `-arch armv7`).
4. `HVM_TRACE_BLOCKS` around a function entry to get its arguments and caller.
5. Mono/Unity games turn faults into managed exceptions through our signal delivery; look for `[hvm] Signal 11 to the app's handler` and the `fault backtrace` after it.
6. Find what iOS really does (headers, docs, open-source objc4/CoreFoundation/dyld) and implement exactly that.
7. "Missing:" log lines list APIs the game called that we don't implement. Implement the ones on the failing path, truthfully.

## 5. Library status and priorities

All of these are in `~/ipas` unless marked. "Device" = Lawson's last Test All Games report (before commit `6884670`). The 18 "failed to reserve 4 GB guest arena" results in that report were a **test-runner bug** (a stopped game's memory not released before the next started), addressed in `3f6db19`; those games' real status is from earlier runs and must be re-checked with the next report.

| Game (bundle id) | Device result (last report) | Known / next step |
|---|---|---|
| Angry Birds (com.clickgamer.AngryBirds) | ✅ OK, 41/52 fps | Keep working (regression guard). |
| Art of Glow (com.natenai.artofglowads) | ✅ OK, 50–60 fps | Regression guard. |
| Mega Jump (com.getsetgames.megajump) | ✅ OK, 34/55 fps | Earlier "no sound" report; check Music/Sound effects checks. Music is .m4a: verify it decodes here. Taps: 60,442@15 (decline OpenFeint), 160,430@22 (PLAY). |
| Jetpack Joyride (com.halfbrick.jetpack) | ✅ OK, 12/52 fps | Software mode slow. |
| Minecraft PE Lite (com.mojang.minecraftpe-demo) | ✅ OK, 19/54 fps | Start Game reaches the world; Join Game scans LAN. |
| Clash Royale (com.supercell.scroll) | ✅ OK | Not in ~/ipas. PR #138 is Lawson's; don't touch. |
| Pocket Ants (com.concretesoftware.anthill) | ⚠️ "OK" but 0 fps, ignores quit | Picture never changes; find out if it's stuck. It also ignores the quit request (had to be force-stopped). |
| Case Program (com.apple.store.caseprogram) | ⚠️ 0 fps | Not in ~/ipas. Low priority. |
| **Minion Rush** (com.gameloft.despicableme2) | ❌ white screen | Game creates an EAGLView but never adds it to the window and never calls `renderbufferStorage:fromDrawable:`. `6884670` added UIWindow key/visible notifications: **verify** with `HVM_DUMP_VIEWS=1`. Dismiss "No Internet" alert with `--tap 160,327@65`. Trace `AppController initDisplay` / `showWelcomeScreen` / FreemiumIGP. |
| **Bad Piggies** (com.rovio.BadPiggies) | ❌ abort() at start (both modes) | `6884670` made `AudioUnitGetProperty` return a stream format / max frames: **verify** it now starts. Then check orientation (Unity 3.5 uses the accelerometer). |
| **Hungry Shark** (com.fgol.HungrySharkEvolution) | ❌ abort() (software), NULL read at 0x21 (GPU) | Same audio change as Bad Piggies may cover the abort; the GPU NULL crash may be separate. Verify both modes. "Everyplay build expired" alert is the game's own. |
| **Clumsy Ninja** (com.naturalmotion.clumsyninja) | ❌ trap in GPU mode; software OK 15 fps | FMOD audio path. `6884670` added `AUGraphGetIndNode`/`AudioUnitAddRenderNotify` etc.: **review**: `AUGraphGetIndNode` always returning node 1 is a fake answer; make the AUGraph model real (track nodes added with `AUGraphAddNode`). |
| **Army of Darkness** (com.backflipstudios.armyofdarknessdefense) | ❌ bad access at 0xfffffbe0 (software), black (GPU), ignores quit | Unexplored. Reproduce both modes. Old branch `106-army-of-darkness-rbegin-abi-*` hints at a libstdc++ `rbegin` ABI issue: read it, don't merge blindly. |
| **Goat Simulator** (com.coffeestainstudios.goatsimulmodifed) | ❌ black, ignores quit | UE3, very slow under the interpreter: `--run-for 300 --gpu`. `6884670` stubbed `EXT_occlusion_query_boolean`: queries must truthfully report "samples passed" = 1. Device crash before was NULL in `TSparseArray<FES2FrameBuffer>` (GameCenter/UI-thread race?). Issue #125. |
| **Minecraft: Story Mode** (com.telltalegames.MC100) | ❌ no frames | Not in ~/ipas (ask Lawson if needed). `6884670` stubbed `glDrawBuffers`/`glReadBuffer`: verify with the next report. |
| RETRY (com.rovio.retry) | arena bug (re-check) | Earlier: launches. |
| Subway Surfers (com.kiloo.subwaysurfers) | arena bug (re-check) | Earlier: launches. |
| Survivalcraft (com.candyrufusgames.survivalcraft) | arena bug (re-check) | Earlier: menu → New World → Play reaches 3D world (taps 209,400 / 247,260 / 290,260). |
| Talking John (com.outfit7.talkingbacteria) | arena bug (re-check) | Earlier: launches. |
| Talking Larry (com.outfit7.talkingbird) | arena bug (re-check) | Earlier: record/share/YouTube form works. |
| Temple Run (com.imangi.templerun) | arena bug (re-check) | Not in ~/ipas. |
| Terraria (com.505games.terraria) | arena bug (re-check) | Earlier: menu → Play → Create Character. |
| The Sandbox (com.bulkypix.thesandbox) | arena bug (re-check) | Earlier: name entry. |
| Tiny Tower (com.eeenmachine.tinytowers) | arena bug (re-check) | Earlier: launches. |

**Priority order:** (1) anything that makes many games fail at once (runner/memory/arena, shared audio or GL paths); (2) crashes at launch; (3) no picture / white / black screen; (4) no input; (5) no sound or music (a Music **warning** can just mean the game's own music setting is off; a **fail** means a file couldn't be decoded); (6) background/resume, close, relaunch; (7) speed.

**Commit `6884670` was written without running the affected games** (they weren't on the VM yet). Before building on it, re-run Minion Rush, Bad Piggies, Hungry Shark, Clumsy Ninja, Goat Simulator and AoD headless, and fix or revert anything that is a fake answer (rule 5) or doesn't help.

## 6. Gates (all must pass before every push)

```sh
./build/hvm_tests fulltest                                                     # 253 passed, 0 failed (more as you add tests)
python3 tools/hvm_testlab.py run --hvm build/hvm --out build/testlab-report     # 13/13
python3 tools/hvm_report.py --skip-build --out build/report-check              # cpu 15/0, ios 6/0, bench 5/0; "1 perf" is normal
```
`hvm_report.py` builds and runs the iOS test apps; a broken test app breaks Linux CI and **skips the IPA build**, so always run it. Drop `--skip-build` after changing the build itself.

The Swift app (`app/HydrogenVM/`, including Test All Games: `BatchTest.swift`, `BatchExplore.swift`, `BatchChecks.swift`, `BatchTestView.swift`) **cannot be compiled on Linux**. Only the `[mac]` CI proves it builds. Keep Swift edits small and careful, keep SwiftUI bodies short (the type checker times out), and check the macOS job result after pushing.

## 7. Shipping a build to Lawson

- Put **`[mac]`** in the commit subject and `git push origin main`. CI ("CI" workflow) builds the device `.ipa` (≈25–30 min). A new push **cancels** an in-progress run: batch your fixes, don't push every few minutes.
- Find the run and tell Lawson:
  ```sh
  gh api "repos/lawsharley0-cyber/HydrogenVM/actions/runs?per_page=5&branch=main" --jq '.workflow_runs[]|select(.name=="CI")|"\(.id) \(.status) \(.conclusion) \(.head_sha[0:7])"'
  ```
  Send him `https://github.com/lawsharley0-cyber/HydrogenVM/actions/runs/<RUN_ID>` and remind him to **sign in to github.com first** (artifact links 404 otherwise) and download **HydrogenVM-device-ipa**.
- With the link, give a short summary: what should now work, what to try on the phone, what's still broken.
- The latest device build when this was written: run `40164535159` (commit `6884670`), success.

## 8. How the device report works (so you read it right)

- JSON: `d['suites']['apps']['apps'][i]` with `bundleId`, `name`, `renderer` (software/gpu), `crashed`, `summary`, `crashReport`, `logs`, `fileHistory`, `explore`, `checks` (list of `{name, status, detail}`), `diagnostics` (mixer and display counters).
- 13 checks per game: Launches, Shows a picture, Game view on screen, Audio output, Music, Sound effects, Responds to touch, Background & resume, Closes cleanly, Relaunch, Missing APIs, Handled faults, Speed. Built in `app/HydrogenVM/BatchChecks.swift`.
- Each game runs twice (software and GPU renderers), in a throwaway copy of its data, so the test never changes Lawson's saves.
- "Game view on screen: fail" = GL frames went to a layer that isn't inside a visible window. "Missing APIs" lists `Missing:` log lines.

## 9. Architecture notes that cost time to learn

- Guest paths look like `/var/mobile/Applications/<UUID>/...`; all host file access goes through `Process::hostPath`/`guestPath`.
- `struct stat` uses the 108-byte INODE64 layout. A "legacy 96-byte" change broke Unity/Mono/Clash and was reverted. Don't redo it.
- Missing imports become stubs that log `Missing:` and return 0 (`--keep-going`). Weak data imports nobody provides are 0, except string constants in `src/runtime/foundation/Register.cpp`: when a game does `ldr r0,[r0]` on a weak constant and faults at 0, add the constant there with its real iOS value.
- Signals: `Engine::setFaultHook` → `deliverGuestFault` (`src/runtime/libc/System.cpp`). PC is the block start.
- ObjC from host code: `rt(x).send(...)`. Objects needing CFRetain/CFRelease must be real ObjC objects with a `HostObject` (see `src/runtime/uikit/AVWriter.cpp`).
- Audio: everything mixes through `Mixer::render` (`src/runtime/audio/`). Decoders: `Decode.cpp` (WAV/AIFF/MP3/IMA…), `HostDecode.cpp` (AAC/m4a via host).
- GL: software renderer and the host GL path (`--gpu`). Host GL entry points are generated from `tools/hostgl_spec.txt` by `tools/gen_hostgl.py` into `src/runtime/gl/HostGLTable.inc`: edit the spec and regenerate, don't hand-edit the `.inc`.
- Networking is offline by design (Reachability = no network); loopback UDP works for LAN games.

## 10. Talking to Lawson

- Start each session by saying in one or two lines what you're about to do.
- After each batch: a short table (game → before → after), the build link, and what he should test on the phone.
- Ask him only for things you can't get yourself: a new device report, a game file that isn't in `~/ipas`, a device-only log, or a decision (e.g. removing files from the repo). Otherwise make the reasonable call and continue.
- If something can't be checked on Linux (device-only crash, orientation, real performance), say so plainly and tell him exactly what to look at on the phone.
