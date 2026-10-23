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

Open groups that must not be double-staffed: A memory, B pixels, C Clash wire, D frameworks, E Apple shell. Open PRs currently reserve graphics Group B (#11/#12) and Clash tools (#13). Recheck current changed-file lists before editing.

## Ownership

| Role | Files | Owner | State |
|---|---|---|---|
| Runtime / CPU and memory | `src/core/cpu/`, `src/core/memory/`, Group A lines in `System.cpp` | — | Needs owner |
| Guest frameworks | `src/runtime/objc/`, `foundation/`, UIKit except GLES, libc symbols | — | Needs owner |
| Graphics, audio, input | `OpenGLES.cpp`, `Shader.*`, `audio/`, gestures | PRs #11/#12 (Group B), #16 (PCM) | Active scoped PRs; other input work unclaimed |
| App compatibility, including Clash | `src/loader/`, `tools/clash_*.py`, `Net.cpp` while Group C is active | PR #13 (Clash), #17 (ZIP) | Active scoped PRs; other loader work unclaimed |
| Test / regression | `tools/compat_harness/`, harness workflow | — | Needs owner for who watches the gate |
| Review / integration / docs | `docs/dev-system/` | — | Needs owner for required reviews |
| Apple shell | `app/**`, `HVMEngine.h` | codex/file-save-overhaul, draft #18 | Active save-editor & HUD/Library UI upgrade completed |

## Current compat results

Cloud Linux x86_64 (Ubuntu 24.04, kernel 6.18.55), Intel Xeon Platinum 8573C, Clang 18.1.3, Release `-O3 -DNDEBUG`, `-ffp-contract=off`, **4096-byte host pages**. Tested clean commit `cb3509e234ae4b83452ce55bbee76935d9fa281d`, branch `codex/zip-entry-bounds`, PR [#17](https://github.com/lawsharley0-cyber/HydrogenVM/pull/17).

Indexed evidence: [20261022T135259Z-cb3509e234ae.json](reports/linux-x86_64/20261022T135259Z-cb3509e234ae.json). `git.dirty=false`, `summary.ok=true`; baseline and previous indexed diffs have **zero regressions and zero attention**. Timing differences from older hosts are informational. Existing tmpnam/regex binding improvements predate this patch and are not credited to #17. The committed baseline remains unchanged.

| Suite | Result |
|---|---|
| ZIP regression | 58 pass in Release and targeted ASan/UBSan; original main fails 10 checks |
| PCM regression | 42 pass in Release and targeted ASan/UBSan |
| fulltest | 191 pass, zero failures |
| CPU | selftest, four asm guests and ten differential C configurations pass |
| Python | three required modules and 56 harness tests pass |
| decoder fuzz / ld64 / CoreMark / Apple device | Not run |

| App | 3-second smoke | Presented frames | Frame signal | Mixer signal |
|---|---|---:|---|---|
| Angry Birds 1.0 | Ran, no crash | 73 | mostly-one-color | silent |
| Tiny Tower 1.0 | Ran, no crash | 60 | varied | active |
| Art of Glow 1.1.2 | Ran, no crash | 37 | varied | silent |
| Clash Royale 2.1306.1 | Optional fixture absent: **skip** | — | not run | not measured |

A 4 KB Linux pass does not prove 16 KB iPhone permissions, precise fault behavior, signed-device operation, speaker output, or full gameplay. The original 13 owner observations remain unresolved.

## Fixed regressions

| ID | Trigger / root cause | Result | Evidence |
|---|---|---|---|
| B-210 | `allocAligned(0xfffffff0,4096)` returned only 4080 usable bytes | Validate size/alignment before mutation and use 64-bit arithmetic; validate ARMv7 posix_memalign alignment | `hvm_heap_tests`: original main 2 pass/4 fail, fixed 6 pass; targeted sanitizers 6 pass; full compatibility gate above |
| B-114 / #16 | Float64 PCM enters a 32-bit accumulator and shifts by 56; NaN reaches an int16 cast; unsupported widths drive unchecked shifts | Validate layouts first, decode float values at their own width, silence NaN and saturate infinities; preserve supported finite PCM outputs | `tests/audio/PcmDecodeTests.cpp`, `tools/test_pcm_decode.sh`, JSON above and matching-build evidence below |
| B-115 / #17 | ZIP64 field length exceeds enclosing entry; parser trusts it for 8-byte replacement reads | Validate field framing and all requested values before reading; reject incomplete/missing ZIP64 metadata and close the file on failed construction | `tests/loader/ZipBoundsTests.cpp`, `tools/test_zip_bounds.sh`, JSON above |

## Pending PRs

| PR | Scope / reserved files | Integration |
|---|---|---|
| #11 / #12 | GLSL bounds / uniform storage and register allocation, Group B and GLES tests | Open; do not duplicate |
| #13 | Clash continuation docs/tools | Open; restoration not complete |
| #15 | Aligned allocations, heap/stdlib and standalone allocator gate; owner app reports | Merged |
| #16 | `audio/Decode.cpp`, synthetic PCM tests and separate PCM workflow | Merged |
| #17 | ZIP loader metadata validation and failed-construction cleanup | Merging |

## Decisions

| Date | Decision | Who | Notes |
|---|---|---|---|
| 2026-10-21 | Integration line for this system is `main` | Scope of this change | `docs/HANDOFF.md` still says `gemini-merge`. That edit is B-003, not a branch rename |
| 2026-10-21 | Scope is the audit backlog, Clash past the 65% load, and performance follow-ons found while speeding apps up | Scope of this change | Does not by itself implement those items |
| 2026-10-21 | Harness fixtures are SHA-256 plus a directory. Three existing IPAs stay grandfathered. New ids cannot use a repo path | This change | Deleting the committed IPAs is still open |
| 2026-10-21 | Issue generator is dry-run. Nothing is auto-filed | This change | No GitHub issues exist on the repo |
| 2026-10-21 | Preserve all 13 owner reports and prioritize broad stability/performance | Owner request; draft #15 | Short Linux smokes do not resolve matching-device/world-load/input/relaunch reports. Mega Jump is a positive performance control |
| 2026-10-21 | Keep allocation-safety work separate from active graphics and unfinished Clash progression | Draft #15 | Measured hot graphics files remain with #11/#12; no FPS or UAR resolution claim |
| 2026-10-21 | B-114 PCM safety fix is cloud-tested in draft #16, not integrated | Cloud author | 42 synthetic checks; Linux compatibility gate green; 4 KB and headless limits retained. No owner app report closed |
| 2026-10-22 | B-115 ZIP64 metadata fix is cloud-tested in draft #17, not integrated | Cloud author | 58 targeted checks; 191 full tests; Linux gate pass; no owner app report closed or speed gain claimed |

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
6. Group B correctness/near clipping precedes raster SIMD or compositing changes; memory permissions precede CPU fusion. Cloud 4 KB passes do not bypass those dependencies.
7. Continue B-003 documentation reconciliation separately. Regex/tmpnam binding improvements already present on main are not new work for this PR.

## October 22 save-editor continuation (PR #18)

Group E is owned by `codex/file-save-overhaul` (PR #18). The shared C++ runtime is unchanged. See [FILE_EDITOR_HANDOFF.md](FILE_EDITOR_HANDOFF.md) for implemented behavior and the remaining ordered queue.

**Final tested source: `d3085f3`.**
[Apple validation](https://github.com/lawsharley0-cyber/HydrogenVM/actions/runs/39881742994)
passed the Swift 6 persistence/model executable, all five iPhone simulator UI tests, and the actual unsigned Release simulator shell compile. UI tests prove nested Save & Close with reopen, discard, returning to the root without losing edits, autosave, and native text entry saved through the keyboard toolbar. The final footer reserves layout space rather than covering the viewport; visible destinations supply toolbars, and text editors explicitly request focus.

Local WSL and CI harness self-tests pass: **64 tests**, including eight doctor checks. `python3 tools/hvm_compat.py doctor` passes on the reconciled documents.

### Final Linux source revision

[Final compatibility run](https://github.com/lawsharley0-cyber/HydrogenVM/actions/runs/39881742977) passed on `d3085f3` (clean PR merge ef383fe348d27251d6d7921ac2eae52c9a10ac17). Exact uploaded report is [20261023T011004Z-ef383fe348d2.json](reports/linux-x86_64/20261023T011004Z-ef383fe348d2.json). All 16 unit/asm/differential configurations pass, including fulltest's **191 passed, 0 failed**. Required Python suites pass; three bundled apps run without recorded crashes and optional Clash remains skipped. Baseline and previous-indexed diffs have zero regressions.
