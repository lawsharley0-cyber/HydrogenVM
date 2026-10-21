# Backlog

Priorities follow the 2026-10-21 audit of `main` at `07fabbb`. Scope added on top of that audit: Clash Royale past the 65% load, and performance items discovered while improving frame time. Those discovered items are listed under B-105 and are not a blank check to rewrite the rasterizer early.

Score is `impact * severity / complexity`.

- Impact and severity are 1–5. Higher means more of the product breaks, or breaks harder.
- Complexity is 1 straightforward, 2 medium, 3 hard. A hard item scores lower so a small crash fix is not buried under a research project, but the P0/P1/P2 band still comes first. Score only sorts inside a band.
- Needs owner items stay in their band. They are not pulled into a coding lap.

Groups A–E are exclusive. Two tasks with the same letter do not run together. An empty group may run beside the lettered work if the files do not overlap. The matrix is in `ROADMAP.md`.

## P0

| ID | Item | Impact | Sev | Cx | Score | Group | Depends on | Parallel | State |
|---|---|---:|---:|---:|---:|---|---|---|---|
| B-001 | Make GitHub Actions execute and publish a report for `HEAD`. Jobs since `239a0ab` die in a few seconds with no steps. The notes blame billing; the API did not return the vendor string. | 5 | 5 | 1 | 25.0 | — | Owner account | Yes, except another edit of the same workflow | Needs owner |
| B-002 | Run `fulltest`, the Python protocol tests, and a 3 second smoke of the three in-repo IPAs on `main`. | 5 | 4 | 1 | 20.0 | — | B-001 for a green check. The command itself has no code dependency | Yes, as its own workflow file | Operational as `.github/workflows/compat-harness.yml` and `tools/hvm_compat.py`. Execution Needs owner. Folding the same suites into `hvm_report.py` is Planned |
| B-003 | Reconcile `docs/ARCHITECTURE.md`, `docs/HANDOFF.md`, and `docs/DEVELOPMENT_AND_RELEASE.md` with the code. Do not delete the shader subset, the missing near clip, or the `CCCrypt` refusal. | 4 | 3 | 1 | 12.0 | — | None | Yes | Planned |
| B-004 | One Mac compile of current `app/`. `06a7b16` changed the file browser, was marked `[mac]`, and Actions never started. | 4 | 4 | 2 | 8.0 | E | B-001 if no local Mac | Not with another `app/**` edit | Needs owner |

## P1

| ID | Item | Impact | Sev | Cx | Score | Group | Depends on | Parallel | State |
|---|---|---:|---:|---:|---:|---|---|---|---|
| B-101 | Enforce guest 4 KB permissions on loads and stores, including the 16 KB host-page case. | 5 | 5 | 3 | 8.33 | A | None, but one owner for the whole group | Not with B-102, B-201, or B-208 | Planned |
| B-102 | Precise fault PC. Do not `siglongjmp` out of an HLE frame that has live C++ objects. | 4 | 5 | 3 | 6.67 | A | Same owner as B-101, after or inside that design | Not with B-101 | Planned |
| B-103 | Homogeneous near-plane clip. Stop taking `abs(clip.w)` in `OpenGLES.cpp`. | 4 | 4 | 3 | 5.33 | B | None | Not with B-104, B-105, or B-111 | Planned |
| B-104 | Widen the GLSL ES 1.00 subset only against a checked-in shader corpus. No guessed sampler roles. | 5 | 4 | 3 | 6.67 | B | B-103. Dump with `HVM_DUMP_SHADERS` first | Not with other B | Planned |
| B-105 | Raster and composite cost: SIMD triangles, threaded slices, dirty-rect present, upload-time texture conversion. Plus CPU-side ideas in `docs/PERFORMANCE.md` that are not Group A (those are B-201). | 5 | 3 | 3 | 5.0 | B | B-103 | Not with other B | Planned |
| B-106 | Clash Royale past 65% load: 21903 sector state, then the next message the pinned client actually sends. Do not invent sector payloads. | 5 | 4 | 3 | 6.67 | C | Pinned IPA hash `29269ede…` | Not with edits to `src/runtime/libc/Net.cpp` | Planned. Fetching the third-party URL is Needs owner |
| B-107 | Instruction oracle for encodings Clang does not emit. Put `tools/decode_fuzz.py` on a weekly job at the documented sample size. | 3 | 3 | 2 | 4.5 | Shares A if decoders change | None for a pure oracle | Beside B and C | Planned. `--fuzz N` on the harness runs it on demand |
| B-108 | `regcomp` / `regexec` / `regerror` / `regfree` (Art of Glow). | 3 | 3 | 2 | 4.5 | — if the patch stays in `src/runtime/libc/` | None | Beside D | Planned |
| B-109 | `CGFont*` getters Tiny Tower queries. | 3 | 3 | 1 | 9.0 | D | Text stack | Not with another D change | Planned |
| B-110 | `tmpnam` (Angry Birds). | 2 | 2 | 1 | 4.0 | — | None | Yes | Planned |
| B-111 | `glPointSizePointerOES` (Tiny Tower). | 3 | 3 | 1 | 9.0 | B | None, but the file is `OpenGLES.cpp` | Not while B-103 is in progress | Planned |
| B-112 | Stop adding commercial IPAs to git. Hashes plus an external store. | 3 | 4 | 1 | 12.0 | — | Owner picks the store | Yes | Harness side Operational. Removing the three committed files is Needs owner |

## P2

