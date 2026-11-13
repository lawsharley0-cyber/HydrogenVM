# Angry Birds Hatchery Island HD — priority-folder bench

Issue #442; branch `codex/angry-birds-island-compat`, based on main `27f2d818`.
The owner prioritized Desktop/IPA/Get these working over the earlier queue.
Fixture: `Angry Birds Hatchery Island.ipa`, bundle
`com.rovio.angrybirdsislandhd`, version 1.7.0, armv7, cryptid 0, minimum iOS 3.2.
SHA256: `64fea529818bffd7596a696b4bb22e4f32e1843c24b8b892ee1eb5c1154ccac3`.
The app and all saves remain under `~/bench`, outside Git. No assets, guest
instructions, Info.plist or saves were edited. Internet access stayed off.

## Shared fixes

The iPad-only app (`UIDeviceFamily=[2]`) previously received an iPhone idiom
and 320x480 canvas, clipping its menu and controls. The runtime now selects
a classic 768x1024-point @1 iPad profile for an iPad-only Number or Array
family, with matching model/idiom and iPad-specific orientation/status keys.
Phone, universal, absent, empty and malformed-family defaults stay covered.
Player screen overrides still apply. The CLI live-frame header reports the
active runtime scale rather than the initial phone scale.
[Apple documents device-family Number/Array values](https://developer.apple.com/library/archive/documentation/General/Reference/InfoPlistKeyReference/Articles/iPhoneOSKeys.html).

A longer run then stopped at 270.4 s on the missing deprecated instance
`-[CLLocationManager locationServicesEnabled]`. Its class form already existed.
The instance query now uses the same disabled-location-source policy, with
explicit Unavailable provenance. It neither requests GPS nor fabricates a
location. Both fixes are shared API behavior, with no title/bundle checks.

## Actual play and saving

Final fresh run: `~/bench/runs/ab-island-final-fresh`, home
`~/bench/homes/ab-island-final-fresh`, 360.260 s, graceful exit, no crash,
missing APIs or stub hits. Whole landscape image is 1024x768; raw portrait
frames are 768x1024 @1. Menus, episode/level selection, tutorial dismissal,
sling drags, a real level-1 clear (28,870 points / one star), level-2 play,
pause/resume and two-finger zoom are captured. Level 2 reached 16,950 points.
The separate 60 s `ab-island-final-relaunch` shows the earned star and unlocked
second level, then starts that level. The encrypted game saves were not edited.

Average frame 21.1487 ms; p95 24.1172 ms; p99 26.7387 ms; maximum 5028.92 ms.
The guide's average-under-33-ms gate passes. The maximum includes loading work;
this is not an all-frames-under-33-ms guarantee.

Audio transport: 15,882,240 frames, 13,947,699 non-silent, zero drops/errors.
The game mixes music and effects itself into one queued OpenAL source, so the
host's zero separate effect-voice count does not imply missing effects.
Captured PCM matches supplied title music (correlation .9398), bird flight
(.8069), pig destruction (.7797), sling (.4670) and victory (.8953), near the
scripted actions. These checks do not claim subjective headphone quality.

## Reproduction and gates

```sh
build/hvm run ~/bench/apps/Angry_Birds_Hatchery_Island_1.7.0/Payload/AngryBirdsIslandHD.app \
  --home ~/bench/homes/ab-island-review-new --gpu --run-for 360 --graceful \
  --input-file /tmp/ab-island-input.txt --frame-file /tmp/ab-island.hvf \
  --report /tmp/ab-island.json --screenshot /tmp/ab-island.png
```

Use a previously nonexistent home. Feed `touch PHASE X Y FINGER` lines in
portrait **points**: 0 begins, 1 moves, 3 ends. At @1, points equal pixels.
Rotate raw frames 90 degrees clockwise for viewing. Tap (hold .15 s):
20 s `(425,512)` PLAY; 26 s `(380,512)` episode; 32 s `(115,936)` level 1;
42 s `(558,338)` tutorial check. At 50 s drag `(450,789)` to `(500,889)` in
ten .04-second steps and release at .55 s. This earns the first clear.
At 95 s tap `(612,384)` Next. Sling `(410,766)` toward `(434,876)` at 110 s,
`(460,866)` at 150 s, `(480,866)` at 185 s and `(447,876)` at 260 s.
Pause at 200 s `(55,969)`; resume at 205 s `(386,809)`. At 295 s pinch two
fingers from `(350,750)/(550,250)` to `(430,550)/(470,450)` over .5 s.
The private bench wrapper captures PCM and five-second contact sheets and
preserves the complete event JSON at `~/bench/ab-island-final-events.json`.
Relaunch the same home and inspect the level grid before entering level 2.

Baseline: 803 fulltest / 33 Test Lab. New profile tests fail before correction;
the delayed location case interrupts before its missing-selector fix.
Final: **805 fulltest**, **35 Test Lab**, **47 support**, **12 heap**, all pass;
**494 pytest passed, 6 skipped, 725 subtests**; stub audit **0 new/changed**,
681 legacy. Fullcompat `build/compat-ab-island-final`: **0 regressions**, no
unit/Python failures or crashed/skipped required apps. Must-launch: **14
available pass, 0 fail, 21 unavailable skip**. No baseline was weakened.
The new fixture is recorded with picture/sound, 382 required frames, 30 s.

TL-AO uses a separate project-owned iPad TestLab variant, isolated home and
per-case SHA256; its executable/resources and original archive are unchanged.
TL-AP tests the legacy location getter. The runner additionally checks all
family representations and unchanged phone/universal defaults. Commands and
outputs are under `~/codex-ab-island-*`; ordinary gates follow AGENTS and
docs/IPA_PLAYABILITY_LOOP.md. Host: Clang18, WSL2 x86-64 Linux, Mesa, 4 KB pages.

Phone GPU behavior, real speed, 16 KB pages and headphone/microphone routing
remain unqualified. This game did not request microphone input. No Swift or
Objective-C app-shell files changed, and no phone results were supplied.
