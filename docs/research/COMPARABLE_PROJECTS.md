# Research: how comparable emulators solved compatibility, JIT-less speed, GLES2, testing and UX

Research report produced 2026-10-23 for HydrogenVM planning. It is reference material, not
policy; the plan that acts on it is [`docs/dev-system/MASTER_PLAN.md`](../dev-system/MASTER_PLAN.md).
Claims sourced from forks, issue trackers and AI-assisted community projects are self-reported.

## TL;DR

- **Graphics.** The highest-impact change is to stop rasterizing in software and hand the guest's
  GLES 1.1/2.0 calls to the iPhone GPU — through Apple's deprecated but still shipping
  `OpenGLES.framework`, or through ANGLE/MetalANGLE on Metal. No comparable project gets
  acceptable 3D performance from a CPU shader interpreter without a JIT. The projects that ship
  on iOS without a JIT (PPSSPP, DolphiniOS, UTM SE) all keep graphics on the GPU.
  SwiftShader is no help: Reactor requires a JIT (LLVM or Subzero) and the project is no longer
  actively maintained. Keep the software rasterizer as a reference renderer and test oracle.
- **CPU.** touchHLE depends on dynarmic's JIT and has no interpreter mode, so there is no
  upstream model for HydrogenVM's interpreter. Best published no-JIT evidence: PPSSPP's IR
  interpreter (block chaining, threaded dispatch), Dolphin's Cached Interpreter 2.0 (3–4× its
  plain interpreter), QEMU TCTI/AOK gadgets (2.3–3.5× fewer host cycles than per-op TCTI).
  Adopt `musttail` + `preserve_none` dispatch, block chaining, operand-specialized handlers,
  and native HLE fast paths for hot libc routines.
- **Compatibility and process.** Copy touchHLE's `--dump=linking-info` +
  `log_unimplemented.sh` loop, its 1–5 star compatibility database and its GDB stub. Add
  Dolphin FifoCI / PCSX2 GS-dump-style GLES command-stream capture and replay with image
  diffing in CI. Require tests and AI disclosure on every change, including agent-authored ones.

## 1. touchHLE and other iOS-on-something emulators

