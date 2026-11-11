# Phone import update (2026-11-10)

[Photos inside Clash](PHONE_PHOTOS_2026-11-10.md) adds direct selection from the
iPhone library and guest-side encoding, with updated host and guest packages.
The computer packer below remains an optional legacy ART1 workflow.

# Custom PNG icons and troop appearances

This is the first static-image art importer for Card Forge. It changes only
chosen custom definitions. An icon PNG appears in the normal deck/collection;
a troop PNG replaces that custom troop's visible body in battle. Logic, card
identity and saves remain native. A single PNG stays static while the troop
moves/attacks; it does not create walking/attack/death animation frames.

The current flow packs images on a computer, then imports the resulting folder
into the guest's Documents. **There is no phone photo picker in this version.**
The phone needs no Python or external server after import. Animated sprite
sheets, multiple facing images and a direct in-game image picker are follow-ups.

## Import your own images

1. Create a custom card in Settings → Mod Lab, then restart Clash and confirm
   it appears. Stop the instance and copy its complete Documents folder as a
   backup. Keep `ClashForge-v1.bin` with its linked game save.
2. On the computer, use the pinned 2.1306.1 extracted `.app` and a copy of that
   instance's `Documents/ClashForge-v1.bin`. The image pack is bound to its exact
   definition prefix. It cannot be borrowed from a different registry.
3. Install Pillow (`python3-pil` on Ubuntu/WSL), then make `images.json`, with
   paths relative to that JSON file:

```json
{
  "0": {"icon": "my-card.png", "troop": "my-troop.png"}
}
```

`0` means the first custom definition, not the original Knight. Other slot
numbers are 1–15, in permanent creation order. Each entry can contain only
`icon`, only `troop`, or both. PNG transparency is supported. Inputs must be PNG
and at most 4096 pixels per side. Artwork is fit without cropping: icon canvas
128×162, troop canvas 128×160, with the troop's feet at the bottom. Movement
and hitbox still use the chosen blueprint. Animation-driven sound/effect parity
needs a separate audit for static body art.

4. From the repository, build into a **new** output directory:

```sh
python3 tools/clash_custom_art.py \
  --bundle '/private/pinned/Payload/Clash Royale.app' \
  --registry '/private/copied-Documents/ClashForge-v1.bin' \
  --images '/private/my-images/images.json' \
  --out '/private/export/ClashForgeArt-v1'
```

The tool refuses an existing output directory. It reads originals and the
registry; it does not edit either. Keep exported SC files private: the icon
atlas preserves original game graphics alongside the added icon.

5. With Clash stopped, copy the **whole** output folder to this instance's
   `Documents/ClashForgeArt-v1/`, including `manifest.bin` and every SC file.
   Use Hydrogen's file management/Files access. Finish copying before launching.
   Restart the same instance. In Mod Lab, select its custom tuning entry and
   press **Images** to see the loaded icon/body status. View the collection and
   enter a battle to verify appearance. The Spawns panel is for behavior edits.

Rebuild/reimport an art pack to change the images. Its cache identity includes
pixel content, even when dimensions stay the same. Export a fresh registry copy
when targeting newly created slots. An existing pack remains valid if later
custom definitions are appended without changing its prefix.

To restore original artwork, stop Clash, move the optional `ClashForgeArt-v1`
folder out of Documents, and restart. Keep `ClashForge-v1.bin`, the game save,
optional modifier/mechanics files and the pre-mod backup. Reset spawns and Reset
draft do not remove art or custom definitions.

## Format and recovery

`manifest.bin` is ART1: eight little-endian uint32 words (magic `0x31545241`,
version 1, definition-prefix count, FNV of those FGR1 records, pinned source-atlas
token, asset count, body FNV, header FNV). The body contains one flags word per
bound definition (icon=1, static troop=2), then asset-count four-word records:
kind, slot, byte size and FNV. Kinds 0/1 are the UI metadata/texture pair;
kinds 2/3 are each troop metadata/texture pair. Filenames are derived from these
bounded integers, never accepted as arbitrary paths. All required pairs, sizes,
checksums and the registry/source identity are checked before using any art.

Missing, incomplete, corrupt or wrong-prefix art remains on disk and falls back
to original artwork. No account reset is needed. Definitions remain mandatory
for CRG2 saves; optional art is a separate contract. Restart is required after
editing the folder. The guest reads assets but never rewrites the art pack.

Overrides match **bundled `res/sc/` source paths only**. Do not broaden them to
all files named `ui_spells.sc`: native `Library/Caches/swfcache/sc/` files are
already decompressed. The first prototype intercepted those reads and failed
on restart. The corrected loader reopens that exact failing cached home without
deleting files. Cold startup alone is not art/relaunch qualification.

The format implementation was checked against original native parsing and
these primary project references: [SC tagged geometry/timelines](https://github.com/jeanbmar/sc-tools/tree/master/lib/supercell-sc),
[texture/compression framing](https://github.com/Galaxy1036/scPacker).
No third-party implementation or proprietary assets are vendored in the repo.
See [the November 9 evidence](MOD_CUSTOMIZATION_2026-11-09.md) for exact checks
and remaining phone/visual limits.
