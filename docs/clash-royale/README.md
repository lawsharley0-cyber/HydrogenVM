# Clash Royale on main

**Latest continuation:** read [PROGRESS.md](PROGRESS.md) first. October 23 work
on current main reaches a native three-crown NPC victory. Phone-local service,
return-home processing and earned persistence remain open; the historical gap
table below is not the current implementation inventory.

Cold-start plan for the pinned Clash Royale client. Another agent should read this file, then [`TODO.md`](TODO.md), before editing code. This is the current plan against `main`. `docs/CLASH_ROYALE_RESTORATION.md`, `docs/HANDOFF.md`, and `docs/LOCAL_TESTING.md` are earlier snapshots written while the integration branch was `gemini-merge`. Where they disagree with this directory, follow this directory.

Tags used below:

- **VERIFIED** — reproduced in this work session, with the command or the file location.
- **REPORTED** — stated by the October 21, 2026 handoff or by an older doc, and not re-run here.
- **UNKNOWN** — no successful observation exists, including cases the handoff explicitly left open.

`main` for this write-up is `606301f3268005bdf57aa207edd1b61a4ffd2299` (`origin/main` on October 22, 2026).

## Goals

From the handoff. The client stays the archived, unencrypted Clash Royale **2.1306.1** ARMv7 build. HydrogenVM should run it as a persistent single-player game with NPC battles, eventually across the arenas that the client actually implements. Sound stays in scope the whole way. The first milestone is one functional game; bugfix, compatibility, and performance work follow that.

The route is shared emulator fixes plus an explicit local protocol peer. Native simulation and AI stay in the client wherever the client implements them. The game executable, original assets, and bundled settings are not modified.

A finished result can load the training tutorial, finish that tutorial, win one NPC battle, return home, and show the earned progress again after a relaunch. Startup, a rendered frame, or a green probe is not that result.

## Current status

### What this session ran

| Claim | Tag | Evidence |
|---|---|---|
| `origin/main` is `606301f` and contains the GLSL ES 1.00 translator from merged PRs #8 and #9 | VERIFIED | `git rev-parse origin/main`, `git log`, `gh pr list` |
| `AGENTS.md` is absent on `main` | VERIFIED | `git cat-file -e origin/main:AGENTS.md` fails. Open PR #10 adds that file |
| Clash unit tests pass: 17 tests, 0 failures | VERIFIED | `python3 -m unittest discover -s tools -p 'test_clash_*.py'` (the original 12, plus the 5 turn tests under `tools/clash/` now that it is a package) |
| Focused turn-decoder discover passes: 5 tests, 0 failures | VERIFIED | `python3 -m unittest discover -s tools/clash -t tools -p 'test_*.py'` |
| `encode_avatar` is a shared helper and the home fixture tests still pass | VERIFIED | `tools/clash_home_state.py` after the split; the 6 home tests above |
| `tools/clash/clash_sector_state.py` imports | VERIFIED | `import clash_sector_state` after adding `tools/` to `sys.path`. `load_template` was not called: it needs the IPA |
| `tools/clash/clash_desktop.py --help` exits 0 | VERIFIED | That command. The wrapper was not run against a peer or a window |
| Main's VInt writer does not emit the native 5-byte forms | VERIFIED | Python snippet against `tools/clash_wire.py`, output below |
| Loopback IPv4 TCP and hostname aliases exist in the engine | VERIFIED | `src/runtime/libc/Net.cpp`, `src/runtime/Process.hpp`, `tests/runner/HvmRun.cpp` (`--loopback-network`, `--loopback-alias`) |
| `hvm_tests audio-decode` already exists | VERIFIED | `tests/runner/TestRunner.cpp` `main`, argument `audio-decode` |
| `AudioFileOpenWithCallbacks` already rejects empty PCM with `1954115647` | VERIFIED | `af_openWithCallbacks` in `src/runtime/uikit/PlatformStubs.cpp` |
| `AudioFileOpenURL` still returns success after an empty decode | VERIFIED | `af_openURL` in the same file writes the file id and returns 0 |
| No `HostDecode.cpp`, no FFmpeg link, no `observeEntry` | VERIFIED | Repository search on `main` |
| GLSL ES `min`/`max`, a real preprocessor, user functions, derivative builtins, and a per-draw instruction budget are in `src/runtime/graphics/Shader.cpp` | VERIFIED | Code inspection of `main`. Clash's captured shader corpus was not recompiled |
| The pinned IPA is not in this workspace | VERIFIED | No `ClashRoyale*.ipa`. The manifest marks `clash-royale-2-1306-1` optional. The harness treats a missing file as a skip |
| Handoff screenshot file is a 640×1136 8-bit RGBA PNG, 1,396,822 bytes, and is not a flat color field | VERIFIED | PNG IHDR plus an ffmpeg raw sample: mean RGB about 144, 158, 90; about 6.6% of sampled pixels near white. The picture was not re-rendered by HydrogenVM |
| `hvm_tests fulltest` on this `main` tree | VERIFIED | `/tmp/hvm-main-build/hvm_tests fulltest` printed `fulltest: 148 passed, 0 failed`. Built with g++. The log has no entry-observer checks and no `AudioFileOpenURL rejects undecodable audio` line |

