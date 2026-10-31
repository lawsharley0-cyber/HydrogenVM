# Roadmap

Phases match `IMPLEMENTATION_PLAN.md`. There is no calendar on this roadmap. Ordering is by dependency and by the no-parallel groups.

Integration branch for this system: `main`. `docs/HANDOFF.md` separates current `main` integration from archived `gemini-merge` evidence (B-003).

## Phase 0 — Repository system

Operational in this change, except the items marked Needs owner.

- Architecture map, seven roles, groups A–E.
- Hash-addressed app manifest and Linux harness.
- Baseline report checked in as JSON after a local run.
- Workflow file `.github/workflows/compat-harness.yml`.

Still Needs owner: a fixture directory if the in-repo IPAs are retired, and a Mac for `app/`. Actions billing is not outstanding. The compat harness passed on `6af9976` (run 39646706013) and Linux CI published `runs/20261021-175201-39646696896-linux` (run 39646696896). The empty-step failure on `07fabbb` (run 39633887764) was real.

## Phase 1 — Gate `HEAD`

Depends on Phase 0.

1. Done for Linux on `6af9976`. Compat harness run 39646706013 passed. Linux CI run 39646696896 published a report. The macOS job was skipped by its existing condition, not by the old empty-step failure on `07fabbb` (run 39633887764).
2. The harness workflow is the Linux gate: `python3 tools/hvm_compat.py run --baseline docs/dev-system/baselines/linux-headless.json`. A missing required fixture fails the process. `--require-resolved` is still accepted.
3. Reconcile stale docs with the code, keeping the documented shader subset, remaining memory/fault limits, and the `CCCrypt` refusal. Homogeneous clipping is merged in PR #4.
4. Optionally teach `tools/hvm_report.py` to run `fulltest` so `ci-reports` matches the harness. Separate patch, test/regression role only.

## Phase 2 — In-tree apps

Depends on Phase 0. Can overlap Phase 1's doc work. Cannot overlap a broken Group A or Group B if the same files are already being edited.

- Symbols the audited 3 second runs already missed.
- Near clip, then a checked-in shader corpus, then raster cost.
- Guest page permissions and precise faults, one owner, after no other agent is in `Handlers.hpp` or `Engine.cpp`.

## Phase 3 — Clash Royale past 65%

Depends on an owner decision about the pinned client URL (`29269ede…`, recorded in the manifest). The engineering sequence is already written in `docs/CLASH_ROYALE_RESTORATION.md`: 21903 sector state, then whatever that client asks next, using the existing refusal for unsupported state. The harness smoke is not a substitute for the two-launch probe.

This phase is in scope. It does not outrank a crash in Angry Birds, Tiny Tower, or Art of Glow if both are ready to start: they can run together only because they are different groups (C versus D/B).

## Phase 4 — Performance follow-ons

Depends on Phase 2's clipper for raster work, and on Phase 2's memory work for CPU fusion. New ideas found while profiling are added to `BACKLOG.md` before implementation. Known candidates already written down in `docs/PERFORMANCE.md`:

- SIMD triangle raster and threaded scanline slices.
- Dirty-rect compositing.
- Upload-time conversion of textures.
- Budget kept in a register, predicated hot ops, return-address prediction, inlined guest `memcpy`, superblocks.

Do not open a separate "superinstruction" or "Unity shader" agent. Those files belong to the Group A owner and the Group B owner.

## Phase 5 — Device and fixtures

Depends on Needs owner: Mac runner, signing, which devices have the extended virtual-address entitlement, and where fixtures live.

- Compile current `app/` (the `[mac]` job never started for `06a7b16`).
- Point `HVM_FIXTURE_ROOT` at the store and stop resolving `legacyRepoPath`.
- Only then consider removing the three IPAs from the tree. History already contains them; deletion is an owner decision.

## What can run at the same time

A cell is safe only when the two changes do not edit the same file. `PlatformStubs.cpp` is shared by audio and by other UIKit stubs: coordinate, do not both edit it.

|  | A memory | B pixels | C Clash wire | D frameworks | E Apple shell | audio (not PlatformStubs) | harness / docs |
|---|---|---|---|---|---|---|---|
| A memory | no | yes | yes | yes, if not `System.cpp` | yes, if `HVMEngine.h` is frozen | yes | yes |
| B pixels | yes | no | yes, unless a dumped shader is in the same patch | yes, GLES must not edit `UIWidgets.cpp` | yes | yes | yes |
| C Clash wire | yes | yes | no | yes, if not `Net.cpp` | yes | yes | yes |
| D frameworks | yes | yes | yes | no | yes | yes, if not `PlatformStubs.cpp` | yes |
| E Apple shell | frozen `Process` surface only | yes | yes | yes | no | yes | yes |

Also serial, even though they are not a lettered group:

- CPU fusion waits until Group A has landed.
- SIMD raster, threaded raster, and shader widening wait until the near clip has landed. They are all Group B.
- A second editor of `LibStdcxx*.cpp` waits. That sub-area sits inside the guest-frameworks role.
- `tools/hvm_report.py` and `.github/workflows/ci.yml` are one patch at a time. The new compat workflow is a different file and can land beside them.
- The independent reviewer starts after the author, never instead of them.

`regcomp` / `tmpnam` may proceed beside Group D when they stay in `src/runtime/libc/` and do not rename Foundation symbols. `CGFont*` stays in Group D. `glPointSizePointerOES` stays in Group B.

## What this roadmap will not schedule

Until an owner says otherwise: Android APK support, implementing `CCCrypt`, and decrypting FairPlay. The loader already reports encryption and stops.
