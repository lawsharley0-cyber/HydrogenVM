# LEGO Technic Pullback Racers v1.0-4 — Linux bench, 13 November 2026

In progress. Two shared runtime fixes get the unencrypted Unity 4.0.1f2 IPA
to its authentic main menu, level-select and race start. The pull-back
launch works (Touch scheme, or Tilt with a host accelerometer), and a
180 s race ran cleanly (in-game timer 02:40:40). Continuous music is
still open — the IPA is not yet recorded as fully playable. Draft PR:
https://github.com/lawsharley0-cyber/HydrogenVM/pull/448

## IPA

- Source: [Various Testflight Teraleak ipas](https://archive.org/details/various-testflightleak-ipas),
  first file in the metadata listing (archive.org metadata API, six IPAs, no
  Super Hexagon or Canabalt — #444 / #437 were left alone):
  `Lego Technic Pullback Racers v1.0-4 [TF 7b05f4d25353d9440e4ff6374a89267].ipa`
- SHA-256: `2e8c7ae270c96366a2014009750408a4b448806643090fa5f5d7d6e07a71ab08`
  (SHA-1 matches archive.org: `4f977bda7375c0552ab326392c082620d888fc7b`)
- Unpacked at `~/bench/apps/Lego_Technic_Pullback_Racers_1.0-4` (not in the repo)
- Bundle `dk.spacetimefoam.legotechnicracer`, display name "Lego Technic - PULL BACK racers!",
  version 1.0, minimum iOS 4.0, SDK 6.0, `UIDeviceFamily` [1, 2], portrait, armv7 thin, `cryptid` 0
- Unity 4.0.1f2 (Mono AOT; `Assembly-CSharp.dll` method bodies are stripped `ret`,
  the game code is native ARM in the executable), `Data/mainData` is a valid
  13,992-byte serialized file whose version string at offset 20 is `4.0.1f2`

No existing PR or branch already covered this IPA. Work is on
`codex/lego-technic-pullback-racers` (from `6917fc2`).

## First launch (unfixed main)

```sh
H=$(mktemp -d)
./build/hvm run ~/bench/apps/Lego_Technic_Pullback_Racers_1.0-4/Payload/legotechnicracer.app \
  --home "$H" --keep-going --run-for 60 --gpu --graceful \
  --report r.json --screenshot s.png
```

Exited in 0.20 s with no frame presented. The engine printed:

```
Invalid serialized file version. File: ".../Data/mainData". Expected version: 4.0.1f2. Actual version: .
Failed to read file '.../Data/mainData' because it is corrupted.
PlayerInitEngineNoGraphics settings: Could not preload global game manager #0
   engine intialization failed
```

The file itself is not corrupt. The game imports out-of-line GNU libstdc++
`std::string` members that HydrogenVM did not implement; the first two called
were `_M_check_length` and `_M_data`. The stub for `_M_data` returned 0, so the
version string compared empty. On real iOS these are libstdc++.6.dylib.

## Fix 1: out-of-line GNU `std::string` members

The existing COW implementation in `LibStdcxxContainers.cpp` already covers
the usual constructors, `data()`/`c_str()`, `_Rep::_S_empty_rep` and most
mutators. This Unity 4 build also imports (and the first launch listed as
unimplemented):

- `std::string::_M_data()` — the data pointer, same as `data()`
- `std::string::_M_check_length(n1, n2, s)` — throw `length_error` if
  `max_size() - (size() - n1) < n2`
- `std::string::_S_empty_rep()` — same storage as `_Rep::_S_empty_rep()`
- `_S_copy_chars` (raw pointers and `__normal_iterator`; same ABI)
- `insert(pos, const string&)`
- `replace(iterator, iterator, const_iterator, const_iterator)`

Fail-first test next to the existing Army of Darkness out-of-line checks:
`_M_data` returns `"4.0.1f2"`, a legal `_M_check_length` succeeds and one
past `max_size` throws, `_S_empty_rep` matches `_Rep`'s, `_S_copy_chars`
copies, `insert`/`replace` with a string and iterators change the text.
The test failed on unfixed main (`data 0 check 0 …`, 813 passed / 1 failed)
and passes after the fix.

## After the string fix (60 s, fresh home)

No crash, 1760 frames presented, average 7.18 ms, p99 17.56 ms, first frame
at 1.01 s, graceful "The app saved and closed". Screenshot 640×1136 (320×568
@2x, portrait): authentic title "LEGO TECHNIC PULL BACK", an unlocked blue
buggy, a locked grey car, and a bottom bar with settings, play (`>>`) and
help. `PlayerPrefs` wrote `ControlScheme=Tilt` and `Racer_MusicOn=1`. Zero
stub hits. 12 unimplemented imports remain unused (`fork`, `execve`, …,
`CMSampleBufferGetImageBuffer`).

## Fix 2: AudioFile FrameToPacket / PacketToFrame

`Racer_MusicOn` is on. The menu loop is a real MP3 in
`Data/sharedassets0.assets.resS` ("LEGO Racer Menu Loop", 14.24 s, 44.1 kHz
stereo, LAME 3.98r; ffmpeg peak 13639). The first 60 s run was silent
(`mixer` peak 0) and opened/disposed an AudioQueue every presented frame.

Cause of the spin: after `AudioQueueStart`, Unity's FMOD Ex codec
(`fmod_codec_audioqueue.cpp` at ARM `0x57d8bc`–`0x57e620`) asks
`AudioFileGetProperty` for `kAudioFilePropertyFrameToPacket` (`'frpk'`).
That property was unsupported (`'pty?'`), so Unity abandoned the decode.

Fix 2: implement `'frpk'` and `'pkfr'` (`AudioFramePacketTranslation`, 20
bytes on 32-bit) for MP3 (1152 frames/packet) and decoded PCM (1:1).
Fail-first test on the existing MP3 tone fixture: frame `1152*3+50` maps to
packet 3, offset 50; packet 5 maps to frame 5760. The test passes
(fulltest **815 passed, 0 failed** after both fixes, re-confirmed after the
bench toolchain restore).

## Play through: menu → level-select → race start

`--tap 162,521@3` (centre of the orange `>>` play button) opens an authentic
level-select (15 s run, 429 frames, avg 6.40 ms, p99 16.38 ms, mixer still
silent): Back and Home at the top left, four warehouse-track cards, level 1
unlocked (preview + gear score `00`), levels 2–4 locked with padlocks, a
scrollbar, and the same orange `>>` at the bottom.

A second `--tap 162,521@6` starts the race (~7.6 s): authentic 3D warehouse
floor, the blue #98 buggy facing up the aisle, HUD with timer `00:00:00`,
gear `00`, `03` (checkpoints / laps), a MAX gauge, and a pause `||`. A
translucent tutorial hand points at the rear bumper (the pull-back prompt).
`PlayerPrefs` also wrote `Racer_FxOn=1`. A 45 s run presented 1309 frames,
average 7.36 ms, p99 20.45 ms, first frame 639 ms, no crash, zero stub hits.

## Sound (short event bursts; music still not continuous)

A fail-first AudioQueue fixture modelled on FMOD's sequence (21-packet
tone MP3, output callback reading 15 packets at a time, 0-frame prime,
then 4096-frame `OfflineRender` chunks) **passes**: the first four fills
are 4096 frames, the fifth is nonzero, the total is the whole file, and
`AudioQueueFlush` is called at EOF. So a later `AudioQueueOfflineRender`
does **not** return `mAudioDataByteSize == 0` after the fourth fill.
Apple semantics checked against that loop: offline render pulls from
enqueued buffers; the output callback is invoked when a consumed input
buffer can be reused (FMOD fills and enqueues the same buffer); a
0-frame prime does not itself invoke the callback (FMOD calls it
manually — a second call would double-enqueue); `mAudioDataByteSize` on
the render buffer is the PCM produced, not the input size; the inTimestamp
is ignored (FMOD passes a pointer to its running `mSampleTime` double).
`AudioQueueFlush` is still a no-op (only needed at true EOF to drain
decoder delay, a few frames).

What the game actually does, from `HVM_AUDIO_TRACE` (20 s menu, 579
identical cycles): `AudioQueueNewOutput`, 0-frame prime, two
`ReadPacketData` (packets 0–14 and 15–29), four successful 4096-frame
renders (16384 frames, FMOD's stream prefetch), then `AudioQueueDispose`
on the codec close path (`0x57e170`), and a full `AudioFileOpenWithCallbacks`
retry of the same 261,787-byte / 545-packet "LEGO Racer Menu Loop". That
is 5.0 OfflineRenders per NewOutput (1 prime + 4 fills) — **no** fifth
render with `done == 0`. Dispose is FMOD closing after a 16384-frame
prefetch (~0.37 s at 44.1 kHz), not stall-recovery at `0x57e350`.

FMOD's RemoteIO output runs at 24 kHz and is pumped at the right rate;
on the menu its render callback is all zeros (`AudioUnitPcmNonzero` 0),
so no sound is playing. Short AIFF effects (`wind-up*`, `pickup*`,
`m_Stream=1`, `m_UseHardware=False`, in `resources.assets` /
`sharedassets2`) do play: they fit in one 16384-frame prefetch, so
createSound completes and the mixer hears them. Long MP3 loops (menu
14.24 s, race "LEGO Racer Main Music") never stay open, so they never
play through.

A 180 s Touch-scheme race (WAV captured) had only ~2.9 s of audible
audio, in bursts at 6.4 s (level-start sting), 18.4–19.9 s (wind-up /
launch) and 25.5–28.4 s (collectible / race FX), then silence. Mixer
peak 0.405914, `audibleFrames` 129024, `musicVoices` 1. Menu stays
silent. This is not "AAC-only / phone-only" (ffmpeg peak 13639 on the
real menu MP3, audible from frame 0). Continuous music is still the
open audio question; the IPA is not recorded in `must_launch` or
`COMPAT.md`.

## Pull-back launch (works)

`ControlScheme` defaults to `Tilt` (PlayerPrefs). Under that default,
with only the static upright gravity `{0, -1, 0}` that UIAccelerometer
reports until the host sends a reading, touch drags — including a 1.5 s
pull starting on the tutorial hand at the rear bumper (160, 330) — did
**not** launch.

Two general ways to launch:

1. **Touch scheme.** The settings gear on the menu is at **(66, 521)**
   pt (left of the orange `>>`). The dialog shows Tilt (phone with roll
   arrows, default check) and Touch (two hands). Tapping the Touch card
   at **(230, 250)** moves the check and writes `ControlScheme=Touch`.
   The same 1.5 s downward pulls then launch. A first pull at 14 s may
   miss; a second at 18–19.7 s starting on the car body (160, 280) or
   the rear bumper launches. The drag does **not** have to start on the
   bumper once the scheme is Touch. A 28 s run reached timer **00:09:03**,
   gear **02**, checkpoints **02**, with a collectible in the air, no
   tutorial hand.

2. **Tilt scheme with a host accelerometer.** `hvm run --input-file`
   now accepts `accel X Y Z` (UIAcceleration, in g). Under default
   `ControlScheme=Tilt`, accel-only pitch (upright → 90° back → snap
   forward, twice) did not launch. The same touch pull at 18 s **while**
   the accelerometer was pitched back (`accel 0 -0.3 -0.95`) **did**
   launch: 25 s run, timer **00:05:36**, checkpoints **02**, no tutorial
   hand, the car on the warehouse aisle. Tilt launch needs a real host
   reading (not the static upright default) **and** the pull-back drag.
   Tilt steering after launch is the left/right roll the settings icon
   shows.

Unity's touch path is fine for this game: menu and level-select taps
use OnGUI / `GetMouseButton`; the race pull uses the same
`--input-file` `touch` phases (`0` began, `1` moved, `3` ended),
`deltaPosition` and timestamps. `GetMouseButton` emulation is not the
blocker. Physics `OnTriggerEnter` / `DoPullZoneCheck` succeed once the
control scheme and (for Tilt) accelerometer are right. Fail-first tests
cover `parseAccel` / `applyAccel` (three finite floats, signs, rejected
malformed lines, and a valid line reaching `UIState` as
`accelFromHost`).

## Three-minute play and saves

180 s Touch-scheme run (`--run-for 180 --gpu --graceful`, pulls at 14
and 18 s, occasional left/right holds after 28 s): **no crash**, still
running, 5489 frames presented, average **6.99 ms**, p99 **17.83 ms**,
max 136.6 ms (a load hitch), 0 stub hits. Final screenshot: authentic
warehouse, timer **02:40:40**, gear **03**, checkpoints **02**, a
collectible in the air, car still on the aisle. In-game time matches
wall time minus the ~19 s launch (180 − 19 ≈ 161 s ≈ 2:41).

`PlayerPrefs` (`ControlScheme`, `Racer_MusicOn`, `Racer_FxOn`, `Level 0`
… `Level 9`) survive a relaunch of the same home: a 6 s run from the
180 s home loaded `ControlScheme=Touch` and the same unlocks, and
showed the authentic title menu. Level 1 stays 0 — the 180 s run did
not finish the first track, so there is no earned-unlock evidence yet.
Mid-race state is not saved (expected).

## Gates (Linux bench, this branch)

| Gate | Result |
|---|---|
| `hvm_tests fulltest` | **818 passed, 0 failed** (FMOD 4096-frame offline-render loop; `accel` parse and UIKit delivery). Earlier 815/0 after the two runtime fixes; 816/0 after the FMOD-loop test alone. Re-run after the accelerometer input. |
| support / stub / pytest / Test Lab / must_launch | not re-run after these commits. Pytest still has the pre-existing #445 `TestCensus` fixture-path failure on this bench — not "fixed" in this PR. |

No baseline was weakened. Required CI on this commit has not been reported green.
The IPA is not recorded in `must_launch` (music is not continuous). `docs/COMPAT.md`
is left for when the verdict is settled (and to avoid a clash with #447's CoC row).

## Still needed for "playable"

- Continuous music: FMOD's AudioQueue codec closes after a 16384-frame
  prefetch of each long MP3 and retries ~6.5 / s; only short AIFF effects
  reach the mixer. OfflineRender itself is not returning zero. A general
  iOS-accurate reason FMOD's createSound does not keep the stream open is
  still wanted.
- Earned level-unlock across a relaunch (finish track 1).
- Phone: Mesa vs device GPU, real speed, 16 KB pages, headphones.

This is not a blocked verdict: the remaining music gap is general
AudioQueue / FMOD stream work. Set-3 IPA 2 (Lego Technic Racer) is not
started.

## Phone-only / not checked

No iPhone run. This game does not use the microphone.
