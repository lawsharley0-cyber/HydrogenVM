# Card behavior and custom images — November 9, 2026

Owner scope: deployment quantity, Witch/hut and Goblin Barrel summon quantity
and type, projectile selection, broader useful card controls, and custom card
icons/troop appearances from images. This continues on `codex/clash-forge-runtime`
from `35c7d7e` in draft PR #391. The shared runtime remains main `588eebb`;
no `src/` or phone-shell source changes are required for these guest features.

## Behavior controls

Settings → Mod Lab → **Spawns** opens a separate card picker and Mechanics
ON/OFF toggle. Pickers include all 33 original troop/building cards (including
cards that share a unit), original Goblin Barrel, and loaded custom definitions.
The eight existing percentage controls remain under Tuning. Their saved profile
is unchanged. The old mislabeled Deploy speed is now **Windup speed**: it still
scales native LoadTime at `0x1bc`. A new **Deploy delay ms** changes real
DeployTime at `0x1c0` without silently reinterpreting old profiles.

Nine new fields:

| Control | Override | Applicable native data |
|---|---|---|
| Troops / card | 1–16 | SummonCharacter cards, individually |
| Summon count | 1–8 | Existing Witch/hut-style factories |
| Summon troop | Named original non-factory troops | Existing factories |
| Projectile | Named non-spawning projectiles | Units with a native primary projectile |
| Impact troops | 1–8 | Existing projectile summons, including Barrel |
| Impact troop | Named original non-factory troops | Existing projectile summons |
| Spawn pause ms | 500–60000 | Existing factories |
| Deploy delay ms | 100–10000 | Troop/building deployment |
| Elixir cost | 1–10 | All cards in this picker |

Default is encoded as zero and restores the original value; it does not mean
zero troops or free cards. Unsupported controls show N/A. Existing native
factory projectiles/recursive spawn targets are not offered as replacements.
Replacement summons use native level index 0 (Level 1), avoiding an inherited
Skeleton level index against unrelated data. Spell-only effects and independent
weapon/projectile visual sizing are not new controls in this release.

These are native shared references. A unit/projectile shared by original cards
shares relevant edits; per-card deployed count and elixir cost use separate
spell data. Custom blueprints retain independent cloned unit/primary-projectile
references. Projectile substitution uses the selected projectile's native
behavior, not just its picture. Existing shot-speed/damage tuning follows the
active replacement; selected projectile speeds restore from captured originals
before every Apply to prevent compounding or leaking after disable.

Apply spawns validates all drafts, obtains the normal settled home, captures
its checkpoint and retains the first `.before-mods` backup. It commits its
optional file through write/flush/fsync/rename. A failed file commit rolls back
the in-memory configuration. Reset spawns stages defaults/OFF and needs Apply.
Main Mods and Mechanics toggles are independent. Both preserve original training
and friendly statistics: factories/projectiles/count/cost references restore
before friendly entry; apply is gated until all seven original trainers are
complete. Original trainer names, decks and AI remain unchanged.

## Native fields and persistence

The cache offsets were verified against this pinned executable's native getters:
spell SummonNumber `0x7c`, ManaCost `0x74`, SummonCharacter `0x50`, spell
Projectile `0x5c`; unit SpawnCharacter `0xb4`, spawn level `0xbc`, SpawnNumber
`0x1ac`, SpawnPauseTime `0x1b0`, DeployTime `0x1c0`, primary/alternate projectile
`0x64/0x68`; projectile SpawnCharacter `0x50`, count `0x58`, level `0x88`.
Never infer a field's representation just from the CSV integer types; the
previous [size abort](MOD_SIZE_ABORT_2026-11-08.md) remains regression-covered.

`Documents/ClashMechanics-v1.bin` is MCH1. Its seven uint32 header words are
magic `0x3148434d`, version 1, record count, enabled, selected index, body FNV
and header FNV. Each record is its native card ID followed by nine overrides.
IDs must match the generated original-card catalog plus the loaded custom
prefix. Old valid shorter profiles default appended custom entries. Pointers
are resolved anew and never stored. Keep catalog choice ordering and the nine
field indices stable; a changed meaning needs a versioned profile migration. Invalid extents/checksums/IDs/references or
unsupported overrides disable mechanics while retaining the file. Explicit
Reset + Apply archives a rejected file once as `.rejected`; an existing archive
is never overwritten. This is independent of MOD1 percentages and mandatory
FGR1/FGC1 game-save links.

## Image support

[Custom images](CUSTOM_IMAGES.md) documents the PNG art-pack workflow and ART1
format. Icons render in native cards; a static image can replace a custom
unit's body. The pack is tied to its exact FGR1 prefix and pinned atlas, with
original assets untouched. This first version packs images on the computer;
there is no native phone image chooser or synthesized walking/attack animation.
The phone runs the resulting guest/art files without a desktop process.

## Verification and retained evidence

**67 focused guest tests pass**, including all four freestanding ARMv7 compiler
variants with no new unresolved imports, integer/float tuning, safe catalogs,
real native-field apply/restore and training/friendly exclusions, optional MCH1
extent/checksum/identity/reference rejection, static SC format/compression/alpha,
ART1 identity/size/pair checks and explicit source-versus-cache routing. Linux CI
installs Pillow; the Clash investigation now runs this focused suite and tracks
its guest/catalog/art paths. These are not an all-app or full-game audit.

```sh
python3 -m unittest discover -s tools/tests -p '*clash_guest*.py'
python3 tools/build_clash_guest.py /var/tmp/clash-client.ipa \
  /var/tmp/hvm-clash-art-v3.ipa --build-dir /var/tmp/hvm-clash-art-v3-build
```

