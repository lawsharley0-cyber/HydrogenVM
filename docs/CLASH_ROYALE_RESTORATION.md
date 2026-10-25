# Clash Royale solo restoration investigation

## Current architecture — October 25, 2026

The owner now authorizes a modified guest IPA containing its own offline logic.
Read [guest mod progress](clash-royale/GUEST_MOD_PROGRESS.md) first. This prototype
runs on ordinary main `6884670` without a Clash host service. Three earned training
wins, native reward recovery, a paid native card upgrade, and an updated
deck in the next battle are locally reproduced. It is **not the complete offline
game**; the earlier seven-training and arena evidence below belongs to the host
backend, not this guest port. Device behavior remains unverified.

## Current continuation — October 24, 2026

Latest: native crown collection (523/source 3) now completes normal reveal and
actual interrupted-journal recovery in isolated component fixtures. Two
relaunches do not duplicate the reward. These fixtures start with ten crowns;
earned progression to ten remains unverified, and the earned player retains
three. See PROGRESS for exact evidence. Princess facing during animation and
King cannon anchoring remain explicit owner-reported visual bugs; the initial
native direction fix is not proof of complete animation correctness.

Work continues on `codex/clash-offline-continuation`, now integrated with main
`2b60a06`.
See [current evidence and blockers](clash-royale/PROGRESS.md). The historical
October 20 notes below are retained for decoder evidence, not current status.
An unmodified client completed all seven native training NPCs against the
embedded C++ backend and reached the original Arena 1 home. Native card upgrades,
eight deck slots, multiple independent chests and actual interrupted collection
recovery are verified on Linux/WSL. A shared CoreText caret API and native initial
name command unblock the pre-final-trainer name dialog. Full offline arena
progression is **not yet verified**, and these changes need iPhone validation.
Chest reward delivery uses the original
native randomizer and command encoder: the earned chest was unlocked, opened,
collected, and removed. Gold increased from 100 to 116 and Knight/Arrows counts
increased from 1 to 2. Two relaunches retained the completed reward without
another grant. A durable journal retains the pre-claim checkpoint until native
command 213 finishes. Older half-claimed saves without a journal still need
separate recovery. Legacy duplicate-ID saves also need explicit migration.
The first Arena 1 bot battle now completes after correcting its sector trailer.
Native ELO now produces and persists a visible 34-trophy result. Tower level/HP
scaling uses original CSV values. Correct neutral buff values restore tower
occupants and shrinking health bars. Retaining native decoration identities
restores the original trees, fences and other arena-edge scenery. The unchanged
client supplies all rendering; no assets or assertions were altered.
Native free-chest rewards and a Goblins-for-Arrows deck swap now persist;
interrupted free reward recovery was exercised without restoring a backup.
All six arena scenes load attacking bots in isolated synthetic fixtures, which
do not establish earned later-arena progression. Battle costs, crown accounting,
long Silver unlocks and earned later-arena qualification remain open. Earlier wins used incorrect tower
initialization; a subsequent corrected-combat three-crown win raised native
score 34 to 65 and survived two relaunches. Its reward label still disagrees
with the saved cycle chest, and native NPC battles skip crown credit. Both
remain core blockers. Princess orientation and King cannon anchoring also
remain wrong per the owner's clarified report; visibility is not correctness.

Subsequent ledger work aligns native ChestOrderIndex with the earned cycle and
records actual native tower crowns. The original home transfers them; native
crown count 3 survives two relaunches. A future win/reward-label comparison and
crown-chest collection at 10 remain open. Combined gem unlock/claim has actual
interrupted-journal recovery: the gem charge persists, reward applies once.
The earned player also reaches level 2 through original card upgrades. See
PROGRESS for exact evidence and failed attempts. Measured elapsed home time now
uses the original native API, including across closure and interrupted reward
recovery; timestamps account for native ticks/loading pauses. Long Silver
completion still needs qualification. Opposing native facing vectors also
replace zero vectors, correcting initial tower facing locally. Arena match
costs now debit original Gold/FreeGold values before sector delivery; a real
204-to-203 Arena 1 debit survives two relaunches. Full crown collection, victory
reward-label comparison and cannon motion/device validation remain open.
Apple checkpoint a92eb84 compiled/packaged an unsigned device IPA, before the
entry-fee change; successful packaging is not device gameplay validation.

The owner reports intermittent audio, incorrect King cannon animation, missing
Princess Tower princesses and uncentered text on iPhone. Preserve those reports
for remaining presentation work. The owner now requests continuing through all
stages, including bug fixes. Local visual fixes still require phone validation.

Historical date: 2026-10-20. Historical work branch: `gemini-merge`.

## Latest: native home acceptance and first NPC request (2026-10-20)

**The game is still not playable.** The local peer now sends a decoder-derived
**24101 OwnHomeDataMessage**, and the unchanged client accepts it, logging
`Loaded own home from server`. It loads the training location and menu assets,
advances to the **65% loading scene**, then sends **14104 StartMissionMessage**
with bytes `12 00`: NPC data reference **18000000**, the first tutorial mission.
The peer intentionally reports this request as unsupported; it supplies no
invented/opaque sector blob. No battle, deployment, menu interaction or game
progression is verified. This supersedes the older login-only black-frame limit.