VInt comparison run against current `tools/clash_wire.py` (writer bytes versus the native literals from the handoff):

| Value | Writer on `main` | Native literal | Reader given the native literal |
|---|---|---|---|
| `-64` | `40` | `c07f` | `-64` |
| `-8192` | `c000` | `c0807f` | `-8192` |
| `INT_MIN` | `c080808070` | `c080808000` | raises `VInt exceeds signed 32-bit range` |
| `INT_MAX` | `bfffffff0f` | `bfffffff0f` | `2147483647` |
| `-134217728` | `c0808000` | `c08080800f` | raises `VInt exceeds signed 32-bit range` |

The existing round-trip test still passes because the writer and the reader share the same encoding. That test does not lock these native literals.

### What the handoff reported about the old build

These results are from local commit `a33f063` (merge of unpushed `65f7e0b` onto `06a7b16`). Neither `65f7e0b` nor `a33f063` is on `origin/main` or on `origin/gemini-merge`. `06a7b16` is on `main` via merged PR #1.

| Claim | Tag | Evidence |
|---|---|---|
| Training arena and original tutorial render, with cards, elixir, six towers, dialogue, voice, music, highlighting, and input, through the loopback peer | REPORTED | Handoff section 1 and `latest-tutorial-verification.json` |
| Latest merged-runtime tutorial init: 311.14 s, exit 0, `errors=[]`, home accepted, tutorial voice loaded, one NPC fixture, audio 13,699,072 frames / 9,175,937 non-silent / peak 0.932504 / 0 drops | REPORTED | `latest-tutorial-verification.json`. Mixer capture, not a listening test. Engine SHA-256 `aa5573204e4f0d2c4a560e9cb82521e90a862da7dac98647b4368a8eeff9d6a2` |
| That picture is the tutorial scene | REPORTED | Handoff caption. This session only checked that the PNG is a non-flat 640×1136 image |
| 87 runtime checks, 0 failed, on that merged binary | REPORTED | Handoff `runtime-checks.txt` ends with `fulltest: 87 passed, 0 failed` |
| Four-app 15 s scan on `a33f063`: Angry Birds 24.4 FPS, Art of Glow 15.1, Clash 8.1, Tiny Tower 27.4, Tiny Tower's last frame solid black `#000000` | REPORTED | Handoff `merged-regressions.md`, scanned 2026-10-21T17:57:45 |
| 26 captured shaders compile, including the arena shadow shader; 107 IMA WAV assets match libsndfile; 17 M4A assets plus two synthetic AAC samples match FFmpeg CLI with max difference 0 | REPORTED | Handoff section 5. Linux M4A check shares libavcodec with the CLI, so it checks demux and conversion |
| Apple AudioToolbox M4A path compiled and played on a device | UNKNOWN | Handoff: that path was not compiled or device-tested |
| A complete NPC win, rewards, return home, next mission, or earned progression | UNKNOWN | Handoff states these were not verified. Earlier runs saw a Knight deploy and combat audio on a noncanonical tutorial id `49000000` |
| `GameMode::saveOffline` at `0x925b4` is `bx lr; nop` | REPORTED | Handoff and `docs/CLASH_ROYALE_RESTORATION.md`. `tools/clash_royale_probe.py` checks the Thumb prefix `7047`. This session had no IPA, so the bytes were not read again |
| Identity token and the initial home JSON survive relaunch | REPORTED | Restoration doc and the home-probe tests. That is the initial fixture, not earned progress. Handoff saved-home SHA-256 `3a93d54bb5223790b0591424957381ec75e9d347a62be2295abc8ff5544b3ae8` |
| Experimental desktop battle wrapper | UNKNOWN | Handoff: not smoke-tested. On `main` the probe also lacks `--npc-fixture`, and `desktop.py` does not forward loopback flags |
| GitHub Actions for this working tree | UNKNOWN | Handoff: later Actions attempts hit billing limits before steps ran. Do not treat an old green run as current CI |

