# Rayman Origins demo 0.85 — Linux bench, 13 November 2026

The unencrypted Ubisoft/Pastagames UbiArt IPA is playable on the Linux bench
with `--device iPhone4,1`: authentic jungle title menu, PLAY into the Jump /
world 1 select, level 1 starts, left-stick plus jump input moves Rayman through
the stage, and music plus effects run for a full 180 s. A 180 s in-level
session presented **10,614** frames at average **10.81 ms**, p99 **20.97 ms**,
max 82.75 ms (load and composite hitches), with no crash and 0 stub hits.
Audio was audible from **2.1–180.1 s** (7,847,129 non-silent of 7,940,096
frames; mixer music 7,841,792, 8 music voices, 106 effect voices, peak 0.63).
Brief ~0.5 s gaps are the four-movement `Mus_Jun_JoyeuxBaston_Pizz_*_4M.mp3`
loop joins, already present on the title screen. `Documents/scores` (the
default Pastagames table) is written at first launch and still loaded after a
graceful quit and a relaunch of the same `--home`. Must-launch is registered
(30 s, 494 frames, sound, `--device iPhone4,1`).

The only product change is a general `--device MODEL` flag (`hw.machine`,
default still `iPhone5,2`) and a matching `device` field in
`tools/must_launch.json`. This March 2012 TestFlight predates the iPhone 5;
its device table lists `iPhone`, `iPhone3`, `iPhone4`, `iPod`, `iPod4`,
`iPad` and `iPad2`. On the default `iPhone5,2` the game builds a 1×
320×460 backing and shows only the lower-right corner of the title (scores
7–9 and a sliver of the yellow RAYMAN logotype). `--device iPhone4,1`
matches a 3.5-inch retina phone of that era (the app has no
`Default-568h@2x.png`, so HydrogenVM already presents 320×480 @2×) and the
game uses a 2× 960×640 landscape drawable. The log line
`width = 682, height = 1024` is the game's own 3:2 UbiArt design size; it
is printed on both models and is not a HydrogenVM screen-metric bug. The
global default is unchanged, so titles that already run as `iPhone5,2` are
not retargeted.

## IPA

