# HydrogenVM — handoff (2026-10-20)

## Active Clash Royale continuation (updated after runtime fixes)

For the current user-requested Clash Royale work, read
[`CLASH_ROYALE_RESTORATION.md`](CLASH_ROYALE_RESTORATION.md) first.
Runtime commit `095832c`: **55 local runtime checks passed**. Mach send-reference
accounting, checked VM reads, allocator/no-copy ownership and the reached
custom-TrueType/CoreText path now pass strict startup. The four-app local
strict/exploratory/relaunch investigation continues through its time limits;
older-game behavior remains consistent, including Tiny Tower's black frame.
Clash shows a UIKit **Connection error / Try again** dialog over mostly black
game output. Its scene shaders require actual GLES2 semantics (including a
component-wise vec4 position transform); shader dumping is now available via
`HVM_DUMP_SHADERS`. Despite bundled `OfflineMode=true`, connection attempts still
occur and neither offline save file has appeared. No menu or NPC match works.
Investigate actual offline-state entry before choosing a replacement server.
GitHub runs 39508176438 and 39508176627 never started test steps because of the
account billing/spending-limit block; validation for this revision is local.
See the restoration document for API limitations, exact evidence, commands and
next steps. These goals take precedence over the older roadmap below. Work
remains on `gemini-merge`; no game binary or assets were changed.


JIT-less emulator that runs **32-bit iOS apps (.ipa, armv6/armv7) on modern iPhones**.
Sideload-only iPhone app, bundle id `com.lfull.HydrogenVM`.
Repo `github.com/lawsharley0-cyber/HydrogenVM`, **all work on branch `gemini-merge`**.
The owner (Lawson) tests sideloaded builds on device and sends logs and IPAs. He wants
**systematic fixes**: find every issue per IPA, fix it once in the shared runtime, and
verify the fix across all apps.

## Build, test, CI

```sh
cmake -S . -B build -G Ninja && ninja -C build hvm hvm_tests   # host build (Linux/macOS); needs zlib, sqlite3
build/hvm_tests fulltest          # 28 runtime checks, all must pass
build/hvm run <App.app> --home <dir> --run-for 30 [--keep-going] [--screenshot f.png] [--report r.json]
      [--graceful] [--lifecycle bg@8,fg@13] [--launches 2] [--trace] [--tap x,y@t] [--drag ...]
tools/compat.py scan <ipas|apps|dirs> --out compat --seconds 30 [--relaunch] [--only a,b]   # see docs/COMPAT.md
```

* **CI** is in `.github/workflows/ci.yml`. The Linux job runs on every push. The macOS job
  (Release device app, **unsigned IPA artifact `macos-results` → `HydrogenVM-unsigned.ipa`**,
  simulator run) runs only when the commit message contains **`[mac]`** or on
  workflow_dispatch. A new push cancels the in-progress run. Reports go to the `ci-reports`
  branch. Artifact downloads 403 from the Claude sandbox, so the user downloads them himself.
* The iOS app is in `app/` (XcodeGen `project.yml`, Swift 6, iOS 26 target). Swift can only be
  compiled by the macOS CI job; there is no local Swift toolchain on Linux.
* Commit trailers the user's tooling expects: `Co-Authored-By: …` and `Claude-Session: …`.

## Architecture (where things are)

* `src/core/cpu/`: threaded-IR interpreter (ARM/Thumb/VFP/NEON decoders, `Engine.cpp`).
  * Blocks are decoded lazily into ops that tail-call each other; there is no JIT.
  * Supports `fusePairs`/`fuseOps` superinstructions.
  * Per-Cpu `suspend_count`/`in_guest` safepoints back `Engine::suspend/resume` (used by
    `thread_suspend`).
* `src/loader/`: Mach-O loader (fat, dyld-info and classic relocations, **weak binds now
  applied**), IPA/zip, plist.
* `src/runtime/Process.*`: one guest process.
  * Symbol registry: `p.def<&fn>("_sym")` registers typed HLE functions;
    `defineFunction(name, raw)` registers raw ones; `defineDataBlock`.
  * Imports are bound to HLE functions or to "missing" stubs. The heuristics for whether a
    missing import is data or a function are in `makeMissingStub` and `looksLikeData`/
    `looksLikeFunction`. Missing-symbol resolvers provide string constants.
  * Threads, lifecycle API, `processSlot`/`threadSlot`.