**OpenGL ES.** Upstream touchHLE is essentially a GLES 1.1 emulator. Since v0.2.0 it can use a
native OpenGL ES 1.1 driver where available instead of translating to OpenGL 2.1
(`--gles1=gles1_native` / `--gles1=gles1_on_gl2`, tried in order: "Trying: Native OpenGL ES 1.1 …
Failed … Trying: OpenGL ES 1.1 via touchHLE GLES1-on-GL2 layer … Success!"). GLES2 is not
supported upstream (issue #566 is open, labelled "missing or incomplete iPhone OS API").
GLES2 work has happened in forks:

- XaView fork: "OpenGL ES 2.0: Partially", Asphalt 7 "Fully working".
- HyperHLE forks (AI-assisted) pass guest GLSL through to the host driver (often ANGLE on
  Android) and patch it: respect the guest's `glBindAttribLocation` bindings; pre-link merge of
  mismatched uniform struct definitions between stages (Gangstar Rio); robust varying parsing
  with link-failure retry; software PVRTC decode when the host lacks
  `GL_IMG_texture_compression_pvrtc`; `--force-gles1-context` to downgrade ES2 requests.

Lesson: real GLES2 games mostly work through **shader passthrough plus a growing fix-up list.
Nobody interprets the shaders.**

**CPU.** touchHLE runs ARM only through dynarmic. Its FAQ: "touchHLE relies on JIT
compilation" and it "will not officially support iOS". A Chimera maintainer: dynarmic "is a
recompiler and has no interpreter to choose". The Applesauce iOS wrapper: "JIT is required;
there is no interpreter fallback." The LightTouchMac developer called JIT-less iPhone
emulation of old iOS apps "dead on arrival". HydrogenVM is effectively the only JIT-less
option in this niche.

**Choosing and testing apps.**
- "The supported functionality is defined by the supported apps": contributors pick a game and
  implement whatever it needs. UIKit is "the most hacky and incomplete"; GLES and OpenAL are
  fairly complete. Non-game apps are deprioritized.
- v0.2.1 replaced the supported-apps list with a crowdsourced compatibility DB, 1–5 stars:
  1 crashes immediately, 2 menu only, 3 main content with major issues, 4 playable with small
  issues, 5 fully usable. Each report records version, OS, GPU, remarks (remarks hold the crash
  string, e.g. `Class "NSOperationQueue" … is unimplemented`).
- Emulation General Wiki: 398 of 879 reported titles (45.28%) working.
- Issue #250 proposes feeding unimplemented-function logs into the DB automatically.

**Debugging** (`dev-docs/debugging.md`):
- `log!()` always prints; `log_dbg!()` only for modules in `ENABLED_MODULES`. `touchHLE::abi` +
  `touchHLE::dyld` gives a trace of almost all guest→host calls; `touchHLE::mem` logs allocations.
- On a guest panic: r0–r13, SP, LR, PC and a frame-pointer stack trace; then Ghidra.
- `--dump=linking-info` writes JSON of classes, selectors and lazy symbols the binary requests and
  how each is handled; `dev-scripts/log_unimplemented.sh` turns it into a missing-API list.
- `--gdb=localhost:9001`: GDB RSP server (registers, memory, single-step, breakpoints, threads).
  LLDB "should work, but it doesn't"; mixed Thumb/ARM functions need care.
- Unimplemented classes and selectors fail loudly with class and method name.

**Performance/architecture lessons.**
- Lazy dynamic linking: a stub executes `svc`, the CPU halts, the host implementation runs, the
  CPU resumes; once a stub is linked that path "never has to stop again".
- Guest memory accessed directly when possible. Frame rate capped at 60 fps by default.
- v0.3.0 moved guest threads onto coroutines, ending "a dreadfully long saga with threading problems".
- Instead of reimplementing libstdc++/libgcc it ships real guest dylibs (iOS 4.0.1 versions
  since v0.3.0); SQLite3/libxml2 are built from source with a clean toolchain.

**Other efforts.** QEMU-iOS (LLE, needs real firmware) and Corellium (commercial, only
iOS 10.3.3/10.3.4 on the 32-bit side) do not apply to a sideloaded HLE app. ipasim, Darling
and aspen were not covered by primary sources in this pass.

**Ranked recommendations.**
1. Linking-info dump and unimplemented-symbol report: walk lazy/non-lazy symbol tables,
   `__objc_classrefs`, `__objc_selrefs`, superclass refs; diff against the HLE registry; write
   `{app, version, bundle_id, missing_functions[], missing_classes[], missing_selectors[]}`;
   aggregate across the IPA corpus; implement by apps blocked.
2. Fail loudly on unimplemented selectors/functions with class, selector, caller PC/LR and a
   stable crash-signature hash. Silent `nil` returns cause "runs but wrong" failures.
3. Load real guest dylibs (libstdc++, libgcc) instead of reimplementing the C++ runtime.
4. GDB RSP stub in the interpreter (simpler than in a JIT).
5. Coroutines or cooperative scheduling for guest threads.

## 2. JIT-less interpreter performance on iOS and Apple silicon

**UTM SE / QEMU TCTI.** UTM SE uses "a threaded interpreter which performs better than a
traditional interpreter but still slower than JIT … similar to what iSH does" (2024, the first PC
emulator on the iOS App Store; "SE" = "slow edition"). TCTI chains JOP/ROP-ish gadgets; guest
TCG registers map onto host x1–x15, x28 holds the thread-stream pointer, each gadget ends in a
dequeue-jump. Gadgets are precompiled per operand combination (1 operand → 16, 2 → 256,
3 → 4096). AOK gadgets (one per guest instruction, plus fused pairs) need 2.30–3.54× fewer host
cycles than TCTI on an M5 Max for an AArch64 guest. TCTI is "limited by its reliance on emitting a
single gadget for every tiny code operation". JIT-less x86 under UTM is "comparable to a
486/Pentium".

**Dolphin Cached Interpreter / DolphiniOS.** At release, over twice as fast as the interpreter.
Cached Interpreter 2.0 (2407-103, mitaclaw): variable-sized commands, 10–25% uplift, now 3–4×
the interpreter; the main bottleneck was "the number of function pointers emitted … the
unpredictable branch every few tens of host instructions". Dolphin says it "still cannot serve as
a JIT alternative". DolphiniOS "No JIT mode" falls back to it and warns it "is very slow".

**PPSSPP IR interpreter.** 2016: "faster than interpreter, slower than JIT" for iOS. 2024:
removing the count parameter from `IRInterpret` gave 10–15% on x86, 1–5% on M1. PR #22587:
without a JIT the IR interpreter took 47–78% of the emu thread across ten benchmark games;
block chaining ("In GTA, 73 million block entries now take 17,660 calls") gave 4.5–10% fewer
instructions and 1–4% fewer cycles; "clang keeps the threaded dispatch (one indirect jump per op)
only while the shared tail is small." PR #22584: handwritten vertex decoders for no-JIT builds
run "within ~8-15% of the JIT's speed", "four to eleven times faster than the steps". Issue
#15670: the IR interpreter "is considerably less stable in some games than the JIT".

Lesson: on iOS the gains come as much from **replacing generic slow paths with hand-specialized
native code** as from the core loop.

**Tail-call dispatch.** `[[clang::musttail]]` guarantees tail calls; `preserve_none` (Clang ≥ 19.1)
removes callee-saved spills. CPython's tail-calling interpreter needed both ("with musttail alone
the performance was quite bad"). Haberman: the attributes let you be "much less at the mercy of
optimizer heuristics". Ken Jin's caution: part of CPython's original speedup came from a compiler
regression in the baseline — benchmark carefully.

**MIPS on A-series.** No credible published guest-MIPS numbers for ARM-on-ARM interpreters on
A-series chips were found. Measure it.

**Ranked recommendations.**
1. Tail-call threaded dispatch with `preserve_none`:
   `CC Handler(State* s, const Op* op, uint32_t* regs, int32_t budget)` ending in
   `[[clang::musttail]] return op[1].fn(s, op+1, regs, budget);`.
2. Block chaining / direct-branch linking: cached successor per block exit validated by guest
   PC; hash lookup only for indirect branches; return-address predictor stack for `bx lr`.
3. Operand-specialized handlers for the hottest ~50 ARM/Thumb-2 forms plus fused pairs found by
   profiling (`cmp+bcc`, `ldr+ldr`, `vldr+vmul`, `push/pop`).
4. Lazy NZCV: store last result and op kind; materialize on demand; specialize `cmp; b<cond>`.
5. **Run hot leaf functions natively** (biggest HLE-specific lever): `memcpy`, `memset`,
   `strlen`, `sin/cos/sqrtf`, `objc_msgSend`, … by symbol or body hash; match statically linked
   engine math/matrix routines by signature.
6. Measurement: guest-MIPS microbenchmarks (Dhrystone/CoreMark armv7 with the old SDK) plus ~10
   captured game traces; track host cycles per guest instruction per commit.

## 3. Fast GLES2/GLSL without JIT, and handing GLES to the GPU

**Software is a dead end for production.** SwiftShader's Reactor "produces code for a JIT
compiler rather than executing that code" (LLVM or Subzero); its GLES frontend "is no longer
supported"; a 2024 commit says "Swiftshader is no longer actively maintained"; no interpreter
fallback found. llvmpipe depends on LLVM JIT; softpipe is the slow reference path.

**GPU passthrough is viable on iOS.**
- OpenGL ES was deprecated in iOS 12 ("Apps built using OpenGL ES will continue to run").
  Developer Forums reports: OpenGL "works Perfectly on my iPhone 12 with iOS 26.3 and 26.4.2"
  (Godot developer); thread 809362 — a Qt/Esri app that froze with Metal errors on iPadOS 26.1
  worked after switching to OpenGL. zeuxcg: since iOS 10 on newest iPhones "GL is implemented on
  top of Metal".
- Constraints: tops out at GLES 3.0; may be removed in any future iOS; background apps that
  issue GL commands are terminated (stop all GL work on `applicationDidEnterBackground`); the
  simulator's GLES is a slow software renderer (6.3 fps vs MetalANGLE 59.2 fps in an
  openFrameworks test) — don't benchmark there.
