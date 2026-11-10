# Photos inside Clash (2026-11-10)

Owner request: choose custom-card icons and troop pictures directly from the
iPhone photo library inside Clash. This extends the existing Card Forge and
static image pack; no computer conversion or external server is involved.

## Installation and use

This feature needs **both the updated Hydrogen host and the updated Clash guest**.
The guest keeps `com.hydrogenvm.clashroyale.guestdev`, the existing FGR1 registry,
CRG2 game save and mechanics profile. Update the existing instance, preserving
its data. Older Hydrogen builds show “Update Hydrogen to enable Photos.”

At home: Settings → Mod Lab → select a **custom** card → scroll down → Images →
Choose icon photo / Choose troop photo. Select a photo in Apple's system picker,
wait for “Photo saved”, then quit and reopen Clash. The icon appears on the card;
the troop picture is a static appearance in battle, not a generated animation.
The original seven trainers/names/decks/AI remain unchanged.

## Shared runtime and host

`UIImagePicker.cpp` implements a general photo-library service, not a Clash API.
`UIImagePickerController` retains configuration and delivers a UIImage through
the normal modern delegate (or legacy image delegate), or cancellation. Camera,
video and cropping are unsupported. Source availability requires a host provider.
Selection is posted to the guest main run loop. Unique process-independent
request IDs reject stale, duplicate, dismissed and previous-session callbacks.

`GamePhotoPicker.swift` uses PHPicker, granting access only to the selected image.
It accepts common photo formats, normalizes orientation with ImageIO, limits the
decoded image to 1024 pixels per side and exports PNG without location metadata.
The shared bridge caps encoded input at 16 MiB. The guest fits the image into an
icon (128×162) or troop (128×160) canvas with preserved aspect ratio. No photo is
uploaded to a server. iCloud-only photos may require Apple's download first.

Diagnostic CLI provider: `hvm run APP --photo-library-image IMAGE.png` (or `cancel`).
Normal desktop runs without a provider correctly report the library unavailable.

## Guest conversion and persistence

`photo_import.inc`, `photo_assets.inc`, `photo_lzma.inc` convert the selected image
on device. The pinned client's original LZMAReader decompresses existing SC1
atlases. Authored, bounded literal-only LZMA encoding creates compatible SC1
files with MD5 framing; it favors a small guest implementation over compression
ratio. The full icon atlas can therefore occupy several MiB per revision.

ART1 packs remain readable. Phone edits publish ART2:

- Same eight-u32 header, magic `ART2`, version 2; the checksummed body starts with
  a nonzero generation u32, followed by the existing flags and four-u32 file rows.
- Files live under `Documents/ClashForgeArt-v1/gXXXXXXXX/`, with an eight-digit
  hexadecimal generation and fixed derived filenames. No user-controlled paths.
- Allocate a fresh directory, write/flush/fsync all assets and its manifest,
  preserve the first prior manifest as `manifest.bin.before-photos`, then atomically
  rename a flushed temporary root manifest. Old revisions remain on disk.
- No game-save or definition-file writes. Cancellation leaves art files intact.
  An interrupted import cannot expose a partially updated active file pair.
- Later edits preserve previous icons and troop pictures, including old ART1 packs.
  Pixel-dependent metadata aliases avoid stale texture caches on replacement.
- Continue redirecting only bundled `res/sc/` source paths, never decompressed
  `Library/Caches/swfcache/sc/`. Do not delete player data or caches as a repair.

Retained revisions consume space; there is no automatic deletion policy yet.
Physical phone selection/presentation, iCloud behavior, large-library UX and
conversion latency require device qualification. A Linux fixture selecting PNG
bytes does not prove the actual iPhone Photos sheet works.

## Validation and handoff

Before runtime edits: clean main `588eebb`, 682 fulltest checks and 26 Test Lab
cases passed. Fail-before TL-K adds the public allowsEditing round-trip contract;
the older picker setter discarded it and its getter was absent.
After shared edits: **690 fulltest checks and 26 Test Lab cases passed**. Test Lab
also caught recursive superclass deallocation in the first implementation; fixed
the superclass target and added explicit picker release to runtime coverage.

**71 focused guest tests pass**, including independent Python/liblzma decoding of
the literal encoder, SC geometry/pixels, append/replace preservation, malformed
terminal rejection, ART1/ART2 validation and source-versus-cache separation.

Reproduce with the normal tools (private pinned IPA and earned saves required):

```sh
python3 -m unittest discover -s tools/tests -p '*clash_guest*.py'
python3 tools/build_clash_guest.py /var/tmp/clash-client.ipa \
  /var/tmp/clash-phone-photos.ipa --build-dir /var/tmp/clash-phone-photos-build
build/hvm_tests fulltest
python3 tools/hvm_testlab.py run --hvm build/hvm --out /var/tmp/photo-testlab
```

Native fixture evidence is private under `/var/tmp/hvm-photos-native-v1/` and
`/var/tmp/hvm-photos-native-v2/`, using `tools/clash_guest_ui_probe.py` with normal
GPU, touch, audio capture and the OwnHomeData readiness barrier. First troop and
icon imports saved valid ART2 manifests in the native client without an abort.
Later combined/replacement/relaunch/cancel results and Apple CI are recorded in
the PR before delivery. This is scoped customization evidence, not full-game or
physical-device qualification. No assets, photos, IPAs or earned saves belong in Git.
