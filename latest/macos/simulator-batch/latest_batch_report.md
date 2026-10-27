# HydrogenVM — Test All Games Report

**Date:** 2026-10-27T21:57:24Z  
**Device:** iPhone, iOS 26.5  
**Emulator Commit:** `b5df8bc5b4db`  
**Options:** 5s/app · Renderer Both (Software & GPU) · KeepGoing true · Explore 30s · Relaunch true

### Summary

- **Total Tests Run:** 2
- **Passed:** 0
- **Crashed:** 2
- **Wrong Output:** 0
- **Music heard:** 0 · **Sound effects heard:** 0
- **Games with a failed check:** 2
- **Explored:** 2
- **Explore Crashes:** 2
- **Explore Problems Discovered:** 1

### App Results

| App | Mode | Status | FPS | P95 (ms) | Signal | Summary |
|---|---|---|---:|---:|---|---|
| UIKitTest (`com.hydrogenvm.test.uikittest`) | SOFTWARE | ❌ Crashed | 2 | 0.0 | `ok` | Crashed (status 1) |
| UIKitTest (`com.hydrogenvm.test.uikittest`) | GPU | ❌ Crashed | 0 | 0.1 | `ok` | Crashed (status 1) |

### Checklist

| Check | ✅ | ⚠️ | ❌ | ➖ |
|---|---:|---:|---:|---:|
| Launches | 0 | 0 | 2 | 0 |
| Shows a picture | 2 | 0 | 0 | 0 |
| Game view on screen | 2 | 0 | 0 | 0 |
| Audio output | 2 | 0 | 0 | 0 |
| Music | 0 | 2 | 0 | 0 |
| Sound effects | 0 | 2 | 0 | 0 |
| Responds to touch | 0 | 0 | 2 | 0 |
| Background & resume | 0 | 0 | 0 | 2 |
| Closes cleanly | 0 | 0 | 0 | 2 |
| Relaunch | 0 | 0 | 0 | 2 |
| Missing APIs | 2 | 0 | 0 | 0 |
| Handled faults | 2 | 0 | 0 | 0 |
| Speed | 0 | 0 | 0 | 2 |

| App | Mode | Launches | Shows a picture | Game view on screen | Audio output | Music | Sound effects | Responds to touch | Background & resume | Closes cleanly | Relaunch | Missing APIs | Handled faults | Speed |
|---|---|:-:|:-:|:-:|:-:|:-:|:-:|:-:|:-:|:-:|:-:|:-:|:-:|:-:|
| UIKitTest | SOFTWARE | ❌ | ✅ | ✅ | ✅ | ⚠️ | ⚠️ | ❌ | ➖ | ➖ | ➖ | ✅ | ✅ | ➖ |
| UIKitTest | GPU | ❌ | ✅ | ✅ | ✅ | ⚠️ | ⚠️ | ❌ | ➖ | ➖ | ➖ | ✅ | ✅ | ➖ |

#### UIKitTest (SOFTWARE)

- ❌ **Launches:** Crashed (status 1)
- ✅ **Shows a picture:** 1 frames drawn
- ✅ **Game view on screen:** Every frame reached the window
- ✅ **Audio output:** Mixer ran for 5.3 s, peak 0.00
- ⚠️ **Music:** No music heard. It may be off in the game's settings, or this screen has none
- ⚠️ **Sound effects:** No sound effects, even after taps
- ❌ **Responds to touch:** Crashed while tapping around
- ➖ **Background & resume:** The game stopped before this step
- ➖ **Closes cleanly:** The game had already stopped
- ➖ **Relaunch:** Skipped because the first run didn't finish cleanly
- ✅ **Missing APIs:** None reported
- ✅ **Handled faults:** None
- ➖ **Speed:** No frames to time

#### UIKitTest (GPU)

- ❌ **Launches:** Crashed (status 1)
- ✅ **Shows a picture:** 2 frames drawn
- ✅ **Game view on screen:** Every frame reached the window
- ✅ **Audio output:** Mixer ran for 5.9 s, peak 0.00
- ⚠️ **Music:** No music heard. It may be off in the game's settings, or this screen has none
- ⚠️ **Sound effects:** No sound effects, even after taps
- ❌ **Responds to touch:** Crashed while tapping around
- ➖ **Background & resume:** The game stopped before this step
- ➖ **Closes cleanly:** The game had already stopped
- ➖ **Relaunch:** Skipped because the first run didn't finish cleanly
- ✅ **Missing APIs:** None reported
- ✅ **Handled faults:** None
- ➖ **Speed:** No frames to time

