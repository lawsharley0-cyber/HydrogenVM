# Status

Template for the continuous PM lap. Replace the snapshot when a harness run or a merged fix changes the picture. Keep the decision log; append to it instead of rewriting history.

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

Source: `docs/dev-system/BACKLOG.md`. Bands P0–P2. Score is `impact * severity / complexity` inside a band.

Open groups that must not be double-staffed: A memory, B pixels, C Clash wire, D frameworks, E Apple shell. Open PRs currently reserve graphics Group B (#11/#12), Clash tools (#13), allocations (#15), PCM decoding (#16), and ZIP loader metadata (#17). Recheck current changed-file lists before editing.

## Ownership

| Role | Files | Owner | State |
|---|---|---|---|
| Runtime / CPU and memory | `src/core/cpu/`, `src/core/memory/`, Group A lines in `System.cpp` | — | Needs owner |
| Guest frameworks | `src/runtime/objc/`, `foundation/`, UIKit except GLES, libc symbols | — | Needs owner |
| Graphics, audio, input | `OpenGLES.cpp`, `Shader.*`, `audio/`, gestures | — | Needs owner |
| App compatibility, including Clash | `src/loader/`, `tools/clash_*.py`, `Net.cpp` while Group C is active | — | Needs owner |
| Test / regression | `tools/compat_harness/`, harness workflow | — | Needs owner for who watches the gate |
| Review / integration / docs | `docs/dev-system/` | — | Needs owner for required reviews |
| Apple shell | `app/**`, `HVMEngine.h` | — | Needs owner and a Mac |

## Current compat results

Measured in an isolated **cloud Linux x86_64** worktree on clean `cb3509e234ae4b83452ce55bbee76935d9fa281d`, branch `codex/zip-entry-bounds`, draft [#17](https://github.com/lawsharley0-cyber/HydrogenVM/pull/17). Ubuntu 24.04/kernel 6.18.55, Intel Xeon Platinum 8573C, Clang 18.1.3, Release `-O3 -DNDEBUG -ffp-contract=off`, **4096-byte host pages**. Base is main `39904cb`; unmerged #11/#12/#15/#16 are not included.

[Indexed compatibility JSON](reports/linux-x86_64/20261022T135259Z-cb3509e234ae.json): `git.dirty=false`, `summary.ok=true`, no crashes/launch failures, baseline and previous indexed diffs have zero regressions/attention. Timing differences from older hosts are informational. Existing tmpnam/regex binding improvements predate #17 and are not credited here. The `a4b0f8b` baseline and previous `8771deca6655` indexed run remain historical evidence; no baseline was replaced. #15/#16 retain their own branch evidence and do not become ancestors of this branch.

| Suite | Result |
|---|---|
| ZIP metadata regression | 58 pass in Release and targeted ASan/UBSan; main has 39 failed checks and a focused 8-byte heap-buffer-overflow |
| fulltest | 191 pass, zero failures |
| CPU | selftest, four asm guests and ten differential C configurations pass |
| Python | three required modules and 56 harness self-tests pass |
| Decoder fuzz / CoreMark / ld64 / Apple device | Not run |

| App | 3-second smoke | Presented frames | Frame signal | Mixer signal |
|---|---|---:|---|---|
| Angry Birds 1.0 | Ran, no crash | 43 | mostly-one-color | silent |
| Tiny Tower 1.0 | Ran, no crash | 48 | varied | active |
| Art of Glow 1.1.2 | Ran, no crash | 32 | varied | silent |
| Clash Royale 2.1306.1 | Optional fixture absent: **skip** | — | not run | not measured |

[Complete regression and import measurements](reports/performance/20261022-zip64-bounds.json) retain all samples, exact benchmark/driver source, before sanitizer evidence and host/build limits. Twelve serial before/after/after/before archive-read samples (eight complete archive reads each) across the three fixtures return matching entry/file/byte totals and content checksums, with per-entry CRC checks retained. Metadata wall time, resource-read wall time and process CPU time are separate. Timing is noisy; **no established speed gain or no-overhead claim**. This measures archive import work, not startup scenes or gameplay FPS. The earlier incomplete 11-sample capture is retained separately rather than hidden.

Targeted ASan/UBSan in the cloud container uses `ASAN_OPTIONS=detect_leaks=0` because LeakSanitizer cannot enumerate `/proc` tasks here; AddressSanitizer and UB checks remain enabled. The GitHub ZIP regression runs with default sanitizer settings. Current GitHub workflow links are in #17 and the evidence JSON.

The owner's UAR-001..013 remain unresolved and preserved in [#15's USER_APP_REPORTS at e1684a8](https://github.com/lawsharley0-cyber/HydrogenVM/blob/e1684a89bc214cd8eac2bc30c1feb060e9ecfdab/docs/dev-system/USER_APP_REPORTS.md). This synthetic malformed-input defect is not an established cause of the reported startup/gameplay failures. A 4 KB Linux pass does not prove 16 KB iPhone permissions, precise faults, device signing, audible speakers, full gameplay, world loading, same-sandbox relaunch or offline Clash progression.

## Fixed regressions awaiting independent review

| ID | Trigger / root cause | Result | Evidence |
|---|---|---|---|
| B-115 / #17 | ZIP64 field length exceeds enclosing entry; parser trusts it for 8-byte replacement reads | Validate field framing and all requested values before reading; reject incomplete/missing ZIP64 metadata and close the file on failed construction | `tests/loader/ZipBoundsTests.cpp`, `tools/test_zip_bounds.sh`, JSON above |

## Pending PRs

| PR | Reserved scope | Integration |
|---|---|---|
| #11 / #12 | GLSL and uniform/register work, Group B and GLES tests | Open |
| #13 | Clash continuation documentation/tools | Open; restoration incomplete |
| #15 | Allocator overflow and owner report preservation | Open |
| #16 | PCM sample-width/non-finite conversion | Draft, tests green, review pending |
| #17 | ZIP loader metadata validation and failed-construction cleanup | Draft, independent review and owner merge required |

## Decisions

| Date | Decision | Who | Notes |
|---|---|---|---|
| 2026-10-21 | Integration line for this system is `main` | Scope of this change | `docs/HANDOFF.md` still says `gemini-merge`. That edit is B-003, not a branch rename |
| 2026-10-21 | Scope is the audit backlog, Clash past the 65% load, and performance follow-ons found while speeding apps up | Scope of this change | Does not by itself implement those items |
| 2026-10-21 | Harness fixtures are SHA-256 plus a directory. Three existing IPAs stay grandfathered. New ids cannot use a repo path | This change | Deleting the committed IPAs is still open |
| 2026-10-21 | Issue generator is dry-run. Nothing is auto-filed | This change | No GitHub issues exist on the repo |
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

1. Independent review and owner integration of the tested drafts; the author does not merge. Preserve the separate ZIP/PCM/allocator gates when integrating.
2. Select another reproducible shared failure only after rechecking ownership. Exact owner-report fixtures/scenarios remain needed to resolve UAR rows.
3. Group B correctness/near clipping precedes raster SIMD or compositing changes; memory permissions precede CPU fusion. Cloud 4 KB passes do not bypass those dependencies.
4. Continue B-003 documentation reconciliation separately. Regex/tmpnam implementations already present on main are not new fixes in this branch.
