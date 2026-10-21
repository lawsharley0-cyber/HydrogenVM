# Implementation plan

This is the plan for the repository-local development system. It does not change the emulator. Runtime behavior stays where it is; the system detects, classifies, and records work for later changes.

Source of the backlog: the 2026-10-21 read-only audit of `main` at `07fabbb`, plus the scope confirmed for this effort: everything in that audit, Clash Royale past the 65% load, and performance follow-ons discovered while making apps faster. Integration for this system is `main`.

Status words used below:

- **Operational** — present in this tree and usable on a Linux machine that can build HydrogenVM.
- **Planned** — specified, not built, safe to schedule once its no-parallel group is free.
- **Needs owner** — blocked on a decision, credential, account, or machine this repository cannot supply.

## Capabilities

| Capability | Status | Where it lives |
|---|---|---|
| Architecture map and role file ownership | Operational | `docs/dev-system/ARCHITECTURE_MAP.md` |
| Specialist roles (seven) | Operational as a contract. Staffing each role is Needs owner | Architecture map |
| No-parallel groups A–E | Operational as rules | `BACKLOG.md`, architecture map |
| Compatibility manifest addressed by SHA-256 | Operational | `tools/compat_harness/manifests/apps.json` |
| External fixture root (`HVM_FIXTURE_ROOT` or `--fixture-root`) | Operational | `tools/compat_harness/manifest.py` |
| Grandfathered in-repo IPAs (three hashes only) | Operational, transitional | same manifest. Deleting the committed IPAs is Needs owner |
| New commercial binaries committed to git | Refused by the harness | manifest rejects a `legacyRepoPath` on any id outside the three |
| Unit and integration gate (the `tools/run_tests.sh` case list, plus the Python packaging and Clash fixture tests) | Operational | `python3 tools/hvm_compat.py run` |
| Per-app launch, crash, frame, audio, import, and log record | Operational on Linux headless | `tools/compat_harness/runner.py` |
| Failure clustering by likely cause and role | Operational | `tools/compat_harness/cluster.py` |
| Baseline diff that ignores MIPS noise | Operational | `tools/compat_harness/diffing.py` |
| JSON report and a JSON Schema document | Operational | `tools/compat_harness/schema/report.schema.json` |
| Issue markdown, dry-run unless `--write` | Operational. Filing to GitHub is not implemented | `tools/compat_harness/issues.py` |
| Harness self-tests | Operational | `tools/compat_harness/tests/` |
| CI workflow that runs the harness | Operational as a file. A green Actions run is Needs owner | `.github/workflows/compat-harness.yml` |
| Published `ci-reports` row for `HEAD` | Needs owner (Actions has not been starting) | existing `tools/publish_report.sh`, unchanged |
| `fulltest` inside `tools/hvm_report.py` | Planned. The new workflow already runs `fulltest`. Changing the publisher's `--only` list is a follow-up so the existing report job keeps its current shape until billing works | `tools/hvm_report.py` |
| Doc reconciliation (`ARCHITECTURE.md`, `HANDOFF.md`, `DEVELOPMENT_AND_RELEASE.md`) | Planned (backlog B-003). This plan does not rewrite those files | — |
| Independent review as a required merge check | Planned process, Needs owner for branch protection | checklist below |
| Automatic assign / automatic PR merge | Planned only as a human step. No bot is wired | — |
| Continuous PM board | Operational as a template | `docs/dev-system/STATUS.md` |
| Clash Royale past the 65% load (message 21903 and what the client asks next) | Planned. The pinned IPA hash is in the manifest as an optional skip. Downloading the third-party URL is Needs owner | Group C |
| Performance work discovered while speeding apps up | Planned, after clipping, Group B | backlog B-105 |
| macOS / Xcode / simulator / signed device run | Needs owner | Group E |
| 16 KB page-size proof | Planned test, Needs owner for a 16 KB host or a simulated page size | Group A |
| `CCCrypt`, Android APK | Needs owner. Both stay unimplemented until that decision | backlog |

## Phases

### Phase 0 — Map and harness

Done by this change: the architecture map, the role contract, the harness, its tests, the workflow file, and the status template. The first baseline JSON is produced by running that harness on this tree.

Exit: `python3 tools/hvm_compat.py run --require-resolved` completes on Linux, writes `report.json`, and a later run diffs cleanly against the committed baseline aside from informational perf noise.

### Phase 1 — The gate actually runs on `main`

1. Owner confirms why Actions jobs die in seconds (backlog B-001).
2. The compat-harness workflow then becomes a real check. Until then, reviewers run the same command locally.
3. Reconcile the stale docs (B-003) without deleting real limitations: the GLSL subset, the missing near clip, and the deliberate `CCCrypt` refusal.
4. Decide whether `hvm_report.py` should grow a `fulltest` suite so the published report matches the harness. Do not do that in the same patch as a Group A or Group B behavior change.

### Phase 2 — Apps already in the tree

Order inside a group is serial. Across groups it is parallel, using the matrix in `ROADMAP.md`.

- Guest-visible symbols the 3 second smokes already hit: `tmpnam`, `regcomp` and the rest of that family, `CGFont*` getters, `glPointSizePointerOES` (B-108 through B-111).
- Near-plane clipping before any raster optimization (B-103, then B-104 and B-105).
- Memory model and precise faults (B-101, then B-102). One owner for all of Group A.

### Phase 3 — Clash Royale past 65%

