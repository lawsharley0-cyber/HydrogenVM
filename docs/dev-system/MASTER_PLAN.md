# HydrogenVM master plan (2026-10-23)

This is the plan for getting every app in the owner's library to run with its features and
good performance. It reviews the work done since the `fd96801` handoff, gives the measurements
behind the plan, and orders the work so that each credit spent moves an app forward.
The research it relies on is in [`docs/research/COMPARABLE_PROJECTS.md`](../research/COMPARABLE_PROJECTS.md).

## 1. Bottom line

1. **The bottleneck is graphics, not the CPU.** On every app measured, most of each frame goes
   to the software rasterizer and the software UIKit compositor. The guest CPU is often idle
   (Tiny Tower executes 0.4 M guest instructions per second and still takes 22 ms a frame).
   No comparable project reaches playable 3D speed with a CPU shader interpreter without a JIT.
   **Move GLES to the iPhone GPU** (OpenGLES.framework, with MetalANGLE as a fallback) and stop
   compositing every frame on the CPU. This is phase 2 and the largest single win available.
2. **Most reported crashes happen on the device and not on Linux.** Subway Surfers, Angry Birds,
   Tiny Tower, Minecraft PE and Art of Glow all run 25 s on Linux without a crash. The owner
   reports startup or loading crashes for several of them on the phone. The cause is in the
   device-only differences (16 KB pages, memory limits, the watchdog, the Apple shell), and today
   the only way to see them is a hand-sent log. **Get device crash reports back automatically**
   (phase 1). Fixing blind costs more credits than anything else.
3. **The process costs more than the code.** There are about 4,900 lines of process docs, five
   exclusive ownership groups, eight workflows, and five different agents (Codex, Cursor, Jules,
   Gemini, Claude) committing to two integration branches. Every agent session re-reads those
   docs before it starts. Cut the process down to one short page, one branch and one queue
   (section 6).
4. **The CPU core already has the research's recommended techniques**: `musttail` threaded
   dispatch, `preserve_none`, block linking, fused compare and branch, side exits and pair
   superinstructions. What remains there is HLE-specific: running hot statically linked guest
   routines natively. That is phase 4, after the GPU work.

## 2. Measurements (this session, `main` @ `40e978f`, Linux x86-64, 25 s runs)

| App | GL | Guest MIPS | Frames | Avg frame | Composite | Result |
|---|---|---:|---:|---:|---:|---|
| Angry Birds | ES1 | 22 | 466 | 22.5 ms | 22.3 ms | runs |
| Tiny Tower | ES1 | 0.4 | 1,249 | 17.7 ms (p95 43) | 23.0 ms | runs |
| Art of Glow | ES1 | 0.7 | 448 | 55 ms | 11 ms | runs |
| Minecraft PE Lite | ES1 | 0.8 | 300 | 84 ms | 31 ms | runs |
| Jetpack Joyride | ES1 | 108 | 104 | 239 ms (p50 153) | 108 ms | runs; crashes during the terminate path |
| Subway Surfers | ES2 | 127 | 124 | 14 ms after load | 14 ms | runs; 5.8 s to first frame |

Stack samples (25 samples per app, taken with gdb) for Tiny Tower and Minecraft PE: the main
thread is in `Compositor::render`/`drawImage` or in `rasterizeTriangle`/`writeFragment` in
nearly every sample. Every frame, the whole screen is drawn again on the CPU, and the GL layer is
copied again as an image.

Comparison with the handoff build (`fd96801`): Jetpack is unchanged at 238 ms a frame, with the
same teardown crash. Performance has not moved since the handoff. The teardown crash existed
already; it is not a regression. It happens after "The game is being destroyed", during
`applicationWillTerminate`. It is a separate bug, because it can break save-on-exit.

The host path to the screen adds one more full-frame copy per frame:
`present` → `NSData` copy → `CGImage` → SwiftUI. On the phone this sits on top of the software
raster cost.

## 3. Review of the work since `fd96801` (113 commits on `main`)

