# HydrogenVM — Test All Games Report

**Date:** 2026-11-08T00:32:45Z  
**Device:** iPhone, iOS 26.5  
**Emulator Commit:** `a2ed7c6c267c`  
**Options:** 5s/app · Renderer Both (Software & GPU) · KeepGoing true · Explore 30s · Relaunch true

### Summary

- **Total Tests Run:** 2
- **Passed:** 0
- **Crashed:** 2
- **Host memory (not a game fault):** 0
- **Wrong Output:** 0
- **Music heard:** 0 · **Sound effects heard:** 0
- **Games with a failed check:** 2
- **Explored:** 2
- **Explore Crashes:** 2
- **Explore Problems Discovered:** 2

### App Results

| App | Mode | Status | FPS | P95 (ms) | Signal | Summary |
|---|---|---|---:|---:|---|---|
| UIKitTest (`com.hydrogenvm.test.uikittest`) | SOFTWARE | ❌ Crashed | 6 | 0.3 | `ok` | Crashed (status 1) |
| UIKitTest (`com.hydrogenvm.test.uikittest`) | GPU | ❌ Crashed | 5 | 0.1 | `ok` | Crashed (status 1) |

### Checklist

| Check | ✅ | ⚠️ | ❌ | ➖ |
|---|---:|---:|---:|---:|
| Launches | 0 | 0 | 2 | 0 |
| Shows a picture | 2 | 0 | 0 | 0 |
| Graphics calls | 0 | 1 | 0 | 0 |
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

| App | Mode | Launches | Shows a picture | Graphics calls | Game view on screen | Audio output | Music | Sound effects | Responds to touch | Background & resume | Closes cleanly | Relaunch | Missing APIs | Handled faults | Speed |
|---|---|:-:|:-:|:-:|:-:|:-:|:-:|:-:|:-:|:-:|:-:|:-:|:-:|:-:|:-:|
| UIKitTest | SOFTWARE | ❌ | ✅ |  | ✅ | ✅ | ⚠️ | ⚠️ | ❌ | ➖ | ➖ | ➖ | ✅ | ✅ | ➖ |
| UIKitTest | GPU | ❌ | ✅ | ⚠️ | ✅ | ✅ | ⚠️ | ⚠️ | ❌ | ➖ | ➖ | ➖ | ✅ | ✅ | ➖ |

#### UIKitTest (SOFTWARE)

- ❌ **Launches:** Crashed (status 1)
- ✅ **Shows a picture:** 13 frames drawn
- ✅ **Game view on screen:** Every frame reached the window
- ✅ **Audio output:** Mixer ran for 6.1 s, peak 0.00
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
- ✅ **Shows a picture:** 7 frames drawn
- ⚠️ **Graphics calls:** Frames were shown but the game made no GL draw call
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

