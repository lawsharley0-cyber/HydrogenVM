# Interpreter performance — 2026-11-16

Owner handoff item 3, issue #493. This branch stacks on the smoke harness PR #489;
merge/rebase that parent first.

## Safe live block retirement and `_sys_icache_invalidate`

- `Engine::hleBlocks_` now keeps immutable HLE trampoline blocks separate from
  guest-code `blocks_`, so threads blocked in `callHle` never hold references to
  retirable guest blocks.
- `Cpu::in_engine` / `Cpu::active_gen` track whether a thread is inside `Engine::run`
  (outside HLE calls) and which cache generation it observed on entry.
- `Engine::flushCache()` and `Engine::invalidateRange(addr, size)` move `blocks_`
  into `retired_` tagged with the new generation and reclaim batches once no active
  `Cpu` can still execute them.
- `Posix.cpp` wires `_sys_icache_invalidate` to `Engine::invalidateRange`.
- `tests/runner/TestRunner.cpp` adds a multithreaded self-modifying-code regression
  that failed before the fix (`fulltest: 846 passed, 1 failed`) and passes after
  (`fulltest: 847 passed, 0 failed`).

## Implemented no-JIT optimizations

1. **Compact 16-byte `FastEntry`** (`Cpu.hpp`, `Engine.cpp`, `Handlers.hpp`):
   `FastEntry` now stores `{first, key}` (16 bytes instead of 24 bytes), shrinking
   each thread's direct-mapped indirect-branch cache from 384 KB to 256 KB and
   returning `const Op*` directly from `Engine::enterBlock`.
2. **`drawImage` axis-aligned loop hoisting** (`Raster.cpp`):
   Hoisted `y`-invariant bilinear/row calculations out of the inner `x` loop and
   skipped per-pixel `clip.at(x, y)` calls when `!clip.mask`.

## Interleaved before/after results

Baseline executable: `work/HydrogenVM/build/perf-hvm-baseline` (`6993ed89`,
runtime base `7fdc8a18`). Optimized executable: `build-perf/hvm` on this branch.
Measured on Linux x86_64 / WSL2 / Core Ultra 9 185H / Clang 18.1.3 Release (`-O3`).

### CoreMark (3 interleaved pairs, `0x0 0x0 0x66 0`, CRC-validated)

| Pair | Baseline (iter/s) | Optimized (iter/s) |
|---|---:|---:|
| 1 | 1638.941244 | 1461.133840 |
| 2 | 1432.596342 | 1659.751037 |
| 3 | 1529.402768 | 1418.842225 |

Median: baseline `1529.40` vs optimized `1461.13` (mean `1533.65` vs `1513.24`,
within host thermal/scheduling variance; no CoreMark speedup claimed).

### Games (interleaved matching-scene runs, GPU, internet/FPS overrides off)

| Fixture / scene | Build | Frames | Avg frame (ms) | p95 (ms) | p99 (ms) | Guest MIPS |
|---|---|---:|---:|---:|---:|---:|
| Angry Birds Island 1.7.0 (60 s, taps 20/26/32/42 s) | Baseline | 2,550 | 21.1602 | 21.2337 | 23.8551 | 86.9361 |
| Angry Birds Island 1.7.0 (60 s, taps 20/26/32/42 s) | Optimized | 2,696 | 19.7201 | 20.4472 | 22.2822 | 89.3750 |
| Groove Zero 1.0.0 (40 s, tap `160,385@20`) | Baseline | 2,149 | 11.7378 | 16.3840 | 18.0879 | 11.9221 |
| Groove Zero 1.0.0 (40 s, tap `160,385@20`) | Optimized | 2,261 | 11.2916 | 16.3840 | 17.5636 | 12.0653 |

Angry Birds improved avg frame time by **-6.8%** (`+5.7%` presented frames);
Groove Coaster Zero improved avg frame time by **-3.8%** (`+5.2%` presented frames).
Gates after final `src/` edit: `fulltest` **847 passed, 0 failed**; `support`
**47 passed, 0 failed**; Test Lab **41 passed, 0 failed**; stub audit **0 new/changed**.

## What already exists

`Engine::lookupOrDecode` caches decoded `Block`/`Op` records, `enterBlock` has
a per-CPU fast cache, and blocks link directly to their successor. `Cpu.hpp`
and `BlockBuilder.hpp` already use Clang `musttail` threaded handler dispatch.
Handlers are compiled C++; no guest code is emitted into executable memory.
`ObjC::Runtime::lookup` already has a thread-local method cache with generation
invalidation. A second decode/method cache would duplicate existing machinery.