| Area | What landed | Verdict | Better option |
|---|---|---|---|
| GLSL ES (`Shader.cpp`, 2.5 k lines; PRs #8, #9, #11, #12) | Real preprocessor, parser and scalar bytecode, run per fragment. 2×2 quads for derivatives. A shared draw instruction budget | Correct direction for correctness, and it replaces the pattern matching. **Too slow for production.** Each fragment allocates a 32 KB `Values` array, re-initializes its slots and reloads uniforms, and dispatches one `switch` per scalar op. The 200 M budget **silently drops whole draws**, which shows up as missing geometry | Keep it as the **reference renderer and test oracle**. Production GLES2 goes to the GPU (phase 2), where the guest's GLSL is passed through to the driver with a small fix-up pass. Until then: keep `Values` per draw instead of per fragment, fold uniforms once per draw, and log budget aborts in the report instead of hiding them |
| Near-plane clip (#4) | Sutherland–Hodgman in clip space, perspective-correct GLES2 varyings | Good | GLES1 colour and UV are still screen-linear after the clip, which makes textures swim in 3D GLES1 games (Minecraft PE). On the GPU path this goes away |
| Test Lab (TL-A..J, `hvm_testlab.py`) | Deterministic guest with independently derived oracles; one bug class becomes one case | **Best addition.** Keep it as the first gate | Add a GLES trace replay case (phase 3) |
| Compat harness (`tools/compat_harness`, `hvm_compat.py`, baselines, indexed reports) | Schema'd reports, baseline diff, CI job | Good engineering. It **duplicates** `tools/compat.py`, and it runs 3 s smokes of 3 apps | One scanner. Fold `compat.py`'s per-app issue ranking into the harness and retire `compat.py`. Run the full library for 30 s on the owner's private fixtures, not 3 s on 3 |
| Process docs (`docs/dev-system/*`, `AGENTS.md`, roadmap, handoffs) | About 4,900 lines, groups A–E, owner tables, decision logs | **Credit sink.** Each agent session reads thousands of lines, and much of it is restated in three places | A 60-line `AGENTS.md`, this plan, `BACKLOG.md` as the single queue, and reports as JSON only (section 6) |
| Clash Royale (about 1,500 lines of docs, a loopback server protocol, a desktop viewer) | Reverse-engineering an online game's wire protocol | High cost for one online-only title, and it pulls agents away from the shared runtime | **Park it** until phases 1–3 are done |
| Save editor overhaul (about 10 Swift files: plist, JSON, SQLite, compression, snapshots) | Rich, tested | Good for users. The Apple validation workflow fails on the Jules branch | Freeze its scope. Bugs only |
| Player viewport, HUD, orientation (#19) | Letterbox-aware viewport, lifecycle controls | Good. It is the right base for the GPU presentation layer | Host the GPU layer inside it (phase 2) |
| libc and frameworks (regex 1.1 k lines, CF, streams, keychain, Cocos2d threading, RETRY FBO, Mega Jump, Pocket Ants, Minecraft fixes) | App-driven fixes with evidence in `COMPAT.md` | Good. These are the shared runtime fixes the method asks for | Keep using one shared fix per bug class, each with a Test Lab case |
| stb_image and stb_truetype | Replace hand-rolled decoders | Good | — |
| `CCCrypt` | Still `kCCUnimplemented`. Jetpack calls it about 12 times at boot | Its save and config data probably fail to decrypt | Owner decision (B-204). The low-risk route is to **forward to the platform's own CommonCrypto on iOS** (the app is calling Apple's library; we call the same library) and to OpenSSL EVP on Linux, instead of writing cipher code |
| Branches | `main` is 62 commits ahead of `gemini-merge`. `ci-reports` is 180 ahead. There are 14 agent branches | Two "integration" lines, and `HANDOFF.md` still says `gemini-merge` | `main` only. Delete merged agent branches. `gemini-merge` becomes read-only |

## 4. Phases

Each phase ends at a measurable gate. Do not start a phase before the previous gate passes,
except where a line says it may run in parallel.

### Phase 0: housekeeping (one short session, docs and CI only)

- Make `main` the only integration branch. Fix `HANDOFF.md`. Archive `gemini-merge`. Delete merged
  agent branches.
- Shrink `AGENTS.md` to about 60 lines that point here. Mark the long `dev-system` docs as
  reference, so agents do not read them by default.
- Library access: put every IPA in a **private GitHub repo's release assets** (or attach them
  here). The iCloud share link cannot be fetched by tools (robots-disallowed), and Google Drive
  only hands files over in a form too large for an agent to read. The harness uses SHA-256 and a
  fixture directory, so no IPA goes into the public repo.

**Gate:** one branch, a short `AGENTS.md`, and the whole library downloadable by CI and by agents.

### Phase 1: see every device failure (parallel with phase 2)

1. **Crash bundle on device.** On a guest fault, unimplemented import, HLE assert or watchdog,
   write `Documents/HydrogenVM/crashes/<time>-<bundle>.json` containing: a crash signature (a
   hash of the fault PC's offset in its image, the faulting import or selector, and the
   exception kind), registers, backtrace with image offsets, the last 2,000 log lines,
   unimplemented imports, perf counters, device model, iOS version, host page size, app version
   and IPA hash.
2. **Show it, don't just die.** Use a structured error screen ("This game needs X, which
   HydrogenVM doesn't support yet") with Copy, Share and "Send to GitHub" (a pre-filled issue
   URL). Show the report again on the next launch if the process itself was killed.
