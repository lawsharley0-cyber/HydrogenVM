---
name: review-pr
description: Review a HydrogenVM PR from Codex, Gemini or another agent against AGENTS.md gates, ownership groups, hotspot rules and the card's done-when, and decide merge / request changes. Use for every incoming agent PR.
argument-hint: <PR number>
---

Review PR $ARGUMENTS. For anything larger than a small fix, delegate the reading to the `pr-reviewer`
subagent, then decide yourself.

1. Context:
   - `gh api repos/lawsharley0-cyber/HydrogenVM/pulls/$ARGUMENTS --jq '.title,.body'`
   - The list of changed files with +/- counts.
   - The linked card issue and that card's "done when" in docs/EXPANSION_PLAN.md.
2. Check:
   - **Gates in the body:** fulltest count (it should rise when checks were added), Test Lab, Android
     Test Lab, lint "0 new", unittest. For app/ changes, the device build passed.
   - **Ownership:** files stay in the author's group (AGENTS.md A–E). Hotspot files are edited only by
     the named hotspot editor.
   - **Evidence is real:**
     - Game claims name the game, the flags (`--gpu --phone`), the run length and the result.
     - Expected test values come from the spec or are derived by hand, not copied from emulator output.
     - A unit-test-only PR doesn't meet a "game reaches X" done-when.
   - **Honesty:** new constant-return functions are labelled with supportDef/supportMethod. Nothing
     claims "done" beyond its evidence.
   - **Hot paths:** no new work on per-instruction, per-message or per-clock-read paths when the
     feature is off.
   - **Safety:** no game files, tokens or network-default changes.
3. Decide:
   - **Merge** (`/merge-pr`) if sound.
   - **Request changes**: comment with numbered, specific asks.
   - **Fix it yourself** if it's small and blocking (push to a new branch and open a replacement PR).
4. Post the outcome on the card issue, so the agent sees it.
