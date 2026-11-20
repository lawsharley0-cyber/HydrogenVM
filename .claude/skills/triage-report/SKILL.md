---
name: triage-report
description: Triage a HydrogenVM one-tap phone report issue ([signature] bundle.id: …) — find the stop, fix missing APIs honestly with a fulltest check, or route game-file work to Gemini. Use for new report issues.
argument-hint: <issue number>
---

1. Read the issue and its comments. The report JSON is in a ```json block: appVersion, backtrace,
   logs, phase, and a session.hvmrec recording when present.
2. Classify the report:
   - **Missing function or method:** find where neighbours are registered
     (`grep -rn '"_<neighbour>"' src/runtime`). Implement it honestly, or label it with supportDef as
     Approx/Unavailable and give a reason. Add a fulltest check whose expected values come from the
     API's documented behaviour.
   - **Crash / bad memory access:** get the same game version. Reproduce with `/bench-game`; use
     `--replay <recording>` if one is attached. If the game isn't available here, write a card for
     Gemini (it has the files) with the exact repro steps.
   - **Stall / watchdog:** check which thread waits on what in the logs (the `Stall:` lines). Relate
     it to run-loop or lock code.
   - **Known or blocked** (TestFlight asserts, dead servers): link the existing tracking issue, comment
     and close.
3. After a fix, re-run the game to find its next stop; games often need several in a row. Batch
   related fixes into one PR, listing each report.
4. Comment on the issue when the fix merges, then close it. Leave it open if only a phone test can
   confirm it, and say so.
