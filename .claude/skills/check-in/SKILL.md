---
name: check-in
description: Routine head-engineer pass over HydrogenVM — open PRs and their CI, new one-tap report issues, agent card comments, main's CI and the latest IPA — then act (review/merge/build) and summarize for Lawson. Use when asked for status, on a /loop, or at the start of a session.
---

1. Open PRs:
   ```
   gh api "repos/lawsharley0-cyber/HydrogenVM/pulls?state=open&per_page=30" --jq '.[]|"\(.number) draft=\(.draft) \(.user.login) \(.head.ref) \(.title)"'
   ```
   Check CI for each non-draft one, then `/review-pr` and `/merge-pr` the ready ones.
2. New issues since the last check-in: one-tap reports (`[signature] bundle.id:`) go to
   `/triage-report`. Read the latest comments on the agent cards (#570, #571 and any newer) and answer
   their questions there.
3. Check main: its head commit's checks, and whether an IPA exists for it. If merges landed since the
   last IPA, run `/build-ipa`.
4. Summarize for Lawson in plain language:
   - what merged and what it means for players;
   - what's waiting, and on whom;
   - the IPA link (only if that build succeeded);
   - anything that needs him: game files, a decision, a phone test.
