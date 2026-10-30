# HydrogenVM plan (2026-10-30): fix the feedback loop, not the games

**This is the only plan.** It replaces the priorities, work loops and "next steps" in every older
handoff (`HANDOFF.md`, `GROK_HANDOFF.md`, `GEMINI_HANDOFF.md`, `dev-system/MASTER_PLAN.md`,
`dev-system/ROADMAP.md`). Those files stay as *reference* (VM layout, headless run recipes, architecture
notes, per-game history). If one disagrees with this file, this file wins.

Evidence behind it: the research report [docs/research/COMPATIBILITY_STRATEGY_2026-10-30.md](research/COMPATIBILITY_STRATEGY_2026-10-30.md). Key
numbers: of 172 hand-classified `src/` fixes, 1 was game-specific and 2 were CPU bugs; libc/libc++ 23%,
OpenGL ES 23%, Foundation/UIKit/ObjC 22%. A missing API returns 0 silently, crash PCs are imprecise,
and a device test is a 25-30 minute CI build plus a manual iPhone run. Failures therefore surface far
from their cause, and each game rediscovers the same shared holes. Caveat: the 172-fix classification is one reviewer's judgement over an 11-day history, and the import census covers only 3 apps, so treat the percentages as a direction, not a measurement.

## Hard rules (unchanged)