### Open pull requests on October 22, 2026

| PR | State | Relation to this work |
|---|---|---|
| #4 homogeneous clipper | OPEN | Group B, `OpenGLES.cpp` and `TestRunner.cpp`. Re-test Clash drawing after it merges. Whether the tutorial needs it is UNKNOWN |
| #5 POSIX regex and `tmpnam` | OPEN | `Regex.cpp`, `Stdio.cpp`, `TestRunner.cpp`. The October 21 Clash gap list does not name `regcomp`. Art of Glow does |
| #6 frame histograms | OPEN | Edits `tests/runner/HvmRun.cpp` in the same function that entry tracing must hook |
| #7 Codemagic iOS CI | OPEN | Also edits `app/project.yml`, `tools/clash_home_probe.py` (macOS datagram buffer), and `app/HydrogenVM/FileBrowser.swift` (compile fixes). Rebase Clash edits onto it if it lands first |
| #10 re-land `AGENTS.md` | OPEN | Copy the README pointer into `AGENTS.md` after it reaches `main`. This branch does not add `AGENTS.md` |
| #11 constant array sizes up to 2048 and `#extension : warn` | OPEN | Shader translator. Re-run the Clash shader dump after it merges |
| #12 uniform arrays in a side buffer and register recycling | OPEN | Same files as #11 |

Merged and already on `main`: PR #1 (`gemini-merge` at `06a7b16`), PR #2 (dev-system docs and harness), PR #8 and PR #9 (GLSL translator).

## Gap analysis

The handoff's local branch was `gemini-merge` at `a33f063`, two commits ahead of fetched `origin/gemini-merge` (`06a7b16`), plus an uncommitted patch and four untracked Python files. The comparison below is against `main` at `606301f`. Coverage is **covered**, **partial**, or **missing**.

### Already covered, because `06a7b16` is in PR #1

Leave these in place. The handoff depends on them and `main` already has them.

- Opt-in loopback TCP and explicit hostname aliases, including `game.clashroyaleapp.com`.
- RC4 login peer, pinned version `(2, 0, 1306)`, account `(0, 1)`, and the 184-byte initial home/avatar fixture.
- `tools/clash_protocol_probe.py`, `tools/clash_home_state.py`, `tools/clash_home_probe.py`, `tools/clash_wire.py`, `tools/clash_royale_probe.py`, and their 12 unit tests.
- IMA WAV decoding and `tools/test_ima_wav_corpus.py` (the corpus itself needs the unpacked client).
- `tools/desktop.py` pygame viewer and `hvm_tests audio-decode`.
- `AudioFileOpenWithCallbacks` rejecting an empty decode.

### Commit `65f7e0b` (not on any remote)

| Change | Coverage | What to do |
|---|---|---|
| GLSL `min`/`max` and object-like precision aliases (`#define pr highp`, empty `#define`) in the old straight-line interpreter | covered | `main`'s translator already has component-wise `min`/`max` (`minMax` in `Shader.cpp`), a full ES 1.00 preprocessor including object-like macros, user functions, control flow, `GL_OES_standard_derivatives`, and a per-draw budget (PRs #8 and #9). Do not port `Op::Minimum` or the alias-only preprocessor. Re-compile the captured Clash shaders on `main` (task CR-09). PRs #11 and #12 may still change large arrays |
| `src/runtime/audio/HostDecode.cpp`, the `decode()` dispatch, CMake FFmpeg discovery, `AudioToolbox` in `app/project.yml` | missing | Port as an engine-wide decoder. Contract is task CR-04 |
| `af_openURL` returns `1954115647` and writes file id 0 when PCM is empty | missing | The callback entry point already does this. The URL entry point does not. Same task |
| `tests/runner/EntryTrace.hpp`, `Engine::observeEntry`, `H_ObserveEntry`, runner env vars, four `fulltest` checks | missing | Engine-wide diagnostic. Task CR-08. `TestRunner.cpp` and `HvmRun.cpp` are already edited by open PRs |
| `tools/test_m4a_audio.py` | missing | Add it with CR-04. It calls the existing `audio-decode` subcommand. Do not commit generated AAC or game audio |

