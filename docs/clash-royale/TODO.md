# Clash Royale task list

> **For agentic workers:** Read `docs/clash-royale/README.md` first. Implement one task at a time. Each task ends with the verification command in that task. Leave the box unchecked until that command's output matches the acceptance line.

**Goal:** Run pinned Clash Royale 2.1306.1 as a persistent single-player game with NPC battles, using shared emulator fixes and a local protocol peer.

**Architecture:** The guest binary stays unmodified. `main` already speaks loopback TCP and the login/home fixture. The missing peer messages, the M4A decoder, and earned-state persistence are the gap. The GLSL subset from local commit `65f7e0b` is superseded by `src/runtime/graphics/Shader.cpp` on `main`.

**Tech stack:** C++20 engine (Clang or GCC), Python 3 unittest peers under `tools/`, SwiftUI save editor in `app/HydrogenVM/FileBrowser.swift`.

## Global constraints

- Pinned IPA SHA-256 `29269ede437f39ffa62fb4234a1c323303ef417e7eb6ddc5c86f9be76e08fb3f`. Never commit the IPA, unpacked assets, tokens, identity JSON, saves, traces, or game screenshots.
- Canonical tutorial reference `48000000`. NPC account `(-1, 0)` in slot 0. Human account `(0, 1)` in slot 1. NPC id `18000000`. No 21902 turn relay.
- Engine-wide fixes over Clash-only hacks. Assertions and guest bounds checks stay. Other apps must not regress. Tiny Tower's solid black last frame is the known baseline from the 2026-10-21 scan.
- A task is done only when its verification command was run and the output matched. Startup success is not a win.
- Pull requests into `main` only. No merges, force-pushes, or permission changes from the agent.
- Rebase onto open PRs that touch the same files: #4 and #5 and #6 and #11 and #12 (`TestRunner.cpp`), #6 (`HvmRun.cpp`), #7 (`project.yml`, `clash_home_probe.py`, `FileBrowser.swift`), #11 and #12 (`Shader.cpp`).

Checked boxes in CR-01 and CR-02 are the tools landed with the plan. Everything else is open.

---

### CR-01: Share the avatar body between home and sector

**Status:** done with the plan branch.

**Scope:** Clash-specific tool.

**Files:**
- Modify: `tools/clash_home_state.py` (`encode_avatar`)

**Depends on:** none

`encode_home` calls `encode_avatar(w, avatar, account)` and then writes the OwnHome tail `vint(0)`. `encode_avatar` is the previous inline body (three id pairs, name, arena, six data-slot arrays, resources, diamonds, battle counters, `avatar+b0`). Home bytes stay the same.

- [x] **Verify**

```sh
python3 -m unittest tools/test_clash_home_state.py -v
```

Expected: 6 tests, `OK`.

---

### CR-02: Port the four handoff tools

**Status:** done with the plan branch.

**Scope:** Clash-specific. No proprietary data in the files (CSV rows are read from the IPA at runtime).

**Files:**
- Create: `tools/clash/__init__.py`
- Create: `tools/clash/clash_turns.py`
- Create: `tools/clash/clash_sector_state.py`
- Create: `tools/clash/clash_desktop.py`
- Create: `tools/clash/test_clash_turns.py`

**Depends on:** CR-01

The modules insert `tools/` on `sys.path` and import `clash_wire` / `clash_home_state`. `clash_desktop.py` looks for `clash_protocol_probe.py` and `desktop.py` in `tools/`. Behavior matches the handoff, including the strict `ValueError` on an unsupported turn and the error string that still says "turn relay". The desktop wrapper is not a working session until CR-05 and CR-07.

- [x] **Verify**

```sh
python3 -m unittest discover -s tools/clash -t tools -p 'test_*.py' -v
python3 -c "import sys; sys.path[:0]=['tools','tools/clash']; import clash_sector_state, clash_desktop"
```

Expected: 5 tests, `OK`, and the import prints nothing. `python3 -m unittest discover -s tools -p 'test_clash_*.py'` also collects these 5, for 17 tests in total.

---

### CR-03: Match native VInt encodings

**Scope:** Clash-specific protocol peer. `tools/clash_wire.py` is the ByteStream codec for this client, not a general integer format.

**Files:**
- Modify: `tools/clash_wire.py` (`vint`, `Reader.vint`)
- Modify: `tools/test_clash_protocol_probe.py`
- Test: `python3 -m unittest tools/test_clash_protocol_probe.py tools/clash/test_clash_turns.py -v`

