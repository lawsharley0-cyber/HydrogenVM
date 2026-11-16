# Interpreter performance baseline — 2026-11-16 (WIP)

Owner handoff item 3, issue #493. **Baseline and design findings only: no
optimization, before/after improvement or completion of item 3 is claimed.**
This branch stacks on the smoke harness PR #489; merge/rebase that parent first.

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
runs and absent perf symbols; five synthetic parser tests pass. Parent runtime
gates were 846 fulltest / 47 support / 41 Test Lab, zero new stub findings;
the parent smoke tools suite passed 423 tests with 4 skips. No src/app changes
are made by this measurement PR.

## Remaining before item 3 can be completed

Minion Rush, Groove Coaster Zero and Army of Darkness matching private fixtures
are absent from this bench directory. Obtain their smoke-qualified profiles,
then choose a measured hotspot rather than assuming malloc/ObjC dominates.
The available game's profile alone does not justify a cross-game host-call
rewrite. Add and prove cache-write/flush retirement correctness before extending
the cache. For any proposed speed patch, collect interleaved matching-scene
before/after game and CoreMark numbers and identical conformance/fulltest/smoke
results. No decode-cache, dispatch or host-call optimization is included here.