The merge commit `a33f063` only joins `65f7e0b` to `06a7b16`. It has no extra behavior. Do not merge `gemini-merge` again.

### Uncommitted patch and untracked files

| Change | Coverage | What to do |
|---|---|---|
| `encode_avatar` factored out of `encode_home` | covered | Landed with this plan. Home wire tests still pass. `tools/clash/clash_sector_state.py` calls it |
| `tools/clash/clash_turns.py`, `clash_sector_state.py`, `clash_desktop.py`, `test_clash_turns.py` | partial | Source is in the tree and the turn tests pass. The sector loader needs the IPA. The desktop wrapper still passes `--npc-fixture` and `--loopback-network` to programs that do not accept those flags yet. The turns module still raises on an unsupported command, and one error string still says "turn relay" |
| Native VInt literals in `tools/clash_wire.py` and `tools/test_clash_protocol_probe.py` | missing | Task CR-03. Measured mismatch is in the table above |
| `--npc-fixture`, `--seconds 0`, message 21903, and 14102 decoding in `tools/clash_protocol_probe.py` | missing | Task CR-05. Also stop an unsupported turn from dropping the connection |
| `--npc-fixture` and `--live-controls` in `tools/clash_home_probe.py` | missing | Task CR-06. Keep PR #7's macOS socket-buffer lines if that PR has merged |
| Loopback flags in `tools/desktop.py`; `battle` mode in `tools/local_test.sh` | missing | Task CR-07. Point the shell script at `tools/clash/clash_desktop.py` |
| Workflow steps for FFmpeg, M4A, the NPC probe, and the new tools | missing | `.github/workflows/clash-royale.yml` on `main` matches `06a7b16` and still triggers on `gemini-merge`. Task CR-16. The handoff's own patch text still says tutorial id `49000000` and "12 Python tests"; those sentences are stale. The canonical tutorial reference is **48000000** |
| New sections in the three historical docs | missing on purpose | They contradict the translator that is now on `main`, and they repeat the stale tutorial id. This directory replaces them as the working plan. Leave the historical narrative in place |

### Protocol facts the peer must keep

These are REPORTED from the handoff's decoder work. Re-check them against the pinned binary when the IPA is local. Do not replace them with the opaque sector from the nearby 1.2.3 reference client.

- OfflineMode in bundled `settings.csv` does not create a save. The reached flow still resolves `game.clashroyaleapp.com`.
- LoginOk is decoder-derived. Fresh login is 197 bytes, retained login is 245 bytes.
- Initial home is 184 bytes and is accepted (`Loaded own home from server`). The client then sends **14104** for NPC **18000000**.
- Sector state is message **21903**, version 1: compressed flag `1`, little-endian uncompressed size, then zlib.
- Npc1 / FirstMatch / training arena / BasicDeck1. Seed 2. NPC deck is one Goblins card, ref **26000002**. NPC account `(-1, 0)` is avatar slot 0. Human account `(0, 1)` is avatar slot 1. `LogicTutorialManager::getPlayerAvatar` reads avatar 1 and rejects a negative id.
- Six towers. Owner 0 is world Y under 32. Global ids are **6000000..6000005**. Native object reference expects type 5; the wire class is 6.
- First tutorial reference is **48000000**. Completion commands carry 48000001 and so on. The earlier id 49000000 initialized a battle and is noncanonical.
- There is **no turn relay**. Client turns are message 14102. A temporary echo of those turns as 21902 was removed. NPC commands already execute natively twice (flags 1 validate, flags 3 commit). Heartbeat 21902 units are 10 simulation ticks. Client 14102 uses actual ticks. Simulation is 20 ticks/second.
- `tools/clash/clash_turns.py` understands type 1 DoSpell and type 2 CompleteTutorialBattle only, at most 64 commands, trailer integer `-1`.

Native addresses for this exact ARMv7 binary are listed in the handoff section 10. The ones the tasks use:

| Address | Role |
|---|---|
| `0x925b4` | `saveOffline` stub |
| `0x1938d4` / `0x193748` | `readVInt` / `writeVInt` |
| `0x12978` / `0x12b4e` | DoSpell execute / finish requirement (`== 5`) |
| `0x11fa60` / `0x11fa44` | CompleteTutorialBattle execute / decode |
| `0x127974` | tutorial `getPlayerAvatar` |
| `0x1499c` / `0xd7558` | battle ended |
| `0x11371c` | BattleResult decode |