- MetalANGLE: "OpenGL ES 2.0 functionalities are 100% completed"; iOS 9+, Metal from iOS 11;
  MGLKit "mimics Apple's deprecated EAGL & GLKit API"; largely merged into Google ANGLE (whose
  Metal backend WebKit uses). ANGLEGLKit: a "drop-in replacement for OpenGLES framework for iOS".

**Ranked recommendations.**
1. **GPU passthrough backend (transformative).** HLE EAGL by creating a real `EAGLContext` (or
   MGLContext) per guest context; back `renderbufferStorage:fromDrawable:` with a host
   `CAEAGLLayer`/`MGLLayer`; forward GL calls almost verbatim. The work is:
   (a) copy client-side vertex arrays and texture data out of guest memory;
   (b) map guest object names to host names;
   (c) validate/patch GLSL 1.00 before `glShaderSource` (attribute binding, uniform struct
   mismatches, precision qualifiers);
   (d) PVRTC: decode to RGBA8 on upload where the GPU family lacks it;
   (e) BGRA and `APPLE_*` extensions.
   Ship OpenGLES.framework as default and MetalANGLE as a runtime switch, mirroring touchHLE's
   `--gles1=` fallback chain. GLES 1.1 fixed function also runs natively.
2. Keep the software rasterizer as a reference renderer and CI oracle for replayed streams.
3. If a software fallback must remain: SoA shader execution over 2×2 quads / 4–8-wide NEON
   lanes; half-space rasterization over 8×8 tiles with hierarchical early-Z; tile binning with
   2–4 worker threads; constant-fold uniforms per draw; pattern-match common shaders (textured ×
   color, modulate, alpha test) to native NEON kernels keyed by shader hash; pre-convert textures
   at upload into 8888 tiled/Morton layout. (Engineering judgment; no published JIT-less numbers.)

