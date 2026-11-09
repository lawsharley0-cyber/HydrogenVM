# Guest Card Forge — November 8, 2026

This continues from **main `588eebb83fb591b60f0e6eeb037647f337693e5b`**, on
`codex/clash-forge-runtime`. PRs #366 and #381 were closed and integrated through
#382 (`a2ed7c6`); do not reopen the old PRs or overwrite the newer phone/audio
fixes. The separate keyboard accessory follow-up is draft PR #390.
The guest continuation is draft [PR #391](https://github.com/lawsharley0-cyber/HydrogenVM/pull/391).

The pinned client remains unencrypted ARMv7 **2.1306.1**, running inside the
alpha HydrogenVM interpreter. This guest owns its offline transport, saves and
now its definition loader. The phone needs no Python, desktop process or game
server. It still needs a compatible Hydrogen runtime; a modern iPhone cannot
execute this 32-bit guest as a standalone native iOS app.

## User flow and implementation

After the seven original trainers, Settings → Mod Lab allows the player to
select an original unit blueprint, scroll down, enter a name and press
**Create custom card**. Stop and relaunch the same Clash instance to load it.
Do not reinstall or reset its sandbox. Up to 16 immutable definitions are
supported. Names permit valid single-line UTF-8, at most 32 Unicode code points
and 96 bytes. Definition slots/base cards/names cannot be deleted or reassigned.
Reset draft resets optional tuning, not card definitions.

The existing 31-unit blueprint catalog covers original troops/buildings and
deduplicates shared summoned units. This is not every spell variant or group-28
spell. New definitions reuse original artwork and descriptions. Each gets an
independent summoned unit and, where present, primary projectile; secondary
spawn/death-effect references remain shared. Custom entries appear after the
original units in the tuning picker after relaunch. Movement, attack/deploy
speed, troop scale, projectile speed, HP, damage and range use the existing
modifier path. Only original blueprints can be cloned from that picker.

`tools/clash_custom_cards.py` derives private append-row templates through the
reference generator. The builder embeds those templates and original table
sizes/checksums into the guest payload. No proprietary rows are checked into
Git. At startup, guest code obtains its own NSBundle path, reads the originals,
checks their pinned sizes/FNV checksums and writes regenerated copies under
`Documents/ClashForge-v1/`. The `_fopen` import hook redirects only the six
selected CSV resource reads; other file operations use the original import.
Original archive assets remain byte-for-byte unchanged.

Data tables are loaded by the original client. After validation and before
OwnHomeData reaches the client UI, pending definitions are granted through the
native reward factory (`0x133749`), collection add (`0x83439`) and native avatar
Cards Found setter. The decoded home has no live GameMode, so discovery uses
its explicit avatar and native total owned-card count. Existing ownership
prevents repeat grants. Native factory level index 0 displays Level 1.
Granting before delivery avoids requiring a second restart for the collection.
Original training, pending rewards and friendly transactions are protected.

Custom TIDs use the existing native StringTable hook to return the exact
registry name through the native String constructor. This avoids the archived
CSV parser's doubled-quote display behavior. Before ownership handling, native
String equality checks the resolved name against the immutable UTF-8 record.
Other keys, including original trainer names, retain original lookup behavior.

## Save contract and recovery

- **FGR1** `Documents/ClashForge-v1.bin`: five little-endian uint32 words
  (magic `0x31524746`, version 1, count, body FNV-1a, header FNV-1a), followed
  by count × 108-byte records: original base card ID, UTF-8 byte length, and
  100 name bytes with canonical zero padding. An absent file represents zero
  definitions. Empty replacement files/count-zero registries are rejected.
- A definition append uses write/flush/fsync/rename. It also compares the
  existing file against the in-memory registry before replacing it; a completed
  external change is retained and the editor refuses the stale write. This is
  not a qualified concurrent-writer locking protocol.
- The live process retains its original loaded prefix. Newly staged definitions
  become data-table objects only on process restart. A pending append therefore
  does not invalidate the older checkpoint.
- The existing checkpoint path stays `Documents/ClashGuestOffline-v1.bin`.
  Legacy **CRG1** is accepted. Saves requiring definitions use **CRG2** magic
  `0x32475243` and a mandatory **FGC1** extension: tag, required prefix count,
  canonical record-prefix FNV, native body hash and extension-header FNV.
  Extending an unchanged registry prefix is valid. Missing, changed, truncated
  or corrupt required data is rejected before native home decode; save bytes
  remain untouched. CRG2 requires the extension even if the entire extension
  was removed cleanly. Older guests reject CRG2 rather than decode unknown IDs.
- Optional `ClashModLab-v1.bin` remains independent. Its record count can grow
  from 31 through 47; shorter valid profiles initialize added units at 100%.
  Invalid optional modifiers retain their existing disable/archive behavior.
- The first `.before-mods` native checkpoint is retained, never overwritten.
  Definition identity is immutable; mutable tuning is not part of that identity.
  FNV detects corruption/mismatches; it is not authentication. FGR1 is defined
  for this pinned client; changing native table ordering requires migration.

Keep the registry and checkpoint together when exporting current progress.
Deleting the registry while a CRG2 save is active intentionally prevents boot.
To return to a pre-forge CRG1 backup: stop the instance, back up the current
save/registry/modifier files, restore the pre-mod checkpoint, and move the active
registry out of the way before relaunch. Otherwise its definitions will be
granted again. No automatic save reset or in-game recovery browser is added.

## Verification

The clean current-main runtime passes **682 fulltest checks and 26 Test Lab
cases**. This PR changes no shared runtime `src/` code. **59 focused guest
checks pass**, including freestanding ARMv7 compilation without new unresolved
compiler helpers, malformed registry/UTF-8/padding checks, mandatory prefix
validation, CSV escaping, independent modifier pointers/math, original training
guards and readiness-barrier behavior. Run:

```sh
python3 -m unittest discover -s tools/tests -p '*clash_guest*.py' -v
python3 tools/build_clash_guest.py /private/pinned.ipa /private/forge.ipa \
  --build-dir /private/new-build-directory
```

The native QA uses fresh private copied sandboxes, the normal current-main CLI
and `tools/clash_guest_ui_probe.py`. Main already supports text-input commands;
the final main-based creation tests need no CLI instrumentation. Use
`--ready-log 'GuestOffline: queued response=24101' --ready-timeout 180` before
settling/injecting input, and sufficient `--seconds` for the complete plan.
A loading frame alone is not readiness. The barrier records a log event only;
screenshots and actual save/ownership/transaction outcomes still require review.

Committed example plans `tools/clash_guest/qa/forge-create.json` and
`forge-equip.json` reproduce the tested logical 320×568 coordinates. Use only
a stopped, copied earned fixture with the original 10 owned cards, no previous
custom definitions, default tuning selection at Knight, and no pending
reward/name/friendly transaction. Review the picker before creation. After
create, relaunch the same copied instance and verify the registry, ownership
and name before using the equip plan; its collection coordinates assume exactly
one custom card. Adjust and re-review plans for other profiles, never run a
creation plan against the owner's only save. For example:

```sh
python3 tools/clash_guest_ui_probe.py --hvm build/hvm \
  --app '/private/Clash Royale.app' --home /private/copied-earned-home \
  --plan tools/clash_guest/qa/forge-create.json --out /private/new-create-run \
  --gpu --capture-audio --seconds 600 --startup-seconds 12 \
  --ready-log 'GuestOffline: queued response=24101' --ready-timeout 180
```

Observed native cases:

- Settings/editor typing/Create, registry write, first relaunch, collection and
  native Level 1 Info. Both plain and comma/quote-containing names were tested.
  Native name equality and reviewed Info confirm exact quotes after the TID fix.
- Custom picker entry and saved movement 125%/HP 200%; a separate relaunch
  displays **1,300 HP** and **Fast** for the cloned Knight. Old modifiers are
  retained when a second definition is appended; the new entry defaults to 100%.
  The exact final package separately shows the original Level 2 Knight at
  **715 HP / Medium**, confirming the clone's tuning does not change it. A
  deliberately damaged generated characters cache is rebuilt to its previous
  exact bytes, with the registry unchanged.
- A mixed troop/building registry appends Cannon as `27000010` after Knight
  `26000023`. The source spell tables contain 23 and **10** named groups;
  do not guess the building count from the deduplicated unit catalog. The old
  custom card is not regranted; a subsequent relaunch retains resources exactly.
- Ordinary Use replaces the first deck slot, preserving the original Musketeer
  in the collection. Normal touch input deploys the created Knight in a paid
  Arena 1 battle. No private deck reordering, forced completion, elixir override
  or suppressed check is used in this new run. The original NPC wins; return
  and relaunch retain the custom deck, Gold/free Gold 75→74, losses 8→9,
  wins 9, native trophies 59→58, Gems 87, XP 8 and King level 2. The ELO formula
  is not independently qualified by recording that observed native transition.
- This main-based battle reports StoreKit purchases disabled as its only missing
  API category. Mixed audio has 7,225,344 frames, nonzero music/effects, zero
  dropped transport blocks and zero transport errors. The earlier MS ADPCM
  `demon_step_01.wav` gap is already resolved by #382's decoder integration;
  do not port the older runtime over the newer audio fixes. This is not a
  listening-based sound-quality or phone stutter qualification.
- Missing registry, corrupt registry, valid-but-changed prefix, and removed FGC1
  all refuse startup with the game save and registry bytes retained. The exact
  final package repeats these guards. An external registry append during the
  open editor is retained; the attempted stale UI append is refused.
- A fresh profile with staged definitions still starts at the original
  **Trainer Earl**, welcome/tutorial, wins 0 and no custom ownership grant.
  Exported original fixture data remains separate from the appended templates.

Private reproduction files live under `/var/tmp/hvm-forge-v3-qa/`,
`/var/tmp/hvm-forge-main-qa/`, `/var/tmp/hvm-forge-v6-qa/` and
`/var/tmp/hvm-forge-final-guards-qa/`. Helpers are in the task root's private
`outputs/forge_*qa.py`; keep their sandboxes, CSV copies, WAVs and IPAs out of
Git. The original earned source checkpoint remains read-only, SHA-256
`65a8200e82e2d4aa22a402f2b9669c3faf0400532e74ef2c58cd5903e02655d5`.

Final private package: `/var/tmp/hvm-clash-forge-final.ipa`, archive SHA-256
`3ddba2ff714bfae86fa91213128279261438e3d12f296b883904b8a22783bbbc`, executable
`298d8a4991f75c524258b3023dccfa2b9d57bbf2591fd13142eba9db26538adf`.
Its executable differs from the extensively exercised v6 only in eight bytes
inside the existing 110-byte build-time LoginOk payload (server timestamps).
All other executable bytes match. The exact final package also receives a fresh
startup/cache-regeneration smoke. Do not require full-package hash equality
between builds with generated login timestamps.

Rejected/unqualified evidence is retained: the first candidate checked table
readiness too early at connection startup; proactive NSBundle-based generation
fixes that ordering. An early fixed-delay capture closed during loading and
did not prove a grant; the readiness barrier prevents that false interpretation.
An initial append assertion guessed building index 8, and the new scaling test
initially rounded up: the unchanged integer rule is floor(105×250/100)=262.
Correcting those independent fixture/oracle mistakes did not remove runtime
assertions or alter the original scaling implementation.

## Remaining work

Actual iPhone creation/input/import/update/save-folder behavior remains to be
qualified. Do not claim every app button/card/battle outcome or 60 FPS from
these Linux component checks. Finish interrupt-at-each-transaction-stage tests,
multi-writer behavior, maximum-size/name/card-count UI cases, all blueprint
variants, custom projectile/spawner battles and wins/reward flows. Independent
weapon/projectile visual scaling, group-28 spells, other account settings and
new artwork remain unimplemented. Continue the existing clan-event and complete
UI/visual/audio/performance backlog; advanced AI refinement stays deferred.
