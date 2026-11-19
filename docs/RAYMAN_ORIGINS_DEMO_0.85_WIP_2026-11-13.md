# Rayman Origins demo 0.85 — Linux bench, 13 November 2026

The unencrypted Ubisoft/Pastagames UbiArt IPA is playable on the Linux bench
on the **default `iPhone5,2`** (no `--device`): authentic jungle title menu,
PLAY into the Jump / world 1 select, level 1 starts, left-stick plus jump
input moves Rayman through the stage, and music plus effects run for a full
180 s. A 180 s session on `iPhone5,2` presented **6,147** frames at average
**28.83 ms**, p50 **27.26 ms**, p95 **33.55 ms**, p99 **47.19 ms**, max
102.59 ms (composite hitches), with no crash and 0 stub hits. Audio was
audible from **2.1–180.1 s** with the same ~0.5 s four-movement MP3 loop
joins (7,845,412 non-silent of 7,940,096 frames; mixer music 7,835,648,
effects 911,360, 11 music voices, 90 effect voices, peak 0.63).
`Documents/scores` (the default Pastagames table) is written at first launch
and still loaded after a graceful quit and a relaunch of the same `--home`.
Must-launch is registered (30 s, 494 frames, sound, **no** `--device`).

The product change is a general **backing-size fallback**: when a guest
`glViewport` is larger than the CAEAGLLayer drawable it is drawing to,
HydrogenVM grows that drawable (and matching depth/stencil renderbuffers)
and `presentRenderbuffer` hands the full frame to the compositor, whose
default `kCAGravityResize` scales it into the layer bounds. A pre-iPhone-5
app without `Default-568h@2x.png` already runs at 320×480 points @2×
(`configureScreen`); this fallback is for games that then pick a
design-sized viewport and would otherwise clip to a corner. Offscreen FBOs
and in-bounds viewports are unchanged, and CAEAGLLayer still defaults to
`contentsScale` 1 (1× games that do not request a larger viewport keep
their backing). Fail-first TestRunner checks (block 2f14) were red
(drawable stayed 240×160; only the lower-left corner colour was visible)
and are green after the software and HostGL paths. The `--device MODEL`
flag from the first pass of this branch remains, but Rayman no longer
needs it.

## Existing resize / screen support (does not cover this case)

Owner feedback asked to reuse already-merged resize support rather than
lie about `hw.machine`. What is on `main`:

- **#298** ([PR](https://github.com/lawsharley0-cyber/HydrogenVM/pull/298)):
  player aspect-fill / crop focus / extended bars (`PlayerScaleMode`,
  `PlayerGeometry`). Phone and Mac player, not the Linux bench framebuffer.
- **#300** ([PR](https://github.com/lawsharley0-cyber/HydrogenVM/pull/300)),
  issue **#281**: taller guest screen and safe-area insets. Env
  **`HVM_VIRTUAL_SCREEN_HEIGHT`** (480..1024, `applyGuestScreenEnv`) and
  **`HVM_SAFE_AREA="top,left,bottom,right"`**. This makes the guest
  *taller*, not a 3.5-inch letterbox. On default `iPhone5,2` with height
  1024 / 682 / 568, Rayman still builds a 1× landscape drawable whose
  height is the 320-point portrait width — a wide incomplete strip (PLAY
  and scores 4–6, no RAYMAN logotype). TestRunner block “2a0. Guest screen
  overrides”.
- **#443** ([PR](https://github.com/lawsharley0-cyber/HydrogenVM/pull/443)):
  iPad-only binaries (`UIDeviceFamily`) get an iPad model, idiom, and
  768×1024 @1. Rayman is universal, so it keeps the phone profile.
- **#19** / `PlayerGeometry`: letterbox of the guest framebuffer onto the
  host phone display (`GuestScreenSettings.virtualHeight`).
- `configureScreen` already presents 320×480 @2× for an app with no
  `Default-568h@2x.png` (and honors a smaller configured height). The
  iOS player letterboxes that 640×960 guest FB on a taller phone.
- OpenGLES `renderbufferStorage:fromDrawable:` already has
  `scale = contentsScale > 0 ? contentsScale : UIScreen.scale`, but
  `UIView::ensureLayer` forces CAEAGLLayer `contentsScale` to 1, so the
  fallback never runs. Changing that default globally would break 1× GL
  games that hard-code a 320×480 viewport.

On `iPhone5,2` at the accurate CAEAGL 1× default, this demo calls
`glViewport(0, 0, 1024, 682)` (221 times in a 4 s `HVM_GL_DEBUG=2` trace)
against a 480×320 drawable, which OpenGL clips to the lower-left corner
of the 3:2 UbiArt design (scores 7–9 and a sliver of RAYMAN). The same
binary on `iPhone4,1` opts into a 2× 960×640 drawable and shows the full
title; `Application::screenWidth = 682` on both models. The device table
lists iPhone, iPhone3, iPhone4, iPod, iPod4, iPad, iPad2 — no iPhone5
(March 2012 TestFlight; the iPhone 5 did not exist). The fallback grows
the 480×320 backing to 1024×682 (logged `GPU: drawable grown from
480x320 to 1024x682`) and the full title is composited into the
320×480-point @2× screen.

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

## Menu, level start, and input (default `iPhone5,2`)

`--tap` coordinates are portrait points (screenshot pixels / 2). For a
pixel `(nx, ny)` on the upright 960×640 (ROTATE_270) view of a 640×960
framebuffer, the portrait point is `(ny/2, (959-nx)/2)` — the same
mapping as on `--device iPhone4,1`, because the guest screen is still
320×480 @2×.

- PLAY at **(169.5, 217.0)@8** opens the four-world select: JUMP world 1,
  FLY world 2, SMASH world 3, WALL RUN world 4, and Back.
