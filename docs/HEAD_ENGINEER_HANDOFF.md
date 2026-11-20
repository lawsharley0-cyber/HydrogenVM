# Head engineer playbook and handoff

Last updated 2026-11-20 against `main` (after #620). It is written for a Claude Code session taking
over coordination of HydrogenVM. The role and Lawson's standing instructions are in `CLAUDE.md`. This
file is the how-to and the current state.

---

## 1. First hour in a new session

1. Read `CLAUDE.md`, this file, then `AGENTS.md` (the gates and ownership) and the phase you are
   working on in `docs/EXPANSION_PLAN.md`.
2. Check the tools work:
   ```
   gh auth status          # must be logged in with push access to lawsharley0-cyber/HydrogenVM
   gh api repos/lawsharley0-cyber/HydrogenVM --jq .permissions   # admin/push true
   ```
   In a claude.ai/code cloud session, GitHub goes through a proxy: `gh api` REST works,
   GraphQL-backed commands like `gh pr merge` and `gh pr view` may not. Use the REST forms in §3.
3. Build once (§5) and run `fulltest`, so you know the baseline is green on your machine.
4. Run `/check-in` (§7) to see open PRs, CI and new reports.

## 2. Who does what

| Who | Strength | Typical cards |
|---|---|---|
| **You (Claude Code)** | Coordination, review, merges, CPU/memory core (`src/core/cpu`, `src/core/memory`), speed work, cross-cutting fixes | Phase I, report fixes, reviews |
| **Codex** | Long, self-contained code work | Phase D (Java/DEX interpreter, `src/runtime/android/dex/`), I4 packs |
| **Gemini** | Has the game files and runs games | Phase C (replay, auto-player, safe mode), game-evidence cards |

- Cards live as GitHub issues (#570 Phase C/Gemini, #571 Phase D/Codex). Post decisions on the card.
  The agents read them.
- Hotspot files have one editor at a time: `Process.cpp/.hpp`, `objc/Runtime.cpp`, `libc/System.cpp`.
  Name the editor on the card (`Hotspot editor: X`) before parallel work starts.
- When Lawson asks for prompts for Codex or Gemini, write a self-contained prompt with these parts:
  - what to read first (only the files needed);
  - ownership;
  - ordered steps, each with a "done when";
  - the exact gates;
  - "one PR per step, draft until gates pass, don't merge";
  - the iteration cap of 8.

## 3. Merging (the part that trips sessions up)

**Why a session refuses or fails to merge, and the fix:**
- **"AGENTS.md says don't merge."** That rule is for worker agents; `CLAUDE.md` records that Lawson
  delegated merging to the head engineer. If you still have doubts, ask Lawson once in chat and
  proceed on his answer.
- **`gh pr merge` fails in a cloud session (GraphQL blocked).** Use REST, shown below.
- **"Pull Request is not mergeable"**: check `mergeable_state`:
  - `dirty` (conflicts): rebase. For an agent's branch, push a rebased copy to a *new* branch, open a
    replacement PR and close the old one with a pointer. Never force-push their branch.
  - `blocked`: it's a draft or a required check is missing. Mark it ready (below) or wait for the check.
  - `unknown`: GitHub is still computing; retry in a few seconds.
- **Not authenticated locally**: `gh auth login` (Lawson does this once on his machine).

**Merge procedure** (or run `/merge-pr <n>`):
```
R=repos/lawsharley0-cyber/HydrogenVM
gh api $R/pulls/<n> --jq '{draft,mergeable_state,head:.head.sha,base:.base.ref}'
sha=$(gh api $R/pulls/<n> --jq .head.sha)
gh api "$R/commits/$sha/check-runs?per_page=50" --jq '.check_runs[]|"\(.name): \(.status)/\(.conclusion)"' | sort -u
gh api -X POST $R/pulls/<n>/ccr/ready_for_review        # if draft (this endpoint works through the proxy)
gh api -X PUT  $R/pulls/<n>/merge -f merge_method=squash
```
- **Stacked PRs** (A ← B ← C, each based on the previous): merge top-down with
  `-f merge_method=merge` into each parent branch, then squash the bottom one into `main`. Retarget
  any later PRs to `main`: `gh api -X PATCH $R/pulls/<n> -f base=main`.
- **CI noise you may merge through** (always say so in your merge comment or summary):
  - The `editor` / "File editor Apple validation" check is known broken.
  - "Linux — tests, report, CoreMark" failing in **Install toolchain** with apt download stalls is a
    mirror flake when another run of the same check on that commit succeeded.
  - The Android Test Lab "IPv6 loopback" case fails on hosts without IPv6.
- **Anything else red:** read the log first (`gh api $R/actions/jobs/<id>/logs | grep -E "error|FAIL"`).
  Fix it, or send it back on the PR.

**After merging:** close the issues the PR fixes, with a comment. Then build the IPA (§4).

## 4. Building the iPhone app and giving Lawson the link

```
gh api -X POST repos/lawsharley0-cyber/HydrogenVM/actions/workflows/ipa.yml/dispatches -f ref=main
gh api "repos/lawsharley0-cyber/HydrogenVM/actions/workflows/ipa.yml/runs?per_page=1&branch=main" --jq '.workflow_runs[0]|"\(.id) \(.status)/\(.conclusion)"'
gh api repos/lawsharley0-cyber/HydrogenVM/actions/runs/<run>/artifacts --jq '.artifacts[]|"\(.id) \(.name)"'
```
- Link format: `https://github.com/lawsharley0-cyber/HydrogenVM/actions/runs/<run>/artifacts/<id>`.
- Only send it when the run concluded `success`.
- Include a short list of what to test on the phone.
- To validate Swift/app changes on a PR branch before merging, dispatch `ipa.yml` with
  `-f ref=<branch>`. The Linux checks don't compile `app/`.
- **Swift 6 strict concurrency is on.** The common failure is sending a non-Sendable value
  (`Notification`, `GCController`) into `MainActor.assumeIsolated`. Read it inside the main-actor
  region, or re-scan.

## 5. Building and the gates

Linux needs Clang, CMake, Ninja, and the packages in AGENTS.md (libstdc++-14-dev, libicu-dev, …).
```
cmake -S . -B build -G Ninja -DCMAKE_CXX_COMPILER=clang++ -DCMAKE_BUILD_TYPE=Release
ninja -C build hvm hvm_tests
./build/hvm_tests fulltest                                     # 905 passed at handoff
python3 tools/hvm_testlab.py run --hvm build/hvm --out build/testlab-report   # needs ld64; CI runs it
python3 tools/android_testlab.py --phone                       # all pass except IPv6 on IPv6-less hosts
python3 tools/lint_stubs.py --baseline tools/stub_lint_baseline.json          # 0 new/changed
python3 -m unittest discover -s tools/tests
```
- `TestRunner.cpp` is huge; a full rebuild of `hvm_tests` takes 15–20 min. Run long builds in the
  background (§8).
- If `fulltest`'s count doesn't change after you add a check, the object is stale. `touch` the file
  and rebuild.
- **Honest stubs:** a function that returns a constant must be labelled.
  `supportDef<&fn>(p, "_sym", SupportStatus::Real|Approx|Unavailable, "reason")` or
  `supportMethod(...)` (runtime/Support.hpp). The lint fails on new unlabelled ones.
- An inline lambda used as a template argument can't contain a `>` comparison. Use a named function.
- The guestlib audit test misparses C++ digit separators (`1'000'000`). Use plain literals in
  `src/runtime`.

## 6. Running games (the bench)

```
./build/hvm import <game.ipa|.apk> <outdir>             # unpacks into <outdir>/<bundle-id>/<Name>.app
./build/hvm run <path/to/Name.app> --gpu --phone --run-for 40 --home <fresh dir> \
    --report out.json --screenshot out.png --quiet
```
- `--phone` emulates the iPhone: 16 KB pages, budgets. `--gpu` matches the phone's renderer.
  Always use both for evidence.
- The report JSON has:
  - `crashed`, `stillRunning`, `exitStatus`, `signature`;
  - `perf.gameSwaps` (frames), `perf.guestInstructions`, `perf.avgFrameMs`, `perf.timeToFirstFrameMs`;
  - `missingApis` (what to implement next);
  - `pageProtection`.
- **A/B timing is noisy and order-biased:** the first run pays cold file caches. Alternate the order,
  compare `guestInstructions` retired in a fixed time, or use valgrind/callgrind instruction counts.
- Kill switches for A/B: `HVM_GUEST_PROT=0` (page protection), `HVM_NO_FAST_SEND=1` (msgSend cache).
- **Game files** are on Lawson's PC (Desktop folders `IPA` and `APK`) and Google Drive (`IPA`, `APK`).
  A local Claude Code session on his PC can use them directly. The nightly runner
  (`tools/nightly/`, see `tools/CORPUS_TOOLS.md`; #557) reads both folders.
- **Report triage** (`/triage-report`): one-tap phone reports arrive as issues titled
  `[signature] bundle.id: …`.
  - "Missing method/function X": implement X honestly and add a fulltest check. Then rerun the game
    to find the next stop; games often need several in a row.
  - "Crash / bad memory access": get the game, reproduce with `--phone --gpu`, and use the
    record/replay recording if one is attached (C1, `--replay`).
  - Close the issue only when the fix is merged, saying it ships in the next IPA.

## 7. Status at handoff (vs each card's "done when")

**Met:**
- A, the feedback loop.
- B, Android breadth. B3 and B7 continue as games need them.
- C3, safe-mode retry.
- C1, replay. A crash was reproduced from a short recording; "after minutes of play" is not yet shown.
- D1–D3.
- D4 libGDX. Shattered Pixel Dungeon runs 60 s with no missing APIs.
- I2 cpudiff.
- I6, 16 KB page protection and precise fault PC.
- I7: the msgSend cache (176→56 ns per send), guest qsort, the FP fast path.

**Built, not yet proven on a game (next work):**
- D4 pure-Java: Dodge only boots. Needs Canvas.
- C2 auto-player: Dead Space fails before its first frame. The vision step has never been run with a
  real key.
- E1 launch cache: Alto's cold start before/after not measured.
- E2 HD: no before/after screenshots.
- E4 quick save: resume after an app kill not shown.
- F1: the Rayman pack's endpoints were not captured from the game, and Rayman getting past
  "Connecting" isn't shown.
- G1: nothing publishes or refreshes the compatibility list.
- H1/H2: no end-to-end planted fix, and no pack delivered to the phone.
- I4: prototype only (~12 instruction kinds). Dead Space with a pack is the big remaining job.

**Not started:**
- H3, on-device helpers.
- I3, profile-guided speedups (target 1.5× guest MIPS).
- Streaming music: tracks are currently decoded whole.
- E3 on-screen layouts. Controllers already work from B6.

**Open at handoff:**
- **PRs:**
  - #614: repair planner inputs.
  - #617: oversized Quick Save.
  - #618: damaged block-cache records.
  - #619: Java monitor ownership.
- **Issues:**
  - #612, #613, #615, #616: the follow-ups behind those PRs.
  - Reports #538 (iPhoto), #541 (Minecraft PE 0.9.0, only 0.10.0 on hand), #533 (Cut the Rope
    GameKit), #530 (Crossy yodo1 stall), #529 (Rayman), #531 (Vector), and #540/#542/#549 (the
    watchdog stops the game before it finishes saving).

**Proposed next split** (Lawson had not confirmed yet):
- You: merge #614, #617, #618 and #619; then I3 and streaming music.
- Gemini: game-level proofs. Alto start time, HD screenshots, quick-save resume, Rayman past
  "Connecting", and Dead Space under the bot.
- Codex: pure-Java/Canvas, and the next I4 step.

## 8. Using Claude Code's own tools on this project

- **Plan mode** (Shift+Tab, or `claude --permission-mode plan`): use it before touching hotspot files
  or the CPU core. Explore, write the plan, then post it on the card's issue (an AGENTS.md rule) before
  editing.
- **Subagents** (`.claude/agents/`):
  - `pr-reviewer`: an independent, read-only review of an agent's PR against the gates and ownership
    rules. Use it for every non-trivial PR, so the author's work isn't graded by the author.
  - `game-bencher`: runs a list of games with `--gpu --phone` and returns a crash/frames/missing-API
    table. Run it in the background while you keep reviewing.
  - The built-in `Explore` agent is for wide code searches, such as "where is X registered".
- **Skills** (`.claude/skills/`): `/merge-pr`, `/build-ipa`, `/review-pr`, `/bench-game`,
  `/triage-report`, `/check-in`. They encode the procedures above so you don't retype them. Improve
  them when a procedure changes.
- **Background work:** run builds, `fulltest` and game batches as background shell tasks. Check on
  them, and keep reviewing PRs meanwhile.
- **Worktrees:** use `git worktree add` (or Claude Code's worktree support) when you need a second
  branch, such as rebasing an agent's PR while your own build runs. Don't switch branches under a
  running build; ninja then rebuilds most of the tree.
- **Recurring check-ins:** `/loop` in a CLI session re-runs `/check-in` on an interval while the
  terminal stays open. For overnight or away-from-PC watching, use a cloud scheduled task (routine) or
  start a cloud session with `claude --cloud "Run /check-in every 90 minutes until 8am Central"`.
- **Auto-fix:** `/autofix-pr` on a PR branch makes a cloud session watch it and fix CI failures and
  review comments. Useful for your own PRs while you work on the next thing.
- **Remote Control:** `/remote-control` in a local session lets Lawson follow and steer it from his
  phone.
- **Memory:** add durable project facts (new gates, CI quirks, decisions) to this file or `CLAUDE.md`
  in the same PR as the change. Then the next session inherits them.

## 9. Habits that kept quality up

- **Measure before optimizing,** and prove the speedup on the phone-mode bench, not only in a
  micro-benchmark.
- **Every new real-game bug gets a minimal check** that fails before the fix: fulltest, an Android
  Test Lab case, or a Test Lab case.
- **Expected values come from the spec or are worked out by hand,** never copied from emulator output.
- **Report evidence honestly.** "Built but not proven" is a valid status. Don't mark a card done until
  its "done when" line is met.
- **Keep Lawson's replies short and in plain language.** Say what changed for the player, what to test,
  and the IPA link.