## 4. Compatibility and testing methodology

- **Dolphin FifoCI** (since Jan 2015) replays recorded CPU→GPU FIFO logs per build and notifies
  developers on rendering differences; caught regressions in PRs before merge. `hwfifoplayer`
  replays the same logs on a real Wii to separate game bugs from emulator bugs. Dolphin admits it
  covers only "a small sampling"; opt-in usage statistics caught others (Skies of Arcadia "~4 FPS").
- **PCSX2 GS dumps**: isolated replay of GS command streams; community dump collection for
  regression testing (TellowKrinkle/gsdumps); runner with `-perf` and frame-range looping; a
  comparer producing `changes.html` (PR #15088 made it multithreaded, 2m52s → 35s). PCSX2's PR
  template asks "Did you use AI to help find, test, or implement this issue or feature?"
- **Agent discipline.** touchHLE reviews via GerritHub and keeps `AGENTS.md`/`CLAUDE.md`. SDL's
  `AGENTS.md` bans generative-AI contributions. Emulation General Wiki refuses to recommend
  HyperHLE because it "uses AI-assisted code" — the reputational cost of unverified agent commits.
- RPCS3, Ryujinx/yuzu, Wine conformance, Vita3K were not covered by primary sources here.

**Ranked recommendations.**
1. **GLES command-stream capture ("HydrogenTrace")**: serialize every HLE GL/EAGL call with
   arguments and referenced guest memory per frame (zstd). Replay headless through the GPU backend
   and the reference rasterizer; diff against golden PNGs with a perceptual threshold; block
   merges on unexplained diffs.
2. **Automated boot matrix**: every IPA in a private corpus, headless N seconds with scripted taps;
   record stage reached (load → first GL frame → first touch → in-game), crash signature, missing
   symbols; publish a 1–5 star page; opt-in user reports add device/iOS.
3. **Priority score** = apps blocked × popularity ÷ cost, re-ranked from the boot matrix.
4. **Agent guardrails**: each change cites the app or trace it fixes, adds a replay or unit test,
   never edits golden images without human sign-off, passes the boot matrix with no star
   regressions; PR template with an AI-disclosure field.

## 5. Mobile front-end UX

- **Delta**: import via "+" and Files picker, plus "Open in…"; library visible in Files;
  long-press per-game actions ("Import Save File"); `.deltaskin` controller skins (`info.json`
  with `items`/`inputs`/`frame`/`extendedEdges`, `screens`, `mappingSize`), with a debug overlay;
  recommends in-game saves over save states; Auto/General/Locked states; Delta Sync (Dropbox,
  Google Drive) with version history and conflict resolution.
- **PPSSPP**: drag-to-place touch layout editor drawn over the game; per-game configs
  (`<GAMEID>.ini`, gear icon); "Report Feedback" compatibility button (hidden when cheats are
  on); exception handler keeps the app alive when a game crashes.
- **DolphiniOS**: "waiting for JIT" screen with "Use No JIT Mode"; JIT diagnostics in Debug menu.
- **UTM** anti-pattern: raw "QEMU exited from an error … (no message)" dialogs and black screens;
  users told to enable "Debug Log" and "Export Log…".

**Ranked recommendations.**
1. **Structured error screen instead of a crash**: "This game needs [NSOperationQueue], which
   HydrogenVM doesn't support yet", with copy/share of a report bundle (crash signature, missing
   symbols, last 2,000 log lines, bundle ID/version hash, device/iOS) and a link to the game's
   compatibility page.
