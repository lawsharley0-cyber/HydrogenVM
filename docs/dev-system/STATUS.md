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

Open groups that must not be double-staffed: A memory, B pixels, C Clash wire, D frameworks, E Apple shell. None of them are staffed in this snapshot.

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

Measured on Linux x86_64, Clang 18.1.3, host page size **4096**, commit `a4b0f8b` (the commit that contains this JSON is the one after that; the report's `git.commit` is the code that was executed, and `git.dirty` was false). Artifact: `docs/dev-system/baselines/linux-headless.json`. `summary.ok` is true. Schema check passed. A self-diff has no regressions.

Harness self-tests: 56 passed, 0 failed (`python3 -m unittest discover -s tools/compat_harness/tests`). Counted on this branch. The baseline JSON below is still the `a4b0f8b` run.

Indexed run from this branch, not a new baseline: `docs/dev-system/reports/linux-x86_64/20261021T185933Z-8771deca6655.json`. `git.commit` is `8771deca6655` and `git.dirty` was false. `summary.ok` is true. Diff against `baselines/linux-headless.json`: 0 regressions, 0 attention, 0 informational. Clash is still a skip. `docs/dev-system/reports/index.json` lists that one row. `latest.json` points at it.

| Suite | Result |
|---|---|
| selftest | pass. 10.0 M instructions, result `0xA05A0848`, about 541 MIPS on this VM |
| fulltest | pass. 79 passed, 0 failed |
| asm | pass. arm_alu 40, arm_mem 17, thumb 30, vfp_neon 23 (110) |
| differential C | pass. 5 integer configs × 27 and 5 VFP/NEON configs × 29 (280) |
| Python | pass. `verify_ipa`, `clash_protocol`, `clash_home_state` |
| decoder fuzz | not run (pass `--fuzz`) |
| ld64 iOS guests, CoreMark, simulator, device | not run. See headless limits |

| App | Status | Frames | Frame signal | Audio | Load-time gaps |
|---|---|---:|---|---|---|
| angry-birds | ran, no crash | 95 | mostly-one-color (about 93% near-white, 6% other). Not a title-screen proof | mixer silent | `tmpnam` |
| tiny-tower | ran, no crash | 69 | varied | mixer active | `CGFontCopyTableForTag`, `CGFontGet*` queries, `glPointSizePointerOES` |
| art-of-glow | ran, no crash | 50 | varied | mixer silent | `regcomp`, `regexec`, `regerror`, `regfree`. `SCNetworkReachability` reported offline (signal). `kCFNumberNaN` and infinities zero-filled (signal) |
| clash-royale-2-1306-1 | skipped | — | not run | not measured | IPA not in the fixture root. Skip is not a pass |

A 4 KB pass is not an iPhone memory-model pass. Frame counts move between runs; the diff does not fail on that unless presented frames drop to zero. MIPS, average frame time, and mixer sample counts (`frames`, `nonSilentFrames`) that move by 25% or more while the signal stays `active` are informational. A smaller sample-count move is not noted.

Linux Actions is green on `6af9976`. The compat harness passed in run [39646706013](https://github.com/lawsharley0-cyber/HydrogenVM/actions/runs/39646706013). The Linux CI job published `runs/20261021-175201-39646696896-linux` in run [39646696896](https://github.com/lawsharley0-cyber/HydrogenVM/actions/runs/39646696896). The empty-step failure on `07fabbb` (run 39633887764) was real; it is history, not the current gate. The macOS CI job on that push was skipped by its existing condition.

## Fixed regressions

None in the infrastructure change. The harness does not alter runtime behavior, so it cannot close a product bug by itself.

| ID | Was | Fix | Proven by |
|---|---|---|---|
| — | — | — | — |

## Pending PRs

| PR | Branch | What it contains | Merged |
|---|---|---|---|
| This change | `cursor/infra-dev-system-1960` | Docs, harness, tests, workflow, baseline JSON | No |
| [#3](https://github.com/lawsharley0-cyber/HydrogenVM/pull/3) | `cursor/compat-report-index-c782` | `AGENTS.md`, indexed JSON reports, active-mixer sample notes. Base is the row above | No |

## Decisions

| Date | Decision | Who | Notes |
|---|---|---|---|
| 2026-10-21 | Integration line for this system is `main` | Scope of this change | `docs/HANDOFF.md` still says `gemini-merge`. That edit is B-003, not a branch rename |
| 2026-10-21 | Scope is the audit backlog, Clash past the 65% load, and performance follow-ons found while speeding apps up | Scope of this change | Does not by itself implement those items |
| 2026-10-21 | Harness fixtures are SHA-256 plus a directory. Three existing IPAs stay grandfathered. New ids cannot use a repo path | This change | Deleting the committed IPAs is still open |
| 2026-10-21 | Issue generator is dry-run. Nothing is auto-filed | This change | No GitHub issues exist on the repo |

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

1. Teach `tools/hvm_report.py` to run `fulltest` so the published `ci-reports` row matches the harness (B-002 follow-up). The compat job already runs `fulltest`. Do not treat the skipped macOS job as a pass.
2. B-003 doc reconciliation, review/docs role, no runtime files.
3. Staff Group B for the near clip (B-103) or Group D for `CGFont*` (B-109) or libc for `regcomp` (B-108). Do not staff two writers for one letter.
4. After a fixture decision, start B-106 (21903) on Group C alone.
5. Do not start SIMD raster or CPU fusion until B-103 and B-101 have landed.

## October 22 save-editor continuation (draft #18)

Group E and documentation/harness work on codex/file-save-overhaul, based on main 39904cb. Shared C++ runtime is unchanged. File-save behavior, SQLite buffering, rolling backups, staged snapshots and guarded editor navigation are implemented; see [FILE_EDITOR_HANDOFF.md](FILE_EDITOR_HANDOFF.md). Signed-device behavior and actual guest consumption of edits remain unverified.

Local WSL: 62 harness unit tests passed (including six new doctor checks); doctor found no unavailable commit references in the reconciled backlog/status/plan. Swift persistence tests, including actual JSON/plist model round trips and SQLite WAL tests, passed on the Mac runner at 41562a0. Final shell/UI checks are pending and must be recorded by exact commit below. Earlier UI compile at 20aa20b passed; it is historical evidence, not a final-code pass.

B-003 docs now distinguish current main integration from historical gemini-merge checkpoints. B-103, B-108 and B-110 reflect their merged PRs. The original compatibility snapshot above remains historical. No new app/gameplay, memory-page, performance, speaker or Clash progression result is claimed.