* `src/runtime/libc/`: libc/POSIX, pthread + Mach threads, `Unwind.cpp` (SJLJ C++/ObjC
  exceptions), `LibStdcxx*.cpp` (GCC 4.2 libstdc++: COW string, rb-tree, list, file streams,
  string streams/ostream formatting), `Zlib.cpp`, `Sqlite.cpp` (host sqlite), `Net.cpp`
  (offline sockets), `CommonCrypto.cpp`, `Posix.cpp`, `Stdlib.cpp` (malloc zones).
* `src/runtime/objc/`: ObjC runtime. `ClassBuilder` (`r.defineClass`, `r.builder(cls)`,
  `.m<&fn>(sel, types)`, `.cm<>`); placeholder "stub" classes return nil; `forwardHook`
  handles `-forwardInvocation:`.
* `src/runtime/foundation/`: Foundation/CF.
  * Strings, collections, defaults, run loops, dispatch, formatters, `Calendar.cpp`.
  * `Network.cpp`: CFNetwork/NSStream, offline.
  * `UrlLoading.cpp`: NSURLConnection/Request, offline (fails with -1009).
  * `Invocation.cpp`: NSInvocation and forwarding.
  * `Extras.cpp`: NSScanner, NSXMLParser, NSURLResponse, NSHTTPCookie(Storage), NSCache,
    NSDecimalNumber.
  * `Security.cpp`: keychain.
* `src/runtime/uikit/`: UIKit, nibs, widgets, QuartzCore, GameKit, and audio stubs.
  * `OpenGLES.cpp`: GLES1 fixed-function plus a **fake GLES2**; see the GLSL section below.
  * `PlatformStubs.cpp`: audio (OpenAL, AudioQueue, AVAudioPlayer, ExtAudioFile, AudioFile)
    and StoreKit.
  * `MiscFrameworks.cpp`: UIWebView, CLLocationManager (denied), MPMoviePlayer (finishes
    immediately), MPMusicPlayer, UIImagePicker (unavailable), UTType, CoreTelephony,
    CaptiveNetwork.
  * `UIApplication.cpp`: the lifecycle handler.
* `src/runtime/audio/`: software mixer (AVAudioSourceNode pulls it on device), decoders for
  WAV/CAF/AIFF/MP3 (minimp3).
