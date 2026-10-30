# HydrogenVM plan (2026-10-30): fix the feedback loop, not the games

**This is the only plan.** It replaces the priorities, work loops and "next steps" in every older
handoff (`HANDOFF.md`, `GROK_HANDOFF.md`, `GEMINI_HANDOFF.md`, `dev-system/MASTER_PLAN.md`,
`dev-system/ROADMAP.md`). Those files stay as *reference* (VM layout, headless run recipes, architecture
notes, per-game history). If one disagrees with this file, this file wins.

Evidence behind it: the research report [docs/research/COMPATIBILITY_STRATEGY_2026-10-30.md](research/COMPATIBILITY_STRATEGY_2026-10-30.md). Key
numbers: of 172 hand-classified `src/` fixes, 1 was game-specific and 2 were CPU bugs; libc/libc++ 23%,
OpenGL ES 23%, Foundation/UIKit/ObjC 22%. A missing API returns 0 silently, crash PCs are imprecise,
and a device test is a 25-30 minute CI build plus a manual iPhone run. Failures therefore surface far
from their cause, and each game re-discovers the same shared holes. Caveat: the 172-fix classification is one reviewer's judgement over an 11-day history, and the import census covers only 3 apps, so treat the percentages as a direction, not a measurement.

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
