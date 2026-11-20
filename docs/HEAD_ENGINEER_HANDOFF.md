# Head engineer handoff (2026-11-20)

For a new Claude session taking over HydrogenVM coordination from the previous one. Read this, then
AGENTS.md and docs/EXPANSION_PLAN.md. Everything below was checked against `main` at 9f3ca9a.

## Role and standing instructions from Lawson (owner)

- You are head engineer: decide smaller questions yourself, split work between yourself, Codex and
  Gemini, review and merge their PRs, and build the IPA.
- Give Lawson an IPA link only after the build succeeded. Link format:
  `https://github.com/lawsharley0-cyber/HydrogenVM/actions/runs/<run>/artifacts/<id>`.
- Plain-language replies (Lawson is a marketing student with a CS minor, not a C++ engineer).
- No proprietary binaries or game files in the repo; game files stay private. Don't vendor Arm's
  AArch32 spec. Don't route around the network allowlist via CI. Never store tokens in the repo.

## How work is done

- PRs via `gh api` REST (GraphQL is blocked in the cloud sandbox). Draft → ready:
  `POST repos/…/pulls/N/ccr/ready_for_review`; merge: `PUT repos/…/pulls/N/merge -f merge_method=squash`.
  Stacked PRs: merge top-down with `merge_method=merge` into each parent branch, then squash the base.
- After merges, dispatch the device build: `POST repos/…/actions/workflows/ipa.yml/dispatches -f ref=main`.
- Don't force-push agents' branches; to fix a conflicting agent PR, push a rebased copy to a new
  branch, open a replacement PR and close the old one with a pointer.
- Known CI noise: "File editor Apple validation"/`editor` check is broken; "Linux — tests" failures
  that end in apt download stalls are mirror flakes (a success rerun usually exists). The
  "IPv6 loopback" Android Test Lab case fails on hosts without IPv6.
- Swift 6 strict concurrency is on: never send non-Sendable values (Notification, GCController)
  into `MainActor.assumeIsolated`; read them inside the main-actor region or re-scan.

## Gates (put output in every PR)

```
ninja -C build hvm hvm_tests && ./build/hvm_tests fulltest        # 905 passed on 9f3ca9a
python3 tools/hvm_testlab.py run --hvm build/hvm --out build/testlab-report   # needs ld64 (CI)
python3 tools/android_testlab.py --phone                           # 158/160 here; 2 = IPv6 env
python3 tools/lint_stubs.py --baseline tools/stub_lint_baseline.json   # 0 new
python3 -m unittest discover -s tools/tests
```
Bench runs: iOS and Android games with `--gpu --phone`. Wall-clock A/B is noisy and order-biased
(first run pays cold caches) — alternate order, or use valgrind/callgrind instruction counts.

## Phase status vs the plan's "done when"

Met: A (feedback loop), B (Android breadth; B3/B7 continue as games need), C3 safe-mode retry,
C1 record/replay (crash reproduced from a short recording; "after minutes of play" not yet shown),
D1–D3, D4 libGDX (Shattered Pixel Dungeon runs 60 s, no missing APIs, title/story screen verified
on the bench), I2 cpudiff, I6 16 KB page protection, I7 msgSend cache (176→56 ns/send), guest qsort,
FP fast path.

Built but not proven on a game (next work):
- D4 pure-Java: Dodge only boots.
- C2 auto-player: works on Liquid Wars; Dead Space fails before its first frame; vision step never
  run with a real model key.
- E1 launch cache: wired into the app; Alto cold start before/after not measured.
- E2 HD: toggle wired; no before/after screenshots.
- E4 quick save: save works and is in the app; resume after app kill not shown.
- F1 server toolkit: Rayman pack exists, endpoints are not captured from the real game; Rayman past
  "Connecting" not shown.
- G1 compatibility list: generator only; nothing publishes or refreshes it.
- H1/H2: planner + Ed25519 pack signing (2 unit tests); no end-to-end planted fix or pack-to-phone.
- I4 recompiled packs: prototype (~12 instruction kinds, 10-step loop test); Dead Space target far.

Not started: H3 on-device helpers, I3 profile-guided speedups (target 1.5× guest MIPS), streaming
music (tracks are decoded whole), E3 on-screen control layouts (controllers work from B6).

## Open right now

- PRs awaiting review/merge: #614 (repair planner inputs), #617 (oversized Quick Save), #618
  (damaged block-cache records), #619 (Java monitor ownership). Issues: #612, #613, #615, #616.
- Player reports still open: #538 iPhoto, #541 Minecraft PE 0.9.0 NULL crash (only 0.10.0 on hand),
  #533 Cut the Rope GameKit, #530 Crossy yodo1 stall (phone check), #529 Rayman, #531 Vector,
  #540/#542/#549 watchdog-before-save.
- Cards: #570 (Phase C, Gemini), #571 (Phase D, Codex), #568 (I6, done).

## Proposed next split (Lawson had not answered yet)

- Claude: review/merge #614/#617/#618/#619; then I3 and streaming music.
- Gemini: prove the game-level goals — Alto start time (E1), HD screenshots (E2), quick-save resume
  (E4), Rayman past "Connecting" (F1), Dead Space under the bot (C2).
- Codex: pure-Java games (Canvas), next step of I4.

## Test material

Game files live on Lawson's PC (Desktop folders `IPA`, `APK`) and Google Drive (`IPA`, `APK`); the
nightly runner (#557) reads both. A cloud session has no copy unless Lawson attaches or links them.
