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
| TL-B | CPU correctness | 10 M iterations of xorshift32; expected result `0xA05A0848` (matches `hvm_tests selftest`); timing is diagnostic only |
| TL-C | Graphics | GLES 2.0 offscreen FBO 64×64; vertex-colour triangle; pixel readbacks at three NDC positions check R≥200 / G≥200 / B≥200 respectively |
| TL-D | Audio | Parses the bundled `tone.wav` (original 440 Hz sine); checks rate=44100 Hz, channels=1, samples≈44100, non-zero energy; speaker playback is **not** automatically verified |
| TL-E | Persistence | Writes `0x48564D54` through `NSUserDefaults`; stops; relaunches to the same sandbox; reads and verifies; second launch only reads, never re-imports the IPA |

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
