# Custom menu and battle music

The Music editor under Settings, Mod Lab imports a local PCM WAV or MP3, previews it, assigns it independently to the menu or arena battles and restores each original soundtrack. Imports are limited to 8 MB and three minutes. Duration is inspected from PCM WAV or MPEG Layer III frame headers before decoding, with a second decoder validation afterward. Restart Clash after saving or restoring an assignment. A guest update and the Hydrogen build with document-picker support are required for iPhone Files import. Keep the existing guest instance.

MenuMusic has three original filenames. The guest redirects AudioFileOpenURL and ExtAudioFileOpenURL only for those exact bundled menu paths. The assigned audio lives in Documents/ClashMusic-v1 and survives guest updates. The generated menu CSV enables the native loop flag and removes its fade-out, while retaining the original play count and setting duration to the validated track length rounded up to seconds. TutorialMusic and sound effects keep their existing playback routes. Battle routing is described below. Subsequent work covers result assignments, playlists, volume controls and more formats.

The MUC1 assignment is five little-endian words containing magic, generation, file size, file checksum and header checksum. Tracks are immutable generation files. Import copies data in chunks, flushes and fsyncs the track, validates its complete checksum and then publishes the selected menu.bin or battle.bin with flush, fsync and rename. Cancellation and rejected imports keep the previous candidate and assignment. Restore publishes a zero generation and retains the audio files.

## Shared runtime

UIDocumentPickerViewController supports single-file import mode through a host observer. Results are copied into the guest's tmp folder and delivered to the delegate on the guest main loop. Request IDs guard stale and duplicate deliveries. Unsupported modes and failed copies deliver cancellation. The iPhone uses the system Files picker with security-scoped access and a temporary copy that remains available after the picker closes. Keep that copy alive until the guest main-loop import can read it. Host temporary copies and guest immutable generations currently consume retained storage. The diagnostic CLI uses --document-import PATH or --document-import cancel.

AVAudioPlayer prepareToPlay now checks for playable decoded samples. The Music editor uses that result so a duration estimate for an unsupported file cannot qualify it for selection.

## Validation

Before shared edits, 690 fulltest checks and 26 Test Lab cases passed on the earlier engine baseline. After integrating main a082288 and the Files/audio changes, **756 fulltest checks passed**. **83 focused guest checks passed**, including actual C profile-reader rejection/preservation and authored pre-decode duration boundaries. The controller discovery test now pumps to its existing 50 ms deadline because one runOnce can return for unrelated work before the timer fires; its outcome checks and deadline remain intact.

Native GPU/touch/audio probes used copied earned homes. A generated 12-second 660 Hz PCM WAV imported and produced the expected immutable bytes and MUC1 checksums. Preview reached about 6996 amplitude in captured PCM. A separate native menu launch reached about 4888 after the game's volume setting. MP3 import, cancellation, invalid-file rejection and restore passed. An initial short capture missed the native timing gap after the short track ended. The final overlay's long test kept the authored signal present through more than a minute of settled menu playback, through several native playlist cycles. The intermediate overlay that blanked count/duration caused rapid variant changes and was discarded. The final overlay retains the original play count and uses the validated track duration. Restore on the final package saved generation zero.

Test Lab passed 25/26 on the integrated engine. TL-J's 50 MFLOPS floor remains unmet locally. Isolated measurements on the same test IPA were 25.5 on the earlier runtime and 23.8 on the integrated engine. Retain this failure; no threshold was changed. The other 25 cases passed. Physical iPhone Files presentation, interruption/lifecycle listening, and audio quality remain unqualified.

The checked-in music-import.json and music-restore.json plans use tools/clash_guest_ui_probe.py, a private copied home, GPU, audio capture and readiness log GuestOffline: queued response=24101. Supply an hvm wrapper appending --document-import PATH to select an authored WAV/MP3; use cancel to test cancellation. Relaunch independently, inspect the native opening log and measure the captured signal against the authored input.

## Packages and continuation

