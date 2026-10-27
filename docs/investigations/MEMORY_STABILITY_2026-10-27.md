# HydrogenVM memory stability investigation — 2026-10-27

Status: unresolved; diagnostic work, not a confirmed Jetsam fix.

## Evidence and scope

Target: Clash Royale 2.1306.1, `com.hydrogenvm.clashroyale.guestdev`,
iPhone18,2 / iOS 27.2. Source baseline: main `0170a0b`.

The owner's handoff reports headroom falling from 2,402 MB at 15:10:04 to
11.1 MB at 15:19:59 (2,390.9 MB / 595 s, about 4.02 MB/s). These are available
process-memory readings, not resident-allocation measurements. The two
same-PID Jetsam snapshots report 241 MB suspended versus 2,566 MB foreground;
activity differs and this is not by itself proof of a leak. The 15:15 report
killed momentsd. Neither its freeze-skip field nor `interrupted-host-session`
proves HydrogenVM was killed for memory. The actual `.ips` and interrupted
session files were not attached or present in this workspace; these figures
are transcribed evidence, not independently parsed diagnostics.

No commercial fixture exists in `/workspace/library-files`, `/workspace/shared`
or `/workspace/scratch`. The guest-owned transport, checkpoints and progression
implementation is in draft PR #138, not this main checkout. Do not substitute
main's earlier Python peer for that guest when testing this incident.

Open work checked through GitHub: PR #138 (offline Clash continuation), PR #118
(failed Jetpack task), issue #33 (Claude's graphics/shell reservation). This
branch does not modify their implementation or change gameplay, saves, AI,
rewards, network policy, graphics ownership or cache eviction. No agents were
spawned. The original checkout was clean; work uses a separate feature branch.

## Ranked hypotheses, not diagnoses

| Rank | Source | Code evidence | Discriminating observation |
|---|---|---|---|
| 1 | Guest heap retention / fragmentation | `Heap::ensureCommitted` maps 4 MB chunks; `free` returns blocks to size-class/best-fit lists; `bigFree` can rewind `top_` but never decommits. `stats().committed` therefore remains a high-water mapping count. | Live heap bytes return after a battle, while committed mappings and RSS remain high. Stable reuse on repeat is different from an unbounded leak. Host allocator and page residency still need attribution. |
| 2 | Graphics objects, upload staging, driver retention | HostGL forwards allocation/deletion to the driver; the software sharegroup owns texture/renderbuffer vectors; native drawable storage and shared contexts have separate ownership. Several recent main commits touch these paths. No per-object byte accounting exists. | Rising texture/renderbuffer/storage counts at equal home-screen milestones; resident/physical growth while guest heap remains flat. Compare GPU and software with identical input; renderer changes are a diagnostic control, not a fix. |
| 3 | Guest asset/font caches and Objective-C ownership | `imageNamed:` caches immortal images in `UIState::imageCache`; autorelease pools drain in run-loop/frame paths, but guest and worker object lifetimes need exact trace evidence. Guest `.sc` / ResourceManager caches are opaque native guest allocations. | Repeated same-arena loads grow live heap/objects/cache bytes rather than plateau; identify stack/asset keys in the owning guest build. Font exhaustion alone does not prove retention. |
| 4 | Audio buffers / queued completion metadata | Mixer voices own shared PCM references and processed-tag vectors. OpenAL/player/decoder owners may retain PCM outside the mixer. | Queue PCM / buffer / processed-tag growth while idle or across equal battle transitions. Mixer counters cover only its references, not every decode cache. |
| 5 | GuestOffline responses, checkpoints, reward journal | These live in #138's guest implementation and cannot be attributed from main's runtime. | Guest-side live message count/capacity, queue depth, pending response bytes, snapshot count/capacity grow after delivery/commit. Aggregate guest heap cannot identify these categories. |
| 6 | Decoded CPU blocks / host presentation and logs | Engine caches decoded blocks; startup code exploration can increase this legitimately. The shell bounds log/sample counts; queued frame/log delivery can still require profiling under load. | Decode counters plateau while RSS continues rising, or Time Profiler / Allocations identifies queued host buffers. Count bounds do not bound individual object sizes. |