| ID | Item | Impact | Sev | Cx | Score | Group | Depends on | Parallel | State |
|---|---|---:|---:|---:|---:|---|---|---|---|
| B-201 | CPU fusion and `preserve_none` follow-ups. Linux Clang 18 does not emit `preserve_none`. | 2 | 2 | 3 | 1.33 | After A | B-101 landed | Not with A | Planned |
| B-202 | libc++ slice for Minecraft Story Mode (`std::__1`). | 2 | 3 | 3 | 2.0 | Guest frameworks, exclusive on `LibStdcxx*.cpp` | None | Not with B-207 | Planned |
| B-203 | Android APK / `armeabi-v7a`. ELF load exists; no Android framework. | 2 | 2 | 3 | 1.33 | — | A stable | Independent of UIKit | Needs owner |
| B-204 | `CCCrypt` (AES/DES). The code returns `kCCUnimplemented` on purpose. Digests and HMAC stay. | 2 | 3 | 2 | 3.0 | — | Policy change | Yes | Needs owner |
| B-205 | `NSNotificationQueue` coalescing. No in-repo app was shown to hit it. | 1 | 2 | 1 | 2.0 | D | None | Not with other D | Planned |
| B-206 | Positional `printf` (`%1$d`). | 1 | 2 | 1 | 2.0 | — | None | Yes if `Format.cpp` is free | Planned |
| B-207 | libstdc++ file-stream virtuals that throw "not supported yet". | 2 | 2 | 2 | 2.0 | Exclusive on `LibStdcxx.cpp` | None | Not with B-202 | Planned |
| B-208 | `GuestArena::findFree` linear scan. | 2 | 2 | 1 | 4.0 | A if mapping locks change | None | Not with other A | Planned |
| B-209 | ASan/UBSan and a coverage build. `siglongjmp` fights ASan. | 3 | 2 | 2 | 3.0 | — | B-102's fault-path decision | Not while A is rewriting the fault path | Planned |

## B-105 children (discovered performance work)

These are Planned. They are not separate roles. Land B-103 first.

| ID | Item | Where the audit or the perf notes put it |
|---|---|---|
| B-105a | SIMD barycentric raster and depth test | `writeFragment` / `rasterizeTriangle` |
| B-105b | Threaded framebuffer slices | same functions |
| B-105c | Dirty-rect compositing | UIKit present path, `Raster.cpp` only if compositing changes |
| B-105d | Convert textures to host RGBA on upload | `glTexImage2D` |
| B-105e | Further CPU ideas that are not fusion: return stack, superblocks, inlined guest memory routines | `docs/PERFORMANCE.md`. Superblocks and handler changes wait for Group A even if they look like speed work |

## B-106 children (Clash past 65%)

| ID | Item | State |
|---|---|---|
| B-106a | Peer answers 21903 with sector state the pinned client already accepts | Planned |
| B-106b | Each later message, recorded in the restoration doc before the code changes | Planned, filled in as the probe shows them |
| B-106c | Two-launch native probe inside the harness when the IPA is in the fixture root | Planned. Today the manifest entry is optional and skips. Absence is not a pass |
| B-106d | 107-file IMA corpus vs libsndfile | Planned, needs the unpacked client and libsndfile. Not run by the default harness |

## No-parallel groups

- **A, memory model.** `src/core/memory/GuestArena.*`, load/store in `src/core/cpu/Handlers.hpp`, fault path in `src/core/cpu/Engine.cpp`, `mprotect` / `vm_protect` in `src/runtime/libc/System.cpp`, `tests/asm/arm_mem.S`. One owner.
- **B, pixels.** `src/runtime/graphics/Shader.cpp`, `Shader.hpp`, `src/runtime/uikit/OpenGLES.cpp`, the GLES checks in `tests/runner/TestRunner.cpp`. `src/runtime/graphics/Raster.cpp` joins this group when compositing changes. One owner. SIMD waits on the clipper.
- **C, Clash wire.** `tools/clash_wire.py`, `clash_home_state.py`, `clash_protocol_probe.py`, `clash_home_probe.py`, `clash_royale_probe.py`, `test_clash_protocol_probe.py`, `test_clash_home_state.py`, and loopback policy in `src/runtime/libc/Net.cpp` plus `Network.hpp`. One owner.
- **D, guest object model.** `src/runtime/objc/`, `src/runtime/foundation/` collections and forwarding, UIKit that calls them (`UIWidgets.cpp`, `Nib.cpp`, `UIView.cpp`, and the rest of `src/runtime/uikit/` except `OpenGLES.cpp`). One owner at a time.
- **E, Apple shell.** `app/**` and the public surface `HVMEngine.mm` uses (`app/HydrogenVM/Bridge/HVMEngine.h`, and `Process` signatures that header calls). C++ and Swift land together. Only a Mac can compile them.

`src/loader/**` is the app-compatibility role but it is not group C. A loader fix may run beside the Clash peer. It may not run beside a second loader fix.

## Scheduling notes

- B-002's code is in tree so the moment B-001 clears, `main` has a harness job. That does not close B-001.
- B-111 shares `OpenGLES.cpp` with B-103. If the clipper is open, point-size waits.
- B-109 shares Group D with any Foundation or UIKit class change. Regex and `tmpnam` do not, if they stay in libc.
- A 4 KB-page green harness run does not close B-101.
- The three in-repo smokes already have unimplemented imports. The diff fails when a **new** load-time import appears. It does not fail on the imports already in the baseline. Whether a brand-new import should fail even the first time it shows up in a longer run is owner question 9 from the audit; the harness treats runtime-only misses as attention, not a hard failure, because a 3 second smoke is not a full playthrough.
