# Forlorn v1.9c — Linux bench, 13 November 2026

**Blocked.** The unencrypted cocos2d TestFlight IPA reaches an authentic
boot splash, then aborts before the main menu. The crash is in guest
code (`NSAssert` is compiled in) and would happen on a real iPhone with
this same binary. It is not a HydrogenVM API gap. Not recorded in
`must_launch`.

## IPA

- Source: [Various Testflight Teraleak ipas](https://archive.org/details/various-testflightleak-ipas),
  third file in the metadata listing (set-3 IPA 3):
  `forlorn f953e14876c586824223eb44af5fac71.ipa`
- SHA-256: `cdfb3d117d244272288cde06344502ffa119e56075f4c45762bbd564ec3df53d`
  (SHA-1 matches archive.org: `248e386804dd7ce975ffb35d16ee638c9093fc6f`;
  MD5 `8c0301c63d9d5a669009f123b75ea052`; 56,979,731 bytes)
- Unpacked at `~/bench/apps/Forlorn_1.9c` (not in the repo)
- Bundle `com.robtop.magegame` (RobTop), display name "Forlorn",
  executable `Forlorn`, `CFBundleVersion` 1.9c, MinimumOSVersion 4.1,
  armv7 32-bit, not FairPlay-encrypted (`cryptid=0`), iPhone only,
  `opengles-1`, iOS 5 SDK (Xcode 4C199). Screen: 320×480 @2x.
- Engine: cocos2d (sources under
  `/Users/Dennerstahl/Desktop/Projects/Forlorn/Forlorn/libs/cocos2d/`,
  CocosDenshion / OpenAL / `AVAudioPlayer` for `.caf` and `.mp3`).

## What runs

Fresh home, `./build/hvm run … --keep-going --run-for 30 --gpu --graceful`.
The authentic "FORLORN" boot splash (blue-hatted mage in a forest,
title lettering) is presented in the portrait framebuffer; the picture
is correct when viewed as landscape (IPA_PLAYABILITY_LOOP: a landscape
game is drawn sideways). 7 frames, first frame 248 ms, then a stop at
0.59 s wall. Audio never starts. 0 stub hits, no missing functions.

The app sets `UIInterfaceOrientation` to landscape right, then
`setRootViewController:` reports "landscape right → portrait". That
reset was not pursued; the run dies in particle setup first.

## Why it stops

```
Assertion failure in -[CCParticleSystemQuadExtra initWithDictionary:],
…/CCParticleSystem.m:280: CCParticleSystem: error loading the texture
```

`MenuSceneBackground initialLoading` → `createBlock` →
`+[ParticleBlock particleBlockWithType:ZType:FreeMode:]` →
`+[CCParticleSystem particleWithFile:]` →
`-[CCParticleSystem initWithDictionary:]`.

`initWithDictionary:` reads `textureFileName` from the Particle
Designer plist and calls `-[CCTextureCache addImage:]`. Host traces
show `-[UIImage initWithContentsOfFile:]` returning **nil**, then a
`valueForKey:` for `textureImageData` also returning **nil**, then
the assertion. 116 of 117 particle plists name a loose PNG that is
**not in the bundle**:

| `textureFileName` | plists | in the `.app`? |
|---|---|---|
| `sun.png` | 58 | no (only a frame in the sheet) |
| `snow.png` | 20 | no |
| `sparkle.png` | 11 | no |
| `crystal_particle.png`, `line.png`, `shine.png`, `cloud.png`, `pixel.png`, `rain.png`, `fireParticle.png`, `boneParticle.png`, `fur.png`, `dot.png` | 1–6 each | no |
| `sunTexture.png` | 1 (`logoEffect.plist`) | **yes**, a real file |

Those names are frames inside `particleImgSheet.plist` /
`particleImgSheet.pvr.ccz` (TexturePacker format 2, 18 frames,
`size {61,127}`). The sheet **is** opened (`fopen` 2,708 bytes,
zlib-decompressed 15,546 = 52-byte PVR v2 header + 61×127
RGBA4444, `flags=0x8010`, tag `PVR!`). There is no `textureImageData`
in the plists, and this cocos2d `addImage:` has no sprite-frame
fallback: after a nil `UIImage` it tries `CCTexture2D initWithImage:`
on that nil, unlocks, and returns nil.

`NSAssert` is **compiled in** (the handler is actually called). On
iOS that raises `NSInternalInconsistencyException` and, uncaught,
terminates the process. HydrogenVM's `NSAssertionHandler` throws
`ObjCError` and stops, which is the same outcome. Disabling the
handler or injecting the missing PNGs into the bundle would be
changing the game, not making the emulator match iOS.

## Diagnostic only (not a fix)

A copy of the app with 16×16 placeholder PNGs for those 18 names got
past the particle assert and then stopped in a **second**, related
assert:

```
Assertion failure in -[Block initWithTexture:rect:],
…/CCSprite.m:200: Invalid texture for sprite
```

`MenuSceneBackground createBlock` → `+[Block blockWithTexture:]` →
`-[CCSprite initWithSpriteFrameName:]`. `spriteFrameByName:` **finds**
the frame (`objectForKey:` returns a frame object), but the frame's
`CCTexture2D` is invalid. GL traces of the unmodified IPA show
`glTexImage2D` for 256×32, 1024×1024 and 512×1024 (RGBA,
`GL_UNSIGNED_SHORT_4_4_4_4`) and some `glCompressedTexImage2D` (PVRTC
sheets), and **no** upload of 61×127. cocos2d-iphone 0.99.5 / 1.0
`CCTexturePVR` historically rejects non-power-of-two PVR images; this
sheet is 61×127. That rejection is guest code, so the frame texture
would also be nil on a real device with this binary.

## What was not an HVM-only gap

- The IPA is unencrypted and loads. Boot art is authentic.
- PVRTC sheets and larger POT RGBA4444 PVRs do upload.
- The 12 unimplemented imports of earlier Unity games are not used
  here; Forlorn's unimplemented list is
  `CFRunLoopCopyAllModes`, `asl_*`, `pthread_getname_np` — none called
  on this path.
- A general `UIImage` or PVR-loader change would not make `addImage:@"sun.png"`
  succeed: that file is not in the bundle, and `UIImage` does not
  consult cocos2d's sprite-frame cache.

## Reproduction

```sh
APP=~/bench/apps/Forlorn_1.9c/Payload/Forlorn.app
H=$(mktemp -d)
./build/hvm run "$APP" --home "$H" --keep-going --run-for 30 --gpu --graceful \
  --report r.json --screenshot s.png
# exit 70, crashReport has CCParticleSystem.m:280
```

## Still needed

A different Forlorn build (App Store / a release with `NS_BLOCK_ASSERTIONS`
and either loose particle PNGs or `textureImageData` in the plists)
would be required to continue playability work. This TestFlight IPA
is not that build. Set-3 continues at IPA 4 (`gba4ios`).

## Phone-only / not checked

No iPhone run. If Lawson has this exact IPA on a device, the prediction
is a crash at the same assertion before the menu. Mesa vs device GPU
was not reached.
