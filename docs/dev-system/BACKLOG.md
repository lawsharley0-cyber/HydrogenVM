# Backlog

## Active owner-directed Clash continuation (2026-10-23)

October 25 UI/stability follow-up (same branch, Group C): owner requests a
clickable-control audit, Settings Help/Support crash fixes, post-upgrade relaunch
recovery, and measured stutter investigation. Phone v19 report identifies
unsupported native command 516 after king level-up, host 6884670, 16 KB pages.
Record coverage and unavailable/locked scenarios explicitly; local tests do not
close phone reports. Shared framework fixes, if necessary, also reserve Group D
and require pre/post Test Lab and fulltest. Preserve the earned/failing saves.

October 26 Arena-1 control audit (Group C): v31's ranking button reproduces
an abort on unported read-only request 14403. Port the original empty-list
schemas for global/local player/clan rankings, without fabricated online
players. Qualify every ranking tab/close against the earned saved player.
Shop opens but has no daily cards and displays a zero-second refresh timer;
shop generation/purchases remain a separate unfinished economy feature.

Next guest core task (Group C): bind a versioned, checksummed wall-clock stamp
to the atomic native checkpoint and reward journal. Migrate unstamped CRG1
without guessing historical elapsed time; use original relative-time APIs,
subtract already-accounted native ticks, defer pending-reward time effects,
and reject corrupt metadata without resetting saves. Qualify closed/repeated
Silver/free timers and interrupted-journal recovery on preserved QA data.

Owner's October 26 ordered extension (continue the current timer/recovery and
control audit first):

1. Local offline clans: the original Clans tab must allow creating a persistent
   clan and interacting with it. No invented online players or remote service;
   distinguish local simulation from actual multiplayer. Inspect the original
   membership/creation/chat/donation schemas and native King-level gate before
   defining which interactions the pinned client can support.
2. Post-training opponent names: research plausible player-handle patterns and
   randomize names for each arena match; retain all original training names.
   Use authored synthetic handles, not harvested private player identities.
3. Arena bots: use cards unlocked at the actual arena and vary decks/playstyle
   across rounds. Research comparable projects and inspect the pinned original
   native bot code; compare actual decision behavior rather than label-only
   difficulty changes. Preserve reproducible seeds/evidence and native combat.
4. Daily store: research this early 2.1306.1 client's original cards/items,
   prices, rarity/count/rotation rules, then implement daily local refresh and
   persistent purchases using actual native economy APIs. Modern store rules
   are not this archived client's reference. Never invent successful IAPs.

Record research, exact behavior/limits and tests in docs/clash-royale so another
agent can continue. Original user audio/visual/stutter reports remain open.

Observed during the v33 original profile audit: 65 trophies/three-crown-win
stat/cards persist, but displayed Wins is 0 while NPC wins is 9 (seven training,
two paid arena wins). Qualify native offline arena profile counters separately;
never label a captured incorrect statistic a functional pass or invent results.

The owner's explicit request resumes offline Clash development despite the
master plan's earlier recommendation to park it. Group C owner:
`codex/clash-offline-continuation`. Restore the pinned NPC sector, native VInt
codec and desktop/headless loopback integration on current main, preserve GPU
rendering and audio, then verify native tutorial completion, one NPC win and
earned-state relaunch. Unsupported read-only turn metadata must be reported
without disconnecting an otherwise valid session. No turn replay or invented
checksum acknowledgments. Initial arena rendering is not battle completion.
Reproduction evidence and remaining blockers belong in `docs/clash-royale/`.

Priorities follow the 2026-10-21 audit of `main` at `07fabbb`. Scope added on top of that audit: Clash Royale past the 65% load, and performance items discovered while improving frame time. Those discovered items are listed under B-105 and are not a blank check to rewrite the rasterizer early.

Score is `impact * severity / complexity`.

- Impact and severity are 1–5. Higher means more of the product breaks, or breaks harder.
- Complexity is 1 straightforward, 2 medium, 3 hard. A hard item scores lower so a small crash fix is not buried under a research project, but the P0/P1/P2 band still comes first. Score only sorts inside a band.
- Needs owner items stay in their band. They are not pulled into a coding lap.

