# Native draw checkpoint continuation — November 6, 2026

After all-six-arena commit `3cb45ef`, on `codex/clash-offline-continuation`,
draft PR #366. This is a scoped persistence fix; full earned progression,
nonzero-crown draw/partial-loss qualification and the complete bug audit remain
open. [PLAYABILITY_2026-11-06.md](PLAYABILITY_2026-11-06.md) records arena content.

## Behavior and native evidence

Original `LogicBattle::battleEnded` uses winner field +0x74, with -1 for a draw.
It skips both native NPC win/loss setters on a draw. Those setters were the
guest's only completed-match save path, so draws had no completion checkpoint.
The guest now handles draws after original battle completion, before the
outer LogicGameMode listener can show the result. It requires the native
completed flag (+0xb4), no winner/loser (`0x14088/0x140b4`), and exactly one
actual battle avatar with local account (0,1). No new score/reward formula is
introduced. `arena_draw.inc` serializes the native battle avatar, decodes a
temporary checkpoint, transfers actual summoner crowns through the existing
native star-count path, refreshes the native arena and atomically commits.
It never calls the winner-chest or NPC win/loss setter paths. Repeated direct
paid-battle callbacks are rejected before replaying native accounting; the
pointer guard resets at each new paid match. Original training and friendly
completion paths retain their existing handling.

An important failed candidate exposed why `GameMode::getClientAvatar` (`0x93c74`)
must not supply this checkpoint. It returns the retained home-screen avatar,
whose Gold predates the guest's paid sector. The first diagnostic native draw
therefore would have restored Gold 75 instead of retaining 74; its explicit
entry-cost assertion failed and that candidate was **not pushed or offered**.
The correction resolves the two battle avatars through original
`LogicGameMode::getAvatar` (`0xd7bd8`), selects by full local account identity,
and rejects missing/duplicate owners. The corrected native test retains Gold 74.

## Qualification and limits

`python3 -m unittest discover -s tools/tests -p '*clash_guest*.py' -v` passes
**41 focused builder/guest tests**. The new native-call-boundary test checks
original draw state, battle-avatar resolution, temporary-copy crown routing,
commit order, missing/duplicate owner rejection and missing decoded state.
Calling the stale home-avatar getter is outside the allowed test API and fails.
Prior 78-profile/seven-trainer and first-36 legacy comparisons remain unchanged;
no policy, sector data or shared runtime code changed in this draw stage.

Private tests and reports are under `/var/tmp/hvm-clash-playability-20261106`.
The test host remains the previously qualified
`/var/tmp/hvm-clan-core-work/build/hvm`; shared src tree stays
`9b29155c175d76eda08111072700403272a19d7e`. These guest-only changes do not
establish a new host, iPhone/16 KB page, audio-quality or performance pass.

- The ordinary native exit control opens **Return to the main menu?** It is
  not a draw result; `draw-x` is an exit-dialog observation only.
- The first high-level component trial initially stayed on original sequential
  level-up panels. The private controller closed those panels through normal
  touches and requested a normal stop. That interrupted probe is not counted
  as a passing draw. Its resumed native match actually lost despite staged
  King 12; `draw-resume` proves a defeat, not a draw. No original owner/earned
  save received those staged levels or any grant.
- For deterministic **native 0–0 draw** qualification, a separate diagnostic
  source copy calls original `LogicGameMode::battleEnded` (`0xd7558`) after
  tick 20. This computes the result from the actual native battle objects and
  invokes the ordinary result/checkpoint path. It does not set the winner,
  fabricate crowns/rewards, disable assertions or change assets. It shortens
  that diagnostic battle; it is **not** a naturally timed or production-game
  draw. The original completion API's own already-ended guard handles subsequent
  calls. The diagnostic source, binary and timing change are private test-only
  artifacts and are absent from the production candidate and Git.
- The corrected diagnostic shows the original DRAW result and commits once:
  Gold/FreeGold 75→74 for the original one-Gold entry; trophies 59, Gems 87,
  XP 8, NPC wins 9/losses 8 and chest-cycle resource remain unchanged. Two
  independent relaunches with the **uninstrumented production candidate**
  retain the exact native resource/counter/history state without another fee,
  reward or replay. The 0-crown fixture does not qualify a 1–1/2–2 crown delta.
- Ordinary Arena-6 defeat and fresh original Trainer Earl regressions run on
  the production candidate. The earlier candidate's ordinary Arena-6 defeat
  and staged-level native defeat also take the original loss path successfully.
  These are not earned later-arena victory/reward or partial-crown evidence.

Private helpers in the current task's `outputs/`: `native_inspect.py`,
`draw_qa.py`, `close_draw_panels.py`, `build_native_draw_probe_v2.py`,
`native_draw_check_v2.py`, `production_draw_smoke.py`. The native diagnostic
source is `native-end-source-v2`; corrected reports are in `draw-v2-validation`.
Keep the rejected v1 probe/evidence separate. Production output is
`/var/tmp/hvm-clash-draw-v2.ipa` (normal battle timing), SHA256:
`81aacd6072f758d4efc9b7101b656cb0d51977f4d00adf9744aaebc8d516a254`;
executable:
`2f03454fa780bb7a4a0abd30640c5dbb3a6148e407caf1c73eaa0e5a2564aa2b`.
Do not distribute the separate diagnostic probe as the playable candidate.

## Next qualification

Reproduce original 1–1/2–2 draws and one/two-crown defeats, assert native
summoner-to-pending-to-home crown transfer exactly once, and test interruption
before/after the completion checkpoint. Qualify earned later-arena wins,
promotion rewards/chests and profile counters. Continue persisted clan events,
full button/save-lifecycle/device/visual/audio/performance audits afterward.
All seven original trainers remain unchanged. Advanced opponent-AI refinements
and the owner's optional modding phase remain deferred until baseline features
and bug fixes are ready. Do not describe this draw checkpoint as a complete
offline-game qualification.
