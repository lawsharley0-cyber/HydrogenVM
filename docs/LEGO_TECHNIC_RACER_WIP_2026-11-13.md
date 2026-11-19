# LEGO Technic Racer v1.0 — Linux bench, 13 November 2026

The unencrypted Unity 4.0.1f2 IPA is playable on the Linux bench: authentic
main menu, level-select and race start, Touch-scheme pull-back launch, and
looping music. A 180 s race was audible from **4.6–180.2 s** (7,747,402
non-silent of 7,950,336 frames, mixer peak 0.48, one music voice),
presented 5,176 frames at average **7.76 ms**, p99 **18.09 ms**, with no
crash and 0 stub hits. A clean pull-back finished track 1 in **00:17:12**
(five gears); PlayerPrefs `Level 1=1` survived a relaunch of the same
home. This is the same studio and engine as LEGO Technic Pullback Racers
(bundle `dk.spacetimefoam.legotechnicracer`, Unity 4.0.1f2) with a
different binary and content. It needs the same general fixes already on
#448 (GNU libstdc++ `std::string` imports, AudioFile `'frpk'`/`'pnfo'`,
and 32-bit iOS `PTHREAD_STACK_MIN` of 8 KiB). No further engine change
was required. Recorded in `must_launch.json` (30 s, 232 frames, sound).

## IPA