## Known blockers

1. The pinned IPA is not available in this environment, and it must not be committed. Tutorial rendering, the shader corpus, M4A asset comparison, `saveOffline` bytes, and any battle result are therefore not re-established on `main`.
2. Tutorial music needs the missing M4A decoder. The handoff's failure mode is an empty successful `AudioFileOpenURL` followed by an EOF retry that overflows the guest stack.
3. The peer on `main` answers login and the initial home. It does not send the 21903 sector. The VInt codec disagrees with the native writer on the 5-byte forms above.
4. Earned progress is not stored. The reported `saveOffline` body returns immediately. The initial `home-state.json` sits beside the guest sandbox, outside the Documents/Library tree the iOS save editor scans.
5. Even on the handoff build, nobody recorded a finished tutorial, an NPC win, or a relaunch that showed earned rewards. An unsupported 14102 payload still becomes a connection error.

Shorter obstacles, in task order: desktop loopback flags, entry-trace hooks that collide with open PRs on `TestRunner.cpp` and `HvmRun.cpp`, and the workflow file still named for `gemini-merge`.

## Obtain and run the IPA locally

The pin, copied from `tools/clash_royale_probe.py` and `tools/compat_harness/manifests/apps.json`:

- SHA-256: `29269ede437f39ffa62fb4234a1c323303ef417e7eb6ddc5c86f9be76e08fb3f`
- Size recorded in the restoration doc: 90,361,183 bytes
- Bundle id: `com.supercell.scroll`
- Version and build: 2.1306.1
- Archive URL already hardcoded in `tools/clash_royale_probe.py` (the `Clients` release asset whose name contains `v2.1306.1`)

`docs/dev-system/STATUS.md` records an owner decision that is still open: whether that third-party URL is licensed for this use. The harness does not download it. The probe does, when `--ipa` is omitted. Using the URL is that owner decision. This plan does not change it.

Never commit the IPA, an unpacked Payload, identity JSON, login tokens, save files, entry traces, or screenshots of the game. Put the file outside the git checkout.

```sh
# Preferred: a copy you already have. The probe checks the hash and stops on mismatch.
python3 tools/clash_royale_probe.py --ipa /var/tmp/clash-client.ipa --out /tmp/clash-report

# Harness location, still not in git. A missing file is a skip, not a pass.
#   $HVM_FIXTURE_ROOT/ipas/ClashRoyale-2.1306.1.ipa

# Confirm nothing under the checkout is the client before committing.
git status --short
```

`/tmp/clash-report/client.ipa` and `/tmp/clash-report/unpacked/` are generated outputs. Delete them or keep them outside the repo. The probe's metadata JSON is safe to read; do not add it to git if it quotes large slices of the client.

Guest home used by the home probe is `<out>/game/com.supercell.scroll`, with `Documents/`, `Library/`, and `tmp/` created by the engine. Identity and the initial home JSON are `<out>/identity.json` and `<out>/home-state.json`, which are **outside** that guest home.

Headless init, once CR-05 and CR-06 have landed and `hvm` is built:

```sh
python3 tools/clash_home_probe.py \
  --hvm /tmp/hvm-main-build/hvm \
  --ipa /var/tmp/clash-client.ipa \
  --out /var/tmp/clash-sector \
  --results /var/tmp/clash-results \
  --seconds 900 --launches 1 --capture-audio --npc-fixture --live-controls
```

Those two flags do not exist on `main` yet. Until they do, the probe's real command is the two-launch home check:

```sh
python3 tools/clash_home_probe.py \
  --hvm /tmp/hvm-main-build/hvm \
  --ipa /var/tmp/clash-client.ipa \
  --out /var/tmp/clash-home \
  --seconds 90 --launches 2
```

Input for a live probe is append-only `launch-1.input.txt` in the results directory. Coordinates are points in a 640×1136 frame. `touch 0 X Y` begins, `touch 1 X Y` moves, `touch 3 X Y` ends, `stop` ends the run. The handoff's dialogue and Knight-drag coordinates are REPORTED, and the highlighted tile moves, so treat them as a starting hypothesis.

## Test commands

No IPA:

