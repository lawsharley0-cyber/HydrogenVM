# Clash sandbox development roadmap

Owner-authorized direction, November 10, 2026. This records all ideas from the
sandbox discussion. The owner requested implementation of the full roadmap on November 11.
See the [workshop handoff](MOD_WORKSHOP_2026-11-11.md) for implemented controls, test evidence and outstanding work.
Hydrogen is an alpha ARMv7 interpreter/runtime; shared engine changes may be
needed. Preserve original seven-trainer names, decks, AI and tutorial sequence.
Advanced opponent-AI refinement remains deferred. Keep earned saves intact.

## Phase 1: dedicated spell editor — current

Existing Mod Lab catalogs favor troops/buildings and spawning spells. Add a
separate stable-ID catalog and optional versioned spell profile; never reorder
MOD1/MCH1/FGR1 records to insert spells. Resolve native data each launch. Verify
representations/getters against pinned 2.1306.1 before editing damage, radius,
duration, travel speed or elixir cost. Show unsupported controls as N/A. Handle
Arrows' separate damaging/decorative projectiles and Lightning's area/projectile
chain. Mirror's dynamic cost/level behavior requires separate investigation.

Require bounded drafts, explicit Apply, defaults/disable restoration, backup,
flush/fsync/rename and rollback on failed writes. Training/friendly behavior must
restore originals. Test catalog identity, corrupt files, unsupported values,
shared references, repeated Apply, battle behavior and save/relaunch. No native
assertion suppression, blind offset writes or save resets.

## Future creation and editing

- Custom spell clones with independent names, photo icons and settings.
- Spell recipes combining projectiles, impact effects and summons; delayed
  detonations, optional friendly fire and burst patterns.
- Pattern designer for lines, rings, cones and staggered impacts, with previews.
- Card archetype templates, parameter relationships and batch faction edits.
  Explicit linked settings versus independent copies; no accidental shared edits.
- Controlled mutation: lock selected properties, randomize others, preview/undo.
- Targeting preferences, acquisition ranges, guard areas, hold/pursue behavior.
- Independent troop/weapon/projectile visual sizes and custom appearance/audio.
- Tower workshop: weapons, attack intervals, projectiles and destruction effects.
- Visual behavior rules: deployment/attack/impact/death triggers, health thresholds,
  attack counters, conditional effects and bounded effect chains.
- Faction packs with names, icons, colors and groups of custom cards.

## Future battle tools and modes

- Pause, slow motion, repeat setup, reset towers, spawn for either side.
- Infinite elixir, regeneration controls, timer rules, invulnerable towers and
  controlled/random starting hands. Preserve native bounds and UI assumptions.
- Formation copy/paste, mirror teams, property eyedropper, troop placement brushes
  with line/circle/grid spacing, reusable battle/arena modules.
- Choose arena, both decks/levels, timed waves and custom win/loss objectives.
- Boss creator: phases, reinforcements and vulnerability periods.
- Campaign creator and linked challenges; escort, survival and capture objectives.
- Draft-and-survive: choose a modifier after each round, escalating encounters.
- Persistent army campaigns with surviving troops, upgrades, injuries and reserves.
- Battle director timeline: deployments, spells, dialogue and rule changes.
- Event triggers for zones, tower destruction and elapsed time.
- Live intervention brushes: heal, damage, freeze, reinforce and switch sides
  where a correct native implementation is possible.
- Scheduled/random arena events and a local clan event director, extending the
  owner's earlier bot-join/drama/events ideas without implying multiplayer.
- Experimental later work: direct control of one troop, collision/pathing-aware
  terrain editing, exact deterministic replay, rewind and branching checkpoints.

## Future usability, testing and sharing

- Separate sandbox profiles, named presets, duplicate, per-card reset, undo,
  automatic backups and restore. Do not overwrite earned progression.
- Import/export challenge/mod packs containing settings, custom images and audio;
  version/dependency validation, previews, thumbnails and descriptions. No vendor
  game assets in distributable packs. Local file sharing should work offline.
- Live unit inspector: health, target, effects and effective custom settings.
- Damage/dummy testing, troop counts, frame times and expensive-effect visibility.
- Compare configurations from the same initial setup. Exact replay requires
  capturing RNG/time and other relevant state; reusing a seed alone is not proof.
- Spawn/effect execution budgets and recursion bounds for predictable performance.

## Custom music — owner addition November 10

- Import user-supplied audio through the iPhone Files picker/share/import flow.
  Begin with local, unprotected audio files; streaming-service catalog access
  does not provide reusable audio bytes or permission to bypass protection.
- Assign music to home/menu, arena/battle, victory and defeat; custom sound
  effects can follow as a separate feature.
- Named playlists, shuffle/sequential order, looping, track preview, independent
  music/effects volume, mute and restore original soundtrack.
- Copy imports into the guest's persistent Documents content store, preserving
  original archive music and surviving guest updates; reference by stable IDs.
- Validate supported codecs with actual decoding/audio tests, interruption/resume,
  app lifecycle and save/relaunch. Avoid loading huge tracks fully into memory.
  Keep this compatible with the shared Hydrogen audio engine and report its bugs
  separately; a nonempty captured waveform is not listening-based qualification.

## Research references and design adaptations

These sources establish the other games' systems. The Clash features above are
our proposals, not claims that these games contain every listed adaptation.

- [Noita](https://noitagame.com/): player-crafted spells and interacting systems.
- [Mindustry logic editor](https://mindustrygame.github.io/wiki/logic/2-editing/):
  mobile-friendly visual rules, parameters and text import/export.
- [TABS](https://store.steampowered.com/app/508440/Totally_Accurate_Battle_Simulator/):
  unit/faction/map/battle/campaign creators, workshop and unit possession.
- [Garry's Mod duplicator](https://wiki.facepunch.com/gmod/duplicator): reusable
  entity groups and saved properties.
- [Factorio parameterized blueprints](https://factorio.com/blog/post/fff-392):
  reusable templates, dependent parameters and numeric relationships.
- [Minecraft command blocks](https://learn.microsoft.com/en-us/minecraft/creator/documents/commandblocks?view=minecraft-bedrock-stable)
  and [structures](https://learn.microsoft.com/en-us/minecraft/creator/documents/structures/introductiontostructureblocks):
  conditions/chains and reusable saved arrangements.
- [WorldBox](https://www.superworldbox.com/): populate, observe and intervene.

Recommended order: spell editing → custom spell clones/templates → battle
testbench → presets/import/export → visual behavior composition. Custom music
is recorded as a separate owner-requested addition; no priority above spells
was requested. Actual implementation/qualification belongs in stage-specific
evidence documents and PR updates, not inferred from this roadmap.