[PR #414](https://github.com/lawsharley0-cyber/HydrogenVM/pull/414) continues closed #391 and includes the current main audio/graphics/video/frame-pacing fixes. Use the same guest instance and replace Hydrogen in place.

- Guest package SHA-256: eca6d344e6c46eb2715c35240b301a3c956b15b8bc281f059be57dbc484a1dee.
- [Hydrogen device artifact](https://github.com/lawsharley0-cyber/HydrogenVM/actions/runs/43102825067/artifacts/13892913154), SHA-256 74ce20db2b9e02571f71f2108e1882c6e547ebb33a5012b8dfa58b8c4a927203. Manifest source is 1978e8b07f126814a79344443505ae2255f6692d. Archive checks and downloaded hash passed locally. Later track-duration changes are guest-only and use this host.
- The host device package passed. Broader Apple/Linux validation for that head was still running at the last check. Previous host 665de50's Linux CI passed. A GitHub Actions outage delayed startup.
- Final native menu-to-battle transition selected the original BattleMusic group. The custom tone dropped out of the combat capture while sound effects continued; this is a routing/signal check, with device listening still open.

Next verify Files import and actual listening on iPhone, then add result assignments, volume controls, track names and playlists. Preserve original training in every context.

## Battle music continuation

The Music editor now has Menu and Battle selectors. Import, preview, Apply and Restore affect the selected context. An imported candidate stays available when switching contexts so it can be assigned to both; switching stops its preview. Restart Clash after Apply or Restore. Legacy menu.bin remains compatible. The separate battle.bin uses the same MUC1 framing and immutable track store. Missing or corrupt optional assignments fall back independently without deleting files or changing progression.

The pinned client shares its first bundled filename between TutorialMusic and BattleMusic. The generated CSV therefore gives only BattleMusic three unique modlab_battle_N aliases. The guest audio-open wrappers redirect those aliases to the battle assignment only after seven earned training wins and outside friendly battles. If an alias is encountered in training/friendly mode it resolves to the corresponding original bundled battle file. TutorialMusic rows and all battle playback metadata stay original. No files are added to the installed game's asset bundle. Four generated CSV variants cover neither, menu only, battle only and both assignments.

Native GPU/touch/audio validation used a copied earned instance and authored 12-second PCM tones: legacy menu assignment 660 Hz, imported battle assignment 880 Hz. The battle Apply produced generation two and exact imported bytes while keeping menu.bin byte-identical. Relaunch reached an arena battle through the native UI and opened the aliases through AudioFileOpenURL/ExtAudioFileOpenURL. The 880 Hz signal remained present through the last 34 analyzed seconds of a 75-second capture, spanning multiple track lengths; the menu signal no longer dominated combat. This establishes routing and sustained delivery, not phone listening quality. The native screenshot showed the battle and card hand normally.

Restore then published battle generation zero without changing menu.bin; another independent native launch opened the custom menu track and the original BattleMusic group, with no custom battle opening.

A separate empty home containing both music assignments reached Trainer Earl, the original welcome dialogue and TrainingArena's original soundtrack with zero wins. No custom battle opening occurred. All 83 focused guest checks passed, including four generated CSV variants, exact aliases, earned-training/friendly gating, independent corrupt-header fallback, legacy profile reading and duration bounds. This stage changes guest code only; it reuses the previously qualified host build. The earlier local Test Lab throughput failure and phone audio/lifecycle qualification remain open.

Reproduce with tools/clash_guest/qa/music-battle-import.json and music-battle-restore.json through the normal GPU/audio UI probe and a private copied home. Supply --document-import through a wrapper. Compare both MUC1 manifests and imported bytes, relaunch independently into combat, inspect the native music-opening log, and measure the authored frequency in mixed.wav after the combat step. Repeat from a fresh training home with the optional music directory copied in. Never modify an earned user instance for a fixture.

Battle guest package SHA-256: 0a3c60f7319ab7ddbde25f5f154da063bbcf8dfac684366a85668feb0217cc5f. Package name: Clash-Offline-Guest-Music-v2.ipa. It uses the same guest bundle ID and the existing Files-capable Hydrogen build above. Result themes, playlists, per-context volume and physical iPhone Files/listening checks remain pending.
