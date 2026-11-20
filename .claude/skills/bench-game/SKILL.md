---
name: bench-game
description: Import and run an iOS .ipa or Android .apk on the HydrogenVM Linux bench in phone mode and summarize crash, frames, missing APIs and a screenshot. Use to reproduce reports, verify fixes, or A/B a change.
argument-hint: <path to .ipa/.apk> [seconds]
---

Requires `build/hvm` (`ninja -C build hvm`).

1. Import into a fresh directory outside the repo:
   ```
   OUT=$(mktemp -d)
   ./build/hvm import "<file>" "$OUT/apps"
   APP=$(find "$OUT/apps" -maxdepth 2 -name "*.app" -type d | head -1)
   ```
2. Run in phone mode with the GPU renderer. Always use both flags for evidence:
   ```
   ./build/hvm run "$APP" --gpu --phone --run-for <seconds, default 40> --home "$OUT/home" \
       --report "$OUT/r.json" --screenshot "$OUT/s.png" --quiet > "$OUT/log.txt" 2>&1
   ```
3. Summarize from r.json:
   - `crashed`, `stillRunning`, `exitStatus`, `signature`;
   - `perf.gameSwaps`, `perf.timeToFirstFrameMs`, `perf.avgFrameMs`;
   - the `missingApis` names;
   - the last `Stopped`/`Crash` lines of log.txt.

   Look at s.png to say what's on screen.
4. For A/B runs, set a kill switch (`HVM_GUEST_PROT=0`, `HVM_NO_FAST_SEND=1`) or use the two builds.
   Alternate the order and run each at least twice: the first run pays cold caches. Compare
   `guestInstructions` retired in a fixed time.
5. Never copy game files into the repo.