`tools/clash_home_state.py` builds an explicit initial home/avatar fixture from
the pinned IPA's own `starting_home.json`. The default body is **184 bytes**:
home starts at offset 0, deck at 15, collection at 64 and avatar at 123. Home ID
uses fixed BE LogicLong; avatar IDs use pairs of signed VInts. Eight deck-presence
booleans share one byte; all byte-oriented reads reset boolean alignment. The
six original cards, empty collection, four empty chest slots and free-chest timer
are retained. Avatar level 1, training arena, 100 total/free gems, and 100
Gold/FreeGold follow the native default-avatar path and bundled globals.
Unnamed members are identified by native object offsets in the encoder and kept
at constructor defaults. This is an initial-state fixture, not a progression engine.

The peer's `--home-client IPA --game-state FILE` options validate the exact pinned
IPA SHA256. Initial home/avatar JSON is stored atomically, separately from the
diagnostic identity/token. Existing state must match the client and account; it
is read without resetting it. Unsupported collection/location state is rejected.
Saving an initial fixture and retaining login identity do not prove game-progress
persistence; command processing, rewards and battle state are still absent.

New shared runtime fixes reached after home acceptance:

- `NSRegularExpression copyWithZone:` now retains its immutable compiled pattern.
  Regex deallocation releases its copied pattern; match results retain/release
  their owning expression. A pool-drain/lifetime test covers these relationships.
- Dictionary subscript and value-setting methods dispatch the subclass's keyed
  primitives, including nil-value removal and nil-key rejection. The client SDK
  has a real `FBGraphObject` dictionary subclass; direct host DictData mutation
  incorrectly rejected it.
- `NSDictionary`'s inherited `allKeys`/`allValues` now enumerate subclass
  `keyEnumerator`/`objectForKey:` primitives. The prior fallback called `allKeys`
  recursively and exhausted the host stack. GDB identified that host crash.
  A subclass missing the required enumerator fails explicitly instead of recursing.

Validation on the native WSL build: **79 runtime checks**, **12 Python protocol/
home-state tests**, **18 shader checks**, and **107 IMA WAV reference comparisons**
pass. Two **75-second** launches against the formal peer both accept the home,
request NPC 18000000 and return exit 0 with `crashed=false`, `errors=[]`. Login
changes from fresh to retained. Both capture **169590 non-silent audio frames**,
peak **0.830139**, with no audio transport errors/drops. These are captured mixer
samples, not another physical speaker or iPhone-device test. Four older/default-
network compatibility probes are also rerun for the shared Foundation changes.

Reproduce (after building with `tools/local_test.sh check`):

```sh
python3 tools/clash_home_probe.py --hvm /path/to/hvm --ipa /path/to/pinned.ipa --out /var/tmp/clash-home-probe --seconds 90 --launches 2 --capture-audio
```

The probe imports once, checks existing app metadata/executable on reuse, and
runs the installed bundle on relaunch. Keep this diagnostic output separate from
the desktop save container. `identity.json` contains a private local token and
must not be published. Each `run-*` directory contains public diagnostic logs,
frames, protocol events and `verification.json`. Probe success is specifically
home acceptance plus an NPC request, **not playable offline mode**. The workflow
now includes these checks; remote execution remains subject to the known account
billing blocker until jobs actually run.

### Current exact blocker: an initial NPC sector snapshot

The native reply type is **21903 SectorStateMessage** (`getMessageType` at
`0x9615c` returns 0x558f). `SectorStateMessage::decode` itself is empty because
`SectorManager::receiveSectorState` parses its ByteStream later. That manager at
`0x5a288` reads a raw compression flag byte; compressed data uses the MySQL-style
LE uncompressed size plus zlib. Plain flag 0 calls the same state parser directly.

`LogicGameMode::decode` at **0xd7804** expects a tick and auxiliary VInt, a checked
**11** marker, LogicTime, a fixed-width random seed, battle state and two avatars,
then checked marker **12**, tutorial state, a trailing value and command-manager
state. `LogicBattle::decode` at **0x14470** needs location/NPC/arena references,
player IDs, per-side values, flags, the game-object manager, two decks, tower
references/lists and battle timers. `LogicGameObjectManager::decode` at
**0x55c4c** contains object types/IDs, object bodies and four passes of components.
An empty object list would not represent a usable battle and is not supplied.

Mission 18000000 corresponds to bundled `npcs.csv` **Npc1**, location **FirstMatch**,
predefined deck **BasicDeck1**, seed **2**, level **1**. Use those original assets
to derive a valid initial sector, including the towers, player/NPC decks and
tutorial state, then follow heartbeat/command handling. Preserve the native marker,
type and range checks. Next verify a visible arena, a real troop deployment, one
complete NPC battle, and backend progression saved across fresh peer/client starts.

## Audio checkpoint (2026-10-20)

The unchanged loading track `res/sfx/scroll_loading_01.wav` is Microsoft IMA
ADPCM WAV (format 0x11), stereo 44.1 kHz. The shared decoder now handles mono/stereo
4-bit IMA WAV: block predictor/index headers, low-first nibbles, four-byte channel
groups, block resets and `fact` sample-count trimming. Invalid headers, step
indices, count overruns and truncated data fail decoding. Apple IMA4 remains a
separate layout. No asset transcoding or game modification is involved.

All **107** archived IMA WAV files match libsndfile PCM sample-for-sample after
trimming its padded final block to each file's `fact` count. The comparison used
the installed system libsndfile and `hvm_tests audio-decode INPUT OUTPUT.pcm`.
`tools/test_ima_wav_corpus.py ROOT --hvm-tests PATH --out REPORT.json
--require-count 107` automates this check; the investigation workflow now includes
it after the runtime checks. It needs system libsndfile1, no audio device.
Synthetic tests independently cover channel separation, nibble order, block reset,
mono duplication, duration, invalid input and stalled desktop-consumer shutdown.
The full local runtime suite passes **75 checks**.

