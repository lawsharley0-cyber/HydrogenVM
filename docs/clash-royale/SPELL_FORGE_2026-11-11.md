# Custom Fireball and Rocket

In a post-training instance open Settings, Mod Lab, Spell Lab. Select the original Fireball or Rocket, open Tools and tap Clone base spell. Accept the suggested name or type another. Tap Create spell and restart the same instance. The clone is now in the native collection and at the end of the Spell Lab picker. Tune it, enable spell overrides and tap Apply. Use the collection's Use button to equip it. To give it its own icon, select the clone in Spell Lab, open Tools, choose Icon photo and restart after saving.

Cloning starts from the base game values. To duplicate an existing tune, copy the source values and paste onto the new spell after relaunch. Forge supports a combined limit of 16 custom troops, buildings and spells. Definitions are permanent. Use this guest or a subsequent compatible build after creating a spell. Earlier guests only understand troop and building definitions. Keep the instance's complete Documents directory together.

## Identity and effect isolation

Custom spells are append-only FGR1 definitions linked to the saved collection by its existing FGC1 prefix. Spell IDs start after the final original spells_other group, including inactive rows. Slot identifiers for cards, localization and private projectiles remain unique across all Forge types. Copies of the original CSV groups retain level rows, visual references and sound. Only the appended card references the appended projectile. NotInUse keeps it out of native loot pools. Post-training native grant adds ownership on the next launch.

The unit and spell editors maintain separate lists of registry slots, so inserting a spell does not shift a troop's MOD1 or MCH1 tuning. Custom SLP1 entries follow the nine original spells; a shorter existing profile receives native defaults for appended spells. Desktop preset import and export accept --registry pointing to the instance's ClashForge-v1.bin. Matching slot, base and name are required for custom records. An actual ten-record preset exported and imported byte-for-byte. Spell artwork uses the existing ART2 publisher and original source texture checks. The archive assets remain untouched.

## Reproduce in Hydrogen

Run `python3 -m unittest discover -s tools/tests -p '*clash_guest*.py'` (80 passed). The mixed-registry fixture covers troop, spell, building and second spell IDs, original CSV prefixes and unique projectile rows. Preset tests reject a clone whose private ID matches but blueprint differs.

Start with an independent copy of an earned save. The reference run already had one Forge Knight, an enabled spell profile and original Fireball selected. Use tools/clash_guest_ui_probe.py with GPU, audio capture, 12-second startup settling and readiness log `GuestOffline: queued response=24101`. Run tools/clash_guest/qa/spell-clone-create.json, exit, then run spell-clone-tune.json against the same copy. Check the native grant log, the two definition records, ten spell records and the other original settings. The first custom spell ID here was 28000009.

The custom Fireball was equipped via the collection, relaunched and cast from the first hand slot. The battle showed a one-elixir cost and a right Princess Tower falling from 1526 to 1323 HP at 125% damage. The collection screen displayed the base elixir cost. Clone and original occupied distinct deck slots. In the same battle the 125% clone left the right tower at 1323 and the 200% source left the left tower at 1201, both from 1526. Use an explicit card selection before each drag. A preliminary second drag without selection did not cast the source. The Linux Photos fixture selected an authored icon; after relaunch the clone showed that image and the original kept the fireball artwork. A second clone from Rocket received ID 28000010 and retained its own saved cost. Physical iPhone input and Photos presentation still need a device pass.

Release guest SHA-256: `c699cd7aa973f54f620670b094582a11fb9239e3aa0204b07d86fa65d5d46b0a`. Host code is unchanged.

## Next

Custom music is the next milestone. After the music editor, extend effect isolation to Arrows by assigning both projectile names, preserving the native distinction between the decorative and damaging shot. Then cover area-object spells and test their secondary effects in native battles. Keep Mirror's dynamic copied-card behavior separate. The other sandbox systems remain on the [roadmap](SANDBOX_ROADMAP_2026-11-10.md).
