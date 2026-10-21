# Compatibility harness

The harness runs the existing Linux tests and a hashed app smoke, writes one JSON report, diffs it against a baseline, and prints issue markdown. It does not modify the emulator and it does not file issues.

`tools/compat.py` is unchanged. It is the interactive scanner. This harness is the versioned gate. The package is `tools/compat_harness` because `tools/clash_royale_probe.py` imports `tools/compat.py`.

## Run

```sh
python3 -m unittest discover -s tools/compat_harness/tests -v

cmake -S . -B build -G Ninja -DCMAKE_CXX_COMPILER=clang++ -DCMAKE_BUILD_TYPE=Release
ninja -C build hvm hvm_tests hvm_decodecheck
tools/build_tests.sh build/tests

python3 tools/hvm_compat.py run \
  --build-dir build \
  --out build/compat-report \
  --baseline docs/dev-system/baselines/linux-headless.json \
  --require-resolved
```

`--require-resolved` fails if Angry Birds, Tiny Tower, or Art of Glow cannot be resolved. Clash Royale is optional: a missing file is a skip, and a skip is not a pass.

Other entry points:

| Command | Effect |
|---|---|
| `unit` | `hvm_tests` selftest, fulltest, asm guests, differential C guests, and the three Python modules |
| `apps --only angry-birds` | One smoke |
| `diff baseline.json current.json` | Regression JSON on stdout. Exit 1 when regressions exist |
| `issues report.json` | Dry-run markdown on stdout |
| `issues report.json --write build/issues` | Writes files. Still does not file them |
| `validate report.json` | Required-field check aligned with `schema/report.schema.json` |
| `run --fuzz 20000` | Also runs `tools/decode_fuzz.py`. Default is off |

Exit status is 1 when a unit or Python case fails, a required fixture is missing or its hash does not match, `hvm` does not write a report, the run times out, or the baseline diff has a regression. A guest crash is recorded. It fails the process only when the baseline did not already crash.

## Fixtures

`tools/compat_harness/manifests/apps.json` is schema version 1. Each app has a SHA-256 and a `fixtureRelPath`. The harness never downloads a binary and never copies one into the repository.

Resolution:

1. If `HVM_FIXTURE_ROOT` or `--fixture-root` is set, the file is `<root>/<fixtureRelPath>`. The path must stay inside the root. A hash mismatch is an error. A missing file does **not** fall back to git.
2. Otherwise, three ids may use `legacyRepoPath`: `angry-birds`, `tiny-tower`, `art-of-glow`. Those IPAs are already on `main`. Any other id with a repository path is rejected at load. `HVM_ALLOW_LEGACY_IPAS=0` or `--no-legacy-repo` disables this fallback.

Removing the committed IPAs is an owner decision. The harness is ready for the move: put the same bytes at the `fixtureRelPath` and set the environment variable. History will still contain the old blobs until someone rewrites it, which this project does not do.

Clash Royale's pinned hash matches `tools/clash_royale_probe.py`. The entry is optional. The third-party URL is not fetched.

## What a report contains

`report.json` (`kind` `hvm-compat-report`, `schemaVersion` 1):

- `git` commit, branch, dirty bit.
- `host.pageSize` and a note that 4 KB is not 16 KB.
- `limits` — the headless split below, copied into every report so a result file stays honest without this document.
- `suites.unit` and `suites.python` — one case per existing test, with exit code and a short tail of the output.
- `suites.apps[]` — launch success, crash report, load error, unimplemented imports, missing functions hit during the run, warnings, perf counters from `hvm --report`, frame signal, audio signal, redacted log excerpt, per-app clusters.
- `clusters[]` — the same findings grouped across apps, with `likelyOwner` and group letter.
- `summary.ok` — true when the harness measured what it was asked to measure and the existing tests passed. Existing unimplemented imports do not clear this flag.

`diff.json` ignores wall-clock noise. Hard regressions:

- a passing unit or Python case that fails or disappears
- launch success becoming false, including a required app that becomes a skip
- a new crash, a new load error, a new load-time unimplemented import
- presented frames dropping from a positive count to zero
- frame signal moving from `varied` to `all-black` or `all-white`

Attention, not a failure: a new runtime-only missing function. A 3 second run does not always reach the same call. Imports and misses whose text says the feature is disabled, offline, unavailable, or not supported are clustered as signals (`unavailable`), because several of those stubs are deliberate. Zero-filled data symbols (`kCFNumberNaN` and the infinities) are the same kind of signal. Informational: MIPS, average frame time (±25%), audio sample counts, page-size differences, signal changes that are not a blanking of a varied frame.

Frame signals sample the PNG (about 80 by 120 steps). Near-black is RGB under 16. Near-white is RGB over 240. `all-black` / `all-white` means that class is at least 97% of the samples. `mostly-one-color` means one 4-bit bin is at least 90% and is recorded as a signal, not a regression by itself. Angry Birds can be mostly dark without being a single-color failure. The audit said not to treat a 3 second Angry Birds run as a title-screen proof.