The desktop viewer now pulls the same mixer as the iPhone frontend and sends its
PCM to SDL/PulseAudio. A 50-second pinned-client run produced **169590 non-silent
frames**, peak **0.830139**, no transport drops/errors and no guest errors/crash.
That first run exposed 288 playback underruns. Three-block startup priming reduced
this to **one underrun in a subsequent 45-second run**, with 971 delivered blocks
and no device errors. Occasional host scheduling/audio starvation remains a
quality concern. Saved PCM/device acceptance does not certify speaker audibility,
battle sounds, every API or iPhone audio; those still require gameplay/device tests.
See `LOCAL_TESTING.md` for capture, mute, device dependency and diagnostics.

Architecture clarification: `file` reports the pinned executable as **Mach-O
armv7**, with no arm64 slice. Re-signing this IPA alone cannot make it run on
iOS 11+, which removed 32-bit app support. The current modern-phone route is the
64-bit HydrogenVM host executing the original guest. A compatible arm64 game
archive would be a separate native-restoration candidate. Offline home state,
NPC battles and progression persistence remain unimplemented/unverified below.

## Earlier checkpoint: local login without home data (2026-10-20)

Runtime/protocol foundation: `de7f95b`; WSL source-mirror commit: `1201143`.
The pinned IPA and its bundled assets/settings remain unchanged.

**Correction to the original offline-save inference:** at virtual address
`0x000925b4`, `GameMode::saveOffline(String const&)` is four bytes,
`70 47 00 bf`: Thumb `bx lr; nop`. It returns immediately. The offline filenames
and selected-deck symbol survive in the executable, but this entry point has no
save implementation. `tools/clash_royale_probe.py` now records this byte-level
fact in `metadata.json.offlineCodeEvidence`, including the Thumb flag and symbol
extent. This does not rule out every other local-state path, but the mere flag
and strings are not evidence of working native offline play.

New observed protocol evidence:

- With opt-in BSD loopback transport, the client resolves
  **`game.clashroyaleapp.com`**, despite `res/settings.csv` naming localhost.
  The captured path therefore does not simply follow that CSV configuration.
- An explicit hostname alias routes that name to **127.0.0.1**, where a local
  diagnostic listener receives actual native packets. No external connection
  or DNS lookup is needed. Default runtime networking remains disabled.
- The client sends message **10108** keepalives and **10101**, version **3**,
  login. Its fresh login body is **197 bytes**, with account `(0,0)`, a null token,
  and signed-VInt wire version **`(2,0,1306)`**. RC4 state and the native login
  encoder/decoder were checked against the pinned executable.
- `tools/clash_protocol_probe.py --login-ok` sends a narrowly scoped **20104**
  LoginOk and **20108** keepalive replies. The LoginOk field order comes from
  this client's decoder at `0x000702e4`; it uses VInts. The nearby 2.1507 reference
  uses different fixed-width fields and extra URLs, so its reply is not reused.
- The native client logs **`accountId (0, 1)`** after that reply. On a subsequent
  launch of the same installed bundle/container, its login body grows to
  **245 bytes**, supplies the retained diagnostic identity/token, and is accepted
  against the same local identity file. No token is written into diagnostic
  event logs. This proves login-identity retention, **not game progression**.
- The peer provides **no OwnHomeDataMessage, NPC state or combat logic**. After
  login the capture shows black output; the peer has supplied no home state. No menu,
  troop deployment, NPC battle or offline avatar/home save is verified.

Shared runtime changes support outbound IPv4 TCP only on loopback, numeric and
localhost `getaddrinfo` resolution, explicit hostname aliases, send/recv, SO_ERROR and selected
real socket options. Normal read/write/close/dup/dup2 use the same tracked socket
ownership; teardown closes retained descriptors. Blocking socket IO polls in
short slices so process stop interrupts it. `fcntl` now applies real descriptor
and nonblocking flags, and host socket errors map to Darwin values. Unsupported
socket operations fail; UDP, listening sockets, external networking and Cocoa
URL-loading remain unsupported. Socket-to-stdio wrapping is explicitly rejected
with ENOTSUP: host FILE buffering would bypass interruptible IO and socket
close ownership. Normal file fdopen behavior is preserved. Reachability still reports offline.

Validation: **71 runtime checks pass**, including nine network/descriptor/stop
checks; **six Python protocol tests pass**; all **18 captured shaders compile**.
The full fresh build and local protocol relaunch use the Linux-native source
mirror. The four-app strict/exploratory/relaunch probes run with networking off.
All four reach the time limit with no reported crashes. Angry Birds and Art of
Glow have varied frames; Tiny Tower retains its black-frame baseline. The short
Clash capture flags mostly black output during startup; a separate 45-second
default-off run reaches the rendered 50% loading/error scene with no reported
errors. See individual frame evidence; a scanner `runs` result is not proof of
game compatibility. No iPhone build was tested in this continuation.

A fresh event-driven pair of launches waits for the native account-confirmation
log before requesting graceful stop. Both return exit 0, the first sends the
fresh 197-byte login, and the second sends the retained 245-byte login. On this
workstation confirmation took about **43 seconds** and **37 seconds**. A fixed
25/30-second probe can therefore finish before login; use an observed milestone
and a longer timeout instead of interpreting the cutoff as a protocol failure.

### Home-state target at that checkpoint (implemented above)

