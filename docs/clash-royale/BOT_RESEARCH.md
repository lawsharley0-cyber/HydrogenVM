# Opponent and simulation references — October 26, 2026

This is research for the owner's stage after local clans, not an implemented
policy or a claim all arenas work. The standalone guest must retain the pinned
2.1306.1 combat, original training names, arena card unlocks and real outcomes.
No runtime Python service, Android VM, Java runtime or desktop trainer may
become a requirement of the shipped ARMv7 guest.

## Pinned original client findings

Executable pin: `3519d7954cb3dc16b9d7746cc5dbd2521d0209eea2bbc163f6ab7cdfa5e52397`.
Addresses below omit Thumb bits. These are inspected original native methods,
not published Supercell source code; no such public source is verified here.

- `LogicSummoner::updateAI` 0x10dcbc checks AI-enabled +0xfc, reads the native
  game tick and initializes its event/heat-map path. It calls `onEventAI(0)`
  after tick 88 when its initial-event flag is unset. `onEventAI` is 0x10e42c;
  further decision/placement mapping still needs inspection and real traces.
- `LogicSpellAIBlackboard::setState` 0x98918 stores a state and converts its
  duration to 50-ms ticks. `tick` 0x98938 decrements/clears it. `shouldDefend`
  0x9894c accepts states 1/3; `shouldAttack` 0x9895c accepts state 2. These are
  concrete native decision-state anchors for testing style differences.
- `LogicProgressSimulator` 0x8e064 has battles/day, donations/day, sleep and
  team flags. Its request routine 0x8f588 directly adds selected card material
  and starts a request cooldown; `getNextBattleResult` 0x90190 consults a
  prescribed outcome schedule. It is an economy/progression estimator, not
  evidence a real NPC fight or donor transfer happened. Do not enable it for
  the player's battles or use it to manufacture clan rewards. Native donation
  capacity/cooldown getters remain useful evidence independently.
- The actual original Arena-1 request picker reports **10 common or 1 rare**.
  `LogicRequestSpellsCommand` **524** uses base metadata, message string and spell
  list; original execute 0x20654 validates membership/tutorial/cooldown and
  eligibility, then starts/saves the request. This finding does not implement
  a donor or qualify a received card. The actual v46 card-selection trace
  rejects 524. Its type getter 0x205fc returns 0x20c (524); 528 belongs to
  LogicBuyChestCommand at 0x5ab74. This corrects the initial research mapping.

Current Arena 1 selection chooses the nearest trophy template among thirteen
bundled non-training NPCs. It does not yet deliver the requested deck/style
variation. Changing a name or seed alone must not be reported as a new style.

## Primary project comparison

These are the authors' documented architectures, not simulations trained or
independently benchmarked by this project. No third-party implementation or
model has been copied into HydrogenVM.

| Reference and inspected commit | Approach | Useful here / boundary |
| --- | --- | --- |
| [crforge](https://github.com/voonhous/crforge/blob/4b235e52bb58769a96e5f3ebe77725f2144bb87f/README.md), `4b235e52bb58`, Apache-2.0 | Deterministic Java battle core, modern game tables and recorded-tick conformance. Its [environment](https://github.com/voonhous/crforge/blob/4b235e52bb58769a96e5f3ebe77725f2144bb87f/docs/environment.md) describes player command submission, legality masks and per-seat observations. | Borrow the validation approach: reproducible inputs, first divergence and precise unsupported behavior. Its modern rule engine/JVM is not a replacement for this old native client. |
| [FirstLight CR](https://github.com/Jaasssoooonnnnn/FirstLight_CR/blob/3ade158c52bf33de3ffcea600ce380946b7de0d8/README.md), `3ade158c52bf`, Apache-2.0 | [Native battle execution](https://github.com/Jaasssoooonnnnn/FirstLight_CR/blob/3ade158c52bf33de3ffcea600ce380946b7de0d8/docs/architecture.md), replay imitation, recurrent policy, PPO self-play/opponent pools and an observation/action contract. Supports a specified modern ARM64 Android engine. | Strong architectural match for retaining original combat while separating decisions. Its offsets, card catalog, weights and external Python/VM stack do not apply to ARMv7 2.1306.1. |
| [ClashAI](https://github.com/vegetableleaf/ClashAI/blob/9bb370cd60f60ddc0f6116b6f6f05f90ef928e9c/README.md), `9bb370cd60f6`, no repository license verified | Native replay reconstruction plus imitation/PPO and a visible-information contract. Reports paired and reactive evaluations, deployment/latency weaknesses and placement collapse. | Evaluate observable deployment behavior, not labels or training loss. Keep modern/specialist card assumptions out; no code reuse without a verified license. Its reported strengths are not our measured results. |
| [Hasty-CR](https://github.com/hastylmao/Hasty-CR/blob/ac4ef71194167e97f34550fc91c026b6d707a0cd/README.md), `ac4ef7119416`, MIT | Integer 50-ms simulator, scripted/rule/learned opponents, masked legal actions and mixed opponent evaluation. Documents overfitting to one opponent and engine exploits. | Use different reactive opponents, fixed held-out seeds and exploit checks. Its modern Hog-cycle model and reimplemented physics cannot establish correctness for this early client. |

## Proposed qualification sequence

1. Finish native local clan interactions first. The original estimator above
   cannot stand in for actual request fulfillment or a played friendly battle.
2. Add authored handles only to non-training opponent avatars. Use a durable
   per-match seed/sequence with adjacent duplicate avoidance; preserve the
   original seven training identities. Do not harvest private player names.
3. Export card-unlock/rarity/data-reference facts from the pinned CSVs. Require
   each selected eight-card deck to be unique, arena-legal and complete. Keep
   native levels/elixir/placement/cycle checks and original card statistics.
4. Trace native event/blackboard/command behavior before selecting profiles.
   Measure defensive response delay, lane/placement choices, elixir at play,
   spell/attack mix and repeat-card patterns across identical player inputs.
   A differing label or deck is insufficient evidence a style changed.
5. Prefer bounded original AI/profile variation initially. A learned policy
   would require a separate portability, memory, latency and fairness study;
   modern checkpoints cannot simply be embedded into this guest.
6. Run real wins, defeats and draws with native results, paid entry, correct
   crowns/chests/counters and persistent relaunches. Record the seed/deck/profile
   before starting the match so an interrupted attempt does not reuse a paid
   action or silently select another outcome.

Daily-shop historical research and implementation remain a later stage. Modern
shop documentation contains cards/items absent from this build and is not its
economic specification; pinned shop randomizer/price/timer code must be read.