The restoration notes stop at a 65% loading scene and an unanswered 21903. That is in scope. The harness records the pinned SHA-256 and skips the app when the file is absent. It does not download the client. After the owner accepts a fixture location, the Clash owner runs the existing two-launch probe and extends the peer only with state the client already accepts. No invented sector blobs.

### Phase 4 — Speed, fed by what the profiles show

`docs/PERFORMANCE.md` already lists SIMD raster, threaded slices, dirty-rect compositing, and upload-time texture conversion. Those stay behind the clipper. CPU fusion and `preserve_none` stay behind Group A. New hot spots found while doing that work are added to `BACKLOG.md` and `STATUS.md` before anyone starts them. They are not a license to open a second editor on `OpenGLES.cpp` or `Handlers.hpp`.

### Phase 5 — Apple shell and fixture migration

A Mac (or a macOS Actions job that actually starts) compiles `app/`. Device signing and the extended-virtual-addressing entitlement stay with the owner. Moving the three committed IPAs out of git happens only after the fixture root is the one the harness is pointed at.

## Autonomous cycle

Each lap is one behavior change. The harness is the lap counter. An agent that owns a group does not review its own lap.

### 1. Detect — Operational

```sh
python3 tools/hvm_compat.py run \
  --baseline docs/dev-system/baselines/linux-headless.json \
  --require-resolved \
  --out build/compat-report
```

Read `build/compat-report/diff.json`. Regressions are launch loss, new crashes, new load errors, new load-time unimplemented imports, unit or Python failures, presented frames dropping to zero, or a framebuffer that went from `varied` to `all-black` or `all-white`. MIPS and average frame time are informational.

### 2. Classify — Operational

`report.json` `clusters[]` groups findings by kind: missing symbol, missing method, missing class, fault (null / HLE / guest PC), GL feature, render signal, audio transport, load, encrypted executable. Each cluster names a likely owner and a no-parallel group. Encrypted IPAs are reported and not run. This project does not decrypt FairPlay.

### 3. Assign — Operational as a suggestion, Needs owner to staff

Use the owner on the cluster and `ARCHITECTURE_MAP.md`. If the group letter is A, B, C, D, or E, do not start a second change in that group. Empty group means the patch may proceed beside the others if it does not touch their files. A human updates `STATUS.md` ownership when someone actually takes the item.

### 4. Fix — Planned

The fix lands in the owner's files, with a `fulltest` assertion when the behavior can be expressed without a game binary. This plan does not prescribe the patches. Known hard constraints from the audit, which the fix must not "simplify" away:

- Tail-call handlers and `siglongjmp` are safe only while no C++ object is live across the fault.
- Guest 4 KB permissions are not consulted by the fast load/store path. A 4 KB Linux pass does not prove a 16 KB iPhone.
- `OpenGLES.cpp` replaces negative clip W with its absolute value. SIMD raster waits until that is fixed.
- The GLSL subset rejects control flow on purpose. Do not fall back to guessed sampler roles.
- `CCCrypt` returns unimplemented on purpose.
- Sockets fail closed unless `loopbackNetwork` is set, and that flag is loopback IPv4 TCP only.

### 5. Test — Operational

Re-run the command in step 1. The diff must be clean of regressions. App crashes that were already in the baseline stay recorded; a new one fails the diff. Optional Clash stays a skip until the fixture file is present, and a skip is not a pass.

### 6. Independent review — Planned, Needs owner to require it

A second reader, not the author, does this and does not push fixes onto the same branch:

- Read the diff for edits inside a group the author does not own.
- Confirm no new IPA, no secret, and no FairPlay decryption helper.
- Re-run the harness, or read the JSON the author attached, including `host.pageSize`.
- Reject a report that calls a 4 KB pass an iPhone memory-model pass.
- Check the GLSL and `CCCrypt` constraints above if those files moved.

GitHub is not configured to require this review. Turning that on is an owner action.

### 7. PR — Planned process

Open a PR against `main`. Do not merge it from the agent that wrote the patch. The compat-harness workflow is the Linux check once Actions starts. The macOS job in `.github/workflows/ci.yml` remains the Apple check, and it still does not start on an ordinary push.

### 8. Continuous PM — Operational template, Planned cadence

Update `docs/dev-system/STATUS.md` when a lap finishes: backlog counts, who owns the open groups, the latest compat summary, regressions fixed, open PRs, decisions, and the next lap. The issue generator can refresh the wording (`python3 tools/hvm_compat.py issues report.json`). It does not file. Filing, and any tracker besides this file, is Needs owner. There are no GitHub issues on the repository today.

## Headless limits

Detail is in `docs/dev-system/COMPAT_HARNESS.md`. Short form:

- Linux without a display can build the C++ engine, run `fulltest`, the asm and C guests, the Python fixture tests, and `hvm run` on a decrypted IPA. Frames are a software RGBA buffer. Audio levels require a datagram socket, which the harness creates; nothing is played.
- The same machine cannot compile `app/`, sign a device build, run the simulator, or see 16 KB page faults.
- `ld64` iOS stub apps and CoreMark are not part of this harness. They remain behind `tools/build_cctools.sh`.
- Actions has not been executing since `239a0ab`. Local JSON is the evidence until an owner restores billing or shows a different cause.

## Out of scope for the harness

- Changing handler behavior, shaders, or the Clash peer in order to make a baseline look green.
- Downloading the Clash client.
- Removing the IPAs that are already in git history.
- Auto-opening GitHub issues.