- JUMP / world 1 at **(100.0, 339.5)@14** opens the medal row (1–5 red /
  unlocked, 6–9 grey / locked). A 15 s snapshot shows this complete UI.
- Level 1 medal at **(79.0, 427.0)@20** starts the jungle stage: Rayman
  on a grassy cliff, a following red heart, god rays, a pause mark in the
  top-left (30 s snapshot).
- A held left stick (finger 1: began `(250, 379.5)`, moved to
  `(250, 229.5)`) plus jump taps on the right (finger 2: `(260, 54.5)`),
  fed from 28 s, advances Rayman: a 45 s snapshot is a later stretch of
  the same level (Rayman airborne with a collected yellow lum). By 70 s
  the run is back on the world-1 medal screen with a different backdrop
  (peach flowers, a yellow lum) — post-death / return, not a freeze of
  the first visit. The rest of the 180 s stayed on that select.

Options at **(152.5, 69.5)@8** opens the Options scene (Reset Scores,
unlocalized `STRCHANGECONTROLS`, Back). Game Center (“Achievement”,
“More Leaderboards”) is offline by design.

## Music and 180 s

`audio_capture.py` on the 180 s default-device run: **31,760,384** bytes,
audible 2.1–9.6, 10.2–17.7, then 18.2 through 180.1 with the same ~0.5 s
joins as the four-part `Mus_Jun_JoyeuxBaston_Pizz_*_4M.mp3` loop.
7,845,412 / 7,940,096 non-silent; mixer 7,835,648 music frames, 911,360
effect frames, 11 music voices, 90 effect voices, peak 0.63 (capture
peak 0.78). `--graceful` logged “The app saved and closed”. No hang:
6,147 frames over 180.14 s wall, average 28.83 ms. The extra cost versus
the earlier `iPhone4,1` 10.81 ms run is almost entirely software
composite of the grown 1024×682 backing (`avgCompositeMs` 20.79); the
game's own frame is typically ~6 ms. Average stays under 33 ms.

## Saves and relaunch

The only document is `Documents/scores` (314 bytes, UTF-16LE
“Pastagames” names and the default 50, 45, … 10 table, SHA-256
`41b0dfab777e0efb…`). It is identical after the 180 s session and after
a 15 s relaunch of the same home (`Loading : …/Documents/scores`;
`Device = iPhone5,2`, drawable grown again, full title). The title
plaque still shows those rows (it pages 4–6 / 35–25 on that visit). The
demo does not write a mid-level checkpoint file; levels 1–5 of world 1
start unlocked. Library/Preferences stays empty. A non-default earned
value was not obtained: scripted play progressed level 1 (45 s airborne
with a lum) then returned to medal select without a write; Options
opens but produced no additional files. Finishing a level is the likely
path to a non-default scores row; that was not reached with this input.
This is the game's save surface.

## Reproduction

```sh
APP=~/bench/apps/Rayman_Origins_Demo_0.85/Payload/raymanorigins1demo.app
H=$(mktemp -d)
# Title menu (correct picture on the default iPhone5,2):
./build/hvm run "$APP" --home $H --keep-going \
  --run-for 30 --gpu --graceful --screenshot menu.png --report menu.json

# Level 1 with run + jump (append touches after 25 s):
python3 tools/bench/audio_capture.py --log run.log --wav out.wav -- \
  ./build/hvm run "$APP" --home $H --keep-going \
  --run-for 180 --gpu --graceful --screenshot end.png --report run.json \
  --tap 169.5,217.0@8 --tap 100.0,339.5@14 --tap 79.0,427.0@20 \
  --input-file touches.txt
```

Must-launch (no `--device`):

```sh
python3 tools/must_launch.py --apps ~/bench/apps --only Rayman_Origins --record
# entry: folder Rayman_Origins_Demo_0.85, 30 s, 494 frames, sound, picture
```

## Gates (this tree, Linux bench)

| Gate | Result |
|---|---|
| `hvm_tests fulltest` | 827 passed, 0 failed (824 plus three new 2f14 backing-size checks; those were red first) |
| `hvm_support_tests` | 47 passed, 0 failed |
| `tools/lint_stubs.py --baseline tools/stub_lint_baseline.json` | 0 new/changed (680 unlabelled, same baseline) |
| `python3 -m pytest -q tools/tests` | 403 passed, 5 skipped, 1 failed (`TestCensus` / #445, pre-existing) |
| Test Lab (`hvm_testlab.py run --skip-build`) | 35 passed, 0 failed |
| `must_launch.py --only Rayman_Origins --record` | device field dropped; 30 s, 494 frames, sound, picture (`FRAME_MARGIN` of this run did not exceed the previous 494 floor) |

Do not read “tests pass” as required CI green; CI was not claimed on this
branch.

## Limits

- Phone untested.
- Game Center (“Achievement”, “More Leaderboards”) is offline by design.
- Options / Bonus / `STRCHANGECONTROLS` were opened; the default in-level
  scheme (hold-left to run, right-side tap to jump) is enough for the
  playability bar.
- A real iPhone 5 would also have reported `iPhone5,2` and, with
  CAEAGLLayer default scale 1, would clip this binary the same way. The
  backing-size fallback is a HydrogenVM compatibility extension, not a
  claim that iOS grew the renderbuffer.
- “Bonus” / “Achievement” banners sit on the right edge of the 3.5-inch
  frame; that is the layout, not a clipped framebuffer.
- AAC is not an issue here (the loop is MP3, and the bench is audible).
- Average frame time is higher than the earlier `iPhone4,1` path because
  the grown 1024×682 backing is scaled in the software compositor
  (Linux has no direct host present). It remains under 33 ms.
