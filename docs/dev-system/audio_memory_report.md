# Audio Memory Report

This note is documentation only. The pull request that added it does not change emulator runtime behaviour. The checks below cover the Mixer voice paths in `tests/audio/AudioLifecycleTests.cpp` and the guest `AudioServicesCreateSystemSoundID` / play / dispose calls in `tests/ios/apps/audiolifecycle`. They do not measure `ATS.sounds` owner counts, interrupted playback, sustained gameplay memory, unsupported WAV decoding, or Clash Royale.

## Findings

1. **Mixer voice lifetime**: `tests/audio/AudioLifecycleTests.cpp` checks that `Mixer::play` with `autoRelease` drops its PCM `shared_ptr` after the buffer finishes, and that `release` (including after `stop`) does the same. That is mixer voice lifetime, not the `ATS.sounds` map.
2. **Guest create/play/dispose**: `tests/ios/apps/audiolifecycle` calls `AudioServicesCreateSystemSoundID`, `AudioServicesPlaySystemSound`, and `AudioServicesDisposeSystemSoundID` for one generated WAV, including two creates of the same URL and an invalid id that must not crash. It checks OSStatus for the create and dispose calls it makes. It does not count `ATS.sounds` entries or PCM bytes before and after dispose.
3. **Clash Royale Leaks**: The suggestion that Clash Royale continuously retains PCM references by allocating system sounds repeatedly without calling `AudioServicesDisposeSystemSoundID` is currently a hypothesis. It requires future verification (e.g., using the proposed telemetry below) to confirm whether this behavior is the primary contributor to memory bloat. Gameplay memory growth has not been measured.
4. **Decoder Missing Logs**: `demon_step_01.wav` fails because it is an unsupported format, logging a missing decoder warning and dropping empty PCM. It does not cause a memory leak.
5. **Note**: We cannot state that all `SystemSound` or audio object pathways have been conclusively proven leak-free across the entire emulator engine unless each variant was directly tested. The current findings apply to the core paths exercised in the new unit tests.

## Proposed Telemetry Metrics

For future profiling and debugging inside the HydrogenVM emulator:
- `audio.system_sounds.live`: Number of tracking entries currently loaded in `ATS(x).sounds`.
- `audio.system_sounds.pcm_bytes`: Accumulated buffer sizes within tracked System Sounds.
- `audio.voices.active`: Real-time count of voices playing in the Mixer.
- `audio.voices.auto_released`: Real-time count of voices automatically cleaned up by the Mixer after completing playback.

## Tests

`tests/audio/AudioLifecycleTests.cpp` checks Mixer auto-release and explicit release, including a `weak_ptr` that must expire after the voice drops the buffer. The guest app checks the AudioServices calls listed above. Neither test is a reason to add or remove an LRU cache, and neither measures gameplay memory.
