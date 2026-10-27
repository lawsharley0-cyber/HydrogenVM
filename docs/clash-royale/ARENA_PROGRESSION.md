# Arena progression continuation — October 27, 2026

After `5040611`, branch `codex/clash-offline-continuation`, draft PR #138.
This stage adds **Arena 2 / Bone Pit**, not all-arena completion or advanced AI.
See [STORE_PURCHASES.md](STORE_PURCHASES.md) for the preceding shop stage and
[CLAN_EVENTS_PLAN.md](CLAN_EVENTS_PLAN.md) for the owner's new social-event request.

## Source and implementation

Pinned `arenas.csv` has Training Camp plus six arenas. Arena 2 promotes at
400 trophies and demotes below 350; its match cost is 2 Gold and its native
PvpLocation is `PvP_bone`. Later promotion thresholds are 800/1000/1200/1400.
No modern arena thresholds or online service are substituted.

The old guest explicitly aborted every paid match outside Arena 1. The build
now emits 36 source-derived sector templates: existing Arena-1 variants 0–17
retain their ordering, and Arena-2 variants 18–35 use NPC missions
18000010/11/12 (450/600/700 trophies, original King 3) and the original Bone Pit
location/assets. Each has six eight-card deck variants. Six Arena-2 decks add
source-validated Balloon, Minions, Skeletons, Tombstone and Bomb Tower options;
each card must unlock by Arena 2 and fit the existing bounded reserve policy.
The seven-elixir Giant Skeleton is not included in these particular authored
decks. This is basic arena content coverage, not a promise to use every card or
an AI/balance upgrade. Current common level 2 / rare-epic level 1 policy remains.

Opponent choice filters by the avatar's **actual native arena before** measuring
trophy distance. Without that filter, a promoted/demoted avatar could select
the neighboring arena's template. Last name/deck/style nonrepetition survives
cross-arena transitions. Existing GBO1 format and Arena-1 indices remain stable;
the generated bound expands from 18 to 36. Historical generated names
`arena1_sectors`, `arena1_missions`, etc. now contain the combined pool.
Native entry payment, original battle simulation, crowns, trophy updates,
winner/loss setters and reward handling remain authoritative. AI regeneration,
reserve styles, decision logic and all seven original trainers are unchanged.

## Verification and private fixtures

Use `python3 -m unittest discover -s tools/tests -p '*clash_guest*.py' -v`:
38 focused tests (builder plus guest modules). New coverage alternates 2000
promotions/demotions with overlapping trophy anchors, checks arena filtering
and name/deck/style nonrepetition, and rejects an empty arena without mutating
history. Authored CSV fixtures check both deck pools and reject an Arena-3
card in an Arena-2 deck. Existing all-seven original-training fallback checks
remain part of this suite.

The private C verifier and independent C++ fixture exporter match all **36 named
and seeded arena sectors plus seven original trainer sectors** byte for byte
for the selected earned deck / King-2 analysis fixture. As before, the oracle's
Card model lacks two UI flags, so only the disposable analysis copy clears them;
actual saves retain their flags. This is 43 comparisons, not a newly repeated
all-level/all-deck exhaustive matrix.

Actual native client tests use the qualified unmodified host
`/var/tmp/hvm-clan-core-work/build/hvm` (source `912a87e`, shared src tree
`9b29155c175d76eda08111072700403272a19d7e`). Reports and saves are private under
`/var/tmp/hvm-clash-arena2-qa` and `/var/tmp/hvm-clash-arena2-final-checks`.
`outputs/arena2_qa.py`, `arena2_final.py` and `arena2_oracle.py` record procedures.
Start from an isolated copy of the prior earned daily-shop save, preserving
the original earned source and the owner's phone-failing save. The **400,
399, 350 and 349 trophy values are explicitly staged component fixtures**.
They are not earned wins, gifts to the player, or an end-to-end earned climb.

- Original refresh on login: Arena 1 remains at 399; promotes at 400; an
  existing Arena 2 remains there at 350 and demotes at 349. Native code performs
  the transition; the fixture changes only trophy input and valid checksums.
- Actual unassisted Arena-2 battle: Azul_9, variant 20, Witch/Skeleton deck,
  mission 18000010, style 0, seed 428685857. Bone Pit scene, native King 3,
  deployments, three-crown defeat and original result/OK are observed.
  Entry Gold 75→73; trophies 400→390; NPC losses 8→9; wins remain 9; no new
  chest, Gems or card grants. Relaunch retains that native outcome without
  another entry payment. The initial v1 and final v2 candidates run separately.
- Demotion from a checkpoint containing an Arena-2 GBO variant must load the
  expanded bound, refresh the avatar and select an Arena-1 variant with its
  original one-Gold entry. This exercises an actual saved cross-arena history,
  not only the pure choice test. The first automation attempt instead opened
  Goblin details on the native "Your New Arena" panel and made no entry debit;
  the corrected probe closes that panel before pressing Battle. Do not count
  the first captured attempt as matchmaking success.
- Final candidate fresh training and existing local-clan return checks preserve
  training shop exclusion, clan identity, resources and cards. A cold concurrent
  training capture at 18 seconds still showed 82% loading; use the later
  `training-long` capture (30-second extra settling) for visual assessment.
  File-field assertions alone do not prove the tutorial was visible.

UI probes use extracted app, exact isolated home, `--gpu --capture-audio`,
12-second minimum startup settling, then scenario-specific waits. Battle entry
is `(160,365)` and result OK `(160,500)` in logical 320x568 coordinates. Inspect
screenshots, log and decoded retained native fields together. A captured frame,
nonzero mixer samples, or a green workflow does not establish full game/device
playability, audio quality or stutter-free performance.

Final v2 IPA SHA256:
`9fd309fa629de452a97e360ffbddb97730096b76fdbcedf134f53c8767febacb`;
executable:
`72fd77970d9588097e3be40b06a37c9e397e49e0317bd247b01cd5b5cea13817`.
No assets, original executable, checkpoint or generated proprietary fixture is
committed. Once a save has an Arena-2 variant, older Arena-1-only guest builds
cannot read that expanded variant and must not be used to downgrade that save.

## Remaining work

Arena 3–6 entry still stops explicitly pending their qualified content port.
Earned promotion/wins/reward chests across the boundary, all-level/all-card
coverage, draws, partial-crown losses and profile counters still need targeted
qualification. Native draw skips the win/loss setter checkpoint path; do not
claim partial-crown draw persistence from three-crown win/zero-crown loss tests.
Continue these core systems, then the saved multi-bot clan-event lifecycle and
full UI/save/device/visual/audio/performance queue. Keep the owner's advanced
battle-AI deferral and complete original training preservation in effect.
