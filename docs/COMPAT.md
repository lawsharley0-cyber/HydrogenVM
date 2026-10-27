# Compatibility workflow

`tools/hvm_compat.py` is the scanner. It runs strict app checks, screenshots,
frame analysis, and shared-gap ranking through `tools/compat_harness`.

```sh
python3 tools/hvm_compat.py run --library docs/dev-system/library.json --seconds 30 --keep-going
python3 tools/triage.py ingest build/compat-report/report.json crash-bundle.json
python3 tools/hvm_compat.py diff before/report.json build/compat-report/report.json
```

The library run checks SHA-256 against `HVM_FIXTURE_ROOT`, including fixtures
stored under other filenames. Missing library entries are reported as optional
skips; a partial run does not validate unavailable games. The ordinary manifest
keeps required-fixture failures and the three-second PR smoke unchanged.
`--keep-going` adds a diagnostic run in a separate sandbox, preserving strict
status, screenshots and saves. Reports retain both runs and aggregate runtime misses.

The compatibility alias remains for IPA files or directories containing IPAs:

```sh
python3 tools/compat.py scan path/to/ipas/ more.ipa --out compat --seconds 30
```

`--relaunch` retains two launches sharing one sandbox; `--only` selects input
filenames or generated app IDs. `compat.macho_info` remains available to the
pinned-client metadata probe.

It writes the harness `compat/report.json`, with raw reports and screenshots
under `compat/work/` and diagnostic runs under `compat/keep-going/`.
Use `hvm_compat.py issues compat/report.json` for ranked shared API gaps and
`triage.py ingest compat/report.json --backlog` for a delimited generated table
in `docs/dev-system/BACKLOG.md`. Original backlog observations are preserved.
The retired `compat.py report`, `results.json`, and `REPORT.md` format are
replaced by `hvm_compat.py issues`, schema-checked JSON and explicit report diffs.

## Real-app regression log

- **Minion Rush / depth-stencil and root view (2026-10-26):** the game returned
  from EAGL setup when `GL_OES_depth24`, `GL_OES_stencil8` or
  `GL_OES_packed_depth_stencil` was absent. Implement format-correct storage,
  packed attachments and stencil clear/test/mask/operations, then advertise them.
  A second bug left a replacement root-controller view outside its window;
  `UIViewController.setView` now swaps the installed root subview. TL-R and
  fulltest were red before both fixes. A 100 s run then presented 130 frames,
  mixed 2,600,960 audible/music frames, and showed the opening video with the
  `EAGLView` on screen and no crash. Gameplay, effects, close and relaunch remain
  unverified.
- **Hungry Shark and Angry Birds / audio (2026-10-26):** RemoteIO output used a
  main-run-loop timer, so its callback could be starved; it now runs on a guest
  audio thread with LPCM conversion and mixer ownership/locking. Normal mutexes
  allow the old-iOS creator-to-worker handoff, and OpenAL `AL_MIN_GAIN` /
  `AL_MAX_GAIN` have their specified 0 / 1 defaults. Hungry Shark measured 36,864
  audible/music frames and Angry Birds 983,040 after both previously measured
  zero. Effects, close and relaunch remain unverified. TL-F/O/Q plus fulltest
  cover the shared behavior; TL-Q was red before the audio-thread fix.
- **Minecraft Story Mode 1.1 / GLES (2026-10-26):** added ES 3 named fragment
  outputs and texture aliases, spec-correct deferred program deletion, and
  locking for shared program bind/create/delete/draw operations. Each new
  fulltest regression was red first, including two-thread crash reproductions.
  A 90 s run rendered the Telltale legal screen and mixed 3,289,088 audible/music
  frames. Gameplay and reliable process teardown are not yet verified.

- **Clumsy Ninja / EAGL worker affinity (2026-10-26):** `ctx_setCurrent` applied
  the software renderer's one-context-per-thread rejection even after HostGL
  had created a shared worker context. The game was told its successful GPU
  bind failed. HostGL now skips only that software affinity rejection and
  records the successful guest binding; software behavior is unchanged.
  TL-N failed before the fix (`workerBound=0`) and passes after it, with a
  matching `fulltest` check. A 10 s software run showed the loading screen
  without handled faults. A 30 s Linux GPU run presented 240 frames without
  the old affinity warnings, but its readback screenshot remained black;
  device visual and FMOD behavior still need the next iPhone report.

`hvm info --linking Some.app --json` loads and binds without executing guest
initializers. Its references list functions, data, classes and selectors, with
missing stubs, zero-filled data and placeholder classes distinguished. Selector
entries identify known candidate implementations; the actual receiver still
determines dispatch at runtime. An implemented binding does not prove the full
API's behavior.

## Verified Game Compatibility Fixes

* **Mega Jump (`Mega Jump.app`)**:
  * Fixed `NSTimer` invocation forwarding for targets with missing or nil invocation selectors.
  * Verified gameplay loop runs cleanly with active coin collection and game over state.
* **Tiny Tower (`Tiny Tower.app`)**:
  * Implemented persistent 2-way sync Keychain storage (`SecItemAdd`, `SecItemCopyMatching`, `SecItemUpdate`, `SecItemDelete`) preventing save-file wipeouts on restart.
  * Added OpenGL 1.1 `glBlendEquationOES` and blend equation extensions to eliminate graphics compositing artifacts.
* **Pocket Ants (`anthill.app`)**:
  * Added multi-threaded OpenGL fallback context handling for Cocos2d-x background texture loading worker threads.
  * Verified gameplay rendering across full level progression.
* **RETRY (`retry.app`)**:
  * Diagnosed and fixed out-of-bounds heap corruption at frame 59 caused by fractional layer bounds mismatch (`std::ceil` vs `std::round`) between FBO color (1136x641) and depth (1136x640) renderbuffers.
  * Clamped software rasterizer bounds in `activeTarget` and added fragment bounds guards in `writeFragment`.
  * Verified app loads all Mono/LVL11 assemblies, passes intro screens, and executes full 3D forward shader render loop past 20+ seconds.