Recent changes reviewed include `7b1cd3f` (renderbuffers), `65a5778` (audio and
shared program ownership), `b67f147` (EAGL worker contexts), `eb4c99b` (VAOs),
`1172b74` (lifecycle/mmap), `f78a63a` (FBO tracking). The heap's last functional
change is `60106d2` (aligned allocation validation). Chronology is not causality.

## Reproduction and required device evidence

Preserve the original owner save. Use copied homes, pinned IPA/executable hashes,
exact runtime commit, renderer, page size, screen, inputs and stage timestamps.
Record a clean pre-launch state, RSS, physical footprint, available headroom and
all available subsystem counters. Do not compare different saves or guest builds
as before/after evidence.

1. Idle on the library screen for 10 minutes (host Instruments / Xcode memory
   capture; no guest process exists here).
2. Launch the exact guestdev IPA; idle at home for 10 minutes.
3. One complete original training battle, return home, wait 60 seconds.
4. On a separate earned Arena-1 copied save, complete ten consecutive battles.
   Record immediately before entry, arena loaded, result/reward, home returned,
   and 60 seconds after return. Keep outcomes and progression checks.
5. Repeat home/battle transitions separately; distinguish new arena assets from
   repeated identical assets. Do not reset caches between iterations.
6. Continue a 30–60-minute session, then background/resume and close/relaunch.
   Capture memory warnings, any termination and the matching Jetsam report.

For each stage report entry/exit/settled bytes, peak footprint, delta per second,
resource counts and rewards/save equivalence. A temporary peak that falls is not
permanent retention. A high mapped arena with low live heap may be reusable
capacity. A rising post-return floor across ten battles requires attribution.
Cloud 4 KB pages cannot qualify the 16 KB device case.

The analysis helper accepts an exported report's `memorySamples` array or JSONL:

```sh
python3 tools/hvm_memory_report.py session.json --out summary.json
```

It preserves null/unsupported counters, reports each counter's observed peak and
endpoint rate, and refuses reversed timestamps. An endpoint rate is descriptive,
not a leak detector. It never interprets declining available memory as RSS growth.

## Separate compatibility backlog

| Issue | Priority | Relation to this investigation |
|---|---|---|
| Font texture exhaustion / repeated font-cache rebuild | Medium | Instrument churn and allocation lifetimes; no evidence yet that it causes this stop. |
| CTLineGetOffsetForStringIndex missing | Medium | Compatibility; reproduce and specify independently. |
| demon_step_01.wav unsupported/silent | Low | Audio fidelity; track decoded buffer ownership separately. |
| Path length 0 / idle AI warnings | Medium | Native AI/pathfinding investigation; preserve original AI. |
| Command 522 error 7; 529 error 5 | Medium | Guest command/progression semantics; separate from allocator work. |
| 35 unresolved launch imports | Compatibility | Static imports do not prove a called fatal function. |
| NO loader resource warnings | Investigate if reproduced | Correlate exact identifiers, assets and failed loads. |

StoreKit/network restrictions are intentional; no causal claim is made about them.

## Implemented diagnostics and boundaries

- `GuestArena::stats()` reads mapped 4 KB guest pages and committed host backing
  under its existing lock. Neither is physical residency. The 4 GB reservation
  is never reported as a 4 GB allocation. Sampling scans ~1.3 MB of metadata on
  this 4 KB host, rather than adding work to every guest load/store.
- `Mixer::memoryStats()` counts voices, queued buffers, referenced PCM capacity,
  processed-tag size and retained tag capacity under the mixer lock. Shared PCM
  references are deliberately counted per queue reference, so the byte count is
  an ownership upper bound, not unique physical storage. Decoder/player/OpenAL
  caches outside the mixer remain unmeasured.