Native runs use `/var/tmp/hvm-forge-main-baseline/build/hvm` (main `588eebb`),
ordinary GPU/touch/audio input through `tools/clash_guest_ui_probe.py`, and the
OwnHomeData readiness barrier. Every scenario uses a private copied home;
source earned checkpoints remain read-only. Existing 682 runtime/26 Test Lab
baseline results are reused, not relabeled as new runs.

- Original Knight quantity 2: native UI Apply stores the exact MCH1 record.
- Custom Knight quantity 2: normal UI Apply/deploy of an already Use-equipped card produces **two visible
  Knights from one card**. Natural native NPC defeat, return and a second
  process retain entry Gold 75→74, loss 8→9 and the profile. Evidence:
  `/var/tmp/hvm-mechanics-v2b-qa/quantity-battle/` and its relaunch.
- Witch: native UI selects count 4, Goblin replacement and FireballSpell
  projectile; the persisted row and a separate relaunch retain those exact
  choices, with progression unchanged. Barrel independently saves impact count
  4 and Goblin and reopens unchanged. Evidence: `witch/`, `barrel/` under that
  same QA root. These Apply/reload cases alone are not combat qualification.
- Corrected custom Witch combat: a staged append-only Witch definition is
  granted natively, configured in the UI for four Goblins per wave and the
  FireballSpell projectile, then equipped via Use **and an explicit deck-slot
  replacement tap**. Actual battle frames show the Witch, Goblin waves and
  Fireball explosions. A natural defeat/return and separate relaunch retain
  the new deck and progression (Gold 74→73, losses 9→10, trophies 58→57).
  Evidence: `/var/tmp/hvm-mechanics-witch-v3b-qa/`. The earlier v3 plan entered
  replacement mode without selecting a slot and deployed the old robot Knight;
  its green process result is explicitly not Witch combat evidence.
- PNG fixture: a distinctive magenta/green icon appears on the custom card,
  while original cards retain their artwork. A cyan/magenta robot replaces its
  body and moves in a real paid Arena 1 battle, reaching the original defeat
  and return. Evidence: `/var/tmp/hvm-custom-art-v2-qa/gallery/` and `battle/`.
- Restart initially failed: the broad SC override substituted compressed
  source data for a decompressed cache read; native error was an invalid
  MovieClip ID, then strict backtrace diagnostics stopped. The v3 path guard
  fixes the cause. A full copy of that exact failed `battle-home`, including
  its untouched cache and save, now reopens with the custom icon and Gold 74
  retained. No assertion or bounds check was suppressed; no cache was deleted.
- Native truncated-art-file and wrong-prefix-manifest tests boot with original
  artwork, preserve the bad files and registry bytes, and retain progression.
  Recovery/fallback evidence: `/var/tmp/hvm-custom-art-v3-guards/`.

The exact final package additionally opens a warm cached home with the custom
icon, active custom-card quantity 2, the Spawns panel and Images status intact.
Its bad-art Images button explicitly reports rejection while preserving the
original appearance/progress. Evidence: `/var/tmp/hvm-customization-final-qa/`;
the final text-only label cleanup repeats the valid-path smoke at
`/var/tmp/hvm-customization-final2-qa/`. All **26 existing root Clash protocol /
state checks** also pass. A byte comparison confirms **all 540 original res/
assets** remain unchanged in the delivered IPA.

Final private package: `/var/tmp/hvm-clash-customization-final2.ipa`.
Archive SHA-256:
`9520f45e37f909b0ef09bc35f4d991a0dce6a59c52821966a7addb22f4f3f41a`.
Executable SHA-256:
`c4f6b69a7f281a340a6d75c7c4c4eea1dccca28c4def83ffa429a4ad4a0e8068`.
Bundle ID remains `com.hydrogenvm.clashroyale.guestdev`; normal import replaces
that bundle while keeping its neighboring Documents/Library/tmp directories.
Back up the full guest Documents before a phone update; do not delete the
instance or definitions. The importer's source is `src/loader/AppBundle.cpp`.
Actual owner-device update behavior remains a separate confirmation.

Committed ordinary plans: `tools/clash_guest/qa/mechanics-witch.json`,
`mechanics-barrel.json` and `mechanics-custom-quantity-battle.json`. The first two
need separate post-training homes with no mechanics file; the last needs the
known sole custom Knight already equipped in slot zero. They are input scripts,
not screenshot or persistence oracles.

The private task-root helpers are `outputs/qa_mechanics*.py`,
`qa_witch_battle_v3.py`, `qa_custom_art.py`, `qa_art_guards_v3.py`. Keep their
IPAs, copied sandboxes, generated CSV/SC files, screenshots and audio outside
Git. The initial drag-on-button UI plan did not scroll; the successful ordinary
probe drags over the label area. An interrupted WSL run is retained separately
and is not a pass. Read the app report and profile values, then review actual
frames; a green capture plan alone is insufficient.

## Remaining work

Owner iPhone import/controls/art-pack verification remains outstanding. Finish
representative Barrel impact combat, max-count/many-blueprint stress, projectile
combination breadth, interruption/concurrent-writer scenarios and phone audio /
stutter qualification. Static body art disables the original animator; parity
of animation-driven footsteps/effects is not qualified by mixed-audio telemetry. Image imports currently require the computer packer;
phone photo/document picking, animated/multiple-facing sprite sheets and preview
editing are follow-ups. Continue the existing UI/visual/clan-event audit and
independent weapon/projectile sizing backlog. Advanced main-bot refinement and
modern-bot comparisons remain deferred; preserve all seven trainers.