- Source: [Various Testflight Teraleak ipas](https://archive.org/details/various-testflightleak-ipas),
  `raymanoriginis1test 2d9adab4b3c92c73359285e80ea8f218.ipa`
- SHA-256: `e2b6e5c652dae3961cb2a50872937198467b84845b003dd80627a45019a02cc1`
  (SHA-1 matches archive.org: `2a093a8f146be819e93a108a13213e467b435e7e`;
  MD5 `794feaad740d608e62df82fc0813a77f`; 35,944,426 bytes)
- Unpacked at `~/bench/apps/Rayman_Origins_Demo_0.85` (not in the repo)
- Bundle `com.ubisoft.raymanorigins1demo`, display name
  `raymanorigins1demo`, CFBundleVersion **0.85**, MinimumOSVersion 3.2,
  iOS 5.0 SDK (Xcode 4C199), universal (`UIDeviceFamily` 1+2), status bar
  hidden, all four orientations declared. FAT unencrypted (ARMv6 +
  ARMv7; HydrogenVM loads the v7 slice, entry `0x2360`).
- No `Default-568h@2x.png`; HydrogenVM logs `Screen: 320x480 @2x, landscape`.
  After launch the view rotates portrait → landscape left. Viewing a
  portrait framebuffer screenshot with a 270° rotation is the upright
  picture (90° is upside-down).

## First launch (default iPhone5,2)

Fresh home, 30 s, no `--device`. No crash, 0 stubs, 1,028 frames, average
26.38 ms, looping Joyeux Baston, default scores written. `Device = iPhone5,2`,
`width = 682, height = 1024`, drawable 320×460 then 480×320 (1×). The
upright picture is only a corner of the title: a dark plaque with
“7–9. Pastagames” and a sliver of a huge yellow “P” (the end of RAYMAN).

## `--device iPhone4,1` and the correct picture

`hvm run … --device iPhone4,1` (or, before the flag, `HVM_DEVICE_MODEL`
in a local diagnostic) prints `Device = iPhone4,1` and a 2× drawable
(640×920, then 960×640). The title is complete: RAYMAN logotype, PLAY,
Options / Bonus / Achievement / More Leaderboards banners, trophies, and
the Pastagames high-score plaque (it pages 1–3, 4–6, …). 12 s CLI smoke:
559 frames, average 10.85 ms, no crash.

`--device` is parsed in `HvmRun.cpp` and writes `ProcessConfig.deviceModel`,
which `SysInfo` already exposes as `hw.machine`. An empty model is rejected
(exit 2). `tools/must_launch.py` passes `--device` when a list entry has a
`device` field; fail-first tests for `build_cmd` were red (`build_cmd`
missing) and are green after the helper and the two `run_game` call sites
were updated. `expectation()` already keeps extra keys, so a recorded
entry retains `device`. The playability-loop document mentions the flag.

## Menu, level start, and input

`--tap` coordinates are portrait points (screenshot pixels / 2). For a
pixel `(nx, ny)` on the upright 960×640 (ROTATE_270) view of a 640×960
framebuffer, the portrait point is `(ny/2, (959-nx)/2)` — verified by
matching a PLAY-letter colour in the raw screenshot.

- PLAY at **(169.5, 217.0)@8** opens the four-world select: JUMP world 1,
  FLY world 2, SMASH world 3, WALL RUN world 4, and Back.
- JUMP / world 1 at **(100.0, 339.5)@14** opens the medal row (1–5 red /
  unlocked, 6–9 grey / locked).
- Level 1 medal at **(79.0, 427.0)@20** starts the jungle stage: Rayman
  on a grassy cliff, a following red heart, god rays, a pause mark in the
  top-left. A 50 s run with those three taps and no further input stayed
  at the starting cave; mixer `effectFrames` 1,282,048 / 197 effect
  voices (level ambience). Average 11.37 ms.

There is no always-visible virtual pad. A held left stick (finger 1:
began `(250, 379.5)`, moved to `(250, 229.5)`) plus jump taps on the
right (finger 2: `(260, 54.5)`) advances Rayman: a 50 s run with that
input after 25 s ends in a later stretch of the same level (palm
foreground, a cliff gap, the heart still following). Desktop touch
lines are logged; RMSE vs the no-input shot is 58. The 180 s session
used 27 of those run-and-jump cycles (25–172 s) and finished back on
the world-1 medal screen (a different backdrop — peach flowers, a
yellow lum — so the select is animated / post-level, not a freeze of
the first visit).

## Music and 180 s

`audio_capture.py` on the 180 s run: **31,760,384** bytes, audible
2.1–9.6, 10.2–17.7, then 18.2 through 180.1 with the same ~0.5 s joins
as the four-part MP3 loop. 7,847,129 / 7,940,096 non-silent; mixer
7,841,792 music frames, 821,248 effect frames, 8 music voices, 106
effect voices, peak 0.63 (capture peak 0.78). `--graceful` logged
“The app saved and closed”. No hang: 10,614 frames over 180.15 s wall,
average 10.81 ms, p50 8.78, p95 16.78, p99 20.97, twelve hitches 45–83
ms. Unimplemented imports none, stub hits 0.

## Saves and relaunch

The only document is `Documents/scores` (314 bytes, UTF-16LE
“Pastagames” names and the default 50, 45, … 10 table). It is identical
after the 180 s session and after a 25 s relaunch of the same home
(`Loading : …/Documents/scores`). The title plaque still shows those
rows (4–6 / 35–25 on that visit; the list pages). The demo does not
write a mid-level checkpoint; levels 1–5 of world 1 start unlocked.
Library/Preferences stays empty. This is the game's save surface.

## Reproduction

```sh
APP=~/bench/apps/Rayman_Origins_Demo_0.85/Payload/raymanorigins1demo.app
H=$(mktemp -d)
# Title menu (correct picture):
./build/hvm run "$APP" --home $H --device iPhone4,1 --keep-going \
  --run-for 30 --gpu --graceful --screenshot menu.png --report menu.json

# Level 1 with run + jump (feed touches after 25 s; see the 180 s input file):
python3 tools/bench/audio_capture.py --log run.log --wav out.wav -- \
  ./build/hvm run "$APP" --home $H --device iPhone4,1 --keep-going \
  --run-for 180 --gpu --graceful --screenshot end.png --report run.json \
  --tap 169.5,217.0@8 --tap 100.0,339.5@14 --tap 79.0,427.0@20 \
  --input-file touches.txt
```

Must-launch:

```sh
python3 tools/must_launch.py --apps ~/bench/apps --only Rayman_Origins --record
# entry: folder Rayman_Origins_Demo_0.85, device iPhone4,1, 30 s, 494 frames, sound
```

## Gates (this tree, Linux bench)

| Gate | Result |
|---|---|
| `hvm_tests fulltest` | 824 passed, 0 failed |
| `hvm_support_tests` | 47 passed, 0 failed |
| `tools/lint_stubs.py` | 0 new/changed (680 unlabelled, same baseline) |
| `python3 -m pytest -q tools/tests` | 403 passed, 5 skipped, 1 failed (`TestCensus` / #445, pre-existing; the four new must_launch `--device` tests are in the 403) |
| Test Lab (`hvm_testlab.py run --skip-build`) | 35 passed, 0 failed |
| `must_launch.py --only Rayman_Origins --record` | new, 494 frames, sound, `device` `iPhone4,1` |

Do not read “tests pass” as required CI green; CI was not claimed on this
branch.

## Limits

- Phone untested.
- Game Center (“Achievement”, “More Leaderboards”) is offline by design.
  Options / Bonus / STR_CHANGE_CONTROLS were not walked in depth; the
  default in-level scheme (hold-left to run, right-side tap to jump)
  is enough for the playability bar.
- The default `iPhone5,2` picture is wrong for this binary. That is the
  game's unknown-device path, not an inaccurate `UIScreen`. Launch with
  `--device iPhone4,1`. A real iPhone 5 would also have reported
  `iPhone5,2`; this TestFlight never had that hardware.
- “Bonus” / “Achievement” banners sit on the right edge of the 3.5-inch
  frame; that is the layout, not a clipped framebuffer.
- AAC is not an issue here (the loop is MP3, and the bench is audible).
