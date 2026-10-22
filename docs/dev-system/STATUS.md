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

Open groups that must not be double-staffed: A memory, B pixels, C Clash wire, D frameworks, E Apple shell. Current open PRs reserve Group B (#11/#12), Clash tools (#13), and heap/allocator work (#15). PR #16 owns only the shared PCM decoder and its standalone tests. Read current PR file lists before editing; this table is not an ownership lock.

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

Cloud Linux x86_64 (Ubuntu 24.04, kernel 6.18.55), Intel Xeon Platinum 8573C, Clang 18.1.3, Release `-O3 -DNDEBUG`, `-ffp-contract=off`, **4096-byte host pages**. Tested clean commit `031b16d9062ebb341ac2505a102d1817611c5e1f`, branch `codex/pcm-decoder-safety`, draft [#16](https://github.com/lawsharley0-cyber/HydrogenVM/pull/16). This runtime does not include unmerged #11/#12/#15.

Indexed evidence: [20261022T043240Z-031b16d9062e.json](reports/linux-x86_64/20261022T043240Z-031b16d9062e.json). `git.dirty=false`, `summary.ok=true`; baseline and previous indexed diffs have **zero regressions and zero attention**. Timing differences from older hosts are informational. Existing tmpnam/regex binding improvements predate this patch and are not credited to #16. The committed baseline remains unchanged. The preceding indexed `8771deca6655` run and `a4b0f8b` baseline remain historical evidence.

| Suite | Result |
|---|---|
| PCM regression | 42 pass in Release and targeted ASan/UBSan/float-cast-overflow; main fails 12 release checks and three separate sanitizer probes |
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

GitHub on `031b16d`: [PCM sanitizer regression](https://github.com/lawsharley0-cyber/HydrogenVM/actions/runs/39722798751), [compatibility gate](https://github.com/lawsharley0-cyber/HydrogenVM/actions/runs/39722798705), and [Linux CI](https://github.com/lawsharley0-cyber/HydrogenVM/actions/runs/39722798790) all succeeded. macOS was skipped, not passed. The cloud container's targeted ASan run used `ASAN_OPTIONS=detect_leaks=0` because LeakSanitizer cannot enumerate `/proc` tasks here; GitHub's sanitizer job passed with its defaults. Address, undefined-behavior and float-cast-overflow instrumentation remained enabled in both.

A 4 KB Linux pass does not prove 16 KB iPhone permissions, precise fault behavior, signed-device operation, speaker output, or full gameplay. The original 13 owner observations remain unresolved, preserved in [#15's USER_APP_REPORTS.md at e1684a8](https://github.com/lawsharley0-cyber/HydrogenVM/blob/e1684a89bc214cd8eac2bc30c1feb060e9ecfdab/docs/dev-system/USER_APP_REPORTS.md). No missing-music report is attributed to this synthetic PCM defect without matching fixtures.

Matching-build [startup measurements and complete targeted evidence](reports/performance/20261021-pcm-safety.json): 12 serial 10-second ABBA samples across the three fixtures. Timings move both directions; **no speed gain established**. All live frame signals stay varied, Tiny Tower mixer stays active, and no audio blocks are dropped. These are fresh-start samples, not same-sandbox relaunch or gameplay validation.

## Fixed regressions awaiting independent review

| ID | Trigger / root cause | Result | Evidence |
|---|---|---|---|
| B-114 / #16 | Float64 PCM enters a 32-bit accumulator and shifts by 56; NaN reaches an int16 cast; unsupported widths drive unchecked shifts | Validate layouts first, decode float values at their own width, silence NaN and saturate infinities; preserve supported finite PCM outputs | `tests/audio/PcmDecodeTests.cpp`, `tools/test_pcm_decode.sh`, JSON above and matching-build evidence below |

## Pending PRs

| PR | Scope / reserved files | Integration |
|---|---|---|
| #11 / #12 | GLSL bounds / uniform storage and register allocation, Group B and GLES tests | Open; do not duplicate |
| #13 | Clash continuation docs/tools | Open; restoration not complete |
| #15 | Aligned allocations, heap/stdlib and standalone allocator gate; owner app reports | Open; no changes taken over here |
| #16 | `audio/Decode.cpp`, synthetic PCM tests and separate PCM workflow | Draft; independent review and owner merge required |

## Decisions

| Date | Decision | Who | Notes |
|---|---|---|---|
| 2026-10-21 | Integration line for this system is `main` | Scope of this change | `docs/HANDOFF.md` still says `gemini-merge`. That edit is B-003, not a branch rename |
| 2026-10-21 | Scope is the audit backlog, Clash past the 65% load, and performance follow-ons found while speeding apps up | Scope of this change | Does not by itself implement those items |
| 2026-10-21 | Harness fixtures are SHA-256 plus a directory. Three existing IPAs stay grandfathered. New ids cannot use a repo path | This change | Deleting the committed IPAs is still open |
| 2026-10-21 | Issue generator is dry-run. Nothing is auto-filed | This change | No GitHub issues exist on the repo |
| 2026-10-21 | B-114 PCM safety fix is cloud-tested in draft #16, not integrated | Cloud author | 42 synthetic checks; Linux compatibility gate green; 4 KB and headless limits retained. No owner app report closed |

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

1. Independent review of #16 and #15; author does not merge. Preserve both standalone gates during owner integration.
2. Recheck current ownership before selecting another reproducible shared failure. Group D `CGFont*` remains a possible synthetic/available-fixture task; exact owner app reports need matching scenarios.
3. Graphics SIMD/compositing work still waits on Group B's clipper/correctness work. CPU fusion waits on B-101; no bypass based on a 4 KB cloud pass.
4. Continue B-003 documentation reconciliation separately. Regex/tmpnam binding improvements already present on main are not new work for this PR.