There is a correctness prerequisite for the requested code-write/cache-flush
invalidation: `Engine.hpp` requires runners stopped before `invalidateRange`
or `flushCache`, because cached blocks are linked by raw pointers.
`libc/Posix.cpp` currently registers `_sys_icache_invalidate` to `p_void2`.
No runtime caller of `Engine::invalidateRange` was found in this audit. This
is a source-level coverage gap, not evidence of a newly reproduced game crash.
Calling the existing flush from a live HLE handler could free its active block
or another thread's block. Before wiring it up, design a safepoint/retirement
scheme and add a minimal self-modifying multi-thread Test Lab case that fails
first. Do not solve it by permitting executable host writes or a JIT.

## Measured configuration

Linux x86_64, WSL2 kernel `6.18.58.1-microsoft-standard-WSL2`, glibc 2.39,
Intel Core Ultra 9 185H, host page size 4096, Clang 18.1.3 Release (`-O3`).
Frozen binary built at `6993ed890543d00c7c7d61a0c2cc866974069641`; its only
changes from runtime base `7fdc8a18` are conformance tests/build targets.
Binary SHA-256:
`83424956aaa8f59f511fd55fbe4ddc07bde0550b9e06be39580ef36ead6368dd`.
Measurements ran after other local build/test/game processes finished.

Available private game: Angry Birds Hatchery Island HD 1.7.0,
`com.rovio.angrybirdsislandhd`, existing unchanged extracted owner fixture.
Fresh home, GPU enabled, network and FPS override off, run 60 seconds; taps in
portrait points: 425,512@20; 380,512@26; 115,936@32; 558,338@42.
This is startup/menu/level-entry and then idle, not active sling gameplay.

| Metric | Measured result |
|---|---:|
| Runner duration | 60.1935 s |
| Presented frames | 3,120 |
| Mean frame | 13.0137 ms |
| p95 / p99 frame | 16.7772 / 18.6122 ms |
| Guest instruction rate | 96.1001 MIPS |
| Missing-API taint / stub hits | false / 0 |
| perf cpu-clock:u samples at 99 Hz | 9,151; zero lost |

Selected **on-CPU** perf symbols (share of user CPU samples): drawImage 5.69%,
memmove 2.57%, UIKit publishFrame 2.12%, memset 1.81%, H_BranchIndirect 1.28%,
FMul handler 1.09%. Some sampled addresses remain unresolved; these samples
cannot justify a complete CPU attribution.

The existing `HVM_GUEST_PROFILE` sampler recorded 100,853 thread/place samples:
13.1% guest and 86.9% HLE. **That is not an 86.9% host-CPU bottleneck:** its
HLE sample table includes usleep (49.05%), UIApplicationMain (24.27%) and
presentRenderbuffer (11.77%), including blocked/waiting threads. StallReport's
normal shutdown snapshot attributed the main's last ten seconds approximately
48% UIApplicationMain, 26% stripped guest `start`, 21% objc_msgSend. It reported
no stall. These samplers answer different questions and must not be combined
as though their percentages share a denominator.

CoreMark uses the existing pinned EEMBC commit
`1f483d5b8316753a742cbf5590caf5bd0a4e4777`, built by `ios_build.py` as an
armv7 Thumb-2 iOS app, `-O2`, PERFORMANCE_RUN=1, iterations=0 (calibration).
All three runs printed **Correct operation validated** with matching CRCs:

| Run | Iterations/s | Timed seconds |
|---|---:|---:|
| 1 | 3027.779880 | 13.211 |
| 2 | 2712.783995 | 14.745 |
| 3 | 3016.591252 | 13.260 |

Median 3016.591252, best 3027.779880. Variation on this laptop is material;
these are one-build baselines. Earlier smoke-run frame numbers were captured
under different host load and **must not be presented as a speedup**.

## Reproduction and evidence handling

```sh
# Use a private, fresh home and an ext4 staging directory for perf.data on WSL.
HVM_GUEST_PROFILE="$OUT/guest-profile.txt" perf record -F 99 -e cpu-clock:u \
  -o "$LINUX_OUT/perf.data" -- build/hvm run "$APP" --home "$FRESH_HOME" \
  --gpu --run-for 60 --graceful --report "$OUT/game.json" \
  --tap 425,512@20 --tap 380,512@26 --tap 115,936@32 --tap 558,338@42
perf report --stdio --no-children --sort symbol --fields overhead,symbol \
  -i "$LINUX_OUT/perf.data" > "$OUT/host.txt"
# Repeat three times with separate fresh homes; require CRC validation and >=10 s.
build/hvm run build/ios/CoreMark.app --home "$CM_HOME" --quiet -- 0x0 0x0 0x66 0
python3 tools/perf_summary.py --report "$OUT/game.json" --perf "$OUT/host.txt" \
  --coremark "$OUT/coremark-1.log" --coremark "$OUT/coremark-2.log" \
  --coremark "$OUT/coremark-3.log" --out "$OUT/summary.json"
```

