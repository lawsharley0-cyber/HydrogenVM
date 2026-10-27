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

## Latest scoped Clash continuation — October 26, 2026

Post-3c07405 inventory preparation: native donation history no longer freezes a
spent card at stock0 forever. GNI1 binds conserved starter inventory to both
actors; strict legacy adoption and required-child detection prevent silent
refills.26 guest tests and fourteen byte-identical bad-file rejections pass;
joined/retired/rare states return twice without economy changes. A private
native211 replenishment reproduces the prior login abort and returns twice in
the corrected default guest. See [DONOR_INVENTORY.md](../clash-royale/DONOR_INVENTORY.md)
for crash-transfer evidence, exact hashes and fixture limitations. No shared
runtime changes or new iPhone/audio-quality/performance qualification.
Reciprocal NPC-request/player-Donate UI remains incomplete.

Post-127ad31 roles/removal: actual original Promote/Demote/leadership/Kick now
persist both actors with exactly one leader and a durable retired-donor state.
Native526 kick intents validate the whole home turn before mutation. GDN1-v1
migrates to v2 only after validation; GCL1 now records a required-donor flag so
whole-record truncation cannot silently seed a new bot. All25 guest tests,
16 damaged-file rejection/retention cases and four deliberate pre/post-commit
abort cases with two normal returns each pass. Economy/full card maps retain
their values. See [CLAN_CONTROLS.md](../clash-royale/CLAN_CONTROLS.md) for exact
package/engine hashes, synthetic fixture limits and the remaining clan work.
Shared src remains the qualified912a87e tree; there is no fresh phone verdict.

Post-0be234d donation component: persistent Local Donor/native profile and finite
original stock now deliver real common Arrows1/10 and rare Giant1/1 with native
receipt toast. Both actors/request commit in one fsync/rename; deliberate
pre/post-commit aborts followed by two same-file returns each produce exactly
one transfer. An actual replacement Giant request stays0/1 with exhausted stock.
All 23 guest tests and 12 damaged-client-file rejection/retention cases pass.
The twelfth reproduces/fixes inbox semantic validation occurring after a
legacy-clan donor migration write; validation now precedes every migration.
See [DONATIONS.md](../clash-royale/DONATIONS.md) for exact candidate hashes,
clock-aged synthetic King-3 fixtures, audio evidence and open boundaries. No
shared runtime change or new iPhone qualification. Reciprocal donations, NPC
stock replenishment, role/kick controls and weekly statistics are unfinished.

Post-Kimi integration: 96003cb's friendly lifecycle is independently reproduced
with the actual native 750-byte result. A real later Giant upgrade (2995→2945,
two materials, level 2 / XP 6) exposes its terminal-friendly wallet freeze at
next login. The follow-up preserves strict active escrow checks but permits
legitimate later economy after completed/cancelled/abandoned bouts. Legacy
friendly stream IDs migrate atomically to namespace 2 / GFR1 version 2 after
all validation, separating them from chat and card requests. Two same-save
returns retain the upgrade; a new friendly sequence charges once and retains
2940 after abandonment/return. The broad guest suite passes 22 tests, with nine
malformed actual-client saves rejected unchanged. See LOCAL_CLANS for exact
host/package hashes, evidence and limits. No shared runtime source changed;
remaining clan interactions and later owner features remain unfinished.

The earlier 0be234d opt-in native donor diagnostic qualifies common/rare 203→211 material
conservation, original rewards and native home/avatar serialization roundtrips
on independent synthetic actors. Empty-stock
203 returns zero without donating, a required guard for the future controller.
Normal/profile objects exclude the probe; all four ARMv7 flag variants compile
without new imports in the same 22-test suite. Actual diagnostic exit/audio
evidence are retained in [DONATIONS.md](../clash-royale/DONATIONS.md); its default
build has since advanced to the synchronous atomic live-transfer subset above.

Guest v42 qualifies persistent solo-clan creation/chat/info/settings/search and
leave/re-create in labeled King-3 component fixtures. Original commands
209/212/208, 1000-Gold creation, actual native member/leader and atomic GCL1/GCS1
storage are retained. An actual owned SIGKILL pending creation recovers without
double debit; eight corrupted-file trials reject and retain the original file.
v46 adds original clan mail, persistent native Inbox and the native 12-hour
cooldown. Owned forced-stop recovery retains one message with no resource
changes; eight damaged-mail files reject unchanged. Fourteen guest tests pass.
v48 fixes actual request command 524 and preserves an unfulfilled 0/10 request,
eight-hour cooldown and owned forced-stop recovery. Eight additional corrupted
GCR1 trials reject unchanged. v64 qualifies the synthetic local-NPC Friendly
Battle lifecycle: exact 5-Gold debit, pre-acceptance refund, pending/accepted
recovery, native abandonment/combat/result and repeated clan-result returns.
Gold/FreeGold restore to 3000 on cancel; accepted outcomes retain 2995 and do
not change ladder trophies, NPC counters, crowns, chests, deck/collection or
reward journal. Four malformed GFR1 client trials reject byte-identical files;
all 15 guest tests pass. Donation fulfillment and real earned King-3/device
clan qualification remain open; see
[local clans](../clash-royale/LOCAL_CLANS.md). The actual earned Silver ID 8
also finishes naturally, grants +28 Gold/three native cards without gem cost,
and retains Gold 147/gems 87 across a clean relaunch. Native profile Wins=0
versus actual arena NPC victories is still an unresolved counter issue.