```sh
python3 -m unittest discover -s tools -p 'test_clash_*.py'          # 17 tests: 12 existing + 5 turn tests
python3 -m unittest discover -s tools/clash -t tools -p 'test_*.py' # the 5 turn tests alone
python3 tools/clash/clash_desktop.py --help                         # imports only
cmake -S . -B /tmp/hvm-main-build -G Ninja -DCMAKE_CXX_COMPILER=clang++ -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build /tmp/hvm-main-build --target hvm hvm_tests --parallel 4
/tmp/hvm-main-build/hvm_tests fulltest
```

`tools/clash` has `__init__.py`, so the first discover enters it. That is the 17 tests the handoff counted.

Needs the IPA, and needs the tasks that add the flags:

```sh
python3 tools/clash_royale_probe.py --ipa /var/tmp/clash-client.ipa --out /tmp/clash-report
python3 tools/test_ima_wav_corpus.py /tmp/clash-report/unpacked --hvm-tests /tmp/hvm-main-build/hvm_tests --out /tmp/ima.json --require-count 107
HVM_DUMP_SHADERS=/tmp/clash-shaders /tmp/hvm-main-build/hvm run /var/tmp/clash-client.ipa --home /tmp/clash-shader-home --run-for 30 --graceful --report /tmp/clash-shader.json
/tmp/hvm-main-build/hvm_tests shaders /tmp/clash-shaders
python3 tools/test_m4a_audio.py --hvm-tests /tmp/hvm-main-build/hvm_tests --assets /tmp/clash-report/unpacked --require-assets 17 --out /tmp/m4a.json
```

`test_m4a_audio.py` is part of CR-04; it is not in the tree yet. The synthetic half of that script can run without the IPA once the decoder exists. The 17-asset half cannot.

In-repo regression IPAs (Angry Birds, Tiny Tower, Art of Glow) are a separate compatibility check. They are not a Clash pass. Tiny Tower's solid black last frame is the REPORTED baseline from the October 21 scan.

This VM's Clang 18 selects GCC 14 and then cannot find `cstddef`. The verification build used g++. That is a machine quirk, not a project change.

## Rules

- No proprietary binaries, unpacked game assets, login tokens, identity files, private saves, or entry traces in git. A screenshot of the game stays local.
- Prefer an engine-wide fix. A Clash-only branch, a suppressed assertion, a blanket success return, or an edited client is a failed fix. Other apps must keep their current behavior. Tiny Tower's black frame is the known baseline; a new crash there is a regression.
- The GLSL subset from `65f7e0b` stays retired. Shader bugs get fixed in `src/runtime/graphics/Shader.cpp` and covered with a small shader that contains no game source.
- `saveOffline` stays a guest stub until a trace shows a real callee contract. Persistence belongs in the peer's save file.
- No turn relay, no synthetic checksum acknowledgement, and no replay of 14102 as 21902.
- Do not mark a task done without a command you ran and output you read. A green startup workflow is not playable. A home-accepted log is not a win. Mixer PCM is not a speaker test. Apple code that was not compiled is not done.
- Integration is `main`. Open a pull request. Do not merge it, force-push, or change permissions. Do not push the scratch branch that was used to read the handoff bundle.
- Group C owns the Clash wire tools and `src/runtime/libc/Net.cpp`. `PlatformStubs.cpp` is shared with other UIKit work. `Engine.cpp` and `Handlers.hpp` are the CPU/memory area. `TestRunner.cpp` is in PRs #4, #5, #6, #11, and #12. `HvmRun.cpp` is in PR #6. `FileBrowser.swift` and `clash_home_probe.py` are in PR #7. Rebase; do not overwrite those diffs.
- One owner per group. Do not start a second editor in `Shader.cpp` while #11 or #12 is active, or in `Engine.cpp` while another memory change is active.

## Engine build in this session

```text
cmake -S . -B /tmp/hvm-main-build -G Ninja -DCMAKE_CXX_COMPILER=g++ -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build /tmp/hvm-main-build --target hvm hvm_tests --parallel 4
/tmp/hvm-main-build/hvm_tests fulltest
fulltest: 148 passed, 0 failed
```

The same log includes `GLSL common built-ins`, `GLSL macro rescan, zero-argument macros and empty replacement`, `GLSL user-defined functions`, `GLSL execute stops when the shared instruction budget is exhausted`, and `GLES2 dFdx/dFdy/fwidth use 2x2 quads`. Those are the constructs the `65f7e0b` subset was adding, covered here by the translator's own tests. They are not a replay of the 26 captured Clash shaders.
