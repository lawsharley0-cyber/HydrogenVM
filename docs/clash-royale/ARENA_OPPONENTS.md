# Varied offline Arena 1 opponents

October 27, 2026, continuation after `b638b88` on
`codex/clash-offline-continuation` / draft PR #138. The guest now chooses an
authored handle, legal deck, native reserve profile and battle seed for each
paid Arena 1 match. The seven original trainers keep their original identities.
This is an offline policy implemented around the pinned client's original AI,
not a claim to reproduce the retired multiplayer matchmaking service.

## Decks, identities and native decisions

`tools/clash_bot_policy.py` defines six eight-card decks: Giant support,
Prince pressure, Skeleton Army counter, Goblin Hut control, Witch push and
Goblin Barrel bait. IDs, unlock arenas, rarity, mana costs and player mana
rates are read and checked from the supplied pinned 2.1306.1 IPA at build time.
All cards must unlock by Arena 1, be unique within a deck and cost at most six
elixir so the most patient profile can still afford them. Common cards use
level 2, rare/epic level 1. No later-era cards or downloaded assets are added.

The existing native NPC missions 18000007..18000009 provide King 2, trophy
anchors 50/150/300 and original arena/location data. Six deck variants per
mission produce 18 private build-time sectors. Choice stays within the nearest
trophy distance plus 100 and excludes the last deck, handle and reserve style.
64 short ASCII handles are authored in `opponent_names.inc`; none are scraped
accounts or purported real online opponents. Training and the separate local
clan actor are not renamed.

The original `LogicSummoner` still builds threat/placement heatmaps, chooses
cards and legal deployment positions, pays card costs and handles urgent
defense. Narrow guest hooks replace only the selected paid NPC's mana getters
and reserve value. Full-pool regeneration uses the pinned player's globals:
28,000 ms for ten elixir, 14,000 ms in the end/overtime phases (2.8/1.4 seconds
per elixir). It is not 28 seconds per elixir. Other NPCs retain their original
CSV rates; training and friendly modes clear the paid-opponent flag.

Reserve profiles are 0, 2 and 4 elixir. The native caller's first 14 seconds,
king below 75% health and last 61 seconds rules remain intact. An exploratory
2/5/8 candidate exposed why these must be bounded: reserve is retained after
spending, and the eight-elixir profile stayed idle with its opening hand. That
candidate is not the qualified build. These three profiles vary native spending
behavior; they are not learned personalities or a replacement combat engine.

The pinned client bypasses its serialized opponent name in two UI paths:
`CombatHUD::initBattleHUD` at call `0xc867c` resolves the CSV trainer label, and
`BattleEndHUD::setResult` at `0x42968` calls the avatar name setter after another
trainer lookup. The former call now resolves the actual selected NPC avatar
String; the latter setter retains that chosen name only for active paid NPC
account `(-1,0)`. Human `(0,1)` and clan `(3,1)` names use the original assignment.
The native avatar getter itself remains unchanged. Result and battle names
therefore agree without returning a temporary String or painting a UI-only alias.

Mana getters `0x100bfc/0x100c10/0x100c24` have guarded eight-byte replacements.
Reserve getter `0x100c38`, name setter `0x99e48` and the HUD call use guarded
four-byte Thumb B.W/BL patches. Neighboring getters/instructions are checked
unchanged. Native assets and shared `src/` are unchanged.

## Persistence

GBO1 is ten little-endian u32 words / 40 bytes: tag `0x314f4247`, version 1,
nonzero xorshift32 state, match serial, variant index, handle index, reserve
style index, positive signed-31-bit seed, current native human-body FNV,
header FNV over the preceding 36 bytes. Bounds are serial 1..1 billion,
variant 0..17, handle 0..63 and style 0..2. Duplicate, truncated, invalid or
cross-bound records reject before migration/writes.