3. **Survive the process kill.** Install a host `SIGSEGV`/`SIGBUS` handler that flushes the
   ring buffer, and log `os_proc_available_memory()` periodically so jetsam kills can be
   identified.
4. **`tools/triage.py`** ingests bundles and groups them by signature into `BACKLOG.md` rows.
   Each row records apps blocked × severity, so the fix order is computed rather than argued.
5. **Linking-info dump** (`hvm info --linking <app>`, as in touchHLE): every class, selector and
   symbol the binary imports, and how each is bound. Aggregated across the library, it says
   which framework gaps block the most apps before anything runs.

**Gate:** every UAR row has a device crash bundle or a confirmed "no crash", and the backlog is
ranked by signature.

### Phase 2: GPU backend (the performance phase)

The design keeps the HLE GL state machine as the front end and adds a backend switch. The
software backend stays for Linux tests and as the oracle.

1. **`GlBackend` seam.** Split `OpenGLES.cpp` into the entry points, object-name tables and guest
   memory copies (shared) and the execution (backend). The software backend is today's code.
   The **host backend** forwards each call to a dispatch table loaded with `dlsym`:
   `OpenGLES.framework` on iOS, and Mesa EGL/GLES (surfaceless llvmpipe) on Linux, so CI tests
   the same path.
   - Guest object names map to host names.
   - Client-side vertex arrays and texture uploads are copied out of guest memory.
   - Index buffers are range-scanned once per draw.
   - `glGet*` results are translated.
   - Unsupported formats (PVRTC on GPUs that lack it, BGRA) decode on upload using the existing
     `Pvrtc.cpp`.
2. **GLSL passthrough.** Use the guest's GLSL ES 1.00 source as-is, with a fix-up pass: honour
   `glBindAttribLocation`, insert missing precision qualifiers, and merge uniform struct
   mismatches across stages. Keep our parser as a validator so failures are reported, not
   silent.
3. **EAGL.** Each guest `EAGLContext` maps to a host context that is current on the same host
   thread (guest threads are host threads). `renderbufferStorage:fromDrawable:` creates an FBO
   backed by an **IOSurface**. `presentRenderbuffer:` hands that IOSurface to the shell, and the
   shell sets it as a `CALayer`'s contents. No CPU copy and no `CGImage`.
4. **Compositor.** If one opaque full-screen GL layer is the only visible content, skip software
   compositing entirely. Otherwise composite the UIKit layers into an overlay buffer **only when
   they are dirty**, and stack it above the GL layer in the shell. Pure-UIKit apps keep today's
   path but get dirty-rect updates.
5. **Lifecycle.** Stop issuing GL when entering the background (the existing park already
   stops guest threads), so iOS doesn't kill the app for GPU work in the background.
6. **Switch.** Per-game setting `Renderer: GPU (default) / ANGLE / Software`. MetalANGLE is
   linked as the fallback in case a future iOS removes OpenGLES.framework.

Milestones:

- **M2a:** GLES1 apps on the GPU: Tiny Tower, Angry Birds, Art of Glow, Mega Jump, Pocket Ants,
  Minecraft PE, Jetpack.
- **M2b:** GLES2 apps: Subway Surfers, RETRY, Goat Simulator, and the others found by the
  linking-info dump.
- **M2c:** zero-copy IOSurface present, plus a dirty-rect overlay.

**Gate:** on the phone, every GLES1 app in the library holds at least 55 fps at p50 in the
scenes from `USER_APP_REPORTS.md`, and GLES2 apps are limited by the guest CPU rather than by
rendering.