**Depends on:** none. CR-05 needs this before it sends a sector.

The pinned writer keeps the sign in bit `0x40` of byte 0 and uses six data bits there. Later bytes use seven data bits, and the fifth byte uses four. Sign extension stops at 31 bits. Choose the width from the magnitude thresholds 63, 8191, 1048575, 134217727, then five bytes.

Replace `vint` with that width rule. In `Reader.vint`, reject a fifth data byte greater than 15, and sign-extend with `min(shift, 31)` rather than the uncorrected shift. Keep the existing range check.

Add this to `test_signed_vint_boundaries`, next to the `1306` / `9a14` assertion:

```python
for value, encoded in ((-64, 'c07f'), (-8192, 'c0807f'), (-(1 << 31), 'c080808000'),
                       ((1 << 31) - 1, 'bfffffff0f'), (-134217728, 'c08080800f')):
    self.assertEqual(vint(value).hex(), encoded)
    self.assertEqual(Reader(bytes.fromhex(encoded)).vint(), value)
```

The current writer emits `40`, `c000`, `c080808070`, `bfffffff0f`, and `c0808000` for those values. `INT_MAX` already matches. The round-trip loop in the same test must still pass, and the five turn tests must still pass.

- [ ] **Verify**

```sh
python3 -m unittest tools/test_clash_protocol_probe.py -v
python3 -m unittest discover -s tools/clash -t tools -p 'test_*.py' -v
```

Expected: protocol tests `OK` including the five literal rows, and 5 turn tests `OK`.

---

### CR-04: Decode M4A in the shared audio path

**Scope:** Engine-wide. WAV, CAF, AIFF, and MP3 stay on their current decoders.

