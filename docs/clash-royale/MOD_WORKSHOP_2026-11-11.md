# Mod Lab workshop, November 11

From the earned home, open Settings, then Mod Lab. Account opens balances, Unit tuning selects a blueprint, Card Forge opens creation and artwork, Spawns edits summon behavior and Spell Lab opens spell settings. Editors returns to the menu. Close returns to Clash.

## Spell controls

Copy values takes a snapshot. Select another spell and paste to transfer the supported fields. The source draft stays independent; spells sharing native projectiles can still share battle effects. Wide area sets radius to 150%. Heavy hit sets damage to 150%. Reset one restores the selected spell's defaults. Reset spells stages defaults for all spells.

Tools jumps to the lower controls. Tap a field to lock it before shuffling. Locked names appear in brackets. Shuffle chooses 50-200% in 25-point steps or one to ten elixir, with a separate RNG state from opponents. Undo edit reverses the last edit, paste, template, per-spell reset or shuffle on that spell. Editors preserves the current session's draft. Back to values returns to the selector. Apply spells saves; the Spells toggle controls activation.

## Presets and isolated comparisons

Use an extracted 2.1306.1 app and a closed guest home. Export and import create new files and leave existing outputs intact. The JSON preset contains spell settings only.

```sh
python3 tools/clash_spell_preset.py export "$app" "$home/Documents/ClashSpellLab-v1.bin" Wide.json --name "Wide spells"
python3 tools/clash_spell_preset.py import "$app" Wide.json restored.bin
python3 tools/clash_sandbox_compare.py --hvm "$hvm" --app "$app" --home "$home" --variant Wide.json --plan tools/clash_guest/qa/spell-battle-compare.json --out comparison
```

The comparison takes two independent copies of the source home before starting either run. The second copy receives the optional preset. Inspect baseline/capture and variant/capture for screenshots, report, input, audio and logs. Use a new output path with space for both homes. Host time and simulation timing can vary. Exact replay remains open.

## Verification

Run `python3 -m unittest discover -s tools/tests -p '*clash_guest*.py'`. All 78 focused checks passed. The native navigation test visited all five destinations. The checked-in spell-workshop plan starts from an earned profile with Fireball at 200% damage, 200% radius, 125% speed and one elixir. It copies the values to Arrows, leaves and resumes the draft, applies Wide area, locks radius, shuffles, undoes, and saves. The delivered guest produced the expected Arrows record (200,150,100,125,1). In the paired run with a two-elixir source preset the result was (200,150,100,125,2). Both captured all 15 steps. The original spell profile remained byte-identical. A preliminary swipe-only probe missed the lower controls; use the dedicated Tools button.

The battle comparison plan requires Knight in slot one with Fireball next. Verify deployment, hand cost, right tower HP and King tower activation in the impact frames. Physical iPhone usability and audio listening remain pending.

Release IPA SHA-256: `6a873d6990d7d2f2605a734526dbec3b834f43dee13f216ecbd5b9efe6d5f246`. Use the current Phone Photos Hydrogen host.

## Remaining engineering work

Next create a separately owned custom spell with a distinct ID and effect that survives relaunch. Current Forge registry validation, generated CSV groups and native ownership cover troop/building clones. Extend and verify all three paths before enabling spell creation. Preserve FGR1/FGC1 identity and SLP1 prefix compatibility.

Phone music import, in-game pack management, live battle controls, formations, spells composed from multiple effects, scenarios, campaigns, visual rules, terrain editing and rewind remain on the [full roadmap](SANDBOX_ROADMAP_2026-11-10.md).
