# HydrogenVM Test Lab

A 32-bit ARMv7 iOS guest app for compatibility testing and performance measurement.
Version 1.0 · five initial cases · schema version 1.

## Contents

- [Overview](#overview)
- [Test cases](#test-cases)
- [Quick start — automated (Linux/macOS)](#quick-start--automated-linuxmacos)
- [Phone import and use](#phone-import-and-use)
- [Report format](#report-format)
- [Architecture](#architecture)
- [Validation gaps](#validation-gaps)
- [Next milestone](#next-milestone)

---

## Overview

The Test Lab IPA is a genuine 32-bit ARMv7 app built through the same
toolchain as the other iOS guests (`tools/ios_build.py`).  When imported into
HydrogenVM on an iPhone it shows a menu; in automated mode each case runs as a
separate process with an external timeout.

Five cases cover import integrity, CPU correctness, GLES correctness, audio
signal, and sandboxed persistence.

---

## Test cases

| ID | Name | What it checks |
|---|---|---|
| TL-A | Import and launch | `CFBundleExecutable`, `CFBundleIdentifier`, `CFBundleShortVersionString`, `tone.wav` resource present, `empty.dat` (zero-length) resource present |
| TL-B | CPU correctness | 10 M iterations of xorshift32; expected result `0x7E6798F5`, derived from a native x86-64 build of the same loop (see [Oracle rule](#oracle-rule)); timing is diagnostic only |
| TL-C | Graphics | GLES 2.0 offscreen FBO 64×64; vertex-colour triangle; probes at pixels (32,55), (13,18), (50,18), just inside each corner, check R≥200 / G≥200 / B≥200 respectively |
| TL-D | Audio | Parses the bundled `tone.wav` (original 440 Hz sine); checks rate=44100 Hz, channels=1, samples≈44100, non-zero energy; speaker playback is **not** automatically verified |
| TL-E | Persistence | Writes `0x48564D54` through `NSUserDefaults`; stops; relaunches to the same sandbox; reads and verifies. Re-importing the IPA keeps `Documents/`, `Library/` and `tmp/`, so the second launch uses the normal `hvm run <ipa>` path |

### Checkpoint / interrupted-case protocol

Before each risky section the app writes `Documents/testlab_checkpoint.json`
containing the case ID and a timestamp.  On normal completion it clears the
file.  On the next launch, if the checkpoint exists it was not cleared — the
case is reported as `interrupted` (the cause could be a crash, forced
termination, or power loss; the checkpoint alone cannot distinguish them).

The host orchestrator never automatically retries an interrupted case.  The
owner or the workflow step continues with remaining cases.

---

## Quick start — automated (Linux/macOS)

### Prerequisites

Same as the compat harness (see `AGENTS.md`): Clang, `cmake`, `ninja`,
`python3`, `lld`, `llvm`, `zlib1g-dev`, `libsqlite3-dev`, `uuid-dev`.
Also needs `ld64` from `tools/build_cctools.sh`.

```sh
# Build the engine
cmake -S . -B build -G Ninja -DCMAKE_CXX_COMPILER=clang++ -DCMAKE_BUILD_TYPE=Release
ninja -C build hvm

# Build ld64 (one-time)
tools/build_cctools.sh /opt/cctools
export HVM_LD64=/opt/cctools/bin/arm-apple-darwin11-ld

# Generate the bundled tone.wav (already committed; only needed if regenerating)
python3 tests/ios/apps/testlab/gen_tone_wav.py tests/ios/apps/testlab/tone.wav

# Run all five cases and write a report
python3 tools/hvm_testlab.py run \
    --hvm build/hvm \
    --out build/testlab-report

# Run a single case
python3 tools/hvm_testlab.py run \
    --hvm build/hvm \
    --case TL-B \
    --out build/testlab-report

# Validate an existing report
python3 tools/hvm_testlab.py validate build/testlab-report/testlab_report.json
```

### Exit codes

| Code | Meaning |
|---|---|
| 0 | All requested cases passed |
| 1 | One or more cases failed |
| 2 | One or more cases were interrupted |
| 3 | Tool / build error |

### Schema unit tests

```sh
python3 -m unittest discover -s tools/testlab_schema/tests -v
python3 -m unittest discover -s tools/tests -p test_hvm_testlab.py -v
```

---

## Phone import and use

1. Build the IPA (see [Quick start](#quick-start--automated-linuxmacos)) or
   download the CI artifact from the `testlab-report` artifact on the
   `testlab.yml` workflow run.
2. Import it into HydrogenVM using the **+** button in the Library view,
   *Open in…* from Files, or drop it into the app's Files folder and pull to
   refresh.
3. Tap `TestLab` in the library to run it.
4. The menu shows five rows (`A: Import`, `B: CPU`, `C: GLES`, `D: Audio`,
   `E: Persist`) and their last-known status (pending/passed/failed/interrupted).
5. Tap a row to run an individual case.  Tap **Run All** to run every case.
6. Tap **Export JSON** to write `Documents/testlab_result.json` — retrieve it
   via the Files app.
7. For TL-E (Persistence):
   - Tap **E: Persist** on first run.  Status shows "phase 1 written".
   - Stop the app from the HydrogenVM run screen.
   - Relaunch TestLab and tap **E: Persist** again.  Status should show "passed".
   - Do **not** use *Reset App Data* between phases — that would delete the
     sandbox and the persisted value.

### What the phone checks that Linux cannot

| Check | Linux | Phone |
|---|---|---|
| GLES pixel values | ✓ (software rasterizer) | ✓ (same software path) |
| Audio WAV parsing | ✓ (PCM decode check) | ✓ |
| Speaker playback | ✗ | Manual listener check only — not automatically verified |
| 16 KB host-page behaviour | ✗ | ✓ (see B-101) |
| EAGLContext on real Metal | ✗ | ✓ (but GLES is software even on device) |
| NSUserDefaults persistence across real relaunches | ✗ (one process per hvm run) | ✓ (real iOS sandbox) |

---

## Report format

Both the guest app and the host orchestrator write JSON files conforming to
schema version 1 (`tools/testlab_schema/testlab_result.schema.json`).

Validate any report with:

```sh
python3 tools/testlab_schema/validate_result.py build/testlab-report/testlab_report.json
```

### Guest report (`Documents/testlab_result.json`)

```json
{
  "schemaVersion": 1,
  "kind": "hvm-testlab-result",
  "generatedAt": "2026-10-22T01:00:00Z",
  "runId": "000004A2E96E9000",
  "appVersion": "1.0",
  "appBuild": "1",
  "sandboxMode": "hvm-home",
  "cases": [
    {
      "id": "TL-A",
      "version": 1,
      "status": "passed",
      "expected": "CFBundleExecutable=TestLab, version=1.0, tone.wav present, empty.dat present",
      "actual":   "CFBundleExecutable=TestLab, version=1.0, tone.wav=present, empty.dat=present",
      "durationMs": 2.1,
      "clockDefinition": "CLOCK_MONOTONIC",
      "checkpointWritten": true,
      "diagnostics": { ... }
    },
    ...
  ]
}
```

### Host report (`build/testlab-report/testlab_report.json`)

Extends the guest schema with host metadata:

```json
{
  "kind": "hvm-testlab-host-report",
  "host": {
    "system": "Linux",
    "machine": "x86_64",
    "pageSize": 4096,
    "compiler": "clang version 18.1.3",
    "emulatorCommit": "39904cb...",
    "emulatorDirty": false
  },
  ...
}
```

`emulatorCommit` is `null` when git is not available.  It is never inferred
from a filename.

A **4 KB Linux result is not a 16 KB iPhone memory-model pass** (see B-101).
Reports record `host.pageSize`.

---

## Architecture

```
tests/ios/apps/testlab/
  main.m          — guest app: menu UI, five cases, checkpoint/result JSON
  gen_tone_wav.py — generates tone.wav (original 440 Hz sine)
  tone.wav        — committed resource (1 s, 44100 Hz, mono, 16-bit PCM)
  empty.dat       — committed zero-length resource (tests stored ZIP entry)

tools/
  hvm_testlab.py         — host orchestrator: build, run, timeout, report
  testlab_schema/
    testlab_result.schema.json  — Draft 2020-12 schema, version 1
    validate_result.py          — lightweight validator (no jsonschema dep)
    tests/
      test_validate_result.py   — schema unit tests

tools/tests/
  test_hvm_testlab.py   — orchestrator unit tests (IPA check, parse, schema)

.github/workflows/
  testlab.yml    — CI: build IPA, run each case, validate report, upload artifact
```

### Responsibility split

1. **Guest (`main.m`)**: exercises ARMv7 ABI and public guest APIs; writes
   checkpoint before risky code; writes result JSON to `Documents/`.
2. **Host runner (`hvm_testlab.py`)**: builds IPA, launches one case per
   process, applies external timeouts, assembles host-side report.
3. **Phone menu**: interactive; shows live status; supports individual and
   Run-All execution.

These layers share the same case IDs, schema, and checkpoint format.  There is
no separate interactive-vs-automated implementation.

---

## Validation gaps

| Gap | Reason | What would close it |
|---|---|---|
| TL-C pixel accuracy on a 16 KB device | Linux GLES is the same software path, but device page layout differs | Phone run with Export JSON |
| TL-D speaker playback | Mixer signal verified; actual sound requires a listener | Manual phone check |
| TL-E two-launch persistence on device | hvm runs one process per case | On-device relaunch via the phone UI |
| macOS/Apple-Silicon GLES code path | Linux has no Metal | macOS CI job (`[mac]` commit or Monday schedule) |
| Interrupted-case from real crash (not forced termination) | Test only uses exit-code and timeout | A deliberate crash case (future milestone) |

The Linux CI workflow marks these gaps in its step output.  A green CI run
does not claim phone-equivalent behaviour.

---

## Next milestone

1. **Verify on device** — import `TestLab.ipa` from the CI artifact, run all
   cases, export JSON, confirm all five pass.
2. **TL-E on device** — verify the two-launch persistence path works in the
   real iOS sandbox (NSUserDefaults survives a stop/relaunch cycle).
3. **TL-C tolerance document** — record the exact pixel values from the device
   run and compare against Linux; tighten or relax the ≥200 threshold if needed.
4. **Performance cases** — add TL-F (CoreMark-style throughput with warm-up and
   repeated runs) per the `PERFORMANCE.md` guidelines after B-101 lands.
5. **Compat-harness integration** — add TestLab as an optional harness app
   entry once the IPA hash is stable and a fixture root is available.

---

## Agent guide — Test Lab is the first gate for every emulator change

### What the IPA is for

`TestLab.ipa` is a small, deterministic, 32-bit ARMv7 iOS app whose pass/fail
answers are known *independently of the emulator*. It is the fastest way to tell
whether a change to the emulator broke something basic. It is **not** a
substitute for running real apps (see [Blind spots](#blind-spots)).

| You changed… | Run | Why |
|---|---|---|
| Anything in `src/` | `hvm_tests fulltest` **and** `hvm_testlab.py run` | unit tests cover internals; Test Lab covers the whole import → launch → result path |
| Zip / IPA import / sandbox (`src/loader/`) | Test Lab (TL-A, TL-E) | Test Lab found the empty-entry zip bug and the re-import data-loss bug |
| CPU / decoder / threaded interpreter | Test Lab TL-B plus `hvm_tests selftest` | independent native oracle |
| GLES / shader compiler / rasterizer | Test Lab TL-C plus a real shader-heavy app | TL-C only uses a passthrough shader |
| Audio | TL-D plus a real app | TL-D checks decode only, not the mixer |

### Run it yourself (no phone needed)

```sh
# one-time: ld64 (no sudo? use a prefix you own)
tools/build_cctools.sh $HOME/cctools
export HVM_LD64=$HOME/cctools/bin/arm-apple-darwin11-ld

cmake -S . -B build -G Ninja -DCMAKE_CXX_COMPILER=clang++ -DCMAKE_BUILD_TYPE=Release
ninja -C build hvm hvm_tests

python3 tools/hvm_testlab.py run --hvm build/hvm --out build/testlab-report   # exit 0 = all pass
python3 tools/hvm_testlab.py run --hvm build/hvm --case TL-C --out /tmp/tl_c    # one case
python3 tools/hvm_testlab.py validate build/testlab-report/testlab_report.json
```

Run a single case by hand and read the guest's own result file — this is the
fastest debugging loop:

```sh
build/hvm run build/ios/TestLab.ipa --home /tmp/tl --run-for 20 -- --run TL-C
cat /tmp/tl/com.hydrogenvm.test.testlab/Documents/testlab_result.json
```

`diagnostics` in each case holds the raw values (pixel RGBA, hashes, byte counts).
Always read those before deciding whether the emulator or the test is wrong.

### On the phone

Import the IPA (CI artifact `testlab-report`, or build locally), open it, tap
**Run All**, then **Export JSON**. Compare against the Linux report: the same
case should produce the same `actual` values.

### Oracle rule

A test is only as good as its expected value. Two of the original five cases
were wrong the first time they ran (TL-B's constant, TL-C's probe pixels) — the
emulator was right and the *test* was wrong. Therefore:

1. Every expected value must be derived from something that is **not** the
   emulator: a native build of the same C, a hand calculation, a spec vector.
2. Write down *how* it was derived in a comment next to the constant.
3. Never "fix" a failing case by copying the emulator's output into the
   expectation. If emulator and oracle disagree, find out which is wrong.

### Blind spots

Things real apps hit that Test Lab currently does **not** exercise. A green
Test Lab run says nothing about these:

| Area | Evidence from real apps | Needed case |
|---|---|---|
| GLSL breadth | Goat Simulator: all 108 UE3 programs failed to compile (helper functions before `main`, nested scopes, large uniform arrays, `ivec`/`int`/`bool`, builtins such as `clamp`/`mix`/`pow`, `#extension`, 3-arg `texture2D`). TL-C uses a 1-line passthrough shader | TL-F: shader feature matrix, one program per feature, readback compared with a CPU reference |
| Threading / run loops | Cocos2d games crashed on thread-affine GL / `NSRunLoop` use | TL-G: worker thread, `performSelectorOnMainThread`, `NSTimer`, `CADisplayLink` ordering |
| Keychain / secure storage | Tiny Tower | TL-H: `SecItemAdd/CopyMatching/Delete` round trip across relaunch |
| Texture formats | NPOT, mipmaps, PVRTC, premultiplied alpha | TL-I: upload each format, read back |
| Performance regressions | Only timing diagnostics, no thresholds; no frame-rate, allocation or syscall pressure | TL-J: fixed workloads with recorded MIPS/fps and a tolerance against `docs/dev-system/baselines/` |
| Touch / lifecycle | Taps drive every real game; background/foreground, memory warnings | TL-K: injected touches (`--tap`), `applicationDidEnterBackground` |
| Audio path | TL-D parses WAV only; mixer/AudioQueue/OpenAL untested end-to-end | TL-L: play through the mixer, compare PCM energy |
| Big/odd archives | Empty zip entries, data descriptors, zip64, long names | Extend TL-A fixtures |
| Crash handling | Interrupted-case protocol never exercised by a real crash | TL-M: deliberate crash case |

### Bug-class process (how a new kind of bug becomes a permanent test)

1. **Reproduce** in the real app. Record the symptom: crash, blank screen,
   wrong output or slow. Capture the log, a screenshot and a perf report.
2. **Classify** it against the blind-spot table. If it fits an existing row,
   extend that case; if not, add a row.
3. **Minimise**: write the smallest guest code that shows the same failure
   (a shader, a thread pattern, a file layout). Add it as a new `TL-X` case in
   `tests/ios/apps/testlab/main.m` with an independent oracle.
4. **Prove the case fails** on the unfixed emulator (`git stash` the fix, or
   run on the base commit). A case that never failed proves nothing.
5. **Fix the emulator.** Then run `hvm_tests fulltest`, `hvm_testlab.py run`,
   and the real app again.
6. **Record**: add the case to the table above, update `docs/COMPAT.md` with
   the app, symptom, root cause and the Test Lab case that now guards it.
7. **Performance**: if the fix or the case changes speed, add the number to
   `docs/PERFORMANCE.md`; do not accept a slowdown without writing it down.

Adding a case: add a `runCaseX`, register it in the case-id lists (menu,
`--run`, `--runall`), add the id to `CASE_IDS` in `tools/hvm_testlab.py`, bump
nothing in the schema unless fields change, and add the row to this file.

### Lessons already learned (keep adding here)

- Empty zip entries are stored by some tools and deflated by others. Both must import (TL-A).
- Re-importing an IPA must keep `Documents/`, `Library/` and `tmp/`; it replaces only the `.app` bundle (TL-E).
- When you see "no `TESTLAB` line in stdout", run the case by hand: the failure is usually an import or loader error on stderr.