**Files:**
- Create: `src/runtime/audio/HostDecode.cpp`
- Modify: `src/runtime/audio/Decode.cpp` (declare `decodeHostContainer` and call it at the start of `decode`, before the MP3 sniffer)
- Modify: `CMakeLists.txt` (Apple: link AudioToolbox. Else: pkg-config `libavformat`, `libavcodec`, `libavutil`. On success, compile `HostDecode.cpp` with `HVM_HAVE_FFMPEG=1` and those include dirs, and link the libraries. A build without the libraries must still compile.)
- Modify: `app/project.yml` (add `AudioToolbox.framework`). Rebase onto PR #7's README/LICENSE excludes if that PR is merged.
- Modify: `src/runtime/uikit/PlatformStubs.cpp` `af_openURL` only
- Modify: `tests/runner/TestRunner.cpp` (two checks; rebase onto #4 #5 #6 #11 #12)
- Create: `tools/test_m4a_audio.py`
- Already present: `hvm_tests audio-decode`. Do not add a second CLI.

**Depends on:** none for the decoder. CR-10 needs it so tutorial music does not overflow the guest stack.

`decodeHostContainer(DecodedAudio& out, const uint8_t* data, size_t size)` returns false unless the buffer is 12 bytes through 256 MiB and bytes 4..7 are `ftyp`. Linux, behind `HVM_HAVE_FFMPEG`, uses a bounded custom AVIO, MOV demux, the first audio stream, decoder flush, and priming. Accept 4000..192000 Hz and 1..8 channels. Output is stereo int16, at most 128M samples. Mono is duplicated at unity gain, not attenuated. More than two channels keep the first two. Non-finite samples become 0. Apple uses `AudioFileOpenWithCallbacks` and `ExtAudioFile` with the same bounds. Failure returns false and leaves the caller to try the older decoders, which will also reject a non-WAV/MP3 buffer.

In `af_openURL`, after `decodeAudioBytes`, if `stereoPcm` is empty write `0` to `outFile` and return `1954115647` (`typ?`). Do this before inserting the instance. `af_openWithCallbacks` already returns that code. Leave `eaf_openURL`'s silence fallback unless a trace shows the music retry uses `ExtAudioFileOpenURL`; if it does, reject the empty decode there too, in this same task, with a `fulltest` check.

`tools/test_m4a_audio.py` takes `--hvm-tests`, optional `--assets`, optional `--require-assets`, and required `--out`. It writes two synthetic AAC files with ffmpeg (`sine`, 0.15 s, mono 22050 and stereo 44100), decodes them with `hvm_tests audio-decode`, and compares s16le PCM to ffmpeg. Mono ffmpeg output uses `pan=stereo|c0=c0|c1=c0` so the CLI does not apply its default -3 dB rematrix. Max sample difference must be 0 or 1, frame counts must match the `rate=` / `channels=` / `frames=` line, and mono channels must be equal. Then it must reject three bad inputs (empty, a short `ftyp` header, and the first 32 bytes of a valid file) with a non-zero `audio-decode` status. The JSON report records hashes of the synthetic inputs, not their bytes. With `--assets` and `--require-assets 17`, every `*.m4a` under the unpacked client is included. Do not commit those assets or the JSON if it is tied to a private path.

In `fulltest`, next to the existing AudioFile coverage, add the two checks from the handoff runtime log:

- `AudioFileOpenURL rejects undecodable audio instead of publishing a successful empty stream` — file bytes `invalid audio`, status `1954115647`, out-id `0`.
- `AudioFileOpenURL decoded LPCM packets retain sample bytes and packet/byte counts` — a real WAV then reads back.

- [ ] **Verify**

```sh
cmake --build /tmp/hvm-main-build --target hvm_tests --parallel 4
/tmp/hvm-main-build/hvm_tests fulltest
python3 tools/test_m4a_audio.py --hvm-tests /tmp/hvm-main-build/hvm_tests --out /tmp/m4a.json
```

Expected: `fulltest` ends in `0 failed`, and the script prints two `ok synthetic-...` lines and writes `"malformedRejected": 3`. The 17-asset run waits for a local IPA and is part of acceptance only when `--assets` is passed. Also run CR-17's three older apps after this lands, because `PlatformStubs.cpp` and `Decode.cpp` are shared.

---

### CR-05: Send the Npc1 sector and keep the session up

**Scope:** Clash-specific peer.

**Files:**
- Modify: `tools/clash_protocol_probe.py`
- Modify: `tools/test_clash_protocol_probe.py`
- Modify: `tools/clash/clash_turns.py` (error string only)
- Uses: `tools/clash/clash_sector_state.py`, `tools/clash_home_state.py`

**Depends on:** CR-02, CR-03

Add `--npc-fixture`. It requires `--home-client`. `--seconds 0` serves until interrupted; the legal range becomes `0 <= seconds <= 3600`. On startup, if the flag is set, call `load_template(home_client)` once.

On message 14104, parse one reference. If it is `18000000` and a template is loaded, encode with `encode_sector`, send message **21903** version **1**, payload `b'\x01' + struct.pack('<I', len(sector)) + zlib.compress(sector)`, and log `npc_fixture_sent` with the uncompressed size and seed. Any other NPC stays `supported: false` and sends nothing.

On message 14102 while a template is loaded, call `decode_turn`. On success log `client_turn`. On `ValueError`, log `unsupported_turn` with the reason and leave the socket open. A framing error, a bad login, or a bad 14104 still logs `connection_error` and drops the connection. Do not answer 14102 with 21902. Do not acknowledge checksums.

Change the custom-spell `ValueError` text so it no longer says "relay". The turn tests only require `ValueError`.

Add a probe test that builds a 14102 payload with command type `1000` (the turn test already knows this shape), feeds it through the connected handler, and asserts the event list contains `unsupported_turn` and does not contain `connection_error`. Add a second test that a type-1 spell with the trailer intact produces `client_turn` and does not disconnect. Use a temporary identity and a fake template injection if `load_template` needs the IPA: a test-only helper that accepts an already-built sector blob is fine, as long as the production path still calls `load_template` on the real IPA. Do not commit an IPA or a captured sector.

Update the probe docstring and the JSON `scope` string so they say the sector is an initial fixture and that turn authority and progression saves are absent.

- [ ] **Verify**

```sh
python3 -m unittest tools/test_clash_protocol_probe.py -v
python3 -m unittest discover -s tools/clash -t tools -p 'test_*.py' -v
```

Expected: previous tests plus the two new probe tests, all `OK`.

---

### CR-06: Drive the sector from the home probe

**Scope:** Clash-specific.

**Files:**
- Modify: `tools/clash_home_probe.py`

**Depends on:** CR-05

Add `--npc-fixture` and `--live-controls`. Without the fixture, seconds stay in `30..300`. With it, the upper bound is `1800`. Pass `--npc-fixture` through to the peer. With `--live-controls`, create `launch-N.input.txt` and add `--frame-file` / `--input-file` on the `hvm run` command.

Record `nativeTutorialVoiceLoaded` when the log contains `Start loading: sfx/king_tut_first_vo_01.wav`. The summary gains `npcFixtureEnabled` and `npcFixturesSent`. Success with the flag requires `npcFixturesSent >= launches` and `nativeTutorialVoiceLoaded` on every launch, plus the existing clean-exit and non-silent audio checks. The summary scope string must say battle completion is not verified.

If PR #7 has merged, keep its Darwin `SO_SNDBUF` / `SO_RCVBUF` lines around the socketpair.

- [ ] **Verify without an IPA**

```sh
python3 tools/clash_home_probe.py --help
python3 -c "
import ast, pathlib
tree = ast.parse(pathlib.Path('tools/clash_home_probe.py').read_text())
src = pathlib.Path('tools/clash_home_probe.py').read_text()
assert '--npc-fixture' in src and '--live-controls' in src and '48000000' not in src
print('probe flags present')
"
```

Expected: help text lists both flags, and the script prints `probe flags present`. The tutorial id lives in the sector encoder, not in this probe.

- [ ] **Verify with an IPA** (required before checking the box)

```sh
python3 tools/clash_home_probe.py \
  --hvm /tmp/hvm-main-build/hvm \
  --ipa /var/tmp/clash-client.ipa \
  --out /var/tmp/clash-sector \
  --results /var/tmp/clash-results \
  --seconds 180 --launches 1 --capture-audio --npc-fixture
```

Expected: exit 0, `nativeHomeAccepted` true, `nativeTutorialVoiceLoaded` true, `npcFixturesSent` 1, `crashed` false, `errors` empty, `nonSilentFrames` > 0, `transportError` 0. This proves initialization only.

---

### CR-07: Forward loopback into the desktop wrapper

**Scope:** Clash-specific tooling. The engine flags already exist.

**Files:**
- Modify: `tools/desktop.py`
- Modify: `tools/local_test.sh`
- Already present: `tools/clash/clash_desktop.py`

**Depends on:** CR-05

`desktop.py` gains `--loopback-network` and a repeatable `--loopback-alias`. Alias without the network flag is an argparse error. When set, the `hvm run` command gains `--loopback-network` and one `--loopback-alias HOST` per hostname. Default invocations gain neither flag.

`local_test.sh` accepts mode `battle` everywhere it accepts `play` (the mode list and the session lock). `battle` runs:

```sh
"$python" "$root/tools/clash/clash_desktop.py" --hvm "$build/hvm" --ipa "$client" --home "$state/battle" --out "$out"
```

after the existing pygame venv check. Do not point it at `tools/clash_desktop.py`.

- [ ] **Verify**

```sh
python3 - << 'PY'
import pathlib
src = pathlib.Path('tools/desktop.py').read_text()
assert '--loopback-network' in src and '--loopback-alias' in src
print('desktop flags present')
PY
bash -n tools/local_test.sh
python3 tools/desktop.py --loopback-alias game.clashroyaleapp.com --hvm /nope --ipa /nope --home /tmp/nope --out /tmp/nope-out > /tmp/desktop-alias.txt 2>&1
status=$?
cat /tmp/desktop-alias.txt
test "$status" -ne 0
grep -q 'loopback-alias requires --loopback-network' /tmp/desktop-alias.txt
```

Expected: `bash -n` is silent, the snippet prints `desktop flags present`, and stderr contains `loopback-alias requires --loopback-network`. Put that check immediately after `parse_args`, before the IPA is opened, or the missing-file error hides it.

A windowed battle session is CR-10's optional smoke, not this box. The handoff had not smoke-tested it.

---

### CR-08: Read-only guest entry observers

**Scope:** Engine-wide diagnostic. Off unless configured. No per-instruction work when no observer is registered: the probe op is inserted only for blocks whose start address is in the map.

**Files:**
- Modify: `src/core/cpu/Engine.hpp`, `src/core/cpu/Engine.cpp`, `src/core/cpu/Handlers.hpp`
- Create: `tests/runner/EntryTrace.hpp`
- Modify: `tests/runner/HvmRun.cpp`, `tests/runner/TestRunner.cpp`
- Rebase: PR #6 owns part of `HvmRun.cpp`. PRs #4 #5 #6 #11 #12 own `TestRunner.cpp`. Do not start this while another change is in `Engine.cpp` or `Handlers.hpp`.

**Depends on:** none. CR-10 uses it when the tutorial requirement is unclear.

`Engine::observeEntry(uint32_t address, EntryObserver)` takes `std::function<void(uint32_t, const Cpu&)>`. It throws `std::logic_error` if any block is already decoded, and `std::invalid_argument` if the callback is empty or the address is an HLE slot. Store the address with the Thumb bit cleared. `notifyEntry` finds the observer, and on any exception sets `cpu.exit = Exit::HostError`, `cpu.r[kPC]` to the address, and `lastHostError` to `Entry observer: ...`. `decodeBlock` inserts `H_ObserveEntry` at the front of a block whose PC is observed. The handler calls `hvmObserveEntry` and returns without `HVM_NEXT` when `exit != None`.

`EntryTrace::attach(Process&)` reads:

- `HVM_ENTRY_TRACE` path (unset means do nothing)
- `HVM_ENTRY_PCS` comma-separated hex addresses, required when the path is set
- `HVM_ENTRY_AFTER` optional gate address; tracing waits until that entry
- `HVM_ENTRY_BUFFER` optional `pointerOffset,indexOffset`, each hex and `<= 252`

Each record is one line: `entry`, `pc`, `lr`, `r0`..`r12`, 32 bytes at `r0` or the word `unmapped`, and when the buffer spec is set, `index` plus 32 bytes at `pointer+index`. Stop after 100000 records with a single `trace limit reached` line. Flush each line. Unmapped checks use `protAt` and reject a range that wraps the guest arena. `HvmRun.cpp` constructs `EntryTrace` before `Process` and calls `attach` immediately after, so the object outlives guest threads. Trace files stay out of CI artifacts.

`fulltest` gains four checks, using the instruction words from the handoff's runner (ARM caller at `0x1000`, callee `add r0, r0, #7; bx lr` at `0x1100`, Thumb `adds r0, #7; bx lr` at `0x1200`):

- cached direct calls observe `r0` values `0,7,14,21,28`, and `r0`/`nzcv` of the caller are unchanged
- `observeEntry` after the first call throws `std::logic_error`
- a throwing observer surfaces as `GuestFault` with `Exit::HostError` and `pc == 0x1100`
- `observeEntry(0x1201)` sees the Thumb entry at `0x1200` once and the call result is `12`

- [ ] **Verify**

```sh
cmake --build /tmp/hvm-main-build --target hvm hvm_tests --parallel 4
/tmp/hvm-main-build/hvm_tests fulltest
```

Expected: the four lines print as `ok`, and the summary has `0 failed`. Run CR-17 after this, because the CPU core is shared.

---

### CR-09: Re-compile Clash shaders on main's translator

**Scope:** Engine-wide if a shader fails. The fix goes in `Shader.cpp` with a minimal regression shader that contains no game source. Do not restore the `65f7e0b` subset.

**Files:**
- Maybe modify: `src/runtime/graphics/Shader.cpp`, `src/runtime/graphics/Shader.hpp`, `tests/runner/TestRunner.cpp`
- Do not modify these while PR #11 or #12 is the active shader change. Re-run after those merge.

**Depends on:** a local IPA and a built `hvm`. Independent of the peer tasks.

```sh
HVM_DUMP_SHADERS=/tmp/clash-shaders /tmp/hvm-main-build/hvm run /var/tmp/clash-client.ipa \
  --home /tmp/clash-shader-home --loopback-network \
  --loopback-alias game.clashroyaleapp.com \
  --run-for 30 --graceful --report /tmp/clash-shader.json
/tmp/hvm-main-build/hvm_tests shaders /tmp/clash-shaders
```

The 30 second run may only capture loading shaders. A run that reaches the arena (after CR-06) is the one that must include the arena shadow shader. Target the REPORTED count of 26 sources once the arena is up. Loading-only is acceptable as a partial log, and the box stays open until the arena dump compiles.

If a source fails, add a reduced shader to the engine tests that uses the same construct (a `min`/`max` overload, a precision macro, a function, a derivative) and fix the translator until both the reduced shader and the dump compile. Delete the dump afterwards. It is game source.

- [ ] **Verify**

`hvm_tests shaders` prints a compile success for every file in the arena dump, and `fulltest` still ends in `0 failed` if `Shader.cpp` changed.

---

### CR-10: Finish the first tutorial cast

**Scope:** Clash-specific investigation. Engine-wide only if the trace shows a shared bug.

**Files:** determined by the trace. Likely `tools/clash_protocol_probe.py` if a decoded message is missing. Guest code is not edited.

**Depends on:** CR-04, CR-05, CR-06, CR-09, and a local IPA. CR-08 is the tool when the requirement is unclear.

Use tutorial `48000000`, human slot 1, and no turn relay. The handoff's blocker is `LogicDoSpell::getFinishRequirement` at `0x12b4e` comparing the requirement to 5, and execute at `0x12978`. Determine whether the committed command is the human's, whether the spell index and tile match the highlighted target, and whether flags bit 2 is set when the completion message needs it (`CompleteTutorialHome` at `0x132174`).

`HVM_ENTRY_TRACE` with `HVM_ENTRY_PCS=12978,12b4e,127c20,11fa60` is the read-only way in. The trace stays in `/tmp`.

Acceptance is a log from one run that shows the client committed the tutorial step (type 2 with tutorial `48000000`, or the native completion path advancing past CastSpell) and the process exiting 0 with an empty error list. A rendered Knight and a `nativeTutorialVoiceLoaded` line are not acceptance. The REPORTED drag through `(160,390)` to `(200,285)` is a hypothesis; the highlighted tile moves.

- [ ] **Verify**

```sh
python3 tools/clash_home_probe.py \
  --hvm /tmp/hvm-main-build/hvm --ipa /var/tmp/clash-client.ipa \
  --out /var/tmp/clash-sector --results /var/tmp/clash-cast \
  --seconds 900 --launches 1 --capture-audio --npc-fixture --live-controls
```

Drive `launch-1.input.txt`, then show `client_turn` or the completion log in `protocol-events.json`. Expected: exit 0 and a quoted event that names tutorial `48000000`.

---

### CR-11: Record one NPC win

**Scope:** Clash-specific. Implement only fields the pinned decoder reads.

**Files:**
- Modify: `tools/clash_protocol_probe.py` and, if the sector snapshot must change, `tools/clash/clash_sector_state.py`
- Test: extend `tools/test_clash_protocol_probe.py` with a fixture built from the captured field list, not from a copied enemy blob

**Depends on:** CR-10

Watch `battleEnded` (`0x1499c`, `0xd7558`), `BattleResult` decode (`0x11371c`), and the go-home path (`0x270b4`). Log every message id after the tutorial completion. Add a reply only when the client's next read is identified from the entry trace or from a failed decode you can quote. Keep native NPC AI (`account high == -1` at `0x33634`). Do not simulate the battle in Python.

Acceptance: one headless or desktop run where the peer log shows the battle-end message, the client leaves the battle (go-home send or the home screen log), exit code 0, `errors` empty. Quote the message id and the decoded fields in the PR. A 180 second init probe is not this task.

- [ ] **Verify**

The CR-10 command, continued until the end-state event, with `--seconds` long enough for 20 Hz simulation on this build (the handoff's tutorial minute was hundreds of frames, about 339 ms each). Expected: `protocol-events.json` contains the battle-end event and the process report has `"crashed": false`.

---

### CR-12: Confirm the saveOffline stub

**Scope:** Clash-specific evidence. No engine change on the success path.

**Files:**
- None if the probe still reports an immediate return
- Modify: `docs/clash-royale/README.md` with the fresh JSON fields

**Depends on:** a local IPA. Can run as soon as the IPA is present. CR-13 waits for it.

```sh
python3 tools/clash_royale_probe.py --ipa /var/tmp/clash-client.ipa --out /tmp/clash-report
python3 -c "import json; d=json.load(open('/tmp/clash-report/metadata.json')); print(d['offlineCodeEvidence'])"
```

Expected REPORTED values to re-check: symbol `__ZN8GameMode11saveOfflineERK6String`, address `0x925b4`, Thumb, body prefix `7047` (`bx lr`), `saveOfflineImmediateReturn` true.

If those match, do not HLE the function, do not NOP anything else, and do not patch the IPA. If they differ, stop and update this plan with the bytes before writing a save format.

- [ ] **Verify:** the printed evidence matches the expected address and `saveOfflineImmediateReturn: True`, and `git status` does not show the IPA or `metadata.json` staged.

---

### CR-13: Persist earned progress and prove a relaunch

**Scope:** Clash-specific peer. The guest stub stays a stub (CR-12).

**Files:**
- Modify: `tools/clash_home_state.py` (extend the JSON schema with earned fields the decoder already accepts: tutorial progress, NPC record, resources)
- Modify: `tools/clash_protocol_probe.py` (apply earned fields on the next home encode)
- Modify: `tools/test_clash_home_state.py`
- Modify: `tools/clash_home_probe.py` so the second launch asserts the earned file, not only `loginKinds[-1] == False`

**Depends on:** CR-11, CR-12

Write the state with the existing atomic `write_state`. Keep it separate from `identity.json`. Store it at **both** the current `<out>/home-state.json` (so the Linux probe keeps its path) and `<guest home>/Documents/clash-royale/home-state.json` (so CR-14's editor can see it). The guest home is the `--home` directory, which the probe sets to `<out>/game/com.supercell.scroll`.

Reject a file whose `clientSha256` or `account` disagrees. The initial fixture's saved-home SHA-256 from the handoff (`3a93d54b...`) is the pre-progression file; the earned file must differ after a win and must match across two launches.

Acceptance: launch, win (CR-11), exit, launch again. The second run's outbound home payload contains the earned tutorial/NPC/resource fields, and the test locks those bytes without an IPA by feeding a synthetic earned JSON through `encode_home`.

- [ ] **Verify**

```sh
python3 -m unittest tools/test_clash_home_state.py tools/test_clash_protocol_probe.py -v
```

And one two-launch IPA run whose second summary shows the earned fields and `loginKinds` ending in `false` (retained login). Expected: unit tests `OK`, and the second launch's report quotes the same earned JSON hash as the file on disk.

---

### CR-14: Tie earned saves into the FR-1 save editor

**Scope:** Clash-specific data in the existing engine-wide editor. FR-1 here means the Save Editor already in `app/HydrogenVM/FileBrowser.swift`: it lists Documents and Library, categorizes `.json` as configs, and can snapshot/restore Documents+Library.

**Files:**
- Prefer no edit to `FileBrowser.swift` (PR #7 is already editing it). The path from CR-13 is `<home>/Documents/clash-royale/home-state.json`, which `scanSandbox` already classifies as configs because the extension is `.json`.
- Modify: `tools/test_clash_home_state.py` if the relative path needs a lock
- Modify: `docs/clash-royale/README.md` with the path

**Depends on:** CR-13

Linux acceptance (this box):

- The peer writes `Documents/clash-royale/home-state.json` under the guest home.
- A unit test writes a sample tree in `tempfile.TemporaryDirectory`, checks the relative path ends with `Documents/clash-royale/home-state.json`, and checks the suffix is `.json`.
- Editing `avatar.name` in that JSON and running `encode_home` changes the avatar string in the payload. That is the edit tie-in the Linux peer can prove.

Mac acceptance, recorded in the PR and not claimed from Linux: open the installed app's Save Editor, see the JSON under configs, change the name, snapshot, restore, relaunch, and see the peer serve the edited name. If the scanner misses the file, then and only then change `FileBrowser.swift`, rebasing onto PR #7.

- [ ] **Verify**

```sh
python3 -m unittest tools/test_clash_home_state.py -v
```

Expected: the new path test passes. The Mac pass is a separate note, not this command.

---

### CR-15: Modding hooks through a host overlay

**Scope:** A host UI on top of the peer JSON. Clash-specific fields, engine-wide only in the sense that the overlay lives in the iOS app shell. No guest hooks, no patched IPA, no in-battle trainer.

**Files:**
- Modify: `app/HydrogenVM/RunView.swift` or `LibraryView.swift` for a pre-run sheet
- Modify: `tools/clash_home_state.py` only if a field is not already validated
- Test: `tools/test_clash_home_state.py`

**Depends on:** CR-14

First slice: before launch, the sheet reads `Documents/clash-royale/home-state.json` and edits `avatar.name` plus the existing resource counts. Validators already reject an empty name, a name over 200 UTF-8 bytes, a negative count, and a resource id outside table 5. The next `encode_home` uses the edited values. Deck, NPC, and arena selectors wait until the sector encoder accepts more than Npc1; do not add them in this task.

Linux acceptance: a test that loads edited JSON and asserts the encoded name and resource counts. SwiftUI behavior is verified on a Mac by opening the sheet, saving, and relaunching. Do not check this box from a Linux-only run of the Python test; check it when both the unit test and a Mac note with the build result are in the PR. If no Mac is available, leave the box open and land the Python validator test under CR-14's file instead of claiming the overlay.

- [ ] **Verify**

```sh
python3 -m unittest tools/test_clash_home_state.py -v
```

Expected: edited name and counts round-trip. The PR text includes the Mac result, or the box stays open.

---

### CR-16: Update the Clash workflow

**Scope:** CI config. Clash-specific workflow, engine packages only as CR-04 requires.

**Files:**
- Modify: `.github/workflows/clash-royale.yml`

**Depends on:** CR-02 now; CR-03, CR-04, and CR-05 before adding those steps

The workflow triggers on `gemini-merge`. Add `main` to that branch list so the job can run for this work, and keep `workflow_dispatch`. Add `tools/clash/**` and `tools/test_m4a_audio.py` to the path filters.

In the compile step, add:

```sh
python3 -m py_compile tools/clash/clash_turns.py tools/clash/clash_sector_state.py tools/clash/clash_desktop.py tools/clash/test_clash_turns.py
python3 -m unittest discover -s tools/clash -t tools -p 'test_*.py'
```

When CR-04 has landed, add `pkg-config libavformat-dev libavcodec-dev libavutil-dev ffmpeg` to the apt line and the M4A synthetic command (no `--assets` in CI unless the IPA step already ran and the owner decision on the download still stands). The job already downloads the client; do not add a second download, and do not upload the IPA as an artifact.

Do not check this box because the YAML parses. Check it when a GitHub Actions run of this workflow is green. A billing failure before steps start is UNKNOWN, not a pass.

- [ ] **Verify**

```sh
python3 -c "import yaml,pathlib; yaml.safe_load(pathlib.Path('.github/workflows/clash-royale.yml').read_text())"
```

And the Actions URL for a completed green run. Expected locally: the YAML loads. Expected for the checkbox: the run's conclusion is `success`.

---

### CR-17: Regression gate after engine edits

**Scope:** Engine-wide. Run this after CR-04, CR-08, and any CR-09 shader edit. Clash-only Python tasks do not require the four-app scan.

**Files:** none, unless a regression shows up. Fix that in the shared code, then rerun.

**Depends on:** the engine task being gated

```sh
/tmp/hvm-main-build/hvm_tests fulltest
python3 tools/hvm_compat.py run --baseline docs/dev-system/baselines/linux-headless.json
```

The compat command needs the fixture root described in `docs/dev-system/`. The three older IPAs in the repo (Angry Birds, Tiny Tower, Art of Glow) are the stand-in when the harness fixture directory is the checkout. Clash's optional fixture stays a skip when the IPA is absent; a skip is not a Clash pass.

Expected: `fulltest` prints `0 failed`. Angry Birds and Art of Glow still run. Tiny Tower may end on a black `#000000` frame; a crash, a new load-time import failure, or a black frame in an app that previously drew is a failure.

- [ ] **Verify:** paste the `fulltest` summary line and the compat conclusion in the PR.

---

### CR-18: Pointer in AGENTS.md

**Scope:** Docs.

**Files:**
- Modify: `AGENTS.md`, only after it exists on `main`

**Depends on:** PR #10 merging, or any other change that adds `AGENTS.md`

Copy the "Clash Royale" section from the root `README.md` into `AGENTS.md`. Do not add `AGENTS.md` in a branch whose only purpose is the pointer; PR #10 already creates that file.

- [ ] **Verify:** `AGENTS.md` on `main` contains the link to `docs/clash-royale/README.md`, and `git diff` shows no other edits.

---

## Order

CR-01 and CR-02 are done. CR-03 and CR-04 can proceed together if they do not share a file (they do not). CR-05 follows CR-03. CR-06 and CR-07 follow CR-05. CR-08 can proceed beside CR-03 unless someone else owns `Engine.cpp`. CR-09 waits on the IPA and on a quiet `Shader.cpp`. CR-12 waits only on the IPA. CR-10 follows CR-04, CR-06, and CR-09. CR-11 follows CR-10. CR-13 follows CR-11 and CR-12. CR-14 follows CR-13. CR-15 follows CR-14. CR-16's Python lines can land with CR-02; the FFmpeg lines wait for CR-04; the checkbox waits for Actions. CR-17 gates every engine task. CR-18 waits for `AGENTS.md`.

Count: 18 tasks. 2 are checked. 16 are open.
