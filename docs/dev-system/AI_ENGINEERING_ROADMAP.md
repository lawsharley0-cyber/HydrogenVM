# HydrogenVM Architecture, Engineering Roadmap & AI Agent Decision Guide

> **Repository:** [lawsharley0-cyber/HydrogenVM](https://github.com/lawsharley0-cyber/HydrogenVM)  
> **Recommended Repo Path:** `docs/dev-system/AI_ENGINEERING_ROADMAP.md` (linked from root [`AGENTS.md`](https://github.com/lawsharley0-cyber/HydrogenVM/blob/main/AGENTS.md))  
> **Target Audience:** Autonomous AI coding agents (`cursor`, `codex`, `gemini`, `claude`) and human maintainers working on `HydrogenVM`.  
> **Baseline Reference Commit:** `main` (`39904cb` / `6af9976` Linux headless baseline)

## 1. Purpose & How Future AI Agents Should Use This Document

This document synthesizes how **HydrogenVM** works end-to-end, audits the current state of `main` (including merged commits, open pull requests, and backlog drift), and establishes the concrete technical roadmap across **Performance**, **App Compatibility**, and **User Experience (UX)**.

When an AI agent enters the repository to select or execute a task, it must use this guide alongside [`AGENTS.md`](https://github.com/lawsharley0-cyber/HydrogenVM/blob/main/AGENTS.md), [`docs/dev-system/BACKLOG.md`](https://github.com/lawsharley0-cyber/HydrogenVM/blob/main/docs/dev-system/BACKLOG.md), and [`docs/dev-system/STATUS.md`](https://github.com/lawsharley0-cyber/HydrogenVM/blob/main/docs/dev-system/STATUS.md) to:
1. Understand the exact invariants of the subsystem it is touching before writing code.
2. Check whether a backlog item has already been solved in recent commits or open PRs.
3. Respect the **Exclusive Group Ownership (`A`–`E`)** boundaries so parallel agent branches merge cleanly.
4. Choose high-leverage engineering solutions that preserve iOS W^X compliance, deterministic test behavior, and zero-regression guarantees.

## 2. How HydrogenVM Works: Complete System Architecture

HydrogenVM is a **JIT-less 32-bit ARMv7 (`armv6` / `armv7` / `armv7s`) user-mode emulator and High-Level Emulation (HLE) runtime** written in C++20, Objective-C++, and SwiftUI. It executes legacy 32-bit iOS applications (`.ipa` bundles predating iOS 11) on modern 64-bit Apple Silicon iOS devices (`arm64`), macOS, and Linux (`x86_64` / `arm64`) **without generating executable code at runtime** and **without requiring iOS JIT entitlements (`dynamic-codesigning` / `CS_EXECSEG_ALLOW_UNSIGNED`)**.

### 2.1 Subsystem Architecture & File Map

| Subsystem | Group | Primary Source Files | Core Responsibility & Mechanism |
| :--- | :--- | :--- | :--- |
| **Guest Memory Arena & Heap** | **Group A** | `src/core/memory/GuestArena.hpp`, `GuestArena.cpp`, `src/runtime/Heap.hpp`, `Heap.cpp`, `src/runtime/libc/System.cpp` | Reserves a contiguous 4 GB host virtual address range (`0x00000000`–`0xFFFFFFFF`) via `mmap` / `vm_allocate`. Translates 32-bit guest pointers via `base_ptr + guest_addr`. Tracks page permissions/commits and manages the guest heap (`malloc`/`free`/`realloc`/`posix_memalign`). |
| **JIT-Less CPU Core & Decoders** | **Group A / Core** | `src/core/cpu/Cpu.hpp`, `Engine.hpp`, `Engine.cpp`, `BlockBuilder.hpp`, `DecodeArm.cpp`, `DecodeThumb.cpp`, `DecodeVfpNeon.cpp`, `DecodeNeon.cpp`, `Handlers.hpp`, `FpHandlers.hpp`, `NeonHandlers.hpp` | Lazily decodes ARM (A32) and Thumb/Thumb-2 (T16/T32) basic blocks on first execution into flat arrays of `Op` structs. Dispatches ahead-of-time compiled C++ handler functions chained via `[[clang::musttail]]` tail-calls. |
| **Binary Loaders & Containers** | **Core / D** | `src/loader/MachO.cpp`, `AppBundle.cpp`, `Plist.cpp`, `Zip.cpp`, `ElfLoader.cpp` | Extracts `.ipa` ZIP/ZIP64 archives, parses XML and binary `Info.plist` files, loads 32-bit thin/fat Mach-O segments (`LC_SEGMENT`), applies rebases relative to the first segment, and binds `LC_DYLD_INFO_ONLY` / classic imports to HLE host trampolines. |
| **HLE ABI, LibSystem & C++ Runtime** | **Group D** | `src/runtime/Abi.hpp`, `Process.hpp`, `src/runtime/libc/*` (`Libstdcxx.cpp`, `LibstdcxxContainers.cpp`, `LibstdcxxStreams.cpp`) | Implements 32-bit Darwin APCS calling conventions, POSIX file/thread/regex/socket shims, 1:1 host-backed `pthread` execution, and ABI-compatible GCC 4.2 `libstdc++` class layouts. |
| **Objective-C Runtime & Cocoa Touch** | **Group D** | `src/runtime/objc/*`, `src/runtime/foundation/*`, `src/runtime/uikit/*` | Parses Mach-O `__objc_classlist`, `__objc_catlist`, and `__objc_protolist`. Implements `objc_msgSend` with inline method caches, ARC, weak tables, blocks, `NSRunLoop`, GCD, NIB/Storyboard unarchiving, persistent Keychain (`SecItem*`), and UIKit views/controllers. |
| **2D/3D Graphics & GLSL ES Engine** | **Group B** | `src/runtime/graphics/Graphics.hpp`, `Raster.cpp`, `OpenGLES.cpp`, `Shader.hpp`, `Shader.cpp` | Software 64 Hz QuartzCore `CALayer` compositor, antialiased 2D path rasterizer, `CgBI` PNG decoder, and OpenGL ES 1.1 (fixed-function) + ES 2.0 (programmable shader compiler/VM with `MaxSlots = 2048` and software rasterizer). |
| **Audio Subsystem** | **Group D** | `src/runtime/audio/*` | Emulates `AVAudioPlayer`, `AVAudioSession`, `AudioToolbox` (`AudioQueue`, `AudioServices`, `AudioFile`, `AudioUnit`), OpenAL (`alc*`/`al*`), and IMA ADPCM (`0x11`) / linear PCM decoding, streaming stereo PCM to `AVAudioEngine` on iOS. |
| **Clash Wire & Offline Protocol** | **Group C** | `tools/clash_*.py`, `src/runtime/libc/Net.cpp`, `docs/CLASH_ROYALE_RESTORATION.md` | Deterministic local protocol bridge and state probes (`clash_home_probe.py`, `clash_protocol_probe.py`) for restoring offline execution of pinned 32-bit client builds. |
| **iOS Host App Shell** | **Group E** | `app/project.yml`, `app/HydrogenVM/HydrogenVMApp.swift`, `LibraryView.swift`, `RunView.swift`, `DiagnosticsView.swift`, `FileBrowser.swift`, `Bridge/HVMEngine.h`, `Bridge/HVMEngine.mm` | Native iOS 26+ SwiftUI host application (`com.lfull.HydrogenVM`) providing `.ipa` import, app library, touch/accelerometer routing, live display, diagnostics, and an integrated multi-format save editor. |
| **CLI, Fuzzer & Compat Harness** | **Infra** | `tools/hvm_compat.py`, `tools/decode_fuzz.py`, `src/runtime/FrameHistogram.hpp`, `docs/dev-system/baselines/linux-headless.json` | Headless CLI (`hvm`), unit/differential test runner (`hvm_tests`), 544k-encoding instruction fuzzer (`hvm_decodecheck`), and automated regression gate (`hvm_compat.py`). |

### 2.2 Critical Architectural Invariants (Never Violate)

1. **Zero Runtime Code Generation (W^X Invariant):** Never emit `PROT_EXEC` memory or runtime machine code on host. All CPU performance optimizations must operate within ahead-of-time compiled C++ handler tables (`Handlers.hpp`, `FpHandlers.hpp`, `NeonHandlers.hpp`), basic-block IR lowering (`BlockBuilder.hpp`), and tail-call dispatch (`[[clang::musttail]]`).
2. **32-Bit iOS APCS ABI Rules (`src/runtime/Abi.hpp`, `docs/HANDOFF.md`):**
   - Unlike standard AAPCS, 32-bit Darwin/iOS APCS **does not align 64-bit integer arguments (`int64_t` / `uint64_t`) to even register pairs**. A function `void foo(int32_t a, int64_t b)` receives `a` in `r0` and `b` in `r1:r2` (not `r2:r3`), and on the stack `int64_t` arguments are 4-byte aligned.
   - Composite types (structs) larger than 4 bytes are returned via a caller-allocated buffer pointer passed in `r0` (shifting visible arguments to `r1..r3`).
3. **Multi-Run Process Isolation:** On iOS, a user can start, stop, and launch another `.ipa` within the same host app lifetime. **Never store guest pointers or per-app state in function-local `static` or global C++ variables** that persist across `Process` instances without explicit teardown on session stop.
4. **4 KB Guest vs. 16 KB Host Page Granularity:** Linux `x86_64` uses 4 KB pages (`host.pageSize = 4096`), whereas Apple Silicon iOS uses 16 KB hardware pages (`16384`). Never assume `mmap` or `mprotect` on the host can directly split a 16 KB page on a 4 KB boundary without software permission tracking in `GuestArena`.
5. **Security & Repository Hygiene:**
   - Never commit proprietary/commercial `.ipa` binaries, Apple signing certificates, or secrets.
   - Keep `CCCrypt` encryption ciphers returning `kCCUnimplemented` and keep guest networking offline unless the repository owner explicitly changes policy (`B-204`).

## 3. Audit of Current Repository State, Open PRs & Backlog Drift

Future AI agents must check the live state below before starting any task in `docs/dev-system/BACKLOG.md`, because several items still marked `Planned` in `BACKLOG.md` have already been implemented on `main` or in open pull requests.

### 3.1 Recently Merged Work on `main` (Update `BACKLOG.md` Status When Touching)

| Commit / PR | Summary of Landed Change | Backlog Item Impact |
| :--- | :--- | :--- |
| `39904cb` ([PR #7](https://www.google.com/search?q=https://github.com/lawsharley0-cyber/HydrogenVM/pull/7)) | Added Codemagic unsigned iOS workflow (`codemagic.yaml`), fixed Swift 6 errors in `app/`, replaced corrupt `AppIcon`, and fixed macOS/Linux audio backpressure in host tests. | Advances **B-004** (macOS/iOS compile verification). |
| `9580bb3` ([PR #6](https://www.google.com/search?q=https://github.com/lawsharley0-cyber/HydrogenVM/pull/6)) | Added bounded frame-time percentiles (`p50`, `p95`, `p99`) and time-to-first-frame (`FrameHistogram.hpp`) to `hvm --report`. | Advances **B-105** observability and performance gating. |
| `a767928` ([PR #5](https://www.google.com/search?q=https://github.com/lawsharley0-cyber/HydrogenVM/pull/5)) | Implemented Darwin POSIX regex (`regcomp`, `regexec`, `regerror`, `regfree` using a one-pass Thompson NFA) and sandbox `tmpnam`. | Resolves **B-108** (*Art of Glow* regex) and **B-110** (*Angry Birds* `tmpnam`). `BACKLOG.md` still lists both as `Planned` and needs status sync. |
| `0c06835` ([PR #4](https://www.google.com/search?q=https://github.com/lawsharley0-cyber/HydrogenVM/pull/4)) | Implemented homogeneous view-volume primitive clipping in `OpenGLES.cpp`. | Resolves **B-103** (homogeneous near-plane clip) and unblocks SIMD rasterization (**B-105**). |
| `d12a943` ([PR #9](https://www.google.com/search?q=https://github.com/lawsharley0-cyber/HydrogenVM/pull/9)) & `ae1eddd` ([PR #8](https://www.google.com/search?q=https://github.com/lawsharley0-cyber/HydrogenVM/pull/8)) | Added GLSL ES preprocessor, user functions, built-ins, ES 1.00 constructs, and per-draw shader instruction budget cap. | Advances **B-104** (GLSL ES 1.00 subset widening). |
| `a914c0b` ([PR #3](https://www.google.com/search?q=https://github.com/lawsharley0-cyber/HydrogenVM/pull/3) / [#10](https://www.google.com/search?q=https://github.com/lawsharley0-cyber/HydrogenVM/pull/10)) & `606301f` ([PR #2](https://www.google.com/search?q=https://github.com/lawsharley0-cyber/HydrogenVM/pull/2)) | Added `AGENTS.md`, `docs/dev-system/*`, Linux headless baseline, and indexed compat reports. | Resolves **B-001** and **B-002**. |

### 3.2 Open Pull Requests & Open Issues (Check Before Editing)

| PR / Issue | Group | Scope & Files Touched | Action for AI Agents |
| :--- | :--- | :--- | :--- |
| **[Issue #14](https://github.com/lawsharley0-cyber/HydrogenVM/issues/14)** | **Group B** | `OpenGLES.cpp:1057` (`GL_MAX_VARYING_VECTORS` returns 16) vs `OpenGLES.cpp:2102` (linker rejects > 32 floats / 8 `vec4`) and `Shader.cpp` (`gl_MaxVaryingVectors = 8`). | Fix in next Group B turn after PR #11 and PR #12 merge. |
| **[PR #11](https://github.com/lawsharley0-cyber/HydrogenVM/pull/11)** | **Group B** | Fold GLSL ES constant-expression array sizes, warn on `#extension : warn`, and guard struct slot bounds in `Shader.cpp`. (Checks 5/5 passing). | Do not start competing Group B shader parser work until PR #11 and PR #12 are merged. |
| **[PR #12](https://github.com/lawsharley0-cyber/HydrogenVM/pull/12)** | **Group B** | Keep uniform arrays in memory and recycle temporary shader registers in `Shader.cpp` / `Shader.hpp`. (Checks 4/4 passing). | Coordinates with PR #11 in Group B. |
| **[PR #13](https://github.com/lawsharley0-cyber/HydrogenVM/pull/13)** | **Group C** | Adds Clash Royale work plan for `main`. (Checks 5/5 passing). | Use as foundation for **B-106** (`21903 SectorStateMessage`). |
| **[PR #15](https://github.com/lawsharley0-cyber/HydrogenVM/pull/15)** | **Group D** | Reject wrapped aligned heap allocations in `Heap.cpp` and preserve owner app reports. (Checks 5/5 passing). | Ready to merge; avoid conflicting edits to `Heap.cpp`. |
| **[PR #16](https://github.com/lawsharley0-cyber/HydrogenVM/pull/16)** | **Group D** | Guard shared PCM sample widths and non-finite float conversions in audio decoder. (Checks 6/6 passing). | Ready to merge; avoid conflicting edits to audio PCM decoding. |
| **[PR #17](https://github.com/lawsharley0-cyber/HydrogenVM/pull/17)** | **Loader** | Reject truncated ZIP64 metadata without leaking archive handles in `Zip.cpp`. (Checks 6/6 passing). | Ready to merge; avoid conflicting edits to `Zip.cpp`. |

## 4. Future Engineering Pillar 1: Performance Optimization

HydrogenVM currently achieves **~2440 CoreMark** (~127% of iPhone 4 at ~1900 CoreMark, ~54% of iPhone 5 at ~4500 CoreMark). However, complex 2D/3D games and shader-heavy scenes are bottlenecked by software triangle/fragment rasterization, full-frame compositing copies, and basic-block dispatch boundaries.

### 4.1 CPU Core & Dispatch Engine (`src/core/cpu/`)

| Initiative | Target Files | Technical Implementation Specification | Expected Impact |
| :--- | :--- | :--- | :--- |
| **P-CPU-1: Pinned In-Register Budget & Hot State (`B-105`, `B-201`)** | `Cpu.hpp`, `Handlers.hpp`, `Engine.cpp` | Move the time-slice instruction counter (`Cpu::budget`) and hot guest registers (`r0`–`r2`, `nzcv`, `GuestArena::base_ptr`) into tail-call function parameters so the compiler pins them in physical host registers across `[[clang::musttail]]` hops instead of reloading `cpu.budget` from memory on every block terminal. Provide clean macro fallbacks when compiling on Linux Clang 18 (which lacks `preserve_none` on some targets) versus Apple Clang. | **+8% to +15% CoreMark / MIPS** by eliminating per-block memory traffic. |
| **P-CPU-2: Superblock Formation & Side-Exit Chaining** | `BlockBuilder.hpp`, `Engine.cpp`, `Handlers.hpp` | Currently, decoding stops at the first branch instruction (`B`, `BL`, `CBZ`, `CBNZ`). Extend `BlockBuilder` to trace across unconditional branches and statically predictable forward/loop branches into multi-block **superblocks** (up to 64–128 instructions), emitting lightweight side-exit ops when a branch is not taken. | **+15% to +25% CPU throughput** on branch-dense Objective-C and C++ game logic. |
| **P-CPU-3: Return Address Stack (RAS) & Indirect Call Cache** | `Cpu.hpp`, `Handlers.hpp` | Add a small 16-entry circular Return Address Stack (`ras[16]`) or 1-entry inline `(guest_pc, Block*)` cache on `BL` / `BLX` and `BX LR` / `POP {..., PC}` handlers. On `BL`, push `(return_pc, next_block_ptr)`; on `BX LR` / `POP {..., PC}`, compare `pc == top.guest_pc` and jump directly to `top.block_ptr` without touching the block lookup table. | **+10% to +18% speedup** in call-heavy C++ / Objective-C code. |
| **P-CPU-4: Intrinsic Fast-Paths for Memory & String Loops** | `Engine.cpp`, `src/runtime/libc/System.cpp`, `BlockBuilder.hpp` | Ensure all Mach-O bindings and PLT stubs for `memcpy`, `memmove`, `memset`, `bzero`, `strlen`, `strcmp`, and `memcmp` execute direct host SIMD implementations over `GuestArena::base_ptr` after a single bounds/permission check. Additionally, detect canonical 2-instruction ARM/Thumb byte/word copy loops in `BlockBuilder` and fuse them into a bulk memory op. | Eliminates multi-million-instruction stalls during asset decompression and buffer copies. |
| **P-CPU-5: Sub-Range Code Cache Invalidation** | `Engine.hpp`, `Engine.cpp` | Replace full code-cache flushes on `sys_icache_invalidate` or `mprotect` with fine-grained page-indexed block invalidation (`invalidateRange(addr, len)`) backed by a per-4KB-page vector of decoded block pointers. | Prevents frame hitches when guests load dynamic bundles or run Mono/Lua interpreters. |

### 4.2 Graphics, Shader VM & Software Rasterizer (`src/runtime/graphics/` — Group B)

Now that homogeneous view-volume clipping (`Commit 0c06835` / `B-103`) is on `main`, **Group B is unblocked to implement SIMD and multi-threaded rasterization (`B-105`)**.

| Initiative | Target Files | Technical Implementation Specification | Expected Impact |
| :--- | :--- | :--- | :--- |
| **P-GFX-1: 2x2 Quad Stamp & 4-Wide SIMD Triangle Rasterizer (`B-105`)** | `OpenGLES.cpp`, `Raster.cpp` | Refactor the inner triangle rasterizer loop to step in **2x2 pixel quads** using ARM64 NEON (`float32x4_t` / `int32x4_t`) on Apple Silicon and SSE2/AVX2 on `x86_64`. Compute edge functions, perspective-correct $1/w$, depth buffer tests, and color blending for 4 pixels in parallel. Stepping in 2x2 quads also makes `dFdx` and `dFdy` screen-space derivatives in `Shader.cpp` exact via horizontal/vertical quad lane subtraction. | **2.5x to 4x reduction in triangle rasterization time** (`p95`/`p99` frame times). |
| **P-GFX-2: 4-Wide SIMD Fragment Shader VM (`Shader.hpp`, `Shader.cpp`)** | `Shader.hpp`, `Shader.cpp`, `OpenGLES.cpp` | After merging [PR #11](https://github.com/lawsharley0-cyber/HydrogenVM/pull/11) and [PR #12](https://github.com/lawsharley0-cyber/HydrogenVM/pull/12), upgrade `Shader::Program::execute` to offer an `executeQuad(QuadValues&)` entry point that runs each `Instruction` across 4 fragments simultaneously (`float lanes[4]` per slot), amortizing opcode switch/dispatch overhead by 4x. Add compile-time dead-code elimination and algebraic simplification (`x * 1.0`, `x + 0.0`, `MAD` fusion `MultiplyAdd`). | **3x faster GLSL ES 2.0 fragment execution** on shader-heavy games (*RETRY*, *Art of Glow*). |
| **P-GFX-3: Upload-Time Texture Conversion & Mipmap Cache (`B-105`)** | `OpenGLES.cpp` | Stop decoding `GL_RGB565`, `GL_RGBA4444`, `GL_RGBA5551`, `GL_LUMINANCE_ALPHA`, and compressed formats per-texel inside the inner sampling loop. Convert textures into contiguous host `RGBA8888` (and precomputed swizzled/tiled layout for cache locality) at `glTexImage2D` / `glTexSubImage2D` upload time, gated by a `dirty` generation flag. | **30% to 50% faster textured draw calls** and better CPU L1/L2 cache hit rates. |
| **P-GFX-4: Multi-Threaded Tile/Band Slicing (`B-105`)** | `OpenGLES.cpp`, `Raster.cpp` | Bin screen-space triangles into 64x64 pixel tiles or horizontal scanline bands and dispatch bands across a persistent host worker thread pool (2–4 threads) when a draw batch exceeds a triangle/area threshold. Because each worker owns disjoint scanlines of the color and depth buffers, zero per-pixel atomic locking is required. | Near-linear scaling across multi-core Apple Silicon (A14–A19) and desktop CPUs. |
| **P-GFX-5: Dirty-Rect `CALayer` Compositing & Zero-Copy Frame Present (`B-105`, Group B/E)** | `Raster.cpp`, `OpenGLES.cpp`, `app/HydrogenVM/Bridge/HVMEngine.mm`, `RunView.swift` | Track a bounding `dirtyRect` union across mutated `CALayer` nodes and skip re-compositing static UI chrome. On iOS (`HVMEngine.mm` / `RunView.swift`), replace per-frame `CGImage` allocation and CPU buffer copies with a double-buffered `MTLTexture` or `IOSurface`-backed `CVPixelBuffer` rendered directly by a lightweight `MTKView`. | Eliminates main-thread UI copying overhead and reduces iOS battery/thermal draw. |

### 4.3 Memory Arena & Allocator (`GuestArena.cpp`, `Heap.cpp` — Group A / D)

- **P-MEM-1: Replace Linear `GuestArena::findFree` Scan (`B-208`):** `GuestArena::findFree` currently performs a linear scan over mapped segments when allocating anonymous guest regions. Replace it with a sorted free-interval set (`std::set` ordered by base address with coalescing on `unmap`) so `mmap` / `vm_allocate` runs in $O(\log N)$ time.

## 5. Future Engineering Pillar 2: Application & Game Compatibility

To expand compatibility from early iOS 3.0–4.x titles (*Angry Birds*, *Tiny Tower*, *Art of Glow*, *Mega Jump*, *RETRY*) to mid-and-late-era 32-bit iOS 5–10 games (*Clash Royale*, *Minecraft: Story Mode*, Cocos2d-x v2/v3, Unity 4.x/5.x 32-bit, Unreal Engine 3 iOS), future agents should execute the following compatibility roadmap in priority order.

### 5.1 Memory Permissions & Precise Fault Handling (Group A)

| Backlog ID | Problem Statement | Architectural Solution for AI Agents |
| :--- | :--- | :--- |
| **B-101** | **Enforce 4 KB Guest Page Permissions on 16 KB Host Pages** | Maintain a flat 1-MB array (`uint8_t page_prot[1048576]`, 1 byte per 4 KB guest page across the 4 GB space) inside `GuestArena`. When all four 4 KB sub-pages within a 16 KB host page share identical permissions, delegate enforcement to hardware `mprotect`. When a guest sets mixed 4 KB permissions inside a single 16 KB host page, set the hardware page to the intersection (or route through the slow-path software check in load/store handlers) and validate against `page_prot[addr >> 12]`. Add unit tests in `tests/asm/arm_mem.S` that simulate both 4 KB and 16 KB host page sizes. |
| **B-102** & **B-209** | **Precise Fault PC & Eliminate Unsafe `siglongjmp` Across C++ Frames** | Currently, a guest memory fault (`SIGSEGV`/`SIGBUS`) can `siglongjmp` out of an HLE function that holds live C++ stack objects (`std::string`, `std::vector`, locks), causing leaks, corrupted mutexes, and ASan/UBSan failures (`B-209`). **Fix:** 1) Before every guest memory instruction or potentially faulting HLE memory helper, store the exact instruction PC into `cpu.fault_pc` (or recover it via a static `(host_pc -> op_index)` table). 2) In HLE functions, never dereference raw untrusted guest pointers directly; use `GuestArena::tryRead` / `GuestArena::tryWrite` helpers that check `page_prot` and set `cpu.exit = Exit::MemFault` cleanly without raising a host signal inside C++ stack frames. |

### 5.2 OpenGL ES 1.1 / 2.0 & CoreGraphics Fidelity (Group B & D)

1. **Resolve `GL_MAX_VARYING_VECTORS` Linker Mismatch ([Issue #14](https://github.com/lawsharley0-cyber/HydrogenVM/issues/14) — Group B):**
   - *Root Cause:* `OpenGLES.cpp:1057` returns `16` for `GL_MAX_VARYING_VECTORS`, `Shader.cpp` sets built-in `gl_MaxVaryingVectors = 8`, and `OpenGLES.cpp:2102` fails linking when varying floats exceed `32` (`8 * vec4`).
   - *Fix:* Unify all three sites to `8` `vec4` (`32` floats, the OpenGL ES 2.0 specification minimum required value on PowerVR SGX535/543) **or** widen the rasterizer's interpolated varying array from 32 floats to 64 floats (`16 * vec4`) and set `gl_MaxVaryingVectors = 16` across `OpenGLES.cpp` and `Shader.cpp`. Widening to 16 `vec4` (64 floats) is recommended so shaders querying `GL_MAX_VARYING_VECTORS` or packing 9–16 varyings both compile and link.
2. **Implement `glPointSizePointerOES` (`B-111` — Group B):**
   - *Tiny Tower* binds per-vertex point sizes via `GL_POINT_SIZE_ARRAY_OES` and `glPointSizePointerOES(type, stride, ptr)`. Add the client state enable flag (`GL_POINT_SIZE_ARRAY_OES = 0x8B9C`), attribute pointer state, and per-vertex point size lookup in `glDrawArrays(GL_POINTS, ...)` / `glDrawElements(GL_POINTS, ...)`.
3. **Implement `CGFontCopyTableForTag` & `CGFontGet*` Metrics (`B-109` — Group D):**
   - *Tiny Tower* and custom UI engines inspect raw TrueType/OpenType font tables (`'cmap'`, `'head'`, `'hhea'`, `'hmtx'`, `'OS/2'`, `'loca'`, `'glyf'`) via `CGFontCopyTableForTag(CGFontRef, uint32_t tag)` and query `CGFontGetUnitsPerEm`, `CGFontGetAscent`, `CGFontGetDescent`, `CGFontGetLeading`, `CGFontGetCapHeight`, `CGFontGetXHeight`, `CGFontGetFontBBox`, and `CGFontGetGlyphBBoxes`.
   - Parse the TrueType table directory in the backing font data, return a **newly retained `CFDataRef`** for `CGFontCopyTableForTag` (respecting CoreFoundation `Copy` ownership rules so the guest's `CFRelease` does not underflow), and populate metric getters from `'head'`, `'hhea'`, and `'OS/2'`.
4. **Corpus-Driven GLSL ES 1.00 Expansion (`B-104` — Group B):**
   - After merging [PR #11](https://github.com/lawsharley0-cyber/HydrogenVM/pull/11) and [PR #12](https://github.com/lawsharley0-cyber/HydrogenVM/pull/12), harvest all vertex/fragment shaders compiled during smoke runs into a checked-in test corpus (`tests/shaders/`) and add support for nested struct member selection, matrix-matrix/matrix-vector compound assignment (`*=`), `in`/`out`/`inout` qualifier edge cases on array/struct parameters, and `gl_FrontFacing` / `gl_PointCoord`.

### 5.3 Clash Royale Restoration Past 65% Load (`B-106` — Group C)

- **Current State:** `tools/clash_home_probe.py` passes home acceptance, and 79 runtime checks + 12 Python protocol/home-state tests + 18 shader checks + 107 IMA ADPCM (`0x11`) WAV checks pass (`docs/CLASH_ROYALE_RESTORATION.md`).
- **Active Blocker (`21903 SectorStateMessage`):** The client stalls at 65% loading into Tutorial Mission `18000000` (`Npc1`, `FirstMatch`, `BasicDeck1`) inside `LogicGameMode::decode` (`0xd7804`) -> `LogicBattle::decode` (`0x14470`) -> `LogicGameObjectManager::decode` (`0x55c4c`).
- **Execution Plan for Group C Agents (following [PR #13](https://github.com/lawsharley0-cyber/HydrogenVM/pull/13)):**
  1. Disassemble and trace the exact bit-stream reads in `LogicGameMode::decode`, `LogicBattle::decode`, and `LogicGameObjectManager::decode` from the pinned binary (`HVM_FIXTURE_ROOT`).
  2. Construct a minimal, bit-exact `21903 SectorStateMessage` payload in `tools/clash_wire.py` that satisfies every checksum, tick counter, map/arena ID, and player/NPC avatar sub-structure without inventing unverified fields.
  3. Capture the next client-emitted message (e.g., `ClientCapabilities` / `EndClientTurn`) and add a deterministic unit test in `tools/test_clash_protocol_probe.py`.

### 5.4 C++ Standard Libraries (`libstdc++` & `libc++`) and Frameworks (Group D)

1. **LLVM `libc++` (`std::__1`) HLE Shim (`B-202`):**
   - Games built with Xcode 5–8 targeting iOS 7–10 link against `/usr/lib/libc++.1.dylib` and `/usr/lib/libc++abi.dylib` (namespace `std::__1`, short-string optimization layout where bit 0 of the first byte indicates short vs. long string on little-endian ARMv7).
   - Create `src/runtime/libc/Libcxx.cpp` implementing the 32-bit `std::__1::basic_string` layout, `std::__1::locale`, `std::__1::ios_base`, `std::__1::mutex` / `condition_variable`, and `__cxa_*` exception/typeinfo bridges to unlock *Minecraft: Story Mode* and modern Cocos2d-x/Unity 32-bit titles.
2. **Complete `libstdc++` File-Stream Virtuals (`B-207`):**
   - Replace the remaining `"not supported yet"` virtual stubs in `src/runtime/libc/LibstdcxxStreams.cpp` (`basic_filebuf::seekoff`, `seekpos`, `xsgetn`, `xsputn`, `underflow`, `overflow`, `sync`) with guest-safe `FILE*` / fd-backed implementations.
3. **Positional Format Specifiers (`B-206`):**
   - Add `%n$` positional argument indexing (e.g., `%1$d`, `%2$@`, `%1$.2f`) to the custom format string parser backing `printf`, `snprintf`, `NSStringWithFormat`, and `CFStringCreateWithFormat`.
4. **`NSNotificationQueue` Coalescing (`B-205`):**
   - Implement `NSNotificationQueue` (`defaultQueue`, `enqueueNotification:postingStyle:`, `enqueueNotification:postingStyle:coalesceMask:forModes:`, `dequeueNotificationsMatching:coalesceMask:`) integrated with `NSRunLoop` `NSPostWhenIdle` and `NSPostASAP` hooks.
5. **High-Impact Missing Game Frameworks (Phase 4 Expansion):**
   - **GameKit (`GKLocalPlayer`, `GKLeaderboard`, `GKAchievement`):** Always invoke `[[GKLocalPlayer localPlayer] authenticateWithCompletionHandler:]` asynchronously on the main runloop with either a stubbed offline player or `GKErrorNotAuthenticated` (`code = 3`) so games never hang waiting for Game Center sign-in.
   - **StoreKit (`SKPaymentQueue`, `SKProductsRequest`):** Return `NO` from `+[SKPaymentQueue canMakePayments]` and complete `SKProductsRequest` with an empty response so in-app purchase queries do not block UI initialization.
   - **MediaPlayer / AVFoundation Video (`MPMoviePlayerController`, `MPMoviePlayerViewController`, `AVPlayer`):** Post `MPMoviePlayerPlaybackDidFinishNotification` on the next runloop tick (or bridge to host `AVPlayer`) so games that play boot logo videos advance immediately to their main menu.

## 6. Future Engineering Pillar 3: Overall User Experience (UX)

While the core VM (`src/`) and the in-app save editor (`app/HydrogenVM/FileBrowser.swift`) have rich underlying functionality, critical ergonomic and reachability issues in `app/HydrogenVM/` (Group E) currently block users from saving file edits and comfortably playing landscape games.

### 6.1 URGENT P0/P1 UX FIX: File Saving & Unreachable Save Button in `app/HydrogenVM/FileBrowser.swift` (Group E)

#### **Problem Diagnosis (Root Cause in `app/HydrogenVM/FileBrowser.swift`)**
In [`app/HydrogenVM/FileBrowser.swift`](https://github.com/lawsharley0-cyber/HydrogenVM/blob/main/app/HydrogenVM/FileBrowser.swift), users currently **cannot save edits** because the `Save` button is unreachable or pushed off-screen across the file editors:
1. **Top-Bar Trailing Overflow on iPhone Portrait (`TextEditorView` ~lines 1350–1460, `KeyValueEditorView` ~lines 1480–1620, `JSONEditorView` ~lines 1650–1820, `PlistEditorView` ~lines 2500–2680, `BinaryEditorView` ~lines 2950–3085):**
   - Each editor places `Button("Save") { save() }.disabled(!dirty)` inside `.toolbar { ToolbarItemGroup(placement: .topBarTrailing) { ... } }` alongside multiple competing controls (`Picker` for view mode/format, `Menu`, `ShareLink`, search/scanner toggles, add-key buttons).
   - On narrow iPhone portrait navigation bars, SwiftUI's `.topBarTrailing` overflows horizontally, pushing the `"Save"` button outside the visible screen bounds or clipping its hit target.
2. **Modal Presentation Without Guaranteed Navigation Bar (`FileEditorSheet` / `FileEditorDestinationView`):**
   - When an editor is opened via `FileEditorSheet` or nested inside `NestedDataInspectorView`, if the presenting hierarchy does not wrap the destination in its own dedicated `NavigationStack` (or if nested sheets conflict with the parent navigation bar), `.topBarTrailing` toolbar items fail to appear entirely.
3. **Keyboard Occlusion & Inline Row Editing Without Commit Action:**
   - When editing values inside `TextField` or `TextEditor` in `PlistEditorView`, `JSONEditorView`, `KeyValueEditorView`, `TextEditorView`, or `BinaryEditorView`, the iOS software keyboard covers the lower half of the screen and there is no `.keyboard` toolbar (`Done` + `Save`) or sticky bottom bar. Furthermore, editing a child node inside a tree/row sheet must immediately propagate `dirty = true` up to the parent editor and offer a 1-tap save without requiring the user to dismiss the keyboard and hunt for a top-right button.

#### **Mandatory Engineering Fix Specification for AI Agents Touching `app/HydrogenVM/FileBrowser.swift`**
Future Group E agents must implement **all four layers** below in [`app/HydrogenVM/FileBrowser.swift`](https://github.com/lawsharley0-cyber/HydrogenVM/blob/main/app/HydrogenVM/FileBrowser.swift):

1. **Add a Persistent, Thumb-Reachable Sticky Bottom Save Bar (`.safeAreaInset(edge: .bottom)`):**
   - Create a reusable `EditorSaveBottomBar` view modifier or component in `FileBrowser.swift` and attach it via `.safeAreaInset(edge: .bottom)` to `TextEditorView`, `KeyValueEditorView`, `JSONEditorView`, `PlistEditorView`, `BinaryEditorView`, and `NestedDataInspectorView`.
   - Whenever `dirty == true` (or always visible with clear state), render a high-contrast floating bottom bar pinned above the home indicator (and automatically riding above the software keyboard) containing:
     - Left: An `"Unsaved Changes"` status pill + `"Discard"` / `"Revert"` button.
     - Right: A prominent, full-height `.buttonStyle(.borderedProminent)` **`"Save Changes"`** button (`Label("Save Changes", systemImage: "square.and.arrow.down.fill")`) that calls `save()`, triggers haptic feedback (`UIImpactFeedbackGenerator(style: .medium)`), and flashes a `"Saved ✓"` confirmation banner.
2. **Add a Keyboard Accessory Toolbar (`ToolbarItemGroup(placement: .keyboard)`):**
   - In every editor view in `FileBrowser.swift`, add a `ToolbarItemGroup(placement: .keyboard)` containing:
     - A `"Dismiss Keyboard"` (`"Done"`) button that clears `@FocusState`.
     - A `Spacer()`.
     - A direct **`"Save"`** button (`Button("Save") { save() }.disabled(!dirty).bold()`) right above the keyboard so users editing a number, string, hex byte, or plist key can save immediately while typing.
3. **Declutter `.topBarTrailing` & Guarantee `NavigationStack` Wrapping in `FileEditorSheet`:**
   - Ensure `FileEditorSheet` always wraps `FileEditorDestinationView` inside an explicit `NavigationStack` so navigation bars never vanish in modal presentations.
   - Move secondary tools (`ShareLink`, encoding/format pickers, hex/tree mode toggles, cheat scanner toggles) out of `.topBarTrailing` into either a compact secondary sub-header bar below the navigation title or a single `Menu { ... } label: { Image(systemName: "ellipsis.circle") }`, leaving **`Button("Save")`** as the sole primary trailing action in `.topBarTrailing`.
4. **Add Auto-Save Option & Unsaved-Changes Close Guard (`.confirmationDialog` / `.alert`):**
   - When `dirty == true`, attach `.interactiveDismissDisabled(true)` to `FileEditorSheet` and intercept the Back / Close button with a confirmation dialog: **`"Save & Close"`**, **`"Discard Changes"`**, and **`"Cancel"`**.
   - In child node editors (such as `PlistEditorView` row detail edits, `JSONEditorView` node edits, and `NestedDataInspectorView`), add an option for **"Save Immediately on Apply"** so committing a single value change automatically writes the updated file to disk (`save()`) and creates a rolling backup snapshot.

### 6.2 Additional iOS Host App Shell UX Improvements (`app/HydrogenVM/` — Group E)

| UX Area | Current State & Limitation | Concrete Engineering Improvement |
| :--- | :--- | :--- |
| **UX-1: Fix Unreachable Save Button & File Saving Workflow (`FileBrowser.swift`)** | `Button("Save")` in `TextEditorView`, `KeyValueEditorView`, `JSONEditorView`, `PlistEditorView`, and `BinaryEditorView` is crammed into `.topBarTrailing` with multiple pickers/buttons and overflows off-screen on iPhone portrait or vanishes in sheets. | Implement the 4-part fix in Section 6.1: sticky `.safeAreaInset(edge: .bottom)` **"Save Changes"** bar, `.keyboard` accessory `"Save"` button, decluttered `.topBarTrailing` with `ellipsis.circle` menu, and `"Save & Close"` confirmation on Back/Dismiss. |
| **UX-2: Dynamic Orientation & Viewport Scaling (`RunView.swift`, `HydrogenVMApp.swift`)** | `RunView.swift` locks the host app to portrait (`OrientationLock.shared`) and renders a fixed image view. Many classic iOS games (*Angry Birds*, *Clash Royale*, *Mega Jump*, *RETRY*) are strictly landscape or switch orientation via `Info.plist` (`UISupportedInterfaceOrientations`) or `[[UIApplication sharedApplication] setStatusBarOrientation:]`. | 1) Expose guest orientation state on `HVMSession` (`interfaceOrientation` read from `Info.plist` on boot and updated when guest calls `setStatusBarOrientation:` or `shouldAutorotateToInterfaceOrientation:`). 2) Rotate the viewport smoothly in `RunView.swift` and transform multi-touch coordinates (`HVMSession.touch:x:y:finger:`) and CoreMotion accelerometer axes (`setAccelerationX:y:z:`) to match the active guest orientation. 3) Add a display mode toggle: **Aspect Fit**, **Integer Pixel-Perfect (2x/3x)**, and **Stretch**, plus Nearest-Neighbor vs. Bilinear filtering. |
| **UX-3: In-Game HUD & Quick Save Backup Integration (`RunView.swift`, `FileBrowser.swift`)** | `FileBrowser.swift` has a 3,000-line Save Editor with one-tap Snapshot Backup & Restore, Plist/SQLite/Hex editors, and a Cheat Value Scanner, but users must stop the game and return to the Library to use it. | Add a collapsible floating HUD pill in `RunView.swift` with: **Pause/Resume Engine**, **Quick Save Snapshot / Restore Snapshot** (flushing `NSUserDefaults` / SQLite WAL and calling the `FileBrowser` snapshot engine), **Live Cheat Scanner / File Inspector sheet** while paused, **Mute / Volume slider**, and **FPS / Frame-Time (`p95`) badge**. |
| **UX-4: Audio Session Lifecycle & Backpressure (`HVMEngine.mm`, `RunView.swift`)** | Audio streams stereo PCM through `AVAudioEngine`, which can glitch or stop after iOS phone calls, Siri invocations, or background/foreground transitions. | Register for `AVAudioSessionInterruptionNotification` and `AVAudioSessionRouteChangeNotification` in `HVMEngine.mm`, automatically restarting the `AVAudioEngine` graph on foreground resume and offering a toggle in settings for **"Respect Hardware Silent Switch"** (`AVAudioSessionCategorySoloAmbient` vs `AVAudioSessionCategoryPlayback`). |
| **UX-5: App Library Compatibility Badges & Import Flow (`LibraryView.swift`)** | `HVMAppInfo` already inspects `runnable`, `encrypted`, `minOSVersion`, and `problem`, but importing encrypted or 64-bit-only `.ipa` files can still confuse users. | 1) Display color-coded status pills on every card in `LibraryView.swift` (**Ready · ARMv7**, **Encrypted · FairPlay Unsupported**, **64-Bit Only · Unsupported**). 2) Support iOS Share Sheet / "Open In HydrogenVM" (`CFBundleDocumentTypes` for `.ipa` in `Info.plist`) and drag-and-drop import. 3) Allow long-pressing an app card to view per-app crash logs, clear caches, or export the app's save folder as a `.zip`. |
| **UX-6: Actionable Diagnostics & 1-Tap GitHub Bug Report Export (`DiagnosticsView.swift`, `HVMEngine.mm`)** | `DiagnosticsView.swift` checks `canReserveGuestArena` and runs `runSelfTest()`, but in-app game crashes only show a transient alert in `RunView.swift`. | Persist the last run's structured JSON report (`FrameHistogram` percentiles, unimplemented selector/symbol warnings, fault PC, guest backtrace, and redacted log tail) per app, and add an **"Export Compatibility Report"** button in `DiagnosticsView.swift` / `LibraryView.swift` so users can paste a ready-made GitHub issue straight into the repo. |
| **UX-7: Hardware Controller & Touch Ergonomics (`RunView.swift`)** | Games that rely on tilt (accelerometer) or rapid multi-touch lack customizable input aids on modern large-screen phones/iPads. | Add optional **Tilt-to-Joystick / Touch-Drag** accelerometer emulation (for playing tilt games like *Mega Jump* flat on a desk) and optional `GCController` (Bluetooth Xbox/PlayStation/MFi gamepad) button-to-touch mapping profiles. |

### 6.3 Developer & AI Agent Workflow UX (`tools/`, `docs/dev-system/`)

1. **Automated Backlog & Status Consistency Check (`tools/hvm_compat.py`):**
   - Add a lightweight `python3 tools/hvm_compat.py doctor` subcommand that checks whether `docs/dev-system/BACKLOG.md`, `STATUS.md`, and `IMPLEMENTATION_PLAN.md` reference valid commit SHAs and flags stale items when tests corresponding to a backlog ID are already passing.
2. **Complete `B-003` Documentation Reconciliation:**
   - Update `docs/ARCHITECTURE.md`, `docs/HANDOFF.md`, and `docs/DEVELOPMENT_AND_RELEASE.md` so file names match reality (e.g., `app/HydrogenVM/Bridge/HVMEngine.mm` and `LibraryView.swift` / `RunView.swift` rather than outdated `ContentView.swift` or `VMBridge.mm` references) while preserving the documented constraints on GLSL ES subsetting and `CCCrypt`.

## 7. Decision Framework & Playbook for Future AI Agents

Every future AI agent working in [`lawsharley0-cyber/HydrogenVM`](https://github.com/lawsharley0-cyber/HydrogenVM) must follow this decision loop from start to finish.

### Step 1: Pre-Flight Context & Collision Check
Before creating a branch or editing any file:
1. Read [`AGENTS.md`](https://github.com/lawsharley0-cyber/HydrogenVM/blob/main/AGENTS.md), [`docs/dev-system/STATUS.md`](https://github.com/lawsharley0-cyber/HydrogenVM/blob/main/docs/dev-system/STATUS.md), [`docs/dev-system/BACKLOG.md`](https://github.com/lawsharley0-cyber/HydrogenVM/blob/main/docs/dev-system/BACKLOG.md), and this document (`docs/dev-system/AI_ENGINEERING_ROADMAP.md`).
2. Inspect `git branch -r` and open Pull Requests to see which **Exclusive Ownership Group (`A`–`E`)** currently has active in-flight work:
   - **Group A (Memory & Faults):** `GuestArena.*`, load/store handlers, fault path, `mprotect`/`vm_protect` in `System.cpp`, `tests/asm/arm_mem.S`.
   - **Group B (Pixels & Shaders):** `Shader.cpp`, `Shader.hpp`, `OpenGLES.cpp`, `Raster.cpp`, GLES checks in `TestRunner.cpp`.
   - **Group C (Clash Wire):** `tools/clash_*.py`, Clash protocol/home tests, `Net.cpp` / `Network.hpp`.
   - **Group D (Frameworks & Runtime):** `src/runtime/objc/`, `src/runtime/foundation/`, `src/runtime/uikit/` (except `OpenGLES.cpp`), `src/runtime/libc/`, `src/runtime/audio/`.
   - **Group E (Apple Shell):** `app/**` and `app/HydrogenVM/Bridge/HVMEngine.h`.
3. **Never edit files belonging to a Group that already has an unmerged open PR** unless you are explicitly updating that exact PR branch.

### Step 2: How to Select the Highest-Value Next Task
When asked to "improve HydrogenVM" or pick the next task autonomously, prioritize in this exact order:

| Priority Tier | What to Pick First | Concrete Next Candidates (as of `39904cb`) |
| :--- | :--- | :--- |
| **Tier 1: Immediate UX & Spec Blockers (`P0` + Urgent Bug Fixes)** | Fix unreachable user controls, resolve spec mismatches, reconcile stale docs/backlog statuses, and land ready PRs. | 1) **UX-1 (`FileBrowser.swift` Save Button Reachability — Group E)**: Add sticky bottom `.safeAreaInset(edge: .bottom)` `"Save Changes"` bar, `.keyboard` save toolbar, decluttered `.topBarTrailing`, and `"Save & Close"` prompt across `TextEditorView`, `KeyValueEditorView`, `JSONEditorView`, `PlistEditorView`, and `BinaryEditorView`.<br>2) **B-003**: Reconcile `docs/ARCHITECTURE.md`, `docs/HANDOFF.md`, `docs/DEVELOPMENT_AND_RELEASE.md`, and mark `B-103`, `B-108`, `B-110` as `Operational` in `BACKLOG.md`.<br>3) **Issue #14** (Group B, after PR #11/#12 merge): Unify `GL_MAX_VARYING_VECTORS` between `OpenGLES.cpp` and `Shader.cpp`. |
| **Tier 2: P1 App Compatibility & Memory Safety** | Fix missing symbols, font/graphics APIs, memory permissions, and protocol blockers hit by real IPAs. | 1) **B-109** (Group D): Implement `CGFontCopyTableForTag` & `CGFontGet*` for *Tiny Tower*.<br>2) **B-111** (Group B): Implement `glPointSizePointerOES` for *Tiny Tower*.<br>3) **B-101** & **B-102** (Group A): 4 KB guest page permissions on 16 KB host pages + precise fault PC without unsafe C++ `siglongjmp`.<br>4) **B-106** (Group C): Complete `21903 SectorStateMessage` for *Clash Royale* past 65%. |
| **Tier 3: P1/P2 Performance Multipliers** | Reduce frame time (`p95`/`p99`) and increase CPU CoreMark / MIPS with zero behavioral regressions. | 1) **B-105 / P-GFX-1 & P-GFX-3** (Group B): Upload-time texture conversion + 2x2 quad SIMD triangle rasterizer.<br>2) **P-CPU-1 & P-CPU-3** (Core): In-register budget pinning + Return Address Stack (`BL` / `BX LR` cache).<br>3) **B-208** (Group A): Replace linear `GuestArena::findFree` scan with interval tree. |
| **Tier 4: Framework Expansion & Host App UX** | Expand iOS 6–10 C++ / framework support and improve the SwiftUI iOS player experience. | 1) **UX-2 & UX-3** (Group E): Dynamic landscape/portrait orientation + in-game quick save/snapshot HUD in `RunView.swift`.<br>2) **B-206 & B-207** (Group D): Positional `printf` (`%1$d`) and `libstdc++` filebuf virtuals.<br>3) **B-202** (Group D): `libc++` (`std::__1`) slice for *Minecraft: Story Mode*. |

### Step 3: Mandatory Verification Protocol Before Opening/Updating a PR
Every code change (outside of macOS-only Group E Swift files when running on Linux) **must** pass the full verification gate before completion:

```bash
# 1. Configure and build Release targets
cmake -S . -B build -G Ninja -DCMAKE_CXX_COMPILER=clang++ -DCMAKE_BUILD_TYPE=Release
ninja -C build hvm hvm_tests hvm_decodecheck
tools/build_tests.sh build/tests

# 2. Run unit, fulltest, assembly, and differential C/VFP/NEON suites
build/hvm_tests selftest
build/hvm_tests fulltest

# 3. Run the end-to-end compatibility harness against the Linux baseline
python3 tools/hvm_compat.py run \
  --build-dir build \
  --out build/compat-report \
  --baseline docs/dev-system/baselines/linux-headless.json \
  --require-resolved

# 4. Diff against baseline and prior indexed report
python3 tools/hvm_compat.py diff \
  docs/dev-system/baselines/linux-headless.json \
  build/compat-report/report.json
python3 tools/hvm_compat.py diff --previous build/compat-report/report.json
```

- **If you modified CPU decoders (`Decode*.cpp`):** Also run `python3 tools/decode_fuzz.py build/hvm_decodecheck 400000` (**B-107**).
- **If you modified Group E (`app/**`) on Linux:** Verify C++ bridge header syntax, ensure Swift 6 strict concurrency compliance (`@MainActor`, `Sendable`), and trigger the Codemagic (`codemagic.yaml`) or GitHub Actions macOS build workflow.
- **Synchronize Documentation in the Same Commit:** Always update [`docs/dev-system/STATUS.md`](https://github.com/lawsharley0-cyber/HydrogenVM/blob/main/docs/dev-system/STATUS.md), [`docs/dev-system/BACKLOG.md`](https://github.com/lawsharley0-cyber/HydrogenVM/blob/main/docs/dev-system/BACKLOG.md), and [`docs/dev-system/IMPLEMENTATION_PLAN.md`](https://github.com/lawsharley0-cyber/HydrogenVM/blob/main/docs/dev-system/IMPLEMENTATION_PLAN.md) alongside your code changes, and never commit `build/` artifacts.
