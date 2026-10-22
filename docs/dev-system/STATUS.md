# Status

Current branch investigation snapshot. Replace measured results as new runs arrive; preserve the decision log and original user observations.

Refresh:

```sh
python3 tools/hvm_compat.py run \
  --baseline docs/dev-system/baselines/linux-headless.json \
  --require-resolved \
  --out build/compat-report
python3 tools/hvm_compat.py issues build/compat-report/report.json
```

Copy `summary` and the regression list into the snapshot. Dry-run issues are notes, not filed tickets. Do not commit `build/`.

## Backlog

Current owner observations: [USER_APP_REPORTS.md](USER_APP_REPORTS.md), UAR-001–013. These are unresolved user reports with unspecified exact host/build/app versions. Investigate startup, world-load and interaction crashes, hangs, black output, missing music and low FPS separately. The historical Linux snapshot below does not supersede them. In particular, exercise Tiny Tower's close/failing-relaunch/next-launch sequence and retain Mega Jump's fast rendering and working effects as a control.

Source: `docs/dev-system/BACKLOG.md`. Bands P0–P2. Score is `impact * severity / complexity` inside a band.

Groups A–E remain exclusive. Open PRs #11/#12 own Group B; #13 owns Clash tools. Draft #15 changes only Heap/Stdlib allocation behavior, its independent tests, CI and documentation. It does not claim ownership of Group A mappings, handlers or faults.

## Ownership

| Role | Files | Owner | State |
|---|---|---|---|
| Runtime / CPU and memory | `src/core/cpu/`, `src/core/memory/`, Group A lines in `System.cpp` | — | Needs owner |
| Guest frameworks | `src/runtime/objc/`, `foundation/`, UIKit except GLES, libc symbols | — | Needs owner |
| Graphics, audio, input | `OpenGLES.cpp`, `Shader.*`, `audio/`, gestures | PRs #11/#12 for Group B | Active Group B; other subareas unclaimed |
| App compatibility, including Clash | `src/loader/`, `tools/clash_*.py`, `Net.cpp` while Group C is active | — | Needs owner |
| Test / regression | `tools/compat_harness/`, harness workflow | — | Needs owner for who watches the gate |
| Review / integration / docs | `docs/dev-system/` | — | Needs owner for required reviews |
| Apple shell | `app/**`, `HVMEngine.h` | — | Needs owner and a Mac |

## Current compat results