- `MemoryDiagnostics` reads real Mach task RSS/phys_footprint and
  os_proc_available_memory on Apple; Linux uses approximate statm RSS and reports
  footprint/headroom as null. getrusage peak RSS is the **host process lifetime**
  peak (possibly including previous games), not this guest session's peak.
- `Process::run` owns a cancellable sampling thread, woken every five seconds,
  joined on return. It emits run-start, interval and run-end samples; existing
  background/foreground acknowledgements also sample. No guest/GPU calls or
  guest allocations occur. Subsystem reads are individually synchronized but
  are not one globally atomic snapshot. History retains at most 120 entries.
- Samples go to existing console/run logs with `[memory]`, survive in core crash
  bundles under `runtimeMemorySamples`, and are exported in ordinary CLI JSON
  reports by `HvmRun.cpp`. The existing shell's pending journal captures the log
  tail after a host kill, but a high-volume guest log can overwrite old samples
  there. The persistent iPhone `Documents/Logs/*.log` is the complete timeline;
  neither the core ring nor the pending tail guarantees a full-hour history.
- `hvm_memory_report.py` reads CLI JSON, core/shell crash reports, raw logs or
  JSONL. It prefers runtime samples over the shell's available-only history and
  reports sampled peaks, minima, endpoint deltas and rates without leak claims.
- CMake builds `hvm_memory_tests`. Its contracts cover page/backing counts,
  overlap/protect/unmap, shared PCM references, completion storage release,
  bounded history, unsupported counters and unchanged guest allocations. Run it
  explicitly; no CI workflow or existing tests were weakened.

**Phase 2 is incomplete:** live GL texture/FBO/RBO counts and bytes require #33's
owner coordination. Guest `.sc`/font/texture caches, pending offline messages,
queue capacity, journal/checkpoint counts and buffers require hooks in #138's
pinned guest build. Those fields remain null, never zero. Memory-warning capture
and guest battle/result/reward/home markers still require shell/guest ownership
coordination. Idle library host sampling, UI export on a clean stop and device
Mach-counter compilation/accuracy remain unqualified. No shell, GPU, offline
transport, progression or allocator policy was changed.

## Minimal allocation experiment and measurements

```sh
cmake --build build --target hvm_memory_tests
build/hvm_memory_tests
build/hvm_memory_tests --heap-probe > heap.jsonl
python3 tools/hvm_memory_report.py heap.jsonl
```

The probe allocates and page-touches 64 MiB, frees it, and repeats ten times.
The expectation is independent: live bytes must fall after free; whether RSS
falls depends on allocator policy. It does not simulate a battle. Before the
patch, a standalone build of the same `Heap`/`GuestArena` loop measured:

| Stage | Live heap bytes | Committed heap bytes | Linux RSS bytes |
|---|---:|---:|---:|
| Before allocation | 0 | 0 | 3,829,760 |
| First allocation | 67,108,864 | 67,108,864 | 71,053,312 |
| First free | 0 | 67,108,864 | 71,053,312 |
| Tenth free | 0 | 67,108,864 | 71,053,312 |

With diagnostics, the linked probe reports tenth free live=0,
committed=67,108,864, RSS=73,895,936 bytes. Linked executables differ in baseline
host overhead, so their absolute RSS difference is not an effect estimate.
Within each run all post-first-free RSS values plateau. This confirms retained
reusable pages, **not** sustained accumulation across repeated identical loads.
No allocator fix is justified by this experiment alone; policy remains unchanged.

Sampling cost in the focused test: 125 samples averaged **0.739 ms/sample**
(4 KB Linux host, with other cloud work running). At a five-second cadence the
measured sampling work is ~0.015% of one core. This is not an iPhone latency,
energy or audio-deadline guarantee; lock contention and larger audio queues can
increase it. The sampler keeps no unbounded collection.

## Regression results (patched working tree based on 0170a0b)