The selection is prepared with the complete sector and saved atomically with
the original native Gold entry debit before delivering 21903. A cancelled or
interrupted match retains its paid entry. No rewards, XP, trophies, cards or
wallet expectations are stored in this metadata. Native victory/defeat and
reward paths remain responsible for progress. Ordinary relaunch advances the
saved generator and excludes the preceding selection.

Legacy files without GBO1 remain loadable; their first paid match seeds from
time and the native body hash. Unlike economic clan records, GBO1 has no parent
required-child flag: whole-record removal is indistinguishable from a legacy
file and restarts variety history. It is not an anti-tamper mechanism. Older
guests reject the new unknown extension; do not downgrade the same save.

## Validation and reproduction

```sh
python3 -m unittest discover -s tools/tests -p 'test_*clash_guest*.py' -v
python3 tools/build_clash_guest.py /var/tmp/clash-client.ipa \
  /var/tmp/hvm-clash-opponent-v5.ipa --build-dir /var/tmp/hvm-clash-opponent-v5-build
python3 -m zipfile -e /var/tmp/hvm-clash-opponent-v5.ipa /var/tmp/hvm-clash-opponent-v5-run
```

33 focused guest tests pass: source unlock/cost validation; 2,000 successive
choices with trophy-band, nonrepeat and six-deck coverage; independent published
xorshift vectors; exact GBO1 wire/checksum and field/truncation boundaries;
native name identity/training fallbacks; LLVM B.W/BL encoding vectors; ARMv7
freestanding/import checks; existing save/clan/request/friendly/clock tests.

