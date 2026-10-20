# Compatibility workflow

`tools/compat.py` finds every issue each app runs into, in one pass per app,
and ranks fixes by how many apps they unblock. One fix is then made once,
in the shared runtime, and verified against every app that had the problem.

## Scan

```sh
tools/compat.py scan path/to/ipas/ more.ipa Some.app --out compat --seconds 30
```

Inputs can be `.ipa` files, `.app` bundles, or folders containing either.
For each app the scanner:

1. Unpacks the IPA and reads `Info.plist`.
2. Checks the executable: 32-bit ARM slice present, FairPlay encryption
   (encrypted apps are reported, not run), GLES1 vs GLES2, Unity.
3. **Strict run** — real behaviour: the first thing that stops or crashes
   the app, plus a screenshot of the last frame.
4. **Keep-going run** — missing functions return 0 and missing methods
   return nil, so every runtime gap the app reaches shows up in one run.
5. **Static gaps** — imports bound to "missing" stubs at load, placeholder
   classes, zero-filled data symbols that the app hasn't reached yet.
6. **Frame check** — flags a last frame that's blank or mostly one colour.

## Read `compat/REPORT.md`

* **Changes since the last scan** — per-app status changes; regressions are
  flagged. Run a scan before and after a change to check it didn't break
  another app.
* **Apps** — status, frames/fps, graphics API, the first blocker.
* **Rendering looks wrong** — apps whose last frame failed the frame check.
* **Fix next: blockers** — each entry stops the listed apps; fixing it
  unblocks all of them. Work top to bottom.
* **Hit at runtime** — gaps the apps reached, grouped by framework, most
  apps first.
* **Features reported unavailable** — deliberate (network offline, no
  StoreKit) or not yet built.
* **Load-time and static gaps** — what the apps will need next.

`compat/results.json` has everything (per-app issues, perf, blockers); the
raw `--report` JSON of each run is under `compat/runs/<app>/`.

## Loop

1. `scan` all apps.
2. Fix the top blocker (or the top runtime gap shared by several apps) in
   the runtime, with a `fulltest` check where it makes sense.
3. `scan --only <apps that had it>` to confirm, then a full `scan` to check
   for regressions.
4. Commit; tag `[mac]` to get a device IPA from CI.
