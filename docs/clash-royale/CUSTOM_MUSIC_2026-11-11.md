# Custom menu music

The Music editor under Settings, Mod Lab imports a local WAV or MP3, previews it, assigns it to the menu and restores the original menu soundtrack. Imports are limited to 8 MB and three minutes. Restart Clash after saving or restoring an assignment. A guest update and the Hydrogen build with document-picker support are required for iPhone Files import. Keep the existing guest instance.

MenuMusic has three original filenames. The guest redirects AudioFileOpenURL and ExtAudioFileOpenURL only for those exact bundled menu paths. The assigned audio lives in Documents/ClashMusic-v1 and survives guest updates. TutorialMusic, battle tracks and sound effects keep their existing playback routes. Subsequent work covers battle and result assignments, playlists, volume controls and more formats.

The MUC1 assignment is five little-endian words containing magic, generation, file size, file checksum and header checksum. Tracks are immutable generation files. Import copies data in chunks, flushes and fsyncs the track, validates its complete checksum and then publishes menu.bin with flush, fsync and rename. Cancellation and rejected imports keep the previous candidate and assignment. Restore publishes a zero generation and retains the audio files.

## Shared runtime

UIDocumentPickerViewController supports single-file import mode through a host observer. Results are copied into the guest's tmp folder and delivered to the delegate on the guest main loop. Request IDs guard stale and duplicate deliveries. Unsupported modes and failed copies deliver cancellation. The iPhone uses the system Files picker with security-scoped access and a retained temporary copy. The diagnostic CLI uses --document-import PATH or --document-import cancel.

AVAudioPlayer prepareToPlay now checks for playable decoded samples. The Music editor uses that result so a duration estimate for an unsupported file cannot qualify it for selection.

## Validation

Before shared edits, 690 fulltest checks and 26 Test Lab cases passed. The first guest build passed 80 focused checks. An additional authored test executes the actual music profile reader with valid, truncated, trailing, checksum-damaged and missing-track inputs, checking that rejected files remain intact and only the menu paths match.

Native GPU/touch/audio probes used a copied earned home. A generated 12-second 660 Hz WAV imported and produced the expected immutable bytes and MUC1 checksums. Preview reached a 660 Hz amplitude of about 6996 in captured PCM. A separate launch selected the custom file through native music loading and reached about 4888 amplitude after the client's volume setting. Restore saved generation zero; a further launch used the original menu route. These measurements validate signal delivery. Physical iPhone listening and Files presentation remain open.

The checked-in music-import.json and music-restore.json plans run through tools/clash_guest_ui_probe.py with a private copied home, GPU and audio capture. Use the literal readiness log GuestOffline: queued response=24101. For import, provide a shell wrapper that executes hvm with the normal probe arguments plus --document-import pointing to an authored WAV/MP3. On relaunch inspect Music Lab opening logs and check the captured signal against the authored input.

The first shared rerun passed 698 checks and failed the existing GCController discovery callback timing check. The first Test Lab run passed 25 cases and missed TL-J's throughput floor (14.8 versus 50 MFLOPS) while other capture jobs were running. Sequential reruns, MP3 and Apple packaging remain in progress. Native cancellation and invalid-file probes retained the previous assignment. Record the final results before delivery.
