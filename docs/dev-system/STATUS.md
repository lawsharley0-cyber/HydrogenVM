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

Pending the verification run on this branch. The committed artifact will be `docs/dev-system/baselines/linux-headless.json` (JSON only). Fill this section from `summary` and from each app's `status`, `launchSuccess`, `crashed`, `frame.signal`, `audio.signal`, and `unimplementedImports`.

Host page size of that run must be copied here. A 4 KB result is not an iPhone memory-model result.

Clash Royale is optional and is a skip unless `HVM_FIXTURE_ROOT` contains the pinned hash. A skip is not a pass.

## Fixed regressions

None in the infrastructure change. The harness does not alter runtime behavior, so it cannot close a product bug by itself.

| ID | Was | Fix | Proven by |
|---|---|---|---|
| — | — | — | — |

## Pending PRs

| PR | Branch | What it contains | Merged |
|---|---|---|---|
| This change | `cursor/infra-dev-system-1960` | Docs, harness, tests, workflow, baseline JSON | No |

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
| Actions billing or the real cause of jobs dying since `239a0ab` | The harness workflow cannot go green until jobs start. Local JSON is the substitute |
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

1. Owner confirms Actions, then the compat-harness workflow is the Linux gate for real.
2. B-003 doc reconciliation, review/docs role, no runtime files.
3. Staff Group B for the near clip (B-103) or Group D for `CGFont*` (B-109) or libc for `regcomp` (B-108). Do not staff two writers for one letter.
4. After a fixture decision, start B-106 (21903) on Group C alone.
5. Do not start SIMD raster or CPU fusion until B-103 and B-101 have landed.