Audio: the harness passes a `socketpair` datagram as `--audio-fd` and drains it so the mixer does not stall the guest. `silent` means the mixer produced samples and they were zero. `not-measured` means the socket could not be created. This is not device audio. The smoke uses a hard `--run-for` stop. `--graceful` lets the guest resign active first and can replace the last frame with a shutdown frame, so it is not the default.

## Headless limits

Verified by reading the tree and by what this Linux VM can execute. The VM is x86_64, Ubuntu, Clang 18.1.3, 4 KB pages. Clang on this image selects the GCC 14 installation, so the link needs `libstdc++-14-dev` even when `libstdc++-13-dev` is installed. `libsqlite3-dev`, `zlib1g-dev`, `uuid-dev`, `ninja`, `lld`, and `llvm` (for `llvm-mc` and the guest tests) are also required. The workflow installs that set.

### Runs on Linux without a display

- Configure and build `hvm`, `hvm_tests`, `hvm_decodecheck`.
- `hvm_tests selftest` and `fulltest` (in-process ObjC, Foundation, UIKit, GLES, audio, sockets, libstdc++, zlib, digests).
- Asm and differential C guests via `tools/build_tests.sh` (`clang --target=armv7-none-eabi`, `ld.lld`). Host expected values come from the same sources compiled for x86_64.
- `python3 -m unittest discover -s tools/tests -p test_verify_ipa.py` (synthetic zips).
- `tools/test_clash_protocol_probe.py` and `tools/test_clash_home_state.py` (in-repo fixtures, no IPA).
- `hvm run` of a decrypted IPA: software framebuffer, `--screenshot` PNG, `--report` JSON. The harness adds the datagram audio socket.
- Optional `tools/decode_fuzz.py` when `--fuzz` is passed and `llvm-mc` is on `PATH`.

### Does not run here

| Gap | Why | What would run it |
|---|---|---|
| SwiftUI `app/`, Xcode device build, simulator UIKit test | No Xcode, no Swift toolchain, no simulator. `tools/sim_test.sh` is macOS-only | Mac with XcodeGen and Xcode, or the macOS Actions job |
| Code signing, entitlements, sideload | No Apple certificate and no device | Owner's signing setup. The macOS job already builds an unsigned IPA when it runs |
| 16 KB page faults and the NULL page on device | This kernel page is 4 KB. Unmapped guest pages that share a host page with a live mapping will not fault the way they do on iPhone | 16 KB host, or a test that simulates the host page. Group A |
| CoreMark and the iOS stub-SDK apps (`CTest`, `ObjCTest`, `FoundationTest`, `UIKitTest`, `ExceptionTest`) | Need `ld64` from `tools/build_cctools.sh`, and CoreMark needs a network clone of eembc/coremark | Optional, slower, not this workflow |
| Clash native two-launch probe and the IMA corpus | The IPA is not in the clone. The harness does not download the third-party release | Owner-approved fixture root plus `tools/clash_home_probe.py` |
| pygame desktop viewer | `tools/desktop.py` wants a display and pygame | A machine with a window |
| Metal, device audio, CoreMotion from real hardware | The engine's GLES path is software. The shell's device services are in the iOS app | Device |
| A green GitHub Actions check | Runs since `239a0ab` die in seconds with an empty step list. `docs/HANDOFF.md` and `docs/DEVELOPMENT_AND_RELEASE.md` attribute that to billing. The vendor string was not readable from the API. This workflow will not start either until that is fixed | Owner account. Local JSON is the evidence in the meantime |
| Decoder fuzz at the ~544,000-sample size cited in older docs | Not the default gate. The audit's own re-run used 20,000 and did not reproduce the larger figure | `run --fuzz` on a schedule (backlog B-107) |

`tools/hvm_report.py` remains the publisher used by `.github/workflows/ci.yml`. It still does not run `fulltest`. The harness does. Pointing the publisher at `fulltest` is backlog, deliberately not mixed into this patch.

## Schema

`tools/compat_harness/schema/report.schema.json` is Draft 2020-12 for external validators. CI uses `tools/compat_harness/schema_check.py`, which checks the same required fields with the standard library. `python3 tools/hvm_compat.py validate report.json` runs that checker.

Screenshots and IPA bytes are not embedded. Logs are truncated, the repository prefix is stripped, and analytics keys and UUIDs in guest logs are redacted. Baseline files under `docs/dev-system/baselines/` are JSON only.

## CI

`.github/workflows/compat-harness.yml` runs on pushes to `main`, on pull requests, and on manual dispatch. Permissions are `contents: read`. It installs the toolchain, runs the harness unit tests, builds, runs the gate against the committed baseline, and uploads `report.json` and `diff.json`.

Until Actions starts executing, reproduce that job locally with the commands at the top of this file. A failed local diff is a real regression. A missing Actions log is not evidence that the suite failed.
