# Custom-card definition foundation — November 7, 2026

The owner requests an in-game card forge alongside the existing Mod Lab. This
stage adds a definition generator and native component evidence. **The shipped
guest does not yet have card-creation controls or a durable custom-card loader.**
Do not distribute the private probe IPA or load its saves in the normal guest:
those saves reference definitions which the normal guest cannot resolve.

## Authored implementation

`tools/clash_custom_cards.py` accepts a version-1 registry with up to 16
append-only records: `slot`, original `baseCardId`, and displayed `name`.
Only original troop/building cards (native data groups 26/27) are accepted.
Names are single-line, bounded UTF-8 text. Existing records cannot be deleted,
reordered, renamed or reassigned. Mutable tuning belongs in a separate profile.

`generate_overlays(bundle, registry, previous)` reads the privately supplied
pinned bundle and returns resource-relative CSV bytes and ID metadata. It does
not write the bundle or ship client data. Each changed CSV retains its original
bytes as an exact prefix. Appended spell rows reference independently appended
unit rows; primary projectiles are cloned when present. Original artwork and
other references are reused. Original level continuation rows remain intact.
Text IDs resolve the chosen name in all named locale columns. The new spell is
`NotInUse=true`, keeping it out of ordinary availability and loot selection.

Mixed troop/building appends retain earlier native IDs. The generator's
`requiredDefinitions` metadata identifies the immutable registry prefix with
count and SHA-256. `require_registry_identity` accepts a later appended registry
with that same prefix and rejects missing/changed definitions. This supports a
future save link without invalidating an old checkpoint when a new definition
is durable but not yet granted. **The production save loader does not enforce
this link yet.** SHA-256 here detects mismatches, not malicious modification.

Six authored-fixture tests cover cloned references/continuation rows, original
byte preservation, mixed-table stable IDs, schema/limits/missing references,
and saved-prefix acceptance/rejection. The complete focused guest suite passes
51 checks:

```sh
python3 -m unittest discover -s tools/tests -p '*clash_guest*.py' -v
```

## Native diagnostic evidence and limits

Private Linux runs use the original pinned 2.1306.1 IPA and the clean latest-main
keyboard candidate from shared host PR #381. A diagnostic guest `fopen` import
hook redirects only selected CSV reads to overlays in a copied sandbox's
Documents directory; all other I/O forwards to the original import. It does
not suppress assertions or patch the original CSV assets. The hook and automatic
grant are diagnostic-only and are deliberately absent from the production builder.

The generated Knight clone resolves as native card `26000023`, with a distinct
CharacterData pointer, original baseline health, `NotInUse=true`, and the name
**Forge Knight** through the real native StringTable. A separate earlier clone
with doubled health returned 1430 versus the unchanged original's 715 at the
tested level. The client creates cross-table references itself.

Native ownership uses the original collection/add/capture path. An already-owned
lookup prevents a second grant. Two launches reach home and exit cleanly, and
the native collection visibly shows the extra Forge Knight with Info/Use
controls. These controls' presence does not prove their complete behavior or a
custom-card battle. The original constructor-only probe displayed Level 2:
`LogicSpellC1` sets internal level index 1. Inspection of the original reward
factory (`LogicRewardRandomizer::createSpell`, Thumb `0x133749`) shows that it
creates index 0 and a native creation timestamp. A second private probe using that factory grants once, saves, relaunches and
visibly shows Forge Knight at Level 1. The initially loaded collection view
remains cached until relaunch, so creation must include a safe refresh/restart.
Use the native factory when implementing real grants; do not copy a C++ object
or guess initialization.

The corrected native probe's Info popup has also been opened and visually
reviewed: **Level 1 Forge Knight**, Common/Troop, 650 HP, 75 damage, 1.1-second
hit interval and inherited original Knight artwork/description. Use/deck/battle
qualification is a separate next check. A screenshot of a selected card with
an Info button alone must not be reported as opening its Info popup.

The first strict accounting assertion deliberately failed: adding a card changes
Cards Found (`5000008`) from 10 to 11. A subsequent relaunch also updates the
three native findcard achievement counters (`60000007`–`60000009`) from 10 to
11, consistent with the pinned achievements table's `findcard` actions. An
additional relaunch/collection visit leaves all those values stable. Gold/free
Gold (75), Gems (87), XP (8), King level (2), wins/losses (9/8), trophies (59),
other inspected resources and original card material counters remain unchanged.
Do not describe this as all counters being unchanged or repeated granting.

Private artifacts are under `/var/tmp/hvm-custom-loader-qa/`: `generated-card`,
`generated-card-relaunch`, `generated-collection`, `generated-card-info`, and
`generated-card-verification.json`. The earned source sandbox remains read-only;
its checkpoint SHA-256 is
`65a8200e82e2d4aa22a402f2b9669c3faf0400532e74ef2c58cd5903e02655d5`.
Probe inputs, modified IPAs, client CSVs and saves must remain outside Git.
Visible card screenshots have been reviewed. No phone custom-card test is claimed.

## Remaining implementation order

1. Implement the mandatory append-only guest registry and durable overlay
   generation/routing before native data-table initialization. It must work on
   the phone without Python or a host game service.
2. Bind the required definition prefix to native checkpoints before decoding
   unknown card IDs. Missing/corrupt required data must preserve the save and
   backup and explain recovery; never silently disable it like optional tuning.
3. Add a post-training create transaction: preserve the pre-mod backup, make
   definitions durable, restart/load them, then grant through native reward
   construction and collection APIs. Ownership lookup must make interruption
   between grant/checkpoint/restart safe. Keep mutable transaction state out of
   the immutable definition fingerprint.
4. Extend the Mod Lab catalog to custom unit/projectile pointers; qualify native
   Info, Use, deck persistence, battle deployment/damage and results/relaunch.
   Preserve all seven original training names, decks and tutorial/AI behavior.
5. Define handling for shared spawn/death effects and investigate independent
   weapon/projectile visual scaling. The present generator clones only the
   primary projectile; it does not promise every referenced subunit is unique.

Original-game full UI/audio/visual/performance audits remain open. This component
evidence is not a release qualification or proof that every modded card works.