Build the pinned client's **OwnHomeDataMessage (24101)** from its actual decoder
and bundled `res/level/starting_home.json`, including the six-card starting deck,
collection, timers and chest slots, plus a matching LogicClientAvatar. Decoder
entry points are `0x0001921c` (OwnHomeDataMessage), `0x00124f9c` (LogicClientHome)
and `0x0009a008` (LogicClientAvatar). Retain type/range/assertion checks and log
exact parser failures. A nearby server's opaque home blob is not a validated
payload for this client.

Then follow `GameMode::sendStartMissionMessage` at `0x00093c84`: it sets the NPC
sector wait state, loads the location tilemap, constructs StartMissionMessage and
sends it via MessageManager. Match the pinned NPC-sector decoder/state format
before testing one complete original-logic battle. Store home/avatar/progression
in the local restoration backend and test save/relaunch; the empty saveOffline
entry point cannot supply that persistence by itself.

The external reference was inspected at
[CR-1.2.3 commit 6c3015d](https://github.com/Magich03/CR-1.2.3/tree/6c3015de6952a5f5113e7a998795f29042340394).
Its README explicitly describes an incomplete test server. No server code or
opaque game-state blobs were copied into HydrogenVM.

### Reproduce the local login experiment

Build via `tools/local_test.sh check` first. On this workstation the new native
binary is `/var/tmp/hvm-local/build-0072df8e000f/hvm`; the helper prints the source
and build directory for other checkout paths. In separate terminals:

```sh
python3 tools/clash_protocol_probe.py --out /var/tmp/clash-peer --login-ok --state /var/tmp/clash-peer/identity.json --seconds 260
```

```sh
/var/tmp/hvm-local/build-0072df8e000f/hvm run /var/tmp/clash-client.ipa --home /var/tmp/clash-peer-game --loopback-network --loopback-alias game.clashroyaleapp.com --run-for 90 --graceful --report /var/tmp/clash-peer/client.json
```

Use a **new diagnostic home**, not the desktop save container. To test relaunch,
run the installed `/var/tmp/clash-peer-game/com.supercell.scroll/Clash Royale.app`
directly with `--home /var/tmp/clash-peer-game/com.supercell.scroll`, retaining the
same peer `--state` file and network arguments. Re-importing an IPA replaces its
container. `protocol-events.json` reports message types, sizes, version and fresh
versus retained identity, without logging the token.

### WSL filesystem and performance finding

Following the [Microsoft WSL filesystem guidance](https://learn.microsoft.com/en-us/windows/wsl/filesystems#file-storage-and-performance-across-file-systems),
`tools/local_test.sh` now builds mounted Windows checkouts through a dedicated
Linux source mirror. Rsync checks **content**, preserves the editable Git tree,
and gives changed build inputs Linux timestamps; this also avoids stale Ninja
objects when Windows and WSL clocks differ. The first native build is fresh.
Custom `HVM_LOCAL_BUILD` settings keep their existing source/cache pairing;
`HVM_LOCAL_NATIVE_SOURCE=0` opts out of automatic mirroring.

The three warm Net.cpp compiler-front-end samples were 3.473/3.461/3.330 seconds
from the Windows mount and 3.045/2.950/3.038 seconds from ext4, about **12% less
median time**. This is not a gameplay-FPS result. Binaries, installed game assets,
sandboxes and the viewer's live-frame IPC already reside on Linux storage.
The desktop/iPhone performance difference still needs a controlled runtime
profile; Windows-side source access alone does not explain that difference.

## Earlier milestone: visible loading scene (2026-10-20)

Runtime commit: `ff2d4fd20523fa7994d4152e53ca23f65fbd1dcc`.

The shared GLES2 renderer now executes a bounded straight-line GLSL subset
instead of guessing transforms/colors from attribute and uniform names. The
unchanged pinned Clash client renders its loading artwork, logo, training-camp
hint, **50% loading bar**, and connection-error panel. **It is still not playable**:
there is no verified menu, tutorial/NPC battle, offline profile or progression.
The previous report of a UIKit dialog over black output was incomplete; the
new frame establishes that the game scene and its styled error panel render.

Implemented in `src/runtime/graphics/Shader.{hpp,cpp}` and `OpenGLES.cpp`:

- Scalar/vector arithmetic, vector/scalar constructors, column-major
  matrix-vector/vector-matrix multiplication, swizzles and overlapping swizzle
  assignments, local constants, compound assignment, `dot`, and `texture2D`.
- Attribute/uniform bindings by declared identifier and assigned GL location,
  multiple varyings, literal-indexed varying arrays, actual sampler texture
  units, and perspective-correct varying interpolation for triangles and lines.
- Per-stage compilation, uniform/varying link validation, zero-initialized
  uniforms on link, real compile/link status and bounded info logs. Unknown
  attribute/uniform names return -1. Unsupported syntax fails explicitly.
- Source/register/nesting bounds and checked literal array/swizzle accesses.
  No guest assertion, memory check, or game asset was bypassed or modified.

This is **not full GLSL ES 1.00 support**. Supported preprocessing is limited to
GL_ES conditionals and `#version 100`; precision qualifiers use host float.
Only floating-point scalar/vector/matrix values and sampler2D uniforms are
implemented. Integer-looking numeric literals currently enter the float subset;
there is no general int/bool type system. Control flow, functions beyond main,
structs, dynamic indexing, uniform arrays, general macros, most builtins,
discard, derivatives, mipmap LOD and homogeneous near-plane clipping remain
unsupported. Such shaders may now report failure where the old renderer
incorrectly claimed success and guessed a picture. GLES1 retains its existing
fixed-function path. No device/IPA release validation is implied by Linux tests.

Local evidence:

- Clang Release build: **62 runtime checks passed, 0 failed**, including five
  new shader/parser/pixel/diagnostic checks. The pixel check verifies two color
  varyings, a nonzero sampler unit and a nonuniform clip-W interpolation result.
- **18/18** original captured Clash shader sources compile, including all nine
  vertex/fragment pairs and the blur varying arrays. The corpus also passes an
  ASan/UBSan compiler run. `hvm_tests shaders <dump-directory>` reproduces source
  acceptance; it does not establish that every shader pair has been drawn.
- `tools/clash_royale_probe.py --ipa /var/tmp/clash-client.ipa` re-verifies the
  pinned SHA256 and unencrypted ARMv7 metadata. The IPA is unchanged.
- The workflow's 15-second strict/exploratory/relaunch scan covers Clash and the
  three pinned older apps. All reach the time limit. Angry Birds and Art of Glow
  have varied frames; Tiny Tower remains black. Clash's scanner `runs` result
  means timed execution, not successful offline gameplay.
- A separate 30-second graceful Clash run presents 348 frames and exits without
  reported errors. Its screenshot shows the 50% connection error. Reached API
  misses remain the loading WAV decoder and reachability reported offline.

Two 25-second launches of the installed bundle with a scheduled retry tap
also close gracefully and return to the same error; the retry delegate and
in-game actions remain unverified. The client still attempts ports 9339, 1863,
3724, 30000 and 843 despite the bundled `OfflineMode=true`. Neither `offline_avatar.json` nor
`offline_home.json` appears. `titan.checkpoint` is not an offline avatar/home
save. Next investigate how the settings table's flag is consumed and what
initial state is required for native offline entry; then verify an actual NPC
battle and save/relaunch before extending arenas or designing a backend. A
static byte-string scan finds no ASCII `OfflineMode` in this executable; CSV
access can be indexed, so this does not establish that the field is unused.

Reproduction on this workstation (separate diagnostic sandbox):

```sh
cmake --build /var/tmp/hvm-clash-build --target hvm hvm_tests hvm_machodump --parallel 4
/var/tmp/hvm-clash-build/hvm_tests fulltest
HVM_DUMP_SHADERS=/var/tmp/clash-shaders /var/tmp/hvm-clash-build/hvm run /var/tmp/clash-client.ipa --home /var/tmp/clash-new-diagnostic-home --run-for 30 --graceful --screenshot /var/tmp/clash.png --report /var/tmp/clash.json
/var/tmp/hvm-clash-build/hvm_tests shaders /var/tmp/clash-shaders
```

`hvm run <IPA>` replaces that app's container on import. For subsequent persistent
launches, run the installed `.app` directly or use `tools/desktop.py`, which
imports once. Use a new diagnostic home instead of overwriting desktop saves.
The investigation workflow now captures and compile-checks the shader corpus.
GitHub Actions remains subject to the previously observed account billing block;
local results are the validation evidence until a new remote job actually runs.
After pushing, [investigation run 39516992151](https://github.com/lawsharley0-cyber/HydrogenVM/actions/runs/39516992151)
and [CI run 39516992084](https://github.com/lawsharley0-cyber/HydrogenVM/actions/runs/39516992084)
both failed with zero executed steps: GitHub reports failed recent payments or
an insufficient spending limit. These are not failed emulator test results.

## Local workstation tester

`tools/local_test.sh` and `tools/desktop.py` now provide a WSLg desktop window,
live frames, mouse touch/drag, background/foreground, screenshots, graceful
shutdown and persistent sandbox reuse. Windows launchers are in the task's
`outputs/` folder. See [`LOCAL_TESTING.md`](LOCAL_TESTING.md) for controls and paths.
The current local suite passes **71 checks**. Shutdown now distinguishes an
intentional Halt/ProcessExit from actual guest/HLE faults; the latter are still
reported. End-to-end viewer testing receives live frames, delivers touch down/up,
and exits cleanly. A Documents marker survives relaunch. This verifies host
sandbox retention, not offline avatar/home progression. The visible game limit
remains the 50% loading connection error described above.

## Earlier milestone: Mach and custom-font startup support (2026-10-20)

Runtime commit: `095832c2c4362010d2da927412361b2b81b97ea2`.
The branch was fetched before editing and matched the supplied `b9c4701` head.
Local WSL/Linux Release build with Clang: **55 runtime checks passed, 0 failed**.
The checksum-pinned client is unchanged, including its bundled offline flag.

**Clash is not playable.** Strict startup now passes the original crash-reporter
and `DataLoaderFont::load` blockers, loads the logic tables and UI assets, and
continues until the probe time limit. The longer capture shows a visible UIKit
**Connection error / Try again** dialog over mostly black game output. This is
not a loading/menu screen or a working NPC match. The scanner's `runs` category
only means the timed launch continued; its frame-variation check can be satisfied
by this error dialog.

Implemented in the shared runtime:

- Per-process, synchronized synthetic Mach send-reference accounting for the
  task, a distinct host port, `mach_thread_self`, and `task_threads` exports.
  `mach_port_mod_refs`, `mach_port_get_refs`, and deallocation validate task,
  name, right, underflow and overflow. No receive rights/IPC were fabricated.
- `vm_read_overwrite` copies only mapped guest ranges with suitable access,
  rejects wrapping addresses, and reports the copied byte count. Guest page
  size/mask/shift data agree on 4 KiB pages. The next crash-reporter operation,
  `mach_make_memory_entry_64`, explicitly returns `KERN_NOT_SUPPORTED` and a null
  handle because there is no memory-object backend. The client recovers from
  that capability failure; this is not successful Mach memory-object support.
- CF allocator context copying, guest retain/release/allocation callbacks,
  thread defaults, and distinct null/system allocators. No-copy CFData retains
  the bytes' allocator and invokes its deallocator on destruction. Custom
  object-storage allocators, including `kCFAllocatorUseContext`, remain an
  explicitly reported unsupported case.
- Custom standalone TrueType fonts preserve provider bytes and supply their own
  cmap, names, metrics, traits, glyph bounds and outlines through CGFont/CTFont.
  The existing rasterizer draws custom outlines into guest bitmap contexts.
  Pinned, unmodified `stb_truetype` supplies decoding; the wrapper validates table,
  cmap and glyph ranges and rejects recursive/unsupported composites. This is
  not a claim of a fully hardened font parser or CFF/TTC support.
- A limited, uniform-attribute, horizontal CoreText typesetter/line/run path,
  including line breaking, owned run lifetimes and glyph extraction. Complex
  scripts, surrogate shaping, arbitrary multi-run text, frame layout and several
  other CoreText APIs remain unsupported. Font subpixel settings are tracked.
- CF mutable character-set creation/copy/range bridges. Fixed the existing
  `NSCharacterSet +new` ownership error (it incorrectly autoreleased its result),
  which otherwise caused a double release after the formatter's pool drain.
- `HVM_DUMP_SHADERS=<existing-or-new-directory>` writes original shader sources
  for diagnosis. The investigation workflow now triggers for all shared runtime
  and runner changes, rather than only the old directory fix.

### Earlier evidence and blockers

Local strict/exploratory/relaunch probes cover the pinned client plus the same
three older IPAs at fixture commit `ceba289b57cbda0e229e778966f195bcc1437fa0`.
Angry Birds and Art of Glow still produce varied output without new runtime API
failures; Tiny Tower's black frame remains its known baseline. Clash's reached
runtime miss is the unsupported audio format of `scroll_loading_01.wav`;
SCNetworkReachability continues to report offline by design. Static unreached
imports remain and must not be confused with reached failures.

At that earlier milestone, the native game scene was not rendered correctly. Captured vertex shaders
use `uniform vec4 myPMVMatrix` and
`gl_Position = myPMVMatrix * a_position + vec4(-1, 1, 0, 0)`. The current GLES2
heuristic only applies a selected `mat4`; it does not execute this component-wise
transform. Fragment shaders also use two color varyings, channel masks and
additive color expressions that the heuristic does not implement. Fix this with
shared shader semantics, not game-specific attribute-name guesses. The dump
contains nine vertex/fragment pairs, including a blur shader with varying arrays.

The client actively attempts ports 9339, 1863, 3724, 30000 and 843, then shows its
connection error despite `OfflineMode=true`. No `offline_avatar.json` or
`offline_home.json` was created in the tested fresh or reused homes. This
establishes that this launch has not initialized native offline play; it does
not establish why the flag is insufficient or that a replacement server is
required. A scripted `--tap 160,337@18` reached UIKit touch handling and was
followed by another error dialog. The delegate retry callback and in-game input
have not been independently verified.

GitHub [investigation run 39508176438](https://github.com/lawsharley0-cyber/HydrogenVM/actions/runs/39508176438)
and CI run 39508176627 did **not start any test steps**. Their check annotation
says account payments failed or the spending limit must be increased. Those
failed badges are infrastructure failures, not remote test results. All results
above are local; no green Actions result is claimed for this revision.

### Reproduce this revision locally

Use the commands in `.github/workflows/clash-royale.yml`: prepare and hash-check
the client with `tools/clash_royale_probe.py`, build `hvm`, `hvm_tests` and
`hvm_machodump`, run `hvm_tests fulltest`, then `tools/compat.py scan` with the
four IPAs, `--seconds 15 --jobs 1 --relaunch`. The local environment had
`llvm-objdump-18`; a PATH shim named `llvm-objdump` allowed the unchanged probe
to run. Builds and IPA/sandbox copies used Linux storage for consistent I/O.

For the longer diagnostic/input capture:

```sh
HVM_DUMP_SHADERS=clash-shaders build-clash/hvm run clash-report/client.ipa \
  --home clash-home --run-for 30 --tap 160,337@18 \
  --screenshot clash-error.png --report clash-run.json
```

Next: implement/validate the required GLES2 shader semantics; trace where the
client actually consumes the settings offline flag and enters its offline
state; verify profile creation, one NPC battle and save/relaunch before arena
expansion. Keep the executable/assets unchanged until specific evidence calls
for an explicit, hash-verified restoration profile.

## Previous verified state: directory and timezone fixes

Tested engine commit: `239a0ab818b2ddb3a7f146dbbef2cae98e7aba5c` on
`gemini-merge`. [Completed run](https://github.com/lawsharley0-cyber/HydrogenVM/actions/runs/39484502121)
and [diagnostics artifact](https://github.com/lawsharley0-cyber/HydrogenVM/actions/runs/39484502121/artifacts/12292748070).

**38 runtime checks passed, 0 failed. Clash Royale is not playable yet.**
The two original startup blockers are fixed in the shared runtime:

1. **Directory domains.** The pinned binary's instructions establish
   `NSSearchPathForDirectoriesInDomains(9, 0xFFFF, true)`, followed by
   `NSSearchPathForDirectoriesInDomains(13, 2, true)`. The latter means
   Caches in the local domain. Previously, every domain mask without the user
   bit returned an empty array.
   `Bundle.cpp` now supplies supported local-domain locations under
   `<app home>/LocalDomain/`, with user locations first for combined masks.
   There is no shared guest filesystem mounted; these paths deliberately use
   a separate per-app root rather than host `/Library`. Searches do not create
   directories. Six checks cover cache paths, ordering, tilde expansion,
   invalid/unmounted domains and NSFileManager URL parity.
2. **Fixed-offset time zones.** `Misc.cpp` implements
   `+[NSTimeZone timeZoneForSecondsFromGMT:]`, stable GMT names and offsets,
   date-independent offset queries and no DST for fixed zones. Explicit
   GMT/UTC names no longer resolve to the host's local zone. Four additional
   checks cover zero/positive/negative offsets, names, date independence and
   named UTC. This does not expand NSDateFormatter's existing support for
   arbitrary nonzero timezone formatting.

Clash Royale now reaches `GameMain::init` and its bundled HockeyApp/PLCrashReporter
startup. The **next strict blocker** is:
`mach_port_mod_refs` at return address `0x00149863` in
`+[BITPLCrashReporter initialize] + 0x62`. Recorded arguments:
task `0x103`, port `0x103`, right `0` (send), delta `1`.

The exploratory run advances to `DataLoaderFont::load` and an original
Debugger assertion. Reached gaps include `CFAllocatorCreate`,
`CGContextSetAllowsFontSubpixelPositioning`,
`CGContextSetShouldSubpixelQuantizeFonts`,
`CGFontCreateWithDataProvider`, and `CTFontCreateWithGraphicsFont`.
It also encounters crash-reporting helpers `backtrace_symbols_fd` and
`vm_read_overwrite`. These exploratory results do not establish the next
strict call order after the Mach API is fixed.

All three older-game probes still run without new runtime misses.
Angry Birds and Art of Glow produce varied frames; Tiny Tower's final frame
is still black, as in the initial baseline. These are short Linux probes,
not device or full-gameplay validation.

### Offline flag and symbols discovered (later save-body correction above)

Static inspection of the exact pinned IPA establishes:

- `res/settings.csv`: ServerURL `localhost`, port `9339`,
  **OfflineMode `true`**, placeholder asset/security URLs `http://asdf`.
- Binary references `../offline_avatar.json`, `../offline_home.json`,
  `GameMode::saveOffline(String const&)` and an offline selected-deck variable.
- The only bundled JSON assets are `res/fingerprint.json` (version
  `2.1306.1`) and `res/level/starting_home.json`.
- The starting-home template has decks, collection, selected-deck and chest
  state. The offline avatar/home save files are **not bundled**.

See [static diagnosis](https://github.com/lawsharley0-cyber/HydrogenVM/actions/runs/39483383480)
and `.github/workflows/clash-static.yml` for the reproducible inspection.

This describes the archived package, not verified pristine retail settings.
No one in this investigation changed its settings or patched its executable.
The flag and symbols are promising, but do not prove native offline mode can
initialize a profile, launch NPC battles, or save/reload successfully.

### Immediate continuation goals

**Historical list from the previous handoff.** The Mach and font startup work
below is now implemented to the extent described in the current section. The
current blockers are rendering and offline-state entry, not the original
`mach_port_mod_refs` miss or `DataLoaderFont::load` assertion.

1. Resolve the observed Mach send-right reference operation consistently with
   HydrogenVM's existing synthetic task/thread ports. Inspect
   `src/runtime/libc/System.cpp` and `Pthread.cpp`; do not register blanket
   successful no-ops for unimplemented IPC or disable array/assert checks.
   Alternatively, a deliberately opt-in, hash-verified restoration profile
   could disable optional crash reporting; that would be a game-specific
   change and must be tested and documented as such.
2. Re-run strict mode, then implement the actual allocator/font prerequisites
   as reached. `CGFontCreateWithDataProvider` currently reports a missing
   function and uses a system-font fallback, while CoreText support is largely
   absent. Replacing missing calls with zero is not a working font renderer.
3. Reach a rendered loading/menu screen, then test whether this client's
   **bundled offline flag** leads to creation of the missing saves from its
   bundled starting template. Prefer this verified path over assuming that a
   replacement network backend is necessary.
4. Validate one NPC match and save/relaunch, then expand arenas. If the native
   offline path is insufficient, investigate the specific missing protocol or
   state handling before choosing an in-process backend.

The previous baseline and research follow for context.


The initial investigation below preceded the runtime fixes described above;
at that baseline, no client bytes or runtime behavior had been changed.

## Reproduce and review

- Tested commit: `0a447062cc1d049b4980ac70054514cfe3177e75`.
- [Completed four-app investigation](https://github.com/lawsharley0-cyber/HydrogenVM/actions/runs/39458108530).
- [Diagnostics and screenshots artifact](https://github.com/lawsharley0-cyber/HydrogenVM/actions/runs/39458108530/artifacts/12282535281).
- Workflow: `.github/workflows/clash-royale.yml`.
- Client preparation/inspection: `python3 tools/clash_royale_probe.py --out clash-report`.
- Compatibility runs: `tools/compat.py scan`, strict and exploratory modes,
  15 seconds per launch, strict relaunch enabled, one app at a time.
- The workflow asserts that all four apps were scanned. Fixtures are fetched
  from pinned main commit `ceba289b57cbda0e229e778966f195bcc1437fa0`.
- A green investigation job means the inspection/probes completed; it does not
  mean Clash Royale passed or is playable.

## Verified client

The archive's release notes identify this as iOS 1.0; the actual IPA metadata
calls both its version and build **2.1306.1**.

- Bundle ID: `com.supercell.scroll`.
- ARMv7, minimum iOS 7.0, GLES2 imports.
- Executable encryption check: unencrypted.
- IPA size: 90,361,183 bytes.
- SHA-256: `29269ede437f39ffa62fb4234a1c323303ef417e7eb6ddc5c86f9be76e08fb3f`.
- [Source release](https://github.com/astralsc/clashroyale_archive/releases/tag/Clients).
- The probe verifies the checksum before inspecting/running the app.

## Initial baseline results (before fixes)

The host engine built successfully. `build-clash/hvm_tests fulltest`:
**28 passed, 0 failed**.

| App | Strict/relaunch probe | Rendering evidence |
|---|---|---|
| Clash Royale 2.1306.1 | Startup crash; same failure in exploratory mode | No frames |
| Angry Birds 1.0 | Runs; no runtime misses | Varied frame, 298 presented frames |
| Tiny Tower 1.0 | Runs; no runtime misses | 363 presented frames, but final frame entirely black |
| Art of Glow 1.1.2 | Runs; reports network unavailable | Varied frame, 264 presented frames |

These are short Linux host smoke probes, not end-to-end gameplay or device
performance results. The frame counts are the scanner's reported counters.
The Tiny Tower black frame is unresolved; this investigation made no runtime
change, so it is not evidence of a newly introduced regression.

## Initial directory blocker (now resolved)

The traced sequence near the end of
`-[AppController application:didFinishLaunchingWithOptions:]` is:

1. `NSSearchPathForDirectoriesInDomains` at caller return address `0x0016d2a1`.
2. `NSArray objectAtIndex:` succeeds, followed by `NSString UTF8String`.
3. A second `NSSearchPathForDirectoriesInDomains` call at `0x0016d2e9`.
4. `NSArray objectAtIndex:` at `0x0016d2f3` throws:
   `index 0 beyond bounds for empty array`.

Trace exit status: 70. Both strict and exploratory probes reproduce the same
launch crash. This is the first observed blocker, before a server-check failure
has been established.

The relevant emulator implementation is `searchPaths` in
`src/runtime/foundation/Bundle.cpp`. It returns an empty vector for unsupported
directory enum values or masks without `NSUserDomainMask`.

**Resolved:** static disassembly identified the second call as `(13, 2, true)`.
The shared directory-domain implementation and its tests are described in the
current-status section above. NSArray bounds checks remain intact.

## Restoration material in the original client

The probe enumerated 54 CSV files. Relevant bundled tables include:

- `res/csv_logic/arenas.csv`.
- `npcs.csv`, `predefined_decks.csv`, `tutorials_npc.csv`,
  `tutorials_home.csv`.
- Character, building, spell, projectile, buff, and progression definitions.
- Arena location data under `res/locations/`, including training, goblin,
  bone, barbarian, spell, royal, dark and pvp layouts.

CSV row counts include headers/type rows, so they are not arena or NPC counts.
These files establish that restoration assets/configuration exist locally.
They do not establish that all simulation logic is present, callable offline,
or compatible with a particular community server.

## Network and rendering constraints

On the tested branch:

- `src/runtime/libc/Net.cpp` makes socket creation fail with Darwin
  `ENETDOWN`; other socket operations are unavailable.
- Foundation stream/URL-loading and reachability implementations also report
  offline behavior.
- The current GLES2 interpreter supports only the documented straight-line
  subset. Loading artwork now renders; other scenes still need direct tests.

Changing the game's server address alone therefore cannot provide a working
local backend in the current runtime.

## Protocol references checked

- [RetroRoyale](https://github.com/retroroyale/ClashRoyale) targets 1.9,
  not this client.
- [CR-1.2.3](https://github.com/Magich03/CR-1.2.3) targets a nearer 2016 version
  and explicitly describes itself as incomplete.
- In that 1.2.3 source, `StartMissionMessage` responds with
  `NpcSectorStateMessage`, whose encoder contains a fixed compressed snapshot.
  This is a lead for investigating NPC battle bootstrapping, not a validated
  1.0 protocol implementation or complete bot simulation.

No external server code was copied into HydrogenVM.

## Implementation sequence

1. Directory lookup, fixed-offset timezone, synthetic Mach references and the
   reached custom-font startup path are fixed. See the current section for the
   deliberately unsupported Mach and CoreText operations.
2. Loading artwork and local login now work to the limited extent above.
   Supply a version-correct home/avatar payload; the menu is not verified.
3. Locate this client's NPC/tutorial battle entry points, message dispatch,
   snapshot format, random seed/tick control and result handling. Confirm what
   the client simulates rather than inferring it from filenames.
4. Opt-in local TCP transport and diagnostic login are implemented. Extend the
   local peer with validated home/NPC state and persistence; retain explicit
   routing and consistent descriptor/readiness/error behavior.
5. Restore a single complete NPC match with original combat logic where
   available; verify troop deployment, elixir, towers, win/loss and replayability.
6. Add local profile/deck/progression saves and a save/relaunch test.
7. Expand to each included arena, with original NPC logic or a measured bot
   controller. Do not claim all arenas until each one has a complete match test.
8. Package a restoration profile keyed to the exact client hash, with explicit
   prerequisites and reversible patches. Keep game-specific state separate
   from the generic emulator.

A useful first proof of HydrogenVM as a restoration platform is one reproducible,
fully playable offline match with local persistence. That milestone is not yet
implemented.