* `src/runtime/graphics/`: PNG, PVRTC decode, 2D raster, text.
* `app/HydrogenVM/`:
  * `Bridge/HVMEngine.mm`: ObjC bridge; HVMSession start/stop/**stopGracefully/
    enterBackground/enterForeground**, audio, per-run logs in Documents/Logs.
  * `RunView.swift`: runs a game; Stop = save then close, with HydrogenVM's background state
    forwarded to the game.
  * `LibraryView.swift`: apps in `Documents/Apps/<bundle id>/<Name>.app`, with the app's
    sandbox beside the bundle.
  * **`FileBrowser.swift`** (new): the per-app file explorer.

## Conventions and gotchas (learned the hard way)

* **32-bit iOS calling convention is APCS.** 64-bit arguments are **not** moved to an even
  register: `f(int a, long long b)` → a=r0, b=r1:r2. Structs larger than 4 bytes are returned
  through r0 (sret). Floats travel in core registers (softfp). `ArgReader`/`p.def` already do
  this. Hand-decoded raw functions must too.
* **Never cache guest addresses in C++ statics.** The iOS app runs one `Process` per launch,
  so a static leaks into the next launch; this caused the crash when reopening a game. Use
  `static const char key=0; uint32_t& v = x.proc.processSlot(&key);` (or `threadSlot`).
* GCC libstdc++ layouts are documented at the top of `LibStdcxx.cpp`,
  `LibStdcxxContainers.cpp` and `LibStdcxxStreams.cpp`.
* Classic Mach-O external relocations are relative to the first segment.
* Weak binds: override only when the symbol is registered; otherwise keep the app's own
  definition.
* Missing data symbols become zero blocks. String-constant names (…Key, …Notification, k…)
  resolve to constant NSStrings, including for weak imports.
* The network is deliberately **offline** everywhere (sockets, CFNetwork, NSURLConnection,
  reachability).
* `CCCrypt` deliberately returns kCCUnimplemented; implementing ciphers was declined.
* **FairPlay-encrypted IPAs are unsupported.** We do not help decrypt.
* Debugging aids:
  * `HVM_TRACE_BLOCKS=lo-hi` logs blocks with registers (slow).
  * `--trace` logs HLE calls.
  * `HVM_TRACE_DRAW`, `HVM_TRACE_TRI`, `HVM_DUMP_TEX`, `HVM_DUMP_RB`.
  * Disassembly: llvm-objdump is too slow on the fat binaries, so extract bytes and use
    `llvm-mc --disassemble -triple=thumbv7-apple-ios`. A helper script for this was in the
    scratchpad and is easy to rewrite.

## Status (compat scan, 30 s, strict and relaunch)

| App | Graphics | Status |
|---|---|---|
| Angry Birds 1.0 | GLES1 | runs, title screen OK |
| Tiny Tower 1.0 | GLES1 | runs; saves on close; relaunch OK |
| Art of Glow 1.1.2 | GLES1 | runs |
| Jetpack Joyride 1.3.5 | GLES1 | runs to its OpenFeint opt-in screen (not tapped through yet), ~3 fps on Linux |
| Minecraft PE Lite 0.1.2 | GLES1 | runs, main menu renders |
| Subway Surfers 1.41 | **GLES2 / Unity** | runs, but **renders almost nothing** (frame check flags 93 % white) |

All six apps have zero runtime misses in strict mode. The remaining load-time gaps are listed
in `compat/REPORT.md` after a scan (single-app: EventKit, NSPredicate, NSRegularExpression,
NSSortDescriptor, NSMapTable/HashTable, NSAttributedString, UIBezierPath, UIPasteboard,
AddressBook, …).

## This session's features (all committed; last commit `1a0787b`)

1. **Compatibility scanner** `tools/compat.py` (see `docs/COMPAT.md`).
   * Per app: strict run (first blocker + screenshot), keep-going run (all runtime misses),
     static gaps, frame check (blank or one-colour frames), GL/engine detection.
   * Fixes are ranked by apps affected; regressions are diffed against the previous scan.
   * `--relaunch` opens each app twice in one process.
2. **Save on exit and background pause.**
   * `Process::requestGracefulStop` delivers resign-active → enter-background →
     will-terminate, flushes NSUserDefaults and stdio, then stops; a watchdog force-stops
     after 5 s.
   * `enterBackground` delivers the background events, then parks the main thread, suspends
     the other guest threads and silences the mixer. `enterForeground` reverses it.
   * The iOS Run screen uses these (shows "Saving…"); `didEnterBackground` uses a background
     task.
3. **Crash when reopening a game: fixed** (the stale statics above).
4. **File explorer** (Library → swipe or long-press → Files).
   * Browses the bundle plus Documents/Library/tmp.
   * Structured plist editor (keeps binary or XML format), text editor, image view, hex view
     with byte patch.
   * Import (replaces same-named files), share, rename, duplicate, delete, new folder/file.
5. Many runtime additions:
   * Libraries: zlib, sqlite, string streams, thread_suspend/get_state/task_threads, malloc
     zones, xattr stubs, asl.
   * Foundation and system frameworks: NSCalendar, NSInvocation, CFNetwork, NSURLConnection,
     SKProductsRequest, NSScanner, NSXMLParser, and the rest of the Extras.cpp classes.
   * Graphics and media: GL multisample resolve, AudioFileOpenWithCallbacks, AudioQueue
     extras.
   * Misc: UTType, CCHmac, objc_getClassList, and std::string accessors.

## Open item to check first

* **CI run `39379622776` (commit `1a0787b`, `[mac]`) was still running when this handoff was
  written.** It is the **first compile of `FileBrowser.swift` and the RunView lifecycle
  Swift**, which were written blind. If the macOS job fails, fix the Swift errors, most
  likely Swift 6 concurrency or SwiftUI API mismatches. Then tell the user to grab the
  unsigned IPA.

## Next steps (agreed with the user, in order)

1. **GLSL ES 1.00 interpreter (main task).** Today `gl_linkProgram` only regex-scans
   declarations and guesses roles: first mat4 = MVP, first sampler = texture, attribute names
   containing pos/col/tex. `shadeVertex`/`rasterizeTriangle` then draw as fixed-function.
   Unity shaders break this. Planned design, all in a new `src/runtime/graphics/glsl/`:
   * **Preprocessor:**
     * `#define`, with and without parameters; `#undef`.
     * `#if`/`#ifdef`/`#ifndef`/`#elif`/`#else`/`#endif`, with expression evaluation and
       `defined()`.
     * `#version`, `#extension`, `#pragma` and `#line` are accepted.
     * Predefined `GL_ES` and `__VERSION__ 100`; line continuations.
   * **Lexer and parser → AST:**
     * Precision, storage and parameter qualifiers.
     * Structs, arrays, and function prototypes with overloading.
     * All statements and expressions, including swizzles, constructors and indexing.
   * **Semantic pass:**
     * Types: float/int/bool scalars and vectors, mat2–4, sampler2D/Cube, structs, arrays.
     * GLSL ES 1.00 has no implicit conversions.
     * Overload resolution and the builtin functions.
   * **Codegen to an IR** with float4 registers (ints and bools as floats; a matrix is N
     registers):
     * Each source operand has a swizzle and negate; each destination has a write mask.
     * **Inline all functions** (GLSL ES has no recursion).
     * Dynamic array indexing ops (skinning bones).
   * **Lane-parallel interpreter**:
     * Runs 16 vertices or fragments per dispatch in SoA layout.
     * Execution masks handle if/loops/break/continue/discard.
     * Ops: arithmetic, the full builtin math set, matrix ops expanded to dp/mad, and
       texture2D/Proj/Lod/Cube through a batched sampler callback.
   * **GL integration:**
     * Real compile and link status and info logs.
     * Active uniform and attribute introspection: glGetActiveUniform/Attrib,
       glGetProgramiv counts, and glGetUniformLocation for `a[i]` and `s.f`. Unity queries
       these.
     * Uniform storage per location, including arrays and samplers.
     * Generic vertex attributes.
     * gl_Position, varyings, gl_FragCoord, gl_FrontFacing, gl_PointCoord and
       gl_PointSize.
   * **Fix the raster stage for everyone:**
     * Homogeneous **near-plane clipping**.
     * **Perspective-correct** interpolation (the `invW` value is computed but unused
       today).
     * Early depth test with late write when the shader may `discard`.
     * Mipmaps and derivatives could come later via 2×2 quads.
   * **Do this first:** add a shader-source dump (env `HVM_DUMP_SHADERS=<dir>` in
     `gl_shaderSource`; that edit was not applied). Collect Subway Surfers' shaders as a
     corpus and make the compiler accept all of them before integrating. Then judge it with
     the frame check and screenshots in `compat.py`.
2. **Minecraft Story Mode** (Telltale) is too large for the user to upload (ask for a zip of
   the `.app` without its big asset folders). It uses **libc++ (`std::__1::…`)**, a second C++
   runtime ABI next to GCC libstdc++: strings, ostream/streambuf, and typeinfo for
   `std::__1` types. Its first blocker, `malloc_create_zone`, is already done.
3. Jetpack: tap through the OpenFeint opt-in (likely fine on device). Performance: Jetpack
   runs at ~3 fps and Subway at ~5 fps on the Linux CI box; device numbers will differ.
4. Work down the remaining load-time gaps from the scanner (start with the ones shared by
   several apps).
5. The user asked earlier about **APK support**; it was discussed only conceptually and
   nothing is started.

Scratch data from this session (IPAs, compat output, screenshots) lived in the Claude sandbox
and is **not** in the repo. The user's IPAs: Angry Birds 1.0, Tiny Tower 1.0, Art of Glow
1.1.2, Jetpack Joyride, Minecraft PE Lite 0.1.2, Subway Surfers 1.41. Ask the user to
re-upload them if needed.
