# Agent instructions

HydrogenVM is a JIT-less ARMv7 user-mode emulator. Guest code is decoded into `Op` records and executed by ahead-of-time C++ handlers. No guest code is written into executable host memory. One process owns a 4 GB `GuestArena`, an `Engine`, guest threads that are host threads, and a symbol table that binds Mach-O imports to host functions. Frameworks are a guest Objective-C runtime in C++. The UI is composited in software. The phone shell is SwiftUI under `app/`. The desktop CLI is `hvm`.

**Start with [docs/PLAN.md](docs/PLAN.md)**: the only plan, with task cards and the rules for running several agents at once. [docs/GROK_HANDOFF.md](docs/GROK_HANDOFF.md) and [docs/GEMINI_HANDOFF.md](docs/GEMINI_HANDOFF.md) are reference (run recipes, per-game history).

The older [docs/dev-system/MASTER_PLAN.md](docs/dev-system/MASTER_PLAN.md) is superseded by PLAN.md. The comparable-project research behind it is [docs/research/COMPARABLE_PROJECTS.md](docs/research/COMPARABLE_PROJECTS.md). Read only the docs your ticket names.

This file is the working agreement for a coding agent. It does not change the emulator. The detail is in `docs/dev-system/`:

- [ARCHITECTURE_MAP.md](docs/dev-system/ARCHITECTURE_MAP.md) — roles, files, and groups A–E
- [IMPLEMENTATION_PLAN.md](docs/dev-system/IMPLEMENTATION_PLAN.md) — operational, planned, and needs-owner
- [ROADMAP.md](docs/dev-system/ROADMAP.md) — phase order and what may run together
- [BACKLOG.md](docs/dev-system/BACKLOG.md) — scored work
- [STATUS.md](docs/dev-system/STATUS.md) — the current snapshot
- [COMPAT_HARNESS.md](docs/dev-system/COMPAT_HARNESS.md) — how to run and read the gate
- [baselines/linux-headless.json](docs/dev-system/baselines/linux-headless.json) — the committed Linux baseline
- [reports/](docs/dev-system/reports/) — indexed harness runs, after `record` has been used

A 4 KB Linux result is not a 16 KB iPhone memory-model pass. Reports record `host.pageSize`.

Read [USER_APP_REPORTS.md](docs/dev-system/USER_APP_REPORTS.md) before compatibility or performance work. It preserves the owner's unresolved startup, interaction, world-load, hang, missing-music and relaunch observations (UAR-001–013), including Mega Jump as a positive performance control. A passing different app version or short Linux smoke does not resolve those reports. Record scenario-specific evidence without removing the original observations.

## Test Lab is the first gate

Before and after any change under `src/`, run the Test Lab (`TestLab.ipa`, a deterministic 32-bit iOS guest with independently derived expected values) in addition to `hvm_tests fulltest`:

```sh
python3 tools/hvm_testlab.py run --hvm build/hvm --out build/testlab-report   # exit 0 = pass
```

Read [docs/TEST_LAB.md](docs/TEST_LAB.md), especially "Agent guide": what each case covers, how to run one case by hand, the oracle rule (never copy emulator output into an expectation), the blind-spot table (what a green run does *not* prove: shader breadth, threading, keychain, texture formats, perf thresholds, touch/lifecycle), and the bug-class process (every new real-app bug becomes a minimal `TL-X` case that is shown to fail before the fix). A green Test Lab run is necessary, not sufficient: still run the affected real app.

## Parallel-agent rules

1. **Plan first.** Before coding, post a short plan (files, tests, risks) as a comment on the card's issue.
2. **Iteration cap: 8.** Eight edit-build-test cycles per card. If the "done when" line is still false, stop, push what you have as a draft and comment what you learned. Lawson or the coordinator can raise the cap on the issue for that card only.
3. **One editor for hotspot files.** `src/runtime/Process.cpp`, `src/runtime/Process.hpp`, `src/runtime/objc/Runtime.cpp` and `src/runtime/libc/System.cpp` are edited by one agent at a time. The designated agent is named in a `Hotspot editor:` line on the card's issue before parallel work starts. Others wait or rebase after it merges.
4. **Cards that need a game run stay on a runner with the IPAs** (the Gemini VM). Copilot, Codex, Jules and Cursor take code-and-test cards only.
5. **Draft until the gates pass.** Open the PR as a draft, then mark it ready for review once the gates in this file have run and their output is in the PR body. Do not merge it yourself.

