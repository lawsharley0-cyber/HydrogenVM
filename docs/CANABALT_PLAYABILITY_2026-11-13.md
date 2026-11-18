# Canabalt 1.4 compatibility bench

Codex, issue #436, branch `codex/canabalt-compat`, initially based on main
`ea5a6d2e`, then integrated with main `f019d477`.
The owner supplied `Canabalt_1.4_ios_3.0.ipa`, SHA-256
`1e931ffb4401447bda44a24a6c0698386d734966a8b715bfa026781cc6bb4155`.
Bundle `com.semisecretsoftware.Canabalt`, version 1.4, minimum iOS 3.0;
both armv6/armv7 slices have cryptid 0. Extraction and saves remain outside Git.
No app asset or binary was modified. No internet/account/server was enabled.

`AGENTS.md` was read in full. `docs/IPA_PLAYABILITY_LOOP.md` was absent at the
initial base; it landed in main `f2b8318d` during this investigation. It was
then read in full and its workflow integrated, including fulltest wiring and
explicit support labels. Main also integrated BADLAND #428 during this batch;
the final branch includes those fixes and preserves its regression cases.

## Shared causes and fixes

1. Startup threw a missing-selector exception in text wrapping.
   Implement bounded `rangeOfCharacterFromSet:options:range:` with UTF-16
   coordinates, directional/anchored search, scalar membership and checked
   range/output handling. The two existing convenience methods delegate to it.
2. Menu labels were blank because `CGContextShowGlyphs` was a stub.
   Render actual selected CGFont outlines through the existing rasterizer;
   preserve font lifetime in saved graphics states, advance text, honor text
   matrix/spacing/drawing modes, and give ShowGlyphsAtPoint its correct ABI.
   Fonts without available outlines remain explicitly unsupported.
3. Now Playing displayed literal indexed formatting directives. Gather the
   argument types and ARMv7 values for contiguous positional formats, then
   reuse ordinary formatting with reordered arguments. Indexed width/precision
   and repeated arguments work; invalid/mixed/gapped formats are rejected.
4. Legacy animation delegates received an NSNumber pointer for scalar BOOL
   `finished`. Pass YES. This is an independent ABI bug; correcting it alone
   did **not** resolve the Scores overlay.
5. Making a UIWindow visible took an extra ownership retain. Remove it:
   applications own their windows, and final release unregisters the window.
6. UIKit pooled input/drawing, but timer callbacks ran outside those pools.
   Canabalt's timer-driven update created autoreleased window notifications
   retained by its outer pool; released Scores windows remained active.
   Pool the entire UIKit main-loop turn, including queued work/timers and
   nested main-loop pumping. Independent worker/Foundation loops remain
   caller-managed. The visibility and timer-pool fixes are both necessary.

Apple's [window programming guide](https://developer.apple.com/library/archive/documentation/WindowsViews/Conceptual/ViewPG_iPhoneOS/CreatingWindows/CreatingWindows.html)
describes application-owned window retention. There are no game-name checks,
synthetic scores, save edits or proprietary fixtures in these fixes.

## Reproduction

Unpack the supplied archive under `~/bench/apps/Canabalt_1.4`, checking ZIP
paths before extraction. A new, previously nonexistent home is required for
the initial run; retain that home for the relaunch.

```sh
build/hvm run ~/bench/apps/Canabalt_1.4/Payload/Canabalt.app \
  --home ~/bench/homes/canabalt-final-fresh --gpu --run-for 230 --graceful \
  --input-file /tmp/canabalt-input.txt --frame-file /tmp/canabalt.hvf \
  --report /tmp/canabalt-report.json --screenshot /tmp/canabalt.png
```

Append newline-delimited input while running. Coordinates use portrait points
(320x480), not the rotated landscape screenshot. `touch 0 x y` begins,
`touch 3 x y` ends; hold a menu touch about 0.15 s and a jump about 0.35 s.
At 8 s tap Scores `(295,240)`, at 20 s Back `(295,455)`, at 32 s PLAY
`(295,65)`. From 36 through 220 s, tap `(160,240)` every 2 s, holding 0.35 s;
this jumps while alive and retries after death. At 120 s substitute pause
`(15,465)`; the next center tap resumes. Do not tap PLAY before the initial
menu has settled. Rotate the raw screenshot 90 degrees clockwise for viewing.

Portable scripted reproduction from the repo root (choose a new home name):

```sh
python3 - <<'PY'
from pathlib import Path
import subprocess, time
events = []
def tap(t, x, y, hold):
    events.extend([(t, f"touch 0 {x} {y}"), (t+hold, f"touch 3 {x} {y}")])
tap(8,295,240,.15); tap(20,295,455,.15); tap(32,295,65,.15)
for t in range(36,221,2):
    tap(t,15 if t==120 else 160,465 if t==120 else 240,.35)
feed = Path("/tmp/canabalt-input.txt"); feed.write_text("")
app = str(Path("~/bench/apps/Canabalt_1.4/Payload/Canabalt.app").expanduser())
home = str(Path("~/bench/homes/canabalt-review-new").expanduser())
with open("/tmp/canabalt.log","w") as log:
    p = subprocess.Popen(["build/hvm","run",app,"--home",home,"--gpu",
        "--run-for","230","--graceful","--input-file",str(feed),
        "--report","/tmp/canabalt.json","--screenshot","/tmp/canabalt.png"],
        stdout=log,stderr=subprocess.STDOUT)
    start = time.monotonic()
    for t,line in sorted(events):
        time.sleep(max(0,start+t-time.monotonic()))
        with feed.open("a") as f: f.write(line+"\n")
    raise SystemExit(p.wait())
PY
```