`sector_verify.c SAVE VARIANT EXPECTED OUTPUT NAME SEED` now checks named
arena sectors. The build's independent C++ `fixture-export BUNDLE SAVE VARIANT
SEED NAME OUTPUT` emits the expectation. All 18 variants and seven original
trainer sectors compare exactly for a private analysis-only King 2/eight-card
fixture, using short/long names and seeds 1/INT_MAX/872109914. The C++ Card model
omits two native card UI flags: an original earned-file comparison differed in
one such retained flag. Only the explicitly labelled analysis fixture clears
those flags for the oracle; it is never launched in the emulator. Actual UI
runs use the untouched earned save and retain its native card bytes.

Local private evidence lives in `/var/tmp/hvm-clash-opponent-qa`:

- `battle5`: final v5, RookAndRoll, barrel deck 5, reserve style 1/value 2,
  seed 861229045. Actual native deployments, three-crown passive-player defeat,
  correct winner name, OK/home and clean close. Gold 143→142, trophies 62→61,
  NPC losses 5→6; earned wins stay 9. No player cards are deployed by this plan.
- `battle6`: next process on the same save, NOVA, Prince deck 1, style 0/value 0,
  seed 1277081170. Actual native defeat and return: Gold 142→141, trophies
  61→60, losses 6→7. The changed name/deck/style and GBO serial 5→6 survive
  process restart. These are actual losses, not synthetic victory evidence.
- `battle7`: next real selection, RazorLeaf, Giant deck 0, patient style 2/value
  4, seed 2110810748. Native Giant/Knight/Goblin/Archer/Musketeer deployments
  and three-crown defeat complete; correct result name and OK/home. Gold
  141→140, trophies 60→59, losses 7→8, GBO serial 7. All three final reserve
  profiles therefore have actual deployment/result evidence, without changing
  a save to force a profile. XP 8, King 2, gems 87 and earned wins 9 remain.
  `earned-return` then opens that exact sandbox again, retaining Gold 140,
  trophies 59, losses 8 and the same serial/selection, with no extra entry debit.
- `fresh-training`: new sandbox shows original Trainer Earl and the native
  welcome tutorial. `clan-return`: copied prior community save loads the actual
  clan conversation, Local Donor identity and requests. This is a return smoke,
  not a repeat qualification of every clan button.
- `bad-gbo-hash` / `bad-gbo-bound`: actual client rejects malformed checkpoints,
  retaining each entire original file unchanged, including a repaired header
  checksum with an out-of-range name. No assertion or bounds check is disabled.

UI probe: immutable host `/var/tmp/hvm-clan-core-work/build/hvm`, extracted app,
exact copied `--home`, `--gpu --capture-audio --startup-seconds 12`; paid entry
tap `(160,365)`, wait for native combat/result, OK `(160,500)`, then independent
relaunch on that same sandbox. Inspect screenshots and decoded saved fields;
`status=captured` alone is not a pass. `outputs/opponent_qa.py`,
`opponent_smoke.py`, `opponent_guards.py` and `opponent_oracle.py` in the local
task directory record exact private procedures. The original earned source
`/var/tmp/hvm-clash-ui-qa-player/com.hydrogenvm.clashroyale.guestdev` remains
hash-unchanged. Preserve that source and the original phone-failing save.

Candidate IPA SHA256:
`09d9b716211ac8be62fb3069eddc4e9b6dc77053eeb056ed99e925fd1a40e850`;
guest executable:
`d850ffabac4185c371252b248ac0fc0d81d462bf79ea05a18659a86c71966aae`.
The final added cost guard does not change the generated policy header or
runtime bytes. Private binaries, original assets, saves and captures stay out of Git.

Shared source remains tree `9b29155c175d76eda08111072700403272a19d7e`;
the previous fulltest 260/260, TestLab 18/18 and regression report remain the
host qualification, not fresh results from this guest change. Linux 4 KB does
not qualify iPhone 16 KB behavior. Captured nonzero mixed audio and loaded
music/SFX do not prove absence of stutter/dropouts or correct audio quality.

## References and remaining work

The primary 2016 behavior reference is the pinned client itself: original
`LogicSummoner::updateAI`, heatmaps, reserve decision and CSV data, inspected
without suppressing its assertions. Public official server bot source was not
established. Do not label a community implementation as official Supercell code.

Comparison with primary project documentation:

- [FirstLight_CR](https://github.com/Jaasssoooonnnnn/FirstLight_CR) uses a native
  offline simulation, structured observations, learned policies and opponent
  pools, including deck specialists. Its target is a modern arm64 Null's Royale
  client and model services; it is not a drop-in dependency for this archived
  ARMv7, guest-owned offline IPA. The useful ideas here are varied pools,
  deck-aware evaluation and reproducible combat observations.
- [clash-jev](https://github.com/bytelabs-oss/clash-jev) separates strategy,
  card choice and legal-square placement with hold/push/defend/counterpush
  decisions. Its screenshot/Android/remote-request approach would add external
  dependencies. Here, bounded spending profiles use the existing native AI
  instead; no code or models from either project were imported.

Next: daily shop fidelity and rollover/purchases with native accounting;
later arena-specific sectors/card pools/towers/progression; draw/results and
remaining complete UI interactions; broad device, save/relaunch, visual, audio
and performance qualification. The owner's princess facing, cannon anchoring,
health-bar, landscape, centering, intermittent audio and stutter reports remain
open unless separately demonstrated fixed. This stage does not establish full
offline gameplay across all arenas or balance comparable to human opponents.

Next shop inspection anchors, not implemented behavior: original
ShopSeedChangedCommand217 (`getCommandType`0x34d08, execute0x34d74) passes seed,
remaining seconds and weekday 1..7 to `LogicClientHome::setShopValues`0x1269a8.
It rerandomizes only if needed, stores seed/day at +0xc0/+0xc4 and starts the
native +0xcc timer using the game-mode timestamp. `randomizeShopItems`0x1265d0
selects common/rare/epic cards through `getRandomSpell`0x12651c and original
eligibility checks, not arbitrary modern shop items. Weekday1 changes its
per-rarity counts to two. The exact ordinary counts, weekday mapping, prices,
purchase limits and durable rollover still need inspection and UI tests;
do not infer historical Sunday behavior from modern documentation.
