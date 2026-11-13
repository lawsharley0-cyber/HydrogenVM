# LEGO Technic Pullback Racers v1.0-4 — Linux bench, 13 November 2026

In progress. Two shared runtime fixes get the unencrypted Unity 4.0.1f2 IPA
to its authentic main menu. Starting a race, sound and a full playability
pass are not done yet.

## IPA

- Source: [Various Testflight Teraleak ipas](https://archive.org/details/various-testflightleak-ipas),
  first file in the metadata listing:
  `Lego Technic Pullback Racers v1.0-4 [TF 7b05f4d25353d9440e4ff6374a89267].ipa`
- SHA-256: `2e8c7ae270c96366a2014009750408a4b448806643090fa5f5d7d6e07a71ab08`
  (SHA-1 matches archive.org: `4f977bda7375c0552ab326392c082620d888fc7b`)
- Unpacked at `~/bench/apps/Lego_Technic_Pullback_Racers_1.0-4` (not in the repo)
- Bundle `dk.spacetimefoam.legotechnicracer`, display name "Lego Technic - PULL BACK racers!",
  version 1.0, minimum iOS 4.0, SDK 6.0, `UIDeviceFamily` [1, 2], portrait, armv7 thin, `cryptid` 0
- Unity 4.0.1f2 (Mono), `Data/mainData` is a valid 13,992-byte serialized file whose
  version string at offset 20 is `4.0.1f2`

No existing PR or branch already covered this IPA. Work is on current main
(`6917fc2`) in `codex/lego-technic-pullback-racers`. This collection does not
include Super Hexagon or Canabalt (#444 / #437 were left alone).

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

## Sound (not done)

`Racer_MusicOn` is on. The menu loop is a real MP3 in
`Data/sharedassets0.assets.resS` ("LEGO Racer Menu Loop", 14.24 s, 44.1 kHz
stereo; ffmpeg peak 13639). The first 60 s run was silent (`mixer` peak 0)
and opened/disposed an AudioQueue every presented frame.

Cause of the spin: after `AudioQueueStart`, Unity asks
`AudioFileGetProperty` for `kAudioFilePropertyFrameToPacket` (`'frpk'`).
That property was unsupported (`'pty?'`), so Unity abandoned the decode.

Fix 2: implement `'frpk'` and `'pkfr'` (`AudioFramePacketTranslation`, 20
bytes on 32-bit) for MP3 (1152 frames/packet) and decoded PCM (1:1).
Fail-first test on the existing MP3 tone fixture: frame `1152*3+50` maps to
packet 3, offset 50; packet 5 maps to frame 5760. The test passes
(fulltest **815 passed, 0 failed** after both fixes).

After this, Unity does `AudioFileReadPackets` (15+15), `EnqueueOffline` and
four `AudioQueueOfflineRender` calls of 4096 frames each — then still
disposes and repeats every frame. The mixer and a captured WAV stayed at
peak 0. The 24 kHz output AudioUnit also reported `PcmNonzero=0`. This
remaining silence is not fixed here.

A play-button tap at (162, 521) at 3 s (90 s run) changed the last frame
(different screenshot hash) but that run's screenshot was lost when the
bench's build directory was cleaned; the next session should confirm the
level-select / race screen and keep working on audio.

## Gates (Linux bench, this branch, before the toolchain regression)

| Gate | Result |
|---|---|
| `hvm_tests fulltest` | **815 passed, 0 failed** (two new checks: Unity string imports, FrameToPacket; string test failed first on unfixed main) |
| support / stub / pytest / Test Lab / must_launch | not re-run after the second fix; the string-only tree had previously passed support 47, stub 0 new/changed, Test Lab 35, must_launch 1/0/34. Pytest still has the pre-existing #445 `TestCensus` fixture-path failure on this bench. |

No baseline was weakened. Required CI on this commit has not been reported green.
The IPA is not recorded in `must_launch` (not fully playable). `docs/COMPAT.md`
is left for when the verdict is settled (and to avoid a clash with #447's CoC row).

## Still needed for "playable"

- Why offline-rendered menu music never reaches the mixer (dispose-every-frame
  loop after a successful 4096-frame render)
- Tap through play → level select → start a race; pull-back / tilt input
- Three minutes without a crash, saves across relaunch, confirm sound and music
- Phone: Mesa vs device GPU, real speed, 16 KB pages, headphones

## Phone-only / not checked

No iPhone run. This game does not use the microphone.
