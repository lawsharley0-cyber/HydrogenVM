# Audio Memory Report

## Findings

1. **System Sounds Caching**: Temporary audio buffers used for system sounds are correctly cached in `ATS(x).sounds`. When playback completes, `MX(x.proc).play()` is called with `autoRelease = true`, successfully discarding the mixer's reference to the PCM `shared_ptr`.
2. **Explicit Disposal**: Disposing sounds via `AudioServicesDisposeSystemSoundID` completely eliminates all tracking entries. There is no static emulator-side leak detected in the tested core paths.
3. **Clash Royale Leaks**: The suggestion that Clash Royale continuously retains PCM references by allocating system sounds repeatedly without calling `AudioServicesDisposeSystemSoundID` is currently a hypothesis. It requires future verification (e.g., using the proposed telemetry below) to confirm whether this behavior is the primary contributor to memory bloat.
4. **Decoder Missing Logs**: `demon_step_01.wav` fails because it is an unsupported format, logging a missing decoder warning and dropping empty PCM. It does not cause a memory leak.
5. **Note**: We cannot state that all `SystemSound` or audio object pathways have been conclusively proven leak-free across the entire emulator engine unless each variant was directly tested. The current findings apply to the core paths exercised in the new unit tests.

## Proposed Telemetry Metrics

For future profiling and debugging inside the HydrogenVM emulator:
- `audio.system_sounds.live`: Number of tracking entries currently loaded in `ATS(x).sounds`.
- `audio.system_sounds.pcm_bytes`: Accumulated buffer sizes within tracked System Sounds.
- `audio.voices.active`: Real-time count of voices playing in the Mixer.
- `audio.voices.auto_released`: Real-time count of voices automatically cleaned up by the Mixer after completing playback.

## Tests Complete

Verified with `tests/audio/AudioLifecycleTests.cpp` ensuring `auto_release` and explicit garbage collection behaviors are working. No manual LRU Cache required within HydrogenVM audio core.