- Source: [Various Testflight Teraleak ipas](https://archive.org/details/various-testflightleak-ipas),
  second file in the metadata listing (set-3 IPA 2):
  `Lego Technic Racer v1.0 [TF 9bd3ea2a1d5ed1ae8d248473ae20e582].ipa`
- SHA-256: `103d3199aa6de228d99af9974a433379a3b9b32b105f0cd2f5a3a132f142931c`
  (SHA-1 matches archive.org: `473ba671cc0606bea20b12935718d33beee762ce`;
  MD5 `fabb6ea39793ba3b53e76115e2829b2e`; 45,425,319 bytes)
- Unpacked at `~/bench/apps/Lego_Technic_Racer_1.0` (not in the repo)
- Bundle `dk.spacetimefoam.legotechnicracer`, display name "Lego Technic Racer",
  executable `legotechnicracer`, MinimumOSVersion 4.0, armv7 32-bit, not
  FairPlay-encrypted (`cryptid=0`), portrait, iPhone+iPad, iOS 6 SDK
  (Xcode 4G1004). Screen: 320×568 @2x.
- Binary is **not** identical to Pullback Racers v1.0-4 (10,256,336 vs
  10,248,032 bytes; 2,977 vs 2,974 imports). Same engine version
  (`Initialize engine version: 4.0.1f2`).

## First launch

Fresh home, `./build/hvm run … --keep-going --run-for 30 --gpu --graceful`.
Authentic title: "LEGO TECHNIC PULL BACK", unlocked blue buggy 98, locked
grey car with a padlock, bottom bar with settings gear, play (`>>`) and
help (`?`). PlayerPrefs wrote `ControlScheme=Tilt` and `Racer_MusicOn=1`.
Music audible 4.7–30.1 s, 1,123,539 non-silent of 1,331,200 frames, peak
0.43, one music voice. 768 frames presented, average **5.81 ms**, p99
**14.81 ms**, first frame 4.53 s, no crash, 0 stubs. The same 12
unimplemented imports as Pullback (`execv`, `fork`, `CMSampleBufferGetImageBuffer`,
…); none were called.

## Input and a finished race

`ControlScheme` defaults to Tilt. As with Pullback, pull-back under Tilt
needs a host accelerometer; under **Touch** it is a downward drag on the
car. The play button is at **(162, 521)** pt. A 50 s run from a home that
only pre-seeded `ControlScheme=Touch` and the music/FX flags (no Level
keys), with play taps at 3 s and 6 s and pull-backs at 14 / 18 / 22 s,
reached the authentic finish dialog: timer **00:17:12**, gears **05**,
checkpoint display **01**, car 98, a checkered flag, home and a green
continue arrow. Mid-race screenshots are authentic (HUD, MAX gauge, pause
`||`, collectible gears, orange arrows). PlayerPrefs then had
`Level 1=1` (earned, not pre-seeded) and second-car upgrade keys at 0.

This version shows **all four tracks unlocked** on level-select from the
start (previews and gear score `00`, no padlocks) — unlike Pullback
Racers, where tracks 2–4 start locked. The second title-menu car stays
padlocked; that is a later unlock.

A later 180 s run with the same early taps, and no mid-race steering,
was still on the aisle at in-game timer **02:36:53** (gears 00). The
finish is sensitive to a clean straight pull; leftover left/right holds
are not required and can miss the short track-1 finish, as on Pullback.

## Music

The 8 KiB `pthread_attr_setstacksize` floor from #448 is what lets FMOD
Ex create its file / mixer / stream threads. Without it, `createSound`
fails with FMOD error 33 and the menu is silent. With it, music loops
for the whole session:

- 30 s menu: audible 4.7–30.1 s, peak 0.43.
- 50 s race: audible 5.2–50.2 s, peak 0.47; RMS rises at launch and
  collectible times on top of the loop (effects are present; the mixer
  still reports `effectFrames=0` / `effectVoices=0`, which is a
  classification note, not silence).
- 180 s race: audible **4.6–180.2 s**, 7,747,402 / 7,950,336 non-silent
  (97.4 %), mixer 7,749,632 music frames, one music voice, peak 0.48.
  Every 5 s window after load stays in the same RMS band (~450–700).
  0 dropped audio blocks.

## Saves across relaunch

A 16 s relaunch of the **same** 50 s home (`/tmp/racer-race-home-kt71`)
kept `Level 1=1`, `ControlScheme=Touch`, and the music/FX flags. The
title menu is authentic (second car still padlocked). Tapping play
opened level-select with all four tracks unlocked. Mid-race state is
not saved (expected).

## Gates (Linux bench, this branch)

| Gate | Result |
|---|---|
| `hvm_tests fulltest` | **822 passed, 0 failed** (same tree as #448; re-run on this bench). |
| `hvm_support_tests` | **47 passed, 0 failed**. |
| `lint_stubs.py --baseline tools/stub_lint_baseline.json` | **0 new/changed** (680 unlabelled, unchanged). |
| `pytest -q tools/tests` | **399 passed**, 5 skipped, **1 failed**: the pre-existing #445 `TestCensus` fixture-path failure (`HVM_FIXTURE_ROOT` missing three grandfathered IPAs). Not introduced here, not "fixed". |
| Test Lab | **35 passed, 0 failed** (`--skip-build` for hvm; TestLab.ipa built with `HVM_LD64=/home/box/cctools/bin/arm-apple-darwin11-ld`). |
| `must_launch.py --apps ~/bench/apps --jobs 3` | **3 passed, 0 failed, 34 skipped** (Island HD, Pullback Racers, and this game; other listed folders are not on this bench). |
| `must_launch.py --only Lego_Technic_Racer_1.0 --seconds 30 --record` | **232 frames, picture, sound**, 1 passed. |

No new runtime code and no new tests on this branch; no baseline was
weakened. Required CI on this commit has not been reported green.

## Reproduction

```sh
# IPA unpacked at ~/bench/apps/Lego_Technic_Racer_1.0 (not in the repo)
APP=~/bench/apps/Lego_Technic_Racer_1.0/Payload/legotechnicracer.app
H=$(mktemp -d)
mkdir -p "$H/Library/Preferences"
python3 -c "import plistlib,pathlib,sys; pathlib.Path(sys.argv[1]).write_bytes(plistlib.dumps({'ControlScheme':'Touch','Racer_MusicOn':1,'Racer_FxOn':1}))" \
  "$H/Library/Preferences/dk.spacetimefoam.legotechnicracer.plist"
python3 tools/bench/audio_capture.py --wav /tmp/racer.wav --threshold 300 -- \
  ./build/hvm run "$APP" --home "$H" --keep-going --run-for 50 --gpu --graceful \
    --tap 162,521@3 --tap 162,521@6 \
    --drag 160,330,160,530@14 --drag 160,280,160,520@18 --drag 160,300,160,520@22 \
    --report r.json --screenshot s.png
```

The finish dialog appears around 30 s wall time (in-game **00:17:12**).
Relaunch the same `$H` and `--tap 162,521@5` to see level-select.

`python3 tools/must_launch.py --apps ~/bench/apps --only Lego_Technic_Racer_1.0 --seconds 30 --record`

## Still needed for "playable"

Linux playability criteria are met (menu, authentic picture, pull-back
input, continuous music, 3 min clean session, average frame well under
33 ms, `Level 1=1` across a relaunch, must_launch recorded). The second
title-menu car is still locked — a later unlock, not a missing save.
This version already shows tracks 1–4 unlocked on a fresh finish of
track 1 (and on first open of level-select).

- Phone: Mesa vs device GPU, real speed, 16 KB pages, headphones.
- Load-scene hitches (max 878 ms in the 180 s run, all in the first
  ~10 s) are noted; steady-state p99 is 18 ms.

## Phone-only / not checked

No iPhone run. This game does not use the microphone.