## Build and test

Linux, Clang, CMake, Ninja. This image needs `libstdc++-14-dev` even when the 13 package is installed. `libsqlite3-dev`, `libssl-dev`, `zlib1g-dev`, `uuid-dev`, `lld`, and `llvm` are also required. The compat workflow installs that set.

```sh
cmake -S . -B build -G Ninja -DCMAKE_CXX_COMPILER=clang++ -DCMAKE_BUILD_TYPE=Release
ninja -C build hvm hvm_tests hvm_decodecheck hvm_heap_tests
tools/build_tests.sh build/tests

build/hvm_tests selftest
build/hvm_tests fulltest
build/hvm_heap_tests

# One asm guest per file in tests/asm/*.S
build/hvm_tests asm build/tests/<stem>.elf tests/asm/<stem>.S

# Differential C guests. Integer configs use expected_ctests.txt.
# VFP/NEON configs use expected_ftests.txt.
build/hvm_tests c build/tests/ctests_arm.elf build/tests/expected_ctests.txt
build/hvm_tests c build/tests/ftests_vfp_arm_hard.elf build/tests/expected_ftests.txt

python3 -m unittest discover -s tools/tests -p test_verify_ipa.py
python3 tools/test_clash_protocol_probe.py
python3 tools/test_clash_home_state.py

python3 -m unittest discover -s tools/compat_harness/tests -v

python3 tools/hvm_compat.py run \
  --build-dir build \
  --out build/compat-report \
  --baseline docs/dev-system/baselines/linux-headless.json \
  --require-resolved

python3 tools/hvm_compat.py diff \
  docs/dev-system/baselines/linux-headless.json \
  build/compat-report/report.json

python3 tools/hvm_compat.py diff --previous build/compat-report/report.json
python3 tools/hvm_compat.py record build/compat-report/report.json
```

`run` already executes selftest, fulltest, every `tests/asm/*.S` guest, the differential C guests, and those three Python modules, then the manifest smokes. Integer guests: `ctests_arm`, `ctests_arm_os`, `ctests_thumb`, `ctests_thumb_os`, `ctests_thumb_idiv`. VFP/NEON guests: `ftests_vfp_arm_hard`, `ftests_vfp_thumb_softfp`, `ftests_neon_thumb`, `ftests_neon_arm`, `ftests_neon_thumb_os`. A missing required fixture fails the run. Clash Royale is optional, and a skip is not a pass. `--require-resolved` is accepted; the missing-fixture failure does not depend on it. Decoder fuzz stays off unless `--fuzz N` is passed. Do not commit `build/`.

Diff against `docs/dev-system/baselines/linux-headless.json`. Regressions fail the process. MIPS, average frame time, and raw audio sample counts (`frames` and `nonSilentFrames`, ±25% while the mixer stays `active`; a zero baseline is noted when the count is no longer zero) are informational. `diff --previous` compares a report with the prior row in `docs/dev-system/reports/index.json`. Run it on the build report before `record`. After `record`, pass the indexed path that command prints. Passing the build path after `record` is the same comparison: a file equal to the last indexed row is that row, so the previous run is the one before it, not a diff against itself. The first indexed row has no previous report, and `diff --previous` exits 2. That check does not replace the baseline.

`ld64` iOS guests, CoreMark, the simulator, and a device run are not part of this Linux gate. See `COMPAT_HARNESS.md`.

## Ownership

Groups A–E are exclusive. Two agents must not edit the same letter at the same time. The file lists are in `ARCHITECTURE_MAP.md` and `BACKLOG.md`.

