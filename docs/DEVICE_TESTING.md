# Testing on the iPhone

The desktop bench runs games through Mesa's OpenGL ES; the phone runs them
through Apple's driver, with real network, audio and timing. Problems that only
show on the phone (black screens, crashes the bench never reaches) need the
phone's own evidence.

## Run

1. Library → **Test All Games**. Keep **Trace graphics calls (GPU)** on: every
   GL call is checked for errors and draw calls are counted. It is slower, so
   it applies to test runs only.
2. Start the run. Each game launches, is explored with taps, sent to the
   background and back, closed and relaunched (each step can be turned off).
3. When it finishes, tap the share button. It exports one folder,
   `HydrogenVM-test-<time>/`:
   - `report.json`: every run's checks, counters and GL errors
   - `report.md`: the same, readable
   - `thumbnails/`: the last frame of each game
   - `logs/<bundle id>_<renderer>.log`: log excerpt, crash report, GL errors
4. **Save to Files** into the desktop's `test_ipa` folder (OneDrive), or AirDrop
   it to the PC.

## Read it on the desktop

```
tools/device_report.py <export>                       # per-game summary
tools/device_report.py <export> --previous <older>    # fixed / new / still failing
```

A game's problems are its failing and warning checks, a crash, a blank screen
and its GL errors, so two runs compare the way `tools/hvm_probe.py --baseline`
compares bench runs.

## What the GL trace shows

- **Graphics calls** check: `N GL errors: glDrawArrays -> 0x0506 [worker]`.
  `[worker]` means the call came from a thread that had no GL context of its
  own, which usually explains a black world under a working UI.
- `Frames were shown but the game made no GL draw call`: the game presents
  without drawing (it is waiting for something, or draws through a path that
  never reaches GL).
- The same trace is available on the desktop: `hvm run <app> --gpu --gl-trace
  --report r.json` adds a `gl` section; `tools/hvm_probe.py` uses it in GPU mode.