| Check | Before | After |
|---|---|---|
| Runtime fulltest | 271 passed / 0 failed | 271 passed / 0 failed |
| HydrogenVM Test Lab | 18 passed / 0 failed / 0 skipped | 18 passed / 0 failed / 0 skipped |
| New memory diagnostics tests | Not present | 12 passed / 0 failed |
| Heap regressions | Not re-run separately | 6 passed / 0 failed |
| Full report | Not re-run | build 1, CPU 15, iOS guests 6, bench 5; all pass |
| Memory analysis Python tests | Not present | 4 passed |
| Clash protocol/home Python checks | Not re-run separately | 6 + 6 passed |
| IPA verifier / compat harness Python checks | Not re-run separately | 6 / 69 passed |
| Full compat / baseline and previous diff | Not re-run | zero regressions; original Clash fixture skipped |

The full report's CoreMark speed notice is informational: 73.2% of the iPhone-5
goal under shared cloud load. The six iOS tests are ARM guest apps on Linux,
not Apple-host or physical-iPhone tests. Compatibility improvements relative to
older indexed baselines predate this diagnostic patch and are not claimed here.
Raw build/test logs remain under `/workspace/hvm-cloud/logs/memory-*`; generated
Test Lab, full report and compatibility artifacts are under ignored
`build/memory-*`. No IPAs, saves or game captures are committed.

## Before/after compatibility control

Same grandfathered Art of Glow 1.1.2 IPA, separate clean homes, HostGL/Mesa,
60-second timed runs, one identical tap at (160,240) at 5 seconds. Screenshots
were inspected: both display the same readable full-version/free-play menu.
These are menu runs, not drawing/gameplay or Apple-driver passes. An external
sampler read each child's Linux VmRSS every five seconds (13 observations).

| Measurement | Main baseline | Diagnostics working tree |
|---|---:|---:|
| Exit / timed run | 0 / 60 seconds | 0 / 60 seconds |
| Sampled peak RSS | 139,001,856 bytes | 133,046,272 bytes |
| RSS around 10 seconds | 139,001,856 bytes | 133,017,600 bytes |
| Last RSS (~60 seconds) | 139,001,856 bytes | 133,046,272 bytes |
| 10–60 second endpoint growth | 0 B/s | 573.4 B/s |
| Presented frames | 3,314 | 3,592 |
| Lifetime p95 frame work | 21.23 ms | 16.78 ms |

The lower absolute RSS and timing in the second run are **not claimed as
improvements**: independent host/driver allocation layout and concurrent cloud
build/test load confound them. The added history is still warming up in the
second run; the separate 125-sample test verifies its 120-entry bound. It has
13 exported samples with run-start/interval/run-end events. The same parser
successfully summarizes that actual CLI report. Raw private control artifacts
are in `/workspace/hvm-cloud/artifacts/memory-control-{before,after}/`.
Decoded guest block/op counters are cumulative execution-engine statistics,
not current code-cache resident bytes. Sampled peaks can miss shorter spikes;
VmRSS/statm/getrusage reads are not an atomic Linux snapshot.

## Remaining work and review status

No confirmed source of the incident, targeted retention fix, before/after Clash
allocation comparison, memory-warning test, ten-battle test, 30–60-minute Clash
session, or iPhone Jetsam qualification is available. Success criteria remain
open. Obtain the exact guestdev IPA/save and original diagnostic files; coordinate
#33 graphics/shell and #138 guest counters; then execute the scenario matrix
above before choosing a fix. Original guest training, simulation, NPC AI, rewards,
transport and checkpoint compatibility require the owning guest suite plus real
scenarios, not these aggregate Linux tests.

The candidate is for draft review by Lawson; do not merge. Independent review
must repeat/read the attached gates including 4 KB host-page metadata, and check
the ownership boundaries. Apple-host compilation and device measurements remain
pending until an exact-candidate macOS CI/device result exists. No Apple device,
Simulator or Xcode was available in this cloud environment.