### Phase 3: correctness burn-down (starts in parallel with M2b)

The loop for each app, in order of signature rank:

1. Reproduce from its crash bundle. On Linux use the same IPA hash, `--keep-going`, `--trace` and
   `HVM_TRACE_BLOCKS`. On device, use the bundle.
2. Write a **minimal Test Lab case** that fails, as `TEST_LAB.md` already requires.
3. Fix it once in the shared runtime.
4. Rerun the library matrix. No app's star rating may go down.

Star scale (from touchHLE): 1 crashes at boot, 2 menu only, 3 main content with big issues,
4 playable with small issues, 5 fully working including audio, saves and relaunch.

**GLES trace capture ("HydrogenTrace")** comes in once M2a lands. `HVM_GL_TRACE=path` records
every GL call per frame along with the guest memory it references. `hvm replay` runs a trace
through the software and host backends and diffs the PNGs. Five short traces per library app go
into the private fixture store. CI fails on an unexplained image diff. This is how Dolphin and
PCSX2 stop rendering regressions without running the games.

Per-app starting points (UAR rows from `USER_APP_REPORTS.md`; the first move is in bold):

| UAR | App | Report | First move |
|---|---|---|---|
| 001 | Angry Birds | Crash at loading screen on device; fine on Linux | **Device bundle** (phase 1); suspect memory or page size |
| 002 | Goat Simulator | Black screen after load | GLES2. Check the draw-budget abort log first, then **M2b** |
| 003 | Jetpack Joyride | Slow, no music; crash in terminate path | **M2a** for speed. Music: trace the AVAudioPlayer / ExtAudioFile path for its music file. Fix the terminate crash for save-on-exit |
| 004 | Mega Jump | Fast, effects but no music | Same music path as Jetpack; likely **one shared fix** |
| 005 | Minecraft 1.1 | Black screen | Get the IPA. Linking dump plus **device bundle** |
| 006 | Minecraft PE | Menu works; crash on world load | Reproduce world load on Linux with `--tap`/`--input-file`, then Test Lab case |
| 007 | Pocket Ants | Freezes after about 10 s | Run 60 s with liveness counters (guest instr/s per thread, frames, audio). A hang with zero instr/s is a lock problem (Cocos2d worker GL contexts). Check `Engine::suspend` interaction |
| 008 | RETRY | 4–15 fps | **M2b** (Unity GLES2) |
| 009 | Subway Surfers | Crash on startup on device; Linux runs | **Device bundle**. Then M2b. 5.8 s to first frame on Linux points at load-time cost (asset decode) on the phone |
| 010 | Talking Larry | Crash when tapping | Get IPA; `--tap` repro, then Test Lab case |
| 011 | SurvivalCraft | Crash on startup | Get IPA; linking dump (Mono/MonoTouch AOT likely). Bundle |
| 012 | Terraria | Crash on startup | Get IPA; linking dump; bundle |
| 013 | Tiny Tower | Second launch after close crashes | `hvm run --launches 3` on one home dir reproduces close and relaunch; diff the saved files between launches |
| — | Minecraft Story Mode | Too large to upload; libc++ (`std::__1`) | B-202 after the above |
| — | Clash Royale | Online-only | Parked |

**Gate:** each library app at 4★ or better, or blocked only by a documented owner decision
(network, `CCCrypt`, in-app purchases).

### Phase 4: CPU work for the apps that are then CPU-bound

Do this only after phase 2 has removed the rendering cost. Expect Jetpack, Subway Surfers and
the Unity titles to be the CPU-bound ones.

1. **Profile by guest symbol.** Map `HVM_TRACE_BLOCKS` samples to Mach-O symbols and find the
   hot guest functions.
2. **Native fast paths for statically linked routines.** Run `memcpy`, `memset`, `strlen`, the
   math library and Unity/Cocos matrix routines natively. Match them by symbol, or by a hash of
   the function body for stripped binaries. This is the largest HLE-specific lever (PPSSPP's
   hand-written no-JIT paths run 4–11× faster than the generic ones).
3. **Engine follow-ups already listed in `PERFORMANCE.md`**: budget in a register, return-address
   stack, predicated ops, superblocks.
4. **Measure on the phone.** The HUD shows guest MIPS and frame percentiles, and the crash
   bundle carries them. Linux VM numbers are only for A/B comparisons.