Groups A–E are exclusive. Two tasks with the same letter do not run together. An empty group may run beside the lettered work if the files do not overlap. The matrix is in `ROADMAP.md`.

## P0

| ID | Item | Impact | Sev | Cx | Score | Group | Depends on | Parallel | State |
|---|---|---:|---:|---:|---:|---|---|---|---|
| B-001 | Make GitHub Actions execute and publish a report for `HEAD`. The empty-step failure on `07fabbb` (run 39633887764) was real. Linux CI on `6af9976` published `runs/20261021-175201-39646696896-linux` (run 39646696896), and the compat harness passed (run 39646706013). | 5 | 5 | 1 | 25.0 | — | — | Yes, except another edit of the same workflow | Operational as of `6af9976`. macOS CI remains conditional |
| B-002 | Run `fulltest`, the Python protocol tests, and a 3 second smoke of the three in-repo IPAs on `main`. | 5 | 4 | 1 | 20.0 | — | — | Yes, as its own workflow file | Operational. The compat job is green on `6af9976` and runs `fulltest`. Folding the same suites into `hvm_report.py` (default `--only` is still `cpu,ios,bench`) is Planned. macOS CI was not part of that green run |
| B-003 | Reconcile `docs/ARCHITECTURE.md`, `docs/HANDOFF.md`, and `docs/DEVELOPMENT_AND_RELEASE.md` with the code. Preserve documented shader limits and `CCCrypt` policy; homogeneous clipping is now merged in PR #4. | 4 | 3 | 1 | 12.0 | — | None | Yes | Planned |
| B-004 | One Mac compile of current `app/`. `06a7b16` changed the file browser, was marked `[mac]`, and Actions never started. | 4 | 4 | 2 | 8.0 | E | B-001 if no local Mac | Not with another `app/**` edit | Unsigned device IPA and simulator build/launch verified via Codemagic in PR #7. Signed iPhone guest gameplay remains unverified |
| B-211 | Owner-priority file saving overhaul: verified writes, rolling backups, buffered SQLite, staged snapshots and reachable guarded Save controls. | 5 | 5 | 3 | 8.33 | E | None | Not with another app/** edit | Implemented in PR #18; Mac persistence, five UI tests, native compile and Linux gate pass on d3085f3. |

## P1

Player continuation on `codex/player-viewport-hud` (stacked on draft #18):

- B-212 / UX-2 (Group E): guest-oriented viewport, display scaling/filtering,
  inverse touch mapping and device-axis acceleration mapping. Implemented in Group E;
  collapsible floating HUD pill and manual orientation override completed.
- B-213 / UX-3 (Group E): lifecycle pause/resume and stop-before-inspect controls.
  Implemented in Group E; direct Save & Files Inspector sheet trigger and status badges completed.

| ID | Item | Impact | Sev | Cx | Score | Group | Depends on | Parallel | State |
|---|---|---:|---:|---:|---:|---|---|---|---|
| B-101 | Enforce guest 4 KB permissions on loads and stores, including the 16 KB host-page case. | 5 | 5 | 3 | 8.33 | A | None, but one owner for the whole group | Not with B-102, B-201, or B-208 | Planned |
| B-102 | Precise fault PC. Do not `siglongjmp` out of an HLE frame that has live C++ objects. | 4 | 5 | 3 | 6.67 | A | Same owner as B-101, after or inside that design | Not with B-101 | Planned |
| B-103 | Homogeneous near-plane clip. `OpenGLES.cpp:2320` is only a zero-W guard. Lines 2449–2452 rasterize the bbox of already-projected vertices; there is no near-plane clip. | 4 | 4 | 3 | 5.33 | B | None | Not with B-104, B-105, or B-111 | Merged in PR #4 on main; homogeneous view-volume clipping is implemented. Follow-ups still share Group B |
| B-104 | Widen the GLSL ES 1.00 subset only against a checked-in shader corpus. No guessed sampler roles. | 5 | 4 | 3 | 6.67 | B | B-103. Dump with `HVM_DUMP_SHADERS` first | Not with other B | Initial translator and bounded execution merged in PRs #8/#9. PRs #11/#12 are merged; retain corpus/unsupported-syntax coverage |
| B-105 | Raster and composite cost: SIMD triangles, threaded slices, dirty-rect present, upload-time texture conversion. Plus CPU-side ideas in `docs/PERFORMANCE.md` that are not Group A (those are B-201). | 5 | 3 | 3 | 5.0 | B | B-103 | Not with other B | Planned |
| B-106 | Clash Royale past 65% load: 21903 sector state, then the next message the pinned client actually sends. Do not invent sector payloads. | 5 | 4 | 3 | 6.67 | C | Pinned IPA hash `29269ede…` | Not with edits to `src/runtime/libc/Net.cpp` | Planned. Fetching the third-party URL is Needs owner |
| B-107 | Instruction oracle for encodings Clang does not emit. Put `tools/decode_fuzz.py` on a weekly job at the documented sample size. | 3 | 3 | 2 | 4.5 | Shares A if decoders change | None for a pure oracle | Beside B and C | Planned. `--fuzz N` on the harness runs it on demand |
| B-108 | `regcomp` / `regexec` / `regerror` / `regfree` (Art of Glow). | 3 | 3 | 2 | 4.5 | — if the patch stays in `src/runtime/libc/` | None | Beside D | Merged in PR #5 on main; current gate no longer reports these imports missing |
| B-109 | `CGFontCopyTableForTag` and the `CGFontGet*` queries Tiny Tower hits. `CGFontCopyTableForTag` is not a getter. | 3 | 3 | 1 | 9.0 | D | Text stack | Not with another D change | Implemented; verified with TestLab TL-B |
| B-110 | `tmpnam` (Angry Birds). | 2 | 2 | 1 | 4.0 | — | None | Yes | Merged in PR #5 on main; current gate no longer reports tmpnam missing |
| B-111 | `glPointSizePointerOES` (Tiny Tower). | 3 | 3 | 1 | 9.0 | B | None, but the file is `OpenGLES.cpp` | Not while B-103 is in progress | Implemented; verified with TestLab TL-H |
| B-112 | Stop adding commercial IPAs to git. Hashes plus an external store. | 3 | 4 | 1 | 12.0 | — | Owner picks the store | Yes | Harness side Operational. Removing the three committed files is Needs owner |
| B-114 | Shared PCM decoder: valid 64-bit float samples reach a 32-bit shift, NaN reaches integer conversion, and unsupported sample widths reach unchecked shifts. Add synthetic WAV/CAF regression coverage before a focused decoder fix. Owner music reports UAR-003/004 remain unresolved. | 3 | 4 | 1 | 12.0 | Audio (`Decode.cpp` only) | None | Beside B; no PlatformStubs changes | Merged in PR #16; clean cloud `031b16d`: 42 targeted checks and Linux gate pass |
| B-115 | ZIP64 extra-field lengths must be checked against the enclosing directory record before reading replacement sizes/offsets. Synthetic malformed IPA import reads outside its metadata buffer. Rejected construction also needs exception-safe file ownership. | 3 | 4 | 1 | 12.0 | Loader (`Zip.cpp`) | None | No other loader editor; beside B/C | Implemented in PR #17: clean cloud `cb3509e`, 58 targeted checks plus Linux gate pass; owner app reports unresolved |

## P2

| ID | Item | Impact | Sev | Cx | Score | Group | Depends on | Parallel | State |
|---|---|---:|---:|---:|---:|---|---|---|---|
| B-201 | CPU fusion and `preserve_none` follow-ups. Linux Clang 18 does not emit `preserve_none`. | 2 | 2 | 3 | 1.33 | After A | B-101 landed | Not with A | Planned |
| B-202 | libc++ slice for Minecraft Story Mode (`std::__1`). | 2 | 3 | 3 | 2.0 | Guest frameworks, exclusive on `LibStdcxx*.cpp` | None | Not with B-207 | Planned |
| B-203 | Android APK / `armeabi-v7a`. ELF load exists; no Android framework. | 2 | 2 | 3 | 1.33 | — | A stable | Independent of UIKit | Needs owner |
| B-204 | `CCCrypt` (AES/DES). The code returns `kCCUnimplemented` on purpose. Digests and HMAC stay. | 2 | 3 | 2 | 3.0 | — | — | Yes | Operational 2026-10-23: forwarded to host CommonCrypto / OpenSSL (`HostCrypto.cpp`), fulltest NIST vectors |
| B-205 | `NSNotificationQueue` coalescing. No in-repo app was shown to hit it. | 1 | 2 | 1 | 2.0 | D | None | Not with other D | Planned |
| B-206 | Positional `printf` (`%1$d`). | 1 | 2 | 1 | 2.0 | — | None | Yes if `Format.cpp` is free | Planned |
| B-207 | libstdc++ file-stream virtuals that throw "not supported yet". | 2 | 2 | 2 | 2.0 | Exclusive on `LibStdcxx.cpp` | None | Not with B-202 | Planned |
| B-208 | `GuestArena::findFree` linear scan. | 2 | 2 | 1 | 4.0 | A if mapping locks change | None | Not with other A | Planned |
| B-209 | ASan/UBSan and a coverage build. `siglongjmp` fights ASan. | 3 | 2 | 2 | 3.0 | — | B-102's fault-path decision | Not while A is rewriting the fault path | Planned |

## B-105 children (discovered performance work)

These are planned follow-ups, not separate roles. B-103 is merged in PR #4; active Group B ownership still applies.

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
| B-106e | Native free-chest command 518 and reward journaling | Reproduced in `free-observed`: original UI enters collection, backend reports unsupported 518/538 and has no reward to deliver. Preserve pre-claim native state, use original source-2 reward command, verify actual collection and interrupted replay. |
| B-106f | Decode native home echoes 500/525/538 | Native deck swap, spell-page list and achievement refresh can share a turn with a requested chest. Decode their exact schemas before dispatching; native checkpoint capture remains authoritative. Unknown/truncated commands must still reject the entire turn. |
| B-106g | Arena reward ledger/native avatar agreement | Corrected-combat win exposes native ChestOrderIndex left at zero while the host cycle advances; NPC result shows Wooden but earned cycle gives Gold. Native NPC mode also skips StarCount. Persist earned cycle and actual native tower crowns with the battle transaction; let original getNextWinnerChest and home crown transfer use that state. Never infer past crowns from win count. |
| B-106h | Combined gem speed-up and claim (506) | Actual Silver Open Now spends native gems and enters collection, but no 503 follows. Journal the native post-cost/unlocked pre-grant checkpoint and source-1 reward atomically, preserving the charge on interruption. |
| B-106i | Original tower facing vectors | Owner reports wrong princess angles and unanchored cannon. Native spawnObject 0x13fe8..0x13ffc sets direction Y +256 for owner 0, -256 otherwise; full sector currently overwrites all six directions with zero. Restore that initialization; independently verify cannon motion afterward. |
| B-106j | Real elapsed home timers across closure/battles | Timestamp complete native home checkpoints, bind elapsed time to the decoded delivered home, and call original LogicClientHome::fastForward(seconds,-1). Preserve reward journal atomicity; old untimestamped saves start measuring prospectively, without invented historical elapsed time. |
| B-106k | Native arena match cost | Arena UI shows its CSV cost, but original command 537 does not deduct it. Commit validated sector plus serialized Gold debit before delivery, using native FreeGold=min(previousFreeGold,newGold) semantics. Prevent an old home snapshot or duplicate pending request from undoing/repeating the debit; preserve the cost when a started match is abandoned. |
| B-106l | Crown-chest command 523/source-3 journal | Original UI creates its chest at ten crowns, but collection stalls on unsupported 523. Add native source-3 randomization/execution/recovery, and flush current source state before claim commands so newly available chests cannot race the periodic checkpoint. Isolated ten-crown fixtures do not prove earned progression to ten. |

## No-parallel groups

- **A, memory model.** `src/core/memory/GuestArena.*`, load/store in `src/core/cpu/Handlers.hpp`, fault path in `src/core/cpu/Engine.cpp`, `mprotect` / `vm_protect` in `src/runtime/libc/System.cpp`, `tests/asm/arm_mem.S`. One owner.
- **B, pixels.** `src/runtime/graphics/Shader.cpp`, `Shader.hpp`, `src/runtime/uikit/OpenGLES.cpp`, the GLES checks in `tests/runner/TestRunner.cpp`. `src/runtime/graphics/Raster.cpp` joins this group when compositing changes. One owner. SIMD waits on the clipper.
- **C, Clash wire.** `tools/clash_wire.py`, `clash_home_state.py`, `clash_protocol_probe.py`, `clash_home_probe.py`, `clash_royale_probe.py`, `test_clash_protocol_probe.py`, `test_clash_home_state.py`, and loopback policy in `src/runtime/libc/Net.cpp` plus `Network.hpp`. One owner.
- **D, guest object model.** `src/runtime/objc/`, `src/runtime/foundation/` collections and forwarding, UIKit that calls them (`UIWidgets.cpp`, `Nib.cpp`, `UIView.cpp`, and the rest of `src/runtime/uikit/` except `OpenGLES.cpp`). One owner at a time.
- **E, Apple shell.** `app/**` and the public surface `HVMEngine.mm` uses (`app/HydrogenVM/Bridge/HVMEngine.h`, and `Process` signatures that header calls). C++ and Swift land together. Only a Mac can compile them.

`src/loader/**` is the app-compatibility role but it is not group C. A loader fix may run beside the Clash peer. It may not run beside a second loader fix.

## Scheduling notes

### Owner-reported investigation queue (unresolved)

Source of truth: [USER_APP_REPORTS.md](USER_APP_REPORTS.md). Preserve its UAR IDs and original observations; exact builds, hosts and app hashes remain unspecified. These are reports to reproduce, not verified root causes or resolved defects.

| Queue | UAR IDs | Required scenario | Ownership after diagnosis |
|---|---|---|---|
| Startup crashes | UAR-001, UAR-009, UAR-011, UAR-012 | Loading transition / first failure, guest PC and reached API | Loader/runtime/framework group established by evidence |
| Later crashes | UAR-006, UAR-010 | Minecraft PE world load; Talking Larry click | Runtime/framework group established by evidence |
| Freeze | UAR-007 | Pocket Ants beyond 10 seconds, execution/frame/audio/input liveness | Establish from hang capture |
| Black output | UAR-002, UAR-005 | Goat Sim scene transition; identify Minecraft 1.1 separately | Coordinate active Group B before graphics edits |
| Performance | UAR-003, UAR-008 | Jetpack Joyride gameplay; Retry's reported 4–15 FPS | Profile first; coordinate measured hot path |
| Music with working effects | UAR-003, UAR-004 | Gameplay music, decoder/queue/loop/output evidence | Audio; preserve Mega Jump's positive performance control |
| Relaunch crash | UAR-013 | Tiny Tower launch, close, failing relaunch, next launch in the same sandbox | Shutdown/persistence/runtime, after reproduction |

- B-210 (P1, impact 4, severity 4, complexity 1, score 16): reject aligned-allocation size overflow and invalid alignments before touching the heap. Owned by `codex/aligned-allocation-safety` (`Heap.cpp`, libc `Stdlib.cpp`, separate heap regression executable). Reproduced on `39904cb`: `allocAligned(0xfffffff0, 4096)` returns a block with only 4080 usable bytes. Draft PR #15: six new checks pass in Release and targeted sanitizers; four fail on original main. Full Linux gate has zero regressions (see STATUS.md/indexed JSON). This shared corruption risk is not attributed to any observed game crash. Does not change Group A mappings, locks, handlers or fault behavior.

- B-001 is operational for Linux as of `6af9976`. The compat job runs the harness. Teaching `hvm_report.py` to run `fulltest` is still open and does not reopen B-001. The `07fabbb` empty-step failure (run 39633887764) stays on the record.
- B-111 shares `OpenGLES.cpp` with B-103. If the clipper is open, point-size waits.
- B-109 shares Group D with any Foundation or UIKit class change. Regex and `tmpnam` do not, if they stay in libc.
- A 4 KB-page green harness run does not close B-101.
- The three in-repo smokes already have unimplemented imports. The diff fails when a **new** load-time import appears. It does not fail on the imports already in the baseline. Whether a brand-new import should fail even the first time it shows up in a longer run is owner question 9 from the audit; the harness treats runtime-only misses as attention, not a hard failure, because a 3 second smoke is not a full playthrough.