- No FairPlay decryption. Never write ciphers; forward to host libraries.
- Never commit IPAs, certificates, provisioning profiles or secrets. Treat downloads as untrusted.
- No fake success: a stub that returns plausible success must be labelled (see T3) or replaced.
- No game-specific code paths. One shared fix per bug class, each with a Test Lab case that fails first.
- Never weaken, delete or skip a test to get green. `main` is protected: every change is a PR.
- Do not resume Clash Royale (PR #138 stays parked). Do not redo the stat-layout change (`c28702f`).
- Agent process rules (plan first, iteration cap of 8, one editor for hotspot files, draft until gates pass, game-run cards stay on a runner) are defined once in [`AGENTS.md`](../AGENTS.md#parallel-agent-rules).

## The work, in order

Each task is a card in the next section. Do them roughly in order, but cards with disjoint files run in
parallel (see "Running several agents").

| # | Card | Why first | Effort |
|---|---|---|---|
| T1 | Teardown / quit bug + per-game arena isolation | Batch results are untrustworthy until a game can quit and the next can start (B-116, "failed to reserve 4 GB") | days |
| T2 | Call-site-aware missing-API record, tainted-run flag, last-N call ring, ObjC symbolication | Turns "log once, return 0" into a causal chain | 3-6 d |
| T3 | Stub status + provenance registry and a fake-success lint | Stops plausible lies relocating failures | 2-4 d |
| T4 | `--dump-support` manifest, per-IPA census, cross-game failure ledger | Computes the fix order instead of arguing it | 3-4 d |
| T5 | Class campaigns: (a) libstdc++ ABI, (b) GL context/thread/FBO model, (c) Unity/Mono startup, (d) libc formatting/memory property tests | ~60% of historical fixes | 1-14 d each |
| T6 | Corpus-gated `git bisect` wrapper, GL ring capture | Needed while many PRs land | 2-3 d |
| T7 | Semantic record/replay (touch, timers, file reads, RNG, thread order) | Long-session crashes (Goat ~219 s) | 1-2 wk |
| T8 | Port objc4 tests as Test Lab cases; vendor CF leaf algorithms only after Lawson decides on the APSL licence | Oracles for Foundation/ObjC | 3-5 d |

Effort figures are estimates, not measurements. Finish T1-T3 before starting T5-T8.

**Do not:** swap in Apple's real libc/objc4/CF wholesale; fuzz the CPU; rank work by import frequency
(65% of symbols are used by one app; N=3 has no predictive power); treat `--keep-going` output as a
compatibility verdict; add title-keyed code.

## Task cards

Every card: read only the files it names, branch from `main`, one PR, PR body states tests run and
what was NOT run. A card is done only when the *done when* line is true and the gates in
`AGENTS.md` pass (Test Lab, `hvm_tests fulltest`, harness).

### T1 Teardown and arena isolation
- **Files:** `src/runtime/Process.cpp`, `src/core/` arena code, quit/lifecycle paths, `app/HydrogenVM/BatchTest.swift` (macOS-only compile, so keep Swift edits minimal and obvious).
- **Do:** first add a last-N call ring (shares code with T2), reproduce with `tools/hvm_probe.py` on Angry Birds, Minecraft PE Lite, Talking Larry, Mega Jump (taps + background/foreground + quit). Find why quit never finishes. Make the batch tester release the arena before starting the next game.
- **Done when:** the four games quit within 4 s after the probe script; a run of 10 games in one batch has zero "failed to reserve 4 GB arena"; a Test Lab case covers it; **and** the call ring exists with a unit test that fills it past capacity and reads back the last N entries in order (T2 builds on it).
- **Ownership:** one agent only. A bug that every agent can reproduce is a monolith; parallel agents all chase it (the lesson from Anthropic's parallel-agent compiler build). Others work on independent failures meanwhile.
- **Needs a runtime:** yes (must run games).

### T2 Observability
- **Files:** `reportMissing` and the missing-function/selector/class paths in `src/runtime/Process.cpp` and `src/runtime/objc/Runtime.cpp`; crash bundle writer.
- **Do:** per missing API record count, first 8-12 frame backtrace, time since launch; set a `tainted` flag on first hit and print "passed with N stub hits"; ring of the last ~256 HLE/ObjC/GL calls in the crash bundle; map ObjC IMP addresses to `-[Class sel]` names.
- **Done when:** unit tests show (1) a missing API produces a record with caller, count, first backtrace and time since launch, (2) the tainted flag is set on the first hit, (3) the crash bundle contains the last N ring entries, (4) an ObjC IMP address maps to `-[Class sel]`. Then `hvm_probe.py` on at least one IPA shows tainted runs separately.
- **Needs a runtime:** unit tests for (1)-(4) need none; the final probe check needs an IPA, so the runner does it or the PR says NOT RUN.

### T3 Stub honesty
- **Files:** registration helpers in `src/runtime/*` (the ones that register host functions/methods), new `tools/lint_stubs.py`.
- **Do:** add a status (Real / Approx / Stub / Unavailable) and one-line provenance to registrations; start with the known lies: `mprotect/madvise/msync`, `dlopen`, `AVAsset` playable, occlusion query = 1, `glDrawBuffers/glReadBuffer`, `AUGraphGetIndNode/NodeCount`, `AudioUnitAddRenderNotify`, `SCNetworkReachabilityGetFlags`. Lint flags constant-success bodies and capability queries returning YES without a status.
- **Done when:** lint runs in CI and lists every unlabelled constant-success body; the known lies are labelled or fixed.
- **Needs a runtime:** no.

### T4 Support manifest and ledger
- **Files:** `tools/` only (plus a small `hvm` flag).
- **Do:** `hvm --dump-support` emits JSON (symbol/class/selector, status, provenance); `tools/census.py <ipa>` lists what an IPA imports versus the manifest; `tools/ledger.py` keys failures by signature across games and tags a failure class. Add `--fast` to `tools/hvm_probe.py`: probe a random sample of games (seeded, so reruns match) and print a summary of at most 40 lines, with details in files and tagged `ERROR:` lines, so agents are not stuck on hour-long full sweeps. Reports stay structured JSON with a final summary object; read the summary, not the exit code alone.
- **Done when:** census of the three repo-root IPAs reproduces the research numbers (Angry Birds 0 unimplemented symbols).
- **Needs a runtime:** build only; IPAs for census.

### T5 Class campaigns (one PR each)
- **a** libstdc++ 4.2 ABI conformance guests (string, RB-tree, streams, SJLJ) as Test Lab cases; **b** write down one GL context/thread/FBO model in `docs/` then enforce it; **c** Unity/Mono startup checklist (stat layout, signals, `dlopen(NULL)`, `getpw*`, audio format) as probe checks; **d** glibc property tests for printf/string/memory helpers.
- **Done when:** each ships tests that fail on the pre-fix code.

### T6 Bisect and capture
- **Files:** `tools/` only. `tools/bisect_corpus.py` wraps `git bisect run` around the probe/harness baseline.
- **Also:** `tools/ab_diff.py <old-build> <new-build>` runs the same game with the same deterministic input script (`--tap`/`--drag`/`--lifecycle`, isolated `--home`) on both builds and reports the first divergence in the report JSON, log lines, frame hashes and audio counters. This is our substitute for a reference implementation.
- **Done when:** on a branch with a deliberately broken commit planted among 10, the bisect script finds that commit and exits 0, and fails clearly (not silently) when the harness cannot run; `ab_diff.py` flags a planted behaviour change in a Test Lab guest and reports no difference between two identical builds.

### T7 and T8
Start only after T1-T3 are merged. Write a one-page design first and wait for review.
- **T7 done when:** a recorded 60 s session of one Test Lab guest replays with identical final output on two runs, and the design lists what is not deterministic.
- **T8 done when:** at least 5 objc4 test cases run as Test Lab cases against our runtime with independent expected values, and Lawson has recorded a licence decision before any Apple source is vendored.

## Running several agents at once

**Rules that make parallel work safe**
1. One card per agent, one PR per card. Name the branch `agent/<card>-<tool>`.
2. Cards that touch the same file do not run together. Safe parallel sets today: {T1, T3, T4}, then {T2, T5a, T5d, T6}. T2 and T1 both add the call ring: T1 lands it, T2 builds on it.
3. Claim a card before starting: comment on its issue (the lock-file idea from parallel-agent builds, using git/GitHub as the arbiter). Before starting, an agent comments its card number on the tracking issue so duplicates (8 of 36 recent drafts were "Continue PR #N" re-dos) are visible.
4. Open as a draft, mark ready once the gates pass and their output is in the PR body. CodeRabbit reviews; Copilot or the author fixes; Lawson adds `safe-to-automerge` (see `.github/copilot-instructions.md`).
5. Rebase on `main` before asking for review; never merge another agent's branch into yours.

**Which agent for which card.** Reliability here means: ran the gates for real, evidence in the PR, no
fake stubs, no weakened tests. Historic signal in this repo (from the research): 13 of 36 draft bodies say
tests were "inferred" or not run, and 10 report clang crashing. Those came from sandboxes that cannot run
games or `ld64`. So:

| Needs | Agents that can do it | Notes |
|---|---|---|
| Run real games, headless probe, long builds | Gemini CLI on the Google VM; Claude Code on a real machine | VM has 2 CPUs and a small disk: one card at a time |
| Pure code/tests/tools, no IPA | Copilot coding agent, Codex, Jules, Cursor | Must show the exact test commands and output in the PR; Jules drafts without runs are not accepted |
| Swift/app work | Only after a macOS CI run proves it compiles (use `[mac]` in the commit subject) | Never claim Swift compiled from Linux |

**Trial before trust.** Give each agent the same small card first (T3 labelling of two known lies, or
T4 census) and score it:

| Check | Pass |
|---|---|
| Gates run and pasted, not described | yes |
| Test fails on old code, passes on new | yes |
| Touches only the card's files | yes |
| No new fake-success or title-keyed code | yes |
| Review rounds to merge | ≤ 2 |

Keep the agents that score 5/5 twice. Route cards by that record, not by reputation. Keep the score in
`docs/AGENT_SCORECARD.md` (one line per PR).

## Manager (start light)

Parallel-agent builds that worked (Anthropic's 16-agent compiler) used no orchestrator: agents claimed
tasks by lock and the test harness steered them. So start with:

1. **Card issues** T1-T8 with labels `card`, `agent:runner` / `agent:coder`, and a status label; blockers
   written on the issue; a pinned tracking issue listing who has what.
2. **A scheduled GitHub Actions nudge bot (no AI, free):** work limit of 3-4 open cards, comment
   `@copilot please fix the unresolved CodeRabbit comments` on PRs with unresolved threads, flag PRs idle
   for 24 h, mark cards `ready` when their blocker merges, update the tracking issue.
3. **An AI check-in only if needed**, on a flat-rate agent (not Vertex credit): daily summary for Lawson.
   It never merges.

Runners: Lawson's desktop (WSL2) with a flat-rate agent first; the Gemini VM only for long probe sweeps,
and stopped when idle (the Google credit is nearly spent).

## What Lawson does

1. Merge the PR that adds this file; allow auto-merge in repo settings; confirm Copilot is enabled.
2. Review PR #229 (auto-merge gatekeeper) for security before it merges.
3. Close duplicate drafts (three Tiny Wings PRs #157/#214/#223; overlapping #167/#206/#215) so agents don't review dead branches. Leave #118 and #138.
4. Upload new device reports to `~/inbox` on the VM; sideload the newest build when a card says to.

## Log

- 2026-10-30: plan created from the compatibility research; older handoffs marked reference-only.