### Phase 5: UX

1. **Library:** import from the Files picker, share sheet and AirDrop. Check encryption and the
   armv7 slice at import. Show the star rating and its last-known blocker on each tile.
2. **Per-game settings** with a gear badge: renderer, FPS cap, screen profile (3.5″/4″/iPad),
   orientation lock, tilt source.
3. **Error screen and report flow** from phase 1. This replaces raw crashes, which is the
   biggest UX gap today.
4. **Saves:** the in-game save on exit already exists. Expose `Documents/` and `Library/` through
   the Files app (`UIFileSharingEnabled` / `LSSupportsOpeningDocumentsInPlace`) and keep the
   built-in editor.
5. **Touch:** 1:1 letterbox mapping already exists in the player viewport. Add an optional
   virtual tilt stick for accelerometer games.

## 5. Order and parallelism

```
Phase 0 ─┬─ Phase 1 (crash bundles, triage, linking dump) ──────────┐
         └─ Phase 2 (GlBackend → M2a → M2b → M2c) ─┬─ Phase 3 burn-down ─ Phase 4 CPU
                                                   └─ Phase 5 UX (needs the phase 1 error screen)
```

Phases 1 and 2 touch different files (shell and diagnostics versus `OpenGLES.cpp` and the
present path), so they can run at the same time with two agents. Phase 2 needs a single owner:
`OpenGLES.cpp` and the present path are a seam that only one author should hold.

## 6. Agents: who does what, and how to save credits

**Recommendation: yes, delegate, but through one queue, with small tickets.** The
coordination cost so far (two branches, duplicated tools, about 5 k lines of docs) came from
agents working without one queue.

| Work | Best fit | Why |
|---|---|---|
| Phase 2 GlBackend seam, EAGL, IOSurface present, GLSL fix-ups | **Claude** (this agent) | It cuts across C++ and the shell, needs design judgment, and is the largest-value item |
| Phase 1 shell side (error screen, crash bundle UI, per-game settings, import) | **Codex** | Swift and the Apple shell. It already wrote the player and save editor. Its output needs a Mac compile through CI `[mac]` or Codemagic |
| Phase 1 tooling (`triage.py`, linking-info dump, folding `compat.py` into the harness) | **Cursor** | It built the harness and should extend it, not write a third scanner |
| Single-signature runtime bugs from the triage queue (one libc/Foundation gap each, with a Test Lab case) | **Jules / Gemini** | Small, isolated, verifiable. One ticket is one PR |
| Review of every PR (runs the Test Lab and the matrix) | A different agent from the author; Claude for phase 2 | Second reader, as `AGENTS.md` already asks |

**How to connect.** The reliable bus is **GitHub issues**, not chat:

- Each ticket is an issue labelled `agent:codex`, `agent:cursor`, `agent:jules` or
  `agent:claude`. It contains the signature, the repro command, the expected result and the
  Test Lab case to add.
- Each agent is pointed at "open issues with my label" and opens one draft PR per issue.
- Claude can create the issues and review the PRs from this session (it has `gh`).
- If the owner also starts Claude cloud sessions, this session can hand them work directly.

**Credit rules (put these in `AGENTS.md`):**

1. Read `AGENTS.md`, the issue and the files it names. Do not read all of `docs/`.
2. One issue is one PR. Keep the diff under about 400 lines unless the issue says otherwise.
3. Every PR has a repro command plus its before and after output. No status prose, no new docs
   unless the issue asks for them.
4. Do not trade a passing app for another: run the Test Lab and the library matrix and attach
   the JSON diff.
5. Disclose the agent and model in the PR, as PCSX2's template asks.

## 7. What the owner needs to do

1. **IPAs:** upload the library to a private GitHub repo as release assets (one release named
   `library`), or attach them here. The iCloud link cannot be read by any tool.
2. **Agents:** connect the agents you want to use (Codex, Cursor, Jules) to the GitHub repo, and
   tell each one: "work on open issues labelled `agent:<name>`, one PR per issue, follow
   `AGENTS.md`." I will create and label the issues.
3. **Decisions:**
   - `CCCrypt` forwarding to the platform library (yes or no).
   - Park Clash Royale (yes or no).
   - Delete `gemini-merge` and the merged agent branches (yes or no).
4. **Device runs:** once phase 1 lands, play each game until it fails and send the crash
   bundle. It is one tap.