2. **Library**: Files picker, share sheet, AirDrop; encryption check and armv7 slice choice;
   star rating shown before launch; icon artwork from the IPA.
3. **Per-game settings** with a gear badge: renderer (GPU / MetalANGLE / software), FPS cap,
   device profile, orientation lock, accelerometer source; curated defaults from the compat DB.
4. **Touch**: 1:1 letterbox-aware mapping by default; JSON overlays only for tilt and controllers.
5. **Saves**: expose `Documents/` and `Library/` through Files and sync; in-game saves over
   save states.

## Caveats

- Fork/community claims ("fully working") are self-reported.
- "OpenGLES.framework works on iOS 26" rests on developer reports and continued SDK presence, not
  an Apple guarantee — keep the MetalANGLE fallback and the software renderer.
- No sourced A-series guest-MIPS figures.
- RPCS3, Ryujinx/yuzu, Wine, Vita3K, ipasim, Darling and aspen sections lack primary sources.

## Sources

- touchHLE: [releases](https://github.com/touchHLE/touchHLE/releases),
  [repo](https://github.com/touchHLE/touchHLE),
  [FAQ](https://touchhle.org/faq.html),
  [debugging.md](https://github.com/touchHLE/touchHLE/blob/trunk/dev-docs/debugging.md),
  [issue #566](https://github.com/touchHLE/touchHLE/issues/566),
  [issue #250](https://github.com/touchHLE/touchHLE/issues/250),
  [appdb 85](https://appdb.touchhle.org/apps/85), [appdb 1020](https://appdb.touchhle.org/apps/1020),
  [function calls in depth](https://hikari.noyu.me/blog/2023-04-13-touchhle-in-depth-1-function-calls.html),
  [DeepWiki](https://deepwiki.com/touchHLE/touchHLE)
- Forks/wrappers: [XaView fork](https://github.com/XaViewDnK/touchHLE-XaView-fork/),
  [HyperHLE](https://github.com/HyperHLE/HyperHLE),
  [HyperHLE-Fork PR 108](https://github.com/KlugKlugTG/HyperHLE-Fork/pull/108),
  [HyperHLE-Reborn #249](https://github.com/KlugKlugTG/HyperHLE-Reborn/issues/249),
  [#266](https://github.com/KlugKlugTG/HyperHLE-Reborn/issues/266),
  [MetalHLE](https://github.com/RadekParek/MetalHLE/releases/tag/v1.0.0),
  [Applesauce](https://github.com/johnny901901901/Applesauce),
  [Chimera #316](https://github.com/ToolAssisted-run/chimera/issues/316),
  [LightTouchMac #9](https://github.com/samhenrigold/LightTouchMac/issues/9),
  [Emulation General: iOS emulators](https://emulation.gametechwiki.com/index.php/IOS_emulators)
- CPU: [UTM](https://github.com/utmapp/UTM), [HN on UTM SE](https://news.ycombinator.com/item?id=40958727),
  [TCTI README](https://github.com/tctiSH/qemu/blob/with_tcti/tcg/aarch64-tcti/README.md),
  [tctiSH PR 5](https://github.com/tctiSH/qemu/pull/5), [#2](https://github.com/tctiSH/qemu/issues/2),
  [#6](https://github.com/tctiSH/qemu/issues/6),
  [Dolphin 2407–2409 report](https://dolphin-emu.org/blog/2024/09/04/dolphin-progress-report-release-2407-2409/),
  [Dolphin PR 12723](https://github.com/dolphin-emu/dolphin/pull/12723),
  [DolphiniOS JIT help](https://dolphinios.oatmealdome.me/jit-help),
  [DolphiniOS releases](https://github.com/OatmealDome/dolphin-ios/releases),
  [PPSSPP forum](https://forums.ppsspp.org/showthread.php?tid=19003),
  [PPSSPP PR 19128](https://github.com/hrydgard/ppsspp/pull/19128),
  [PR 22584](https://github.com/hrydgard/ppsspp/pull/22584),
  [issue 15670](https://github.com/hrydgard/ppsspp/issues/15670),
  [LWN tail calls](https://lwn.net/Articles/1010905/), [LWN](https://lwn.net/Articles/1033373/),
  [CPython #128563](https://github.com/python/cpython/issues/128563),
  [HN](https://news.ycombinator.com/item?id=43317592),
  [rv32emu #601](https://github.com/sysprog21/rv32emu/issues/601)
- Graphics: [SwiftShader (Khronos)](https://www.khronos.org/developers/linkto/swiftshader-reference-implementation-and-fallback),
  [SwiftShader build](https://deepwiki.com/google/swiftshader/4.1-cmake-build-system),
  [SwiftShader commit](https://swiftshader.googlesource.com/SwiftShader/+/6f6ca293c707be352e6e0204810652de9720cc72),
  [MetalANGLE](https://github.com/kakashidinho/metalangle),
  [MetalANGLE #16](https://github.com/kakashidinho/metalangle/issues/16),
  [ANGLE](https://en.wikipedia.org/wiki/ANGLE_(software)),
  [ANGLEGLKit](https://github.com/pengubow/ANGLEGLKit),
  [Apple: multitasking-aware GLES apps](https://developer.apple.com/library/ios/documentation/3ddrawing/conceptual/opengles_programmingguide/ImplementingaMultitasking-awareOpenGLESApplication/ImplementingaMultitasking-awareOpenGLESApplication.html),
  [openFrameworks PR 8577](https://github.com/openframeworks/openFrameworks/pull/8577)
- Testing: [Dolphin dev infrastructure](https://dolphin-emu.org/blog/2015/01/25/making-developers-more-productive-dolphin-development-infrastructure/),
  [Dolphin usage statistics](https://dolphin-emu.org/blog/2016/06/19/optional-usage-statistics-reporting/),
  [PCSX2 PR 15088](https://github.com/PCSX2/pcsx2/pull/15088),
  [SDL AGENTS.md policy](https://discourse.libsdl.org/t/sdl-agents-md-updated-text-with-latest-policy-revisions/69201)
- UX: [Delta](https://github.com/rileytestut/Delta), [Delta FAQ](https://noah978.gitbook.io/delta-docs/faq),
  [Delta skins](https://noah978.gitbook.io/delta-docs/skins),
  [Delta save states](https://faq.deltaemulator.com/using-delta/save-states),
  [Delta Sync](https://faq.deltaemulator.com/using-delta/delta-sync),
  [skin guide](https://whoisryosuke.com/blog/2024/the-guide-for-designing-delta-skins/),
  [PPSSPP controls](https://www.ppsspp.org/docs/settings/controls/),
  [PPSSPP per-game configs](https://www.ppsspp.org/docs/reference/custom-game-configs/),
  [PPSSPP PR 20079](https://github.com/hrydgard/ppsspp/pull/20079),
  [UTM discussion 5840](https://github.com/utmapp/UTM/discussions/5840),
  [UTM #5762](https://github.com/utmapp/UTM/issues/5762)