The private bench wrapper additionally captures 44.1 kHz stereo signed-16
PCM via `--audio-fd`, writes 10-second snapshots/contact sheets, and preserves
the exact event JSON. Its paths are under `~/bench/runs/canabalt-*`.
Audio counters alone are not proof of audible quality.

## Regression tests and gates

Four runner executables, also called from fulltest, cover string search, original CC0 triangle-font
glyphs, indexed formatting, and animation/window/timer ownership. The latter
contains independent tests for the three lifetime/ABI bugs. Test Lab adds
TL-AH through TL-AM; AA through AG are the integrated BADLAND cases.
Each bug has a failing-before guest reproduction and a passing-after result.

```sh
build/hvm_tests fulltest
build/hvm_support_tests
build/hvm_heap_tests
build/hvm_string_search_tests
build/hvm_legacy_glyph_tests
build/hvm_positional_format_tests
build/hvm_legacy_animation_tests
python3 tools/hvm_testlab.py run --hvm build/hvm --out build/testlab-pool-fixed
python3 tools/lint_stubs.py --baseline tools/stub_lint_baseline.json
PYTHONPATH=tools/tests:tools python3 -m pytest -q --import-mode=importlib \
  tools/tests tools/compat_harness/tests
python3 tools/hvm_compat.py run --build-dir build --fixture-root /var/tmp/hvm-fixtures \
  --baseline docs/dev-system/baselines/linux-headless.json --require-resolved \
  --out build/compat-canabalt-final
python3 tools/must_launch.py --apps ~/bench/apps --hvm build/hvm --only Canabalt_1.4 --record
python3 tools/must_launch.py --apps ~/bench/apps --hvm build/hvm --jobs 4
```

## Initial qualification before latest-main integration

Fresh home `canabalt-final-fresh`, 230.082 s, clean graceful exit, no crash:
initial Scores showed no data; Back removed the overlay; PLAY entered the
round. Contact sheets show scrolling environments, jumps across gaps,
game-over/retries, pause at 120 s and resume at 122 s. The game earned 521 m,
473 m, 392 m and other local runs. A separate 60 s relaunch displayed these
scores in the actual Scores screen, then returned to gameplay. SQLite was
read only to corroborate the UI; no save was edited.

Average frame time 10.4497 ms, p95 23.593 ms, p99 27.263 ms; maximum 52.7909 ms.
The guide's average-under-33-ms gate passes; the maximum is an explicit
outlier, not an all-frames-under-33-ms claim. Frame export and PCM capture
were enabled. Mixer: 2 music voices, 689 effect voices, 10,059,776 audible
frames; transport dropped 0 blocks and reported 0 errors. Private PCM
cross-correlation against decoded supplied assets matched `run-title.m4a`
at 1.0000, gameplay `run.m4a` at 0.9941 after 40 s, and `wall.caf` at 0.9653.
Jump and footstep effects were also present in the mix. These are waveform
checks, not a substitute for subjective listening or phone routing tests.

Before integration: fulltest 784/0, Test Lab 32/0, support 47/0, heap 12/0,
four focused runners pass, pytest 492 passed / 6 skipped / 725 subtests,
stub audit 0 new/changed (682 legacy), fullcompat 0 regressions, must-launch
13 available pass / 21 unavailable skip. Final integrated results follow
after rerunning these checks; missing fixtures are never counted as passes.

## Latest-main integration qualification

Runtime source `8d6145af` includes main `f019d477`. The new home
`~/bench/homes/canabalt-integrated-fresh` and run directory
`~/bench/runs/canabalt-integrated-fresh` repeat the same 230-second input
sequence. The run closed gracefully at 230.073 s with no crash, missing APIs,
missing functions or stub hits. It earned 553 m, 371 m, 243 m and other runs.
The separate `canabalt-integrated-relaunch` captures show those scores in
Best Local Runs and successful Back/PLAY navigation again.

Average frame time is 7.22879 ms, p95 18.6122 ms, p99 23.3308 ms, maximum
69.7359 ms. This passes the guide's average gate while explicitly retaining
the outlier caveat. Mixer: 2 music voices, 647 effect voices, 10,113,024
audible frames; PCM transport drops/errors 0/0. Title-music waveform match
1.0000, gameplay music 0.7699 in the busier mix after 40 s, wall effect 0.9641.

Integrated gates: fulltest **806 passed / 0 failed**, Test Lab **39 passed /
0 failed**, support **47/0**, heap **12/0**, all **11** focused BADLAND and
Canabalt CTest targets pass; pytest **492 passed / 6 skipped / 725 subtests**;
stub audit **0 new/changed**, 682 acknowledged legacy findings. Fullcompat:
`build/compat-canabalt-integrated/report.json`, **0 regressions**, 0 unit or
Python failures and no crashed/skipped required apps. No baseline was changed.
The host uses Clang 18, x86-64 WSL2 Linux, 4096-byte host pages and Mesa.
Final must-launch: **14 available games passed / 0 failed / 21 missing
fixtures skipped**, including both BADLAND and Canabalt. Canabalt was recorded
with picture, sound and 265 required frames; all pre-existing expectations
were preserved. Logs are in `~/codex-canabalt-build/*-integrated.log`.

Host qualification does not establish phone GPU-driver behavior, microphone,
headphone routing, 16 KB pages or real iPhone speed. Canabalt does not request
microphone input in these scenarios. No phone results were supplied or claimed.