Measured October 21, 2026 (America/Chicago), Linux x86_64 WSL, Intel Core Ultra 9 185H, Clang 18.1.3, **4096-byte host pages**. Code/instructions at clean commit `d165df4684df18b35bd1a07c6bcc9037bfab987d`, on draft [PR #15](https://github.com/lawsharley0-cyber/HydrogenVM/pull/15), not merged main. Release flags: `-O3 -DNDEBUG -g -fno-omit-frame-pointer`.

Indexed JSON: [20261022T040205Z-d165df4684df.json](reports/linux-x86_64/20261022T040205Z-d165df4684df.json). The filename uses UTC. `git.dirty=false`, `summary.ok=true`, four fixtures resolved, zero app crashes, zero skips. Schema validation passed. Baseline and previous-indexed diffs both have zero regressions. The committed `a4b0f8b` baseline is unchanged. The baseline diff has four informational notes, including new Clash coverage; timing across those different hosts/builds is not an A/B speed comparison. Regex/tmpnam import improvements belong to merged PR #5, not #15.

| Suite | Result |
|---|---|
| selftest | pass, result `0xA05A0848` |
| fulltest | 191 passed, 0 failed |
| asm | arm_alu 40, arm_mem 17, thumb 30, vfp_neon 23; all pass |
| differential C | all 5 integer and 5 VFP/NEON configurations pass |
| Python | packaging, Clash protocol/home suites pass; harness self-tests 56 pass |
| independent heap gate | 6 pass in Release and targeted ASan/UBSan; 4 negative cases fail on original main |
| decoder fuzz, CoreMark, simulator/device | not run in this milestone |

Three-second manifest captures:

| App | Status | Frames presented | Frame | Mixer |
|---|---|---:|---|---|
| angry-birds | ran, no crash | 20 | varied | silent |
| tiny-tower | ran, no crash | 56 | varied | active |
| art-of-glow | ran, no crash | 37 | varied | silent |
| clash-royale-2-1306-1 | ran, no crash | 2 | all-black | active |

Clash's three-second capture is black. The baseline skipped that fixture, so a green diff is new coverage, not proof of correct rendering. Separate ten-second captures reach the Supercell logo and have varied pixels. This does not establish gameplay, peer/tutorial execution or playability. Tiny Tower's separate ten/twenty-second captures show its tutorial; the interactive scanner's black final frame follows graceful shutdown and must not be called a gameplay-render failure.

Serial ten-second startup ABBA runs (before main `39904cb`, after allocator fix `60106d2`; two samples each) retain varied frames and the same mixer signal on all four apps, with no recorded crashes, dropped audio blocks or transport errors. They are loading/idle scenes with no injected input. Timing varies in both directions; no FPS improvement is claimed. The exploratory Angry Birds profile points at `drawImage` (~26%), GLES texture sampling (~22%), fragment writes (~12%) and triangles (~10%). Coordinate Group B before changing those paths.

Tiny Tower 1.0 also completed three separate twenty-second processes with one preserved sandbox per variant, before and after. This did not reproduce the reported intermittent first-relaunch crash and does not resolve UAR-013. All UAR observations remain open until matching scenarios/platforms/versions pass.

A 4 KB pass does not establish the 16 KB iPhone memory model. Audio signal is mixer capture, not a speaker test. On tested commit `d165df4`, GitHub Actions [compat harness](https://github.com/lawsharley0-cyber/HydrogenVM/actions/runs/39720138763) and [Linux CI](https://github.com/lawsharley0-cyber/HydrogenVM/actions/runs/39720138932) both succeeded. The conditional macOS job is not a device pass. Later evidence-only commits do not change the tested runtime; independent review remains pending.

## Fixed defects on this branch (awaiting review)

| ID | Was | Fix | Proven by |
|---|---|---|---|
| B-210 | `allocAligned(0xfffffff0,4096)` returned only 4080 usable bytes | Validate size/alignment before mutation and use 64-bit arithmetic; validate ARMv7 posix_memalign alignment | `hvm_heap_tests`: original main 2 pass/4 fail, fixed 6 pass; targeted sanitizers 6 pass; full compatibility gate above |

This defect has not been attributed to any specific UAR crash. No owner-reported app failure is marked resolved.

## Pending PRs

| PR | Area | State |
|---|---|---|
| [#11](https://github.com/lawsharley0-cyber/HydrogenVM/pull/11) | GLSL constant-expression bounds | Open; Group B |
| [#12](https://github.com/lawsharley0-cyber/HydrogenVM/pull/12) | Uniform arrays/register allocation | Open, stacked on #11; Group B |
| [#13](https://github.com/lawsharley0-cyber/HydrogenVM/pull/13) | Clash continuation plan/tools | Open; local restoration is not merged main |
| [#15](https://github.com/lawsharley0-cyber/HydrogenVM/pull/15) | Aligned allocation safety, owner reports and evidence | Draft; independent review pending; author does not merge |

## Decisions

| Date | Decision | Who | Notes |
|---|---|---|---|
| 2026-10-21 | Integration line for this system is `main` | Scope of this change | `docs/HANDOFF.md` still says `gemini-merge`. That edit is B-003, not a branch rename |
| 2026-10-21 | Scope is the audit backlog, Clash past the 65% load, and performance follow-ons found while speeding apps up | Scope of this change | Does not by itself implement those items |
| 2026-10-21 | Harness fixtures are SHA-256 plus a directory. Three existing IPAs stay grandfathered. New ids cannot use a repo path | This change | Deleting the committed IPAs is still open |
| 2026-10-21 | Issue generator is dry-run. Nothing is auto-filed | This change | No GitHub issues exist on the repo |
| 2026-10-21 | Preserve all 13 owner reports and prioritize broad stability/performance | Owner request; draft #15 | Short Linux smokes do not resolve matching-device/world-load/input/relaunch reports. Mega Jump is a positive performance control |
| 2026-10-21 | Keep allocation-safety work separate from active graphics and unfinished Clash progression | Draft #15 | Measured hot graphics files remain with #11/#12; no FPS or UAR resolution claim |

## Needs owner

| Topic | Why it blocks |
|---|---|
| Fixture directory, and whether the three commercial IPAs may remain in git | Harness is ready. Removal is a history and copyright decision |
| Clash client URL `https://github.com/astralsc/clashroyale_archive/...` and whether that client is licensed for this use | The hash is recorded. The file is not downloaded |
| Who may merge without the independent review pass | Nothing in the repo requires a second reader |
| Mac runner and which devices are sideloaded with the extended-virtual-addressing entitlement | Group E and the 16 KB page question |
| Device signing certificates | Not in this environment, and they must not be committed |
| Strictness of new unimplemented imports on a longer-than-3-second run | Audit question 9. The diff fails closed on new **load-time** imports and treats new runtime-only misses as attention |
| `CCCrypt` ciphers | Code refuses them on purpose |
| Android APK | Listed as a later phase. Not scheduled |
| A tracker other than `STATUS.md` | Issues are unused. The generator will not file |

## Next improvements

1. Obtain matching host/build/app versions and fixtures for the UAR scenarios; reproduce startup, world-load, input-triggered crashes and Pocket Ants' long freeze separately.
2. Reproduce Tiny Tower's close/failing-relaunch/next-launch sequence with the owner's actual sandbox history. Do not reset state as a workaround.
3. Preserve Mega Jump's performance/effects and investigate missing music independently of FPS. Do not assume a shared decoder cause without evidence.
4. After active Group B work settles, profile steady gameplay and compare a focused shared graphics improvement on matching host/build/input, preserving pixels and audio.
5. Keep B-101/B-102 (16 KB permission/fault correctness) open. Merged clipping/regex/tmpnam and Codemagic build work supersede their old planned snapshots; signed-device behavior still needs device evidence.
