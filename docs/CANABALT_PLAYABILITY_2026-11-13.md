# Canabalt 1.4 compatibility bench

Codex, issue #436, branch `codex/canabalt-compat`, based on main `ea5a6d2e`.
The owner supplied `Canabalt_1.4_ios_3.0.ipa`, SHA-256
`1e931ffb4401447bda44a24a6c0698386d734966a8b715bfa026781cc6bb4155`.
Bundle `com.semisecretsoftware.Canabalt`, version 1.4, minimum iOS 3.0;
both armv6/armv7 slices have cryptid 0. Extraction and saves remain outside Git.
No app asset or binary was modified. No internet/account/server was enabled.

`AGENTS.md` was read in full. `docs/IPA_PLAYABILITY_LOOP.md` is absent from
this checkout/main; the owner-provided playability procedure was followed
directly. BADLAND's unmerged changes are not dependencies of this branch.

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

The private bench wrapper additionally captures 44.1 kHz stereo signed-16
PCM via `--audio-fd`, writes 10-second snapshots/contact sheets, and preserves
the exact event JSON. Its paths are under `~/bench/runs/canabalt-*`.
Audio counters alone are not proof of audible quality.

## Regression tests and gates

Four runner executables cover string search, original CC0 triangle-font
glyphs, indexed formatting, and animation/window/timer ownership. The latter
contains independent tests for the three lifetime/ABI bugs. Test Lab adds
TL-AH through TL-AM; AA through AG are reserved by the separate BADLAND PR.
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

Host qualification does not establish phone GPU-driver behavior, microphone,
headphone routing, 16 KB pages or real iPhone speed. Canabalt does not request
microphone input in these scenarios. No phone results were supplied or claimed.