Shared keyboard routing is pushed through fa5a955. Full Linux runtime 260/260,
TestLab 18/18 and the report/compatibility gate pass; the Apple shell compiles.
The separate Apple UI harness now compiles but initially queried its UIKeyInput
viewport as Other although XCTest classifies it as TextView. fa5a955 uses its
stable identifier across types. Its next run times out after a long cold boot,
without a keyboard verdict; 379a8a5 separates build/boot/test stages. Run
40347015755 now passes guest keyboard insertion/deletion/Return. Editor Save
confirmation and landscape input tests fail; 3f6ff51 fixes the missing explicit
confirmation and dismisses obstructing controls while retaining coordinates.
Its follow-up 40354244581 is superseded/cancelled. Run 40355411529 at mail
source 358cf91 passes all 10 UI tests with zero failures, plus persistence,
74 geometry/activity checks and production shell/harness compile.
CI, Compat, TestLab, ZIP and PCM workflows
at that source pass. Those workflows are not proof of all guest controls.
These checks do not establish a fresh real-iPhone game/input/audio pass.

Fresh clean 912a87e Linux gate is indexed at
[20261026T150956Z-912a87e64a33.json](reports/linux-x86_64/20261026T150956Z-912a87e64a33.json):
summary.ok=true, unitFailed=0, pythonFailed=0, appsResolved=3, appsSkipped=1,
appsCrashed=0, zero baseline/previous regressions. Three timing deltas are
informational, not evidence performance is solved. Full report build 1 / CPU 15
/ iOS 6 / bench 5 passes without skips; TestLab 18/18; focused heap/entry/native
state/service tests and twelve guest tests pass. Host page size is 4096 bytes.
The original optional Clash fixture skips; separate modified-game UI/recovery
and negative tests above supply the actual guest-component evidence.

Guest-only continuation v31 now completes two actual Arena-1 bot victories
after seven earned trainers. Native ELO reaches 65; paid entry, matching Silver
chests, cards/King 2 persist. Three actual native crowns transfer once to home
3/10 across two clean relaunches. Eight guest tests/720 private sector component
comparisons pass. This adds no runtime special case. Later arenas, draw/partial
loss crowns, timers, remaining unlocked controls and phone retesting remain;
see [UI audit](../clash-royale/UI_AUDIT.md). The 5fbaa8d Apple workflow completes
device/simulator builds and packaging, but still has four continue-on-error
simulator console probes failing. A green workflow is not a device game pass.

Guest v33 qualifies measured closed/paused home time, stamp corruption
rejection and an actual paid Silver interruption/recovery. Gem cost remains
15 once; native reward applies once. Repeated exact timer comparisons are
155/99/144/224/59 seconds with no double-counting; nine guest tests pass.
Actual QA player: Gold 119, gems 87, King 2/XP 8, ten cards, trophies 65,
crowns 3/10, two Silver chests (one still unlocking), no journal. Long natural
completion and battle-time accounting remain. Ordered next owner features:
local clans, post-training opponent name/deck/playstyle variety, daily shop.
8570b8a CI passes engine/TL/compat/PCM/ZIP; the separate Apple editor UI-test
target fails to compile missing bridge/editor dependencies (UI_AUDIT details).

Clean integrated 5fbaa8d includes main 5968d88. The indexed Linux 4 KB
[gate report](reports/linux-x86_64/20261026T040954Z-5fbaa8d57a17.json) has
summary.ok=true, unitFailed=0, pythonFailed=0, appsResolved=3, appsSkipped=1,
appsCrashed=0 and zero baseline/previous-report regressions. Original optional
Clash skips in this gate; the separate modified guest is tested explicitly.
Fulltest 256/256, TestLab 17/17, focused native state/service/observer/heap tests,
and full report (build 1, CPU 15, iOS 6, bench 5; no skips/failures) pass.
CoreMark 2365.9 remains below the original iPhone-5 goal. Three legacy smokes
do not close the owner's version-specific reports or known black-frame limits.

The standalone guest now has seven earned training wins, King level 2,
nine cards, native initial name/deck swaps, two real defeats with no grants,
and native graduation to Arena 1 (v27 repairs the pre-refresh snapshot).
Actual 241-second final-trainer/audio audit exits cleanly, zero dropped mixer
blocks, no host Clash service. UI/native APIs, keyboard editing, perceptual
audio, phone behavior, Arena matches/economy/timers and later arenas still have
gaps. Read [UI_AUDIT](../clash-royale/UI_AUDIT.md) for exact hosts and evidence.
No proprietary/generated files or saves are in Git. Main's unrelated updates
are preserved; the PR remains a draft and is not merged into main.

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
| `CCCrypt` ciphers | Decided 2026-10-23: forward to the host crypto library. Done |
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

cbde492 completes the shared keyboard route. Exact clean Linux build passes
fulltest 260/260, TestLab 18/18 (TL-R fails on unchanged 5fbaa8d), focused
heap/entry/native state/service suites, nine guest tests and the full report
(build 1, CPU 15, iOS 6, bench 5; no skips/failures). Full compatibility has
zero baseline/previous regressions; its optional original Clash fixture skips,
so modified-client interaction is qualified separately. Indexed report:
`docs/dev-system/reports/linux-x86_64/20261026T082000Z-cbde4922f25a.json`.
The actual original clan-name field accepts UTF-8 text through queued host
input and enables its Create button in `/var/tmp/hvm-clash-clans-name-input-v34`.
This is a synthetic King-3 component fixture, not earned clan progression.
Creation has not been purchased/ported. Apple compilation/keyboard UI tests and
real iPhone input remain pending; Linux output is not that evidence.