The first capture's perf ring-buffer write failed with EFAULT on the Windows
mount. Repeating on Linux ext4 succeeded; the failed attempt is not counted.
Raw private evidence is retained locally in `work/perf-evidence-2/`, outside
Git. Only these aggregate measurements are committed. `perf_summary.py`
rejects crashed/missing/invalid metrics, unvalidated or sub-ten-second CoreMark
runs and absent perf symbols; six synthetic parser tests pass. Parent runtime
gates were 846 fulltest / 47 support / 41 Test Lab, zero new stub findings;
the parent smoke tools suite passed 423 tests with 4 skips. No src/app changes
are made by this measurement PR.

## Remaining before item 3 can be completed

Matching Minion/Groove fixtures have now been measured (follow-up below).
An expanded search also found Army of Darkness 1.0.1 outside
the primary bench. Its 90-second tutorial scenario fails graceful shutdown on
both unchanged runtime `7fdc8a18` (frozen tests-only build `6993ed89`) and profile
refactor `2474be76`: exit 70, `graceful-stop-watchdog`, signature `d5c54ff6d981`.
The baseline perf capture contains 15,812 user CPU samples, but this failed run
is **excluded** from successful performance baselines; raw evidence stays in
`work/perf-aod-evidence/`. Tracked as **#496**; resolve/minimize the shutdown failure first.

Resolve the failed Army scenario and qualify the intended gameplay scenes,
then choose a measured hotspot rather than assuming malloc/ObjC dominates.
The available game's profile alone does not justify a cross-game host-call
rewrite. Add and prove cache-write/flush retirement correctness before extending
the cache. For any proposed speed patch, collect interleaved matching-scene
before/after game and CoreMark numbers and identical conformance/fulltest/smoke
results. No decode-cache, dispatch or host-call optimization is included here.

## Supplied Minion/Groove baseline follow-up

The owner's matching archives resolve the earlier missing-fixture blocker.
Archive identities are recorded in smoke-harness research. Frozen executable
and source provenance are identical to the Angry Birds baseline above.
Each run uses a separate fresh home, GPU, default-off internet/FPS overrides,
cpu-clock:u at 99 Hz and HVM_GUEST_PROFILE. Both exit 0 without timeout, crash,
stall or missing-API taint; zero stub hits and zero lost perf samples.

| Fixture / duration | Frames | Mean ms | p95 ms | p99 ms | Guest MIPS |
|---|---:|---:|---:|---:|---:|
| Groove Zero 1.0.0 / 60.4505 s | 3,448 | 10.2031 | 12.0586 | 16.7772 | 2.49985 |
| Minion Rush 1.0.0 with data / 300.568 s | 3,507 | 40.2809 | 96.469 | 142.606 | 56.0791 |

Selected on-CPU shares: Groove drawImage 9.01%, memmove 4.90%, memset 3.46%,
publishFrame 2.18%. Minion memset 1.02%, memmove 0.82%, drawImage 0.64%,
H_BranchIndirect 0.57%, H_MultiPopPc 0.41%, Engine::callHle 0.35%.
Unresolved addresses remain; no complete CPU attribution is claimed.
Thread/place sampling has 46,404 Groove samples (96.7% HLE) and 4,929,089
Minion samples (98.2% HLE), including waiting threads. These are not CPU shares.

Screenshots show Groove title and Minion's introductory notice. The historical
tap does not dismiss the Minion notice. No terms were accepted; these captures
qualify startup/menu only, not level entry. The profile-refactor smoke runs also
pass, but those unprofiled timings are not a before/after speed comparison.
No interpreter/host-call change or speedup is claimed.

Private evidence is retained outside Git in work/found-fixture-perf-0n5wtad4/.
The 38 MB Minion perf report exposed excessive regex backtracking on wide padding.
The summary parser now splits the explicit symbol marker and optional columns;
six tests pass, including a 10,000-space row. Both actual reports parse
successfully. This evidence-tool repair does not optimize guest execution.
