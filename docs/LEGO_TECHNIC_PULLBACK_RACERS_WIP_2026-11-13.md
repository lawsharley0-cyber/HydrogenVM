# LEGO Technic Pullback Racers v1.0-4 — Linux bench, 13 November 2026

In progress. Two shared runtime fixes get the unencrypted Unity 4.0.1f2 IPA
to its authentic main menu, level-select and race start. Continuous music
and the pull-back launch gesture are not done yet; the IPA is not fully
playable. Draft PR: https://github.com/lawsharley0-cyber/HydrogenVM/pull/448

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

## Sound (still not continuous)

After FrameToPacket the FMOD offline-MP3 path really renders: each cycle is
`AudioQueueNewOutput`, a 0-frame prime `OfflineRender` (FMOD then
`AudioFileReadPacketData` + `Enqueue` of 15+15 packets from packet 0), four
successful `AudioQueueOfflineRender` calls of 4096 frames, then
`AudioQueueDispose`. In a 28 s race run that cycle ran 184 times
(~6.5 / s). Menu-only runs stay at mixer peak 0.

Starting the race produces a short, real burst: mixer peak **0.315521**,
`musicVoices=1`, 6144–8192 audible / music frames (about 0.26–0.34 s at the
24 kHz AudioUnit), then silence. A captured WAV of an earlier 20 s run was
all zeros because that run never left the menu. ffmpeg can decode the asset,
so this is not "AAC-only / phone-only".

FMOD's decode loop (ARM `0x57e24c`) copies `OfflineRender` output into the
codec buffer. `AudioQueueDispose` + recreate at `0x57e350` is stall-recovery
when a later `OfflineRender` returns `mAudioDataByteSize == 0` and the flush
flag at codec `+0x170` is clear. Every cycle restarts `ReadPacketData` at
packet 0; the file is never consumed past the first 30 packets (~0.78 s of
source). `'pnfo'` reports 627840 valid frames (14.24 s), which is correct.

## Pull-back gesture (not launching)

Three slow 1.5 s downward pulls were delivered in a 28 s race
(`--input-file`, 123 `touch` events, logged): starting on the car body
(160, 280), the rear bumper (160, 330) and the hood (160, 240), each
dragged to y ≈ 500–530. The timer stayed `00:00:00`, gear stayed `00`, and
the tutorial hand was still pointing at the car at the end of the run.
An earlier same-frame `--drag` (10 steps over 160 ms) also did nothing.

Menu and level-select taps work (Unity OnGUI / `GetMouseButton`). The
C# side (`WindUp`, `PullbackZone`, `TiltControl`) uses `GetMouseButton`,
`GetTouch`, `ScreenPointToRay` and `PullbackZone.OnTriggerEnter`.
`ControlScheme=Tilt` is the default (steering after launch). The IL in
`Assembly-CSharp.dll` is AOT-stripped, so those methods live only as ARM
in the executable. This is still an open general-accuracy question
(Unity Input.touches vs mouse in the 3D scene, or a physics trigger),
not a game-specific hack.

## Gates (Linux bench, this branch)

| Gate | Result |
|---|---|
| `hvm_tests fulltest` | **815 passed, 0 failed** (two new checks: Unity string imports, FrameToPacket; string test failed first on unfixed main). Re-run after restoring the bench toolchain (ZLIB, ICU, SQLite3, OpenSSL 3, FFMPEG, libstdc++ 14 headers). |
| support / stub / pytest / Test Lab / must_launch | not re-run after the second fix; the string-only tree had previously passed support 47, stub 0 new/changed, Test Lab 35, must_launch 1/0/34. Pytest still has the pre-existing #445 `TestCensus` fixture-path failure on this bench — not "fixed" in this PR. |

No baseline was weakened. Required CI on this commit has not been reported green.
The IPA is not recorded in `must_launch` (not fully playable). `docs/COMPAT.md`
is left for when the verdict is settled (and to avoid a clash with #447's CoC row).

## Still needed for "playable"

- Why FMOD disposes the queue after four 4096-frame offline renders and
  retries from packet 0, so only a ~0.3 s burst reaches the mixer at
  race start (menu stays silent)
- Get the pull-back (or tilt) gesture to charge and launch the car
- Three minutes without a crash once the car is moving; race-progress
  saves across relaunch; confirm music and effects for the whole race
- Phone: Mesa vs device GPU, real speed, 16 KB pages, headphones

This is not a blocked verdict: both remaining gaps look like general
iOS-accuracy work (AudioQueue / FMOD offline playback, Unity 4 input or
physics in the 3D scene). Set-3 IPA 2 (Lego Technic Racer) is not started.

## Phone-only / not checked

No iPhone run. This game does not use the microphone.
