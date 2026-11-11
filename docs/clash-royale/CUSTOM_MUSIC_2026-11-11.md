# Custom menu music

The Music editor under Settings, Mod Lab imports a local PCM WAV or MP3, previews it, assigns it to the menu and restores the original menu soundtrack. Imports are limited to 8 MB and three minutes. Duration is inspected from PCM WAV or MPEG Layer III frame headers before decoding, with a second decoder validation afterward. Restart Clash after saving or restoring an assignment. A guest update and the Hydrogen build with document-picker support are required for iPhone Files import. Keep the existing guest instance.

MenuMusic has three original filenames. The guest redirects AudioFileOpenURL and ExtAudioFileOpenURL only for those exact bundled menu paths. The assigned audio lives in Documents/ClashMusic-v1 and survives guest updates. The generated menu CSV enables the native loop flag and removes its fade-out, while retaining the original play count and duration fields. TutorialMusic, battle tracks and sound effects keep their existing playback routes. Subsequent work covers battle and result assignments, playlists, volume controls and more formats.

The MUC1 assignment is five little-endian words containing magic, generation, file size, file checksum and header checksum. Tracks are immutable generation files. Import copies data in chunks, flushes and fsyncs the track, validates its complete checksum and then publishes menu.bin with flush, fsync and rename. Cancellation and rejected imports keep the previous candidate and assignment. Restore publishes a zero generation and retains the audio files.

## Shared runtime

UIDocumentPickerViewController supports single-file import mode through a host observer. Results are copied into the guest's tmp folder and delivered to the delegate on the guest main loop. Request IDs guard stale and duplicate deliveries. Unsupported modes and failed copies deliver cancellation. The iPhone uses the system Files picker with security-scoped access and a temporary copy that remains available after the picker closes. Keep that copy alive until the guest main-loop import can read it. Host temporary copies and guest immutable generations currently consume retained storage. The diagnostic CLI uses --document-import PATH or --document-import cancel.

AVAudioPlayer prepareToPlay now checks for playable decoded samples. The Music editor uses that result so a duration estimate for an unsupported file cannot qualify it for selection.

## Validation

Before shared edits, 690 fulltest checks and 26 Test Lab cases passed on the earlier engine baseline. After integrating main a082288 and the Files/audio changes, **756 fulltest checks passed**. **83 focused guest checks passed**, including actual C profile-reader rejection/preservation and authored pre-decode duration boundaries. The controller discovery test now pumps to its existing 50 ms deadline because one runOnce can return for unrelated work before the timer fires; its outcome checks and deadline remain intact.

Native GPU/touch/audio probes used copied earned homes. A generated 12-second 660 Hz PCM WAV imported and produced the expected immutable bytes and MUC1 checksums. Preview reached about 6996 amplitude in captured PCM. A separate native menu launch reached about 4888 after the game's volume setting. MP3 import, cancellation, invalid-file rejection and restore passed. An initial short capture missed the native timing gap after the short track ended. The final overlay's long test kept the authored signal present through more than a minute of settled menu playback, with a single native file open. The intermediate overlay that blanked count/duration caused rapid variant changes and was discarded. The final overlay preserves those original fields. Restore on the final package saved generation zero.

Test Lab passed 25/26 on the integrated engine. TL-J's 50 MFLOPS floor remains unmet locally. Isolated measurements on the same test IPA were 25.5 on the earlier runtime and 23.8 on the integrated engine. Retain this failure; no threshold was changed. The other 25 cases passed. Physical iPhone Files presentation, interruption/lifecycle listening, and audio quality remain unqualified.

The checked-in music-import.json and music-restore.json plans use tools/clash_guest_ui_probe.py, a private copied home, GPU, audio capture and readiness log GuestOffline: queued response=24101. Supply an hvm wrapper appending --document-import PATH to select an authored WAV/MP3; use cancel to test cancellation. Relaunch independently, inspect the native opening log and measure the captured signal against the authored input.

## Packages and continuation

[PR #414](https://github.com/lawsharley0-cyber/HydrogenVM/pull/414) continues closed #391 and includes the current main audio/graphics/video/frame-pacing fixes. Use the same guest instance and replace Hydrogen in place.

- Guest package SHA-256: c2763de2a897d13f108a24de98d5279e19e3f674becc36f4d9999c63e51e489a.
- [Hydrogen device artifact](https://github.com/lawsharley0-cyber/HydrogenVM/actions/runs/43098644254/artifacts/13889649959), SHA-256 181811deceb9a73b530062e7e5e06e12e5f2ba1fae61b8fa56ed9f071fcc0220. Its manifest records GitHub test-merge dfdf6c2838559c463738bab870f46425b7b62b7f. Its parents are main a082288 and branch 665de50; the tree matches branch 665de50 exactly. The final menu-loop and duration-inspection changes are guest-only and use this host.
- Device packaging and Linux CI passed for that host. Broader Apple runtime/simulator validation was still running at the last check. A GitHub Actions outage delayed startup.

Next verify Files import and actual listening on iPhone, then add battle/result assignments, volume controls, track names and playlists. Keep original training music while extending the battle context, since the native tutorial and battle groups share a filename.
