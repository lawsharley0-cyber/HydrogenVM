# HydrogenVM performance

## What "100% speed" means

The goal is for guest code to run at least as fast as it did on the original
device. The reference is the last and fastest 32-bit iPhone, the **iPhone 5
(Apple A6, two Swift cores at 1.3 GHz)**. Earlier devices are reported too.

Speed is measured with **CoreMark**, EEMBC's standard embedded CPU benchmark.
It is built as a real 32-bit iOS app (Thumb-2, `-O2`, the iOS armv7 ABI) and
run through the whole HydrogenVM stack: Mach-O loader, libSystem HLE, and the
interpreter. CoreMark's own checksums must validate.

Reference scores are estimated as clock × CoreMark/MHz for each CPU. They live
in `tools/perf_targets.json`:

| Device | CPU | Clock | CoreMark/MHz (approx.) | Single-core CoreMark |
|---|---|---:|---:|---:|
| iPhone 3GS | Cortex-A8 | 600 MHz | 2.4 | 1440 |
| iPhone 4 | A4 (Cortex-A8) | 800 MHz | 2.4 | 1920 |
| iPhone 4S | A5 (Cortex-A9) | 800 MHz | 3.0 | 2400 |
| **iPhone 5** | A6 (Swift) | 1.3 GHz | 3.5 | **4550** |

The per-MHz figures are approximate: published ranges are about 2.0–2.6 for
Cortex-A8, 2.8–3.3 for Cortex-A9 and 3.3–3.8 for Swift, and the result depends
on the compiler. They should be replaced with measured device numbers when
those are available. As a sanity check, HydrogenVM counts about 350k guest
instructions per CoreMark iteration. At 2.4 CoreMark/MHz that implies a
Cortex-A8 IPC of about 0.85, which is plausible for that core.

The report also gives an **effective clock**: emulator CoreMark ÷ CoreMark/MHz.
It shows how fast an A8, A9 or Swift core would need to run to match the
emulator, without depending on the device estimates.

Two other things affect real-app speed beyond this CPU number:

* **Framework code runs natively.** UIKit, Foundation, CoreGraphics,
  compositing, `memcpy` and libm are host code at full speed, so real apps
  spend much less time in the interpreter than CoreMark does.
* **Guest threads run on real host cores.** A dual-core A5/A6 workload uses
  two (or more) host cores.

## How to measure

```sh
python3 tools/hvm_report.py                 # everything: tests + full CoreMark (>= 10 s)
python3 tools/hvm_report.py --quick         # shorter benchmark runs
python3 tools/hvm_report.py --quick --history perf/history.csv   # also log a history line
tools/cm.sh build/hvm 5                     # best of 5 quick CoreMark runs (A/B comparisons)
```

Shared cloud VMs vary by ±10% from run to run. Compare builds with best-of-N,
interleaved, rather than single runs. CI (`.github/workflows/ci.yml`) runs the
report on Linux for every push. It runs on Apple Silicon macOS for commits
containing `[mac]` and once a week. Each run commits its `report.md` and
`report.json` to the `ci-reports` branch, under `latest/<job>/` and
`runs/<date>-<run>-<job>/`.

### Finding what to optimise

* `valgrind --tool=callgrind build-prof/hvm run build/ios/CoreMark.app --quiet -- 0x0 0x0 0x66 150`
  gives deterministic host-instruction counts per handler (build with `-O2 -g`).
* `-DHVM_PROFILE_PAIRS` builds an engine that counts which handler follows
  which. `tools/pairprof.py <hvm> pairprof.txt` lists the hottest ops and
  pairs, which are the candidates for fusion and superinstructions.

## Optimisation log

CoreMark on this project's x86-64 cloud VM, best-of-N runs. Numbers vary by
about ±10% between sessions, so only compare rows measured in the same session.

| Change | CoreMark | vs iPhone 4 | vs iPhone 5 |
|---|---:|---:|---:|
| Baseline (threaded interpreter, block linking, dead-flag elimination), range 1,670–1,780 | ~1,720 | 90% | 38% |
| Compare+branch, cond-skip+branch and CBZ fused into single exit ops; links point at the first op; inline block-cache probe for indirect branches; instruction count derived from the budget | ~1780 | 93% | 39% |
| Per-condition CMP+branch handlers; no r15/T writes on chained branches; `preserve_none` enabled for Apple clang | ~1880 | 98% | 41% |
| Side exits: conditional exits no longer end blocks, so the not-taken path continues in-block | ~2050 | 107% | 45% |
| Pair superinstructions for the 27 hottest simple ops (same session, 5 runs each: median 2,441 vs 2,192 without pairs; best 2,542 vs 2,244) | ~2,440 | ~127% | ~54% |
| GEMINI `fuseOps` (MOVW+MOVT, BL call fusion, POP {pc}, LDR pairs), merged with the above; same session, base `08f6a9d` median 2,162 | (see row above) | | |

Apple Silicon numbers come from the macOS CI job (see `ci-reports`).

## Ideas not done yet

* Keep the time-slice budget in a register (a 4th handler argument) instead of
  loading and storing it at every block exit.
* Predicated forms of the hottest ops (CondSkip + op fused) for IT blocks and
  ARM conditional execution.
* Return-address prediction (a small return stack) for `BX LR` / `POP {pc}`.
* Inline the guest's own `memcpy`/`memset`/`strlen` (and other hot
  libSystem routines) when they are statically linked into the app.
* Superblocks that follow unconditional branches within a page.
* **Graphics & Software Rasterizer Optimizations**:
  * Profiled across 3D Unity titles (e.g. RETRY, Goat Simulator) showing `avgFrameMs` ~65 ms and `avgCompositeMs` ~41 ms on CPU rasterization:
    * **SIMD Triangle Rasterization**: Vectorize barycentric coordinate stepping and depth buffer comparison (`writeFragment` / `rasterizeTriangle`) using 4-wide SSE2 / AVX2 / ARM Neon.
    * **Multi-threaded Slicing**: Divide framebuffer scanlines horizontally across worker threads in the software rasterizer during large `glDrawArrays` / `glDrawElements` calls.
    * **Dirty-Rect Compositing**: Track modified framebuffer regions in UIKit `present` to avoid full-screen 1136x640 RGBA blits when only subregions update.
    * **Fast-Path Texture Sampling**: Pre-convert PVRTC / GL formats to native host 32-bit RGBA on upload (`glTexImage2D`) to eliminate runtime decompression on every fragment.