| Group | Owns | Parallel |
|---|---|---|
| A memory | `GuestArena`, load/store handlers, the fault path, `mprotect` / `vm_protect` in `System.cpp`, `tests/asm/arm_mem.S` | Not with another A change |
| B pixels | `Shader.cpp`, `Shader.hpp`, `OpenGLES.cpp`, the GLES checks in `TestRunner.cpp`. `Raster.cpp` joins when a compositing change shares the framebuffer | Not with another B change. SIMD waits on the near clip |
| C Clash wire | `tools/clash_*.py`, the Clash protocol and home tests, and `Net.cpp` / `Network.hpp` while the peer is in progress | Not with another C change |
| D frameworks | `src/runtime/objc/`, `src/runtime/foundation/`, UIKit class files except `OpenGLES.cpp` | Not with another D change |
| E Apple shell | `app/**` and `app/HydrogenVM/Bridge/HVMEngine.h` | Not with another E change. Linux cannot compile this group |

What may run at the same time, when the edits do not touch the same file: different letters, using the matrix in `ROADMAP.md`. A may run beside B, C, D (not `System.cpp`), and E while `HVMEngine.h` stays frozen. B may run beside C unless the same patch also edits a dumped shader. C may run beside D if the patch does not edit `Net.cpp`. Audio under `src/runtime/audio/` may run beside B. Do not edit `PlatformStubs.cpp` from two roles at once. The harness and `docs/dev-system/` may run beside a lettered group. `regcomp` and `tmpnam` may run beside D when the patch stays in `src/runtime/libc/` and does not rename Foundation symbols. A loader fix may run beside Group C. It may not run beside a second loader fix. One editor at a time for `LibStdcxx*.cpp`, and for `tools/hvm_report.py` together with `.github/workflows/ci.yml`.

## Workflow

One branch per agent. Open a draft pull request. Do not merge it. Do not force-push. An independent reviewer, not the author, re-runs the suites in this file, or reads the attached JSON including `host.pageSize`, and checks the group boundaries. The owner merges. The agent that wrote the patch does not. Nothing in the repository requires that second reader yet. Turning the requirement on is an owner action.

The integration line for this system is `main`. `docs/HANDOFF.md` now separates the current `main` continuation from archived `gemini-merge` evidence (B-003). A change that stacks on an unmerged parent uses that parent as its pull-request base until the parent lands.

## Rules

- Do not add proprietary binaries, commercial IPAs, signing certificates, or secrets. Fixtures are SHA-256 plus a directory: `HVM_FIXTURE_ROOT` or `--fixture-root`. The harness does not download binaries. Three IPAs already on `main` are grandfathered (`angry-birds`, `tiny-tower`, `art-of-glow`). New ids cannot use a repository path. Deleting those three files is an owner decision.
- Do not trade a passing test, or an app the baseline already runs, for a one-off win. Prefer a fix that helps many apps over a special case for one title.
- Do not mark a backlog row, a status line, or a bug resolved without a reproducible run that shows the new result. The issue generator (`python3 tools/hvm_compat.py issues`) is a dry run. It does not file GitHub issues.
- Do not weaken `fulltest`, and do not change the emulator, in order to make a baseline look green. The harness records. A crash that was already in the baseline stays recorded. A new one fails the diff.
- No FairPlay decryption. `CCCrypt` is forwarded to the host platform library (system CommonCrypto on Apple, OpenSSL on Linux); never add cipher code to the emulator. There is no general outbound network. Loopback IPv4 TCP exists only when that flag is set.

## Where to record status

- `docs/dev-system/STATUS.md` — replace the snapshot when a harness run or a merged fix changes the picture. Append to the decision log. Do not rewrite history. Do not commit `build/`.
- `docs/dev-system/BACKLOG.md` — add discovered work here before starting it. Score stays inside the P0–P2 bands. Needs-owner items are not a coding lap.
- Indexed runs: `docs/dev-system/reports/<platform>/<UTC timestamp>-<short sha>.json`, plus `index.json` and `latest.json`. Commit the JSON only. `record` does not replace `docs/dev-system/baselines/linux-headless.json`. Replacing the baseline is a separate owner decision.
