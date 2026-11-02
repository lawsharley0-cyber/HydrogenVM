# T7: Semantic Record and Replay — Architecture & Design

Owner: **Antigravity** · Card: [#240](https://github.com/lawsharley0-cyber/HydrogenVM/issues/240) · Status: **Design for Review**

---

## 1. Executive Summary & Objective

**Goal.** Provide deterministic record and replay for HydrogenVM sessions to reliably reproduce, debug, and bisect long-session failures (such as Goat Simulator’s ~219 s SIGSEGV, world-load stalls, and race conditions).

**Scope & Nature.** HydrogenVM is a JIT-less ARMv7 user-mode emulator where guest threads are mapped to host OS threads and framework functions are high-level C++ implementations. Full machine execution recording (like QEMU’s instruction-level rr or Linux `rr`) is not directly applicable across different host architectures, operating systems, or GPU drivers. Instead, **T7 implements semantic record/replay**: recording non-deterministic inputs and host-boundary events at the guest ABI/HLE layer and replaying them against a deterministic virtual execution clock.

**Done When.** A recorded 60-second interactive session of a Test Lab guest replays with bit-for-bit identical final output (memory state checksum, retired instruction count, and output event log) across repeated runs.

---

## 2. Determinism Audit: Boundaries & Non-Deterministic Sources

To guarantee deterministic replay, all sources of entropy, timing jitter, and external state must be isolated, recorded, and synthetic during replay.

<!-- coderabbit-ai-generated:v1 -->

<!-- coderabbit-ai-generated-section:v1:6a8d329c390cd65c55ab07960612608a240da4b786e62b23fbe0b97fd79aee57 -->

| Category | Deterministic Under HydrogenVM? | Non-Deterministic Sources & Gaps | Mitigation in Record/Replay |
| :--- | :---: | :--- | :--- |
| **ARMv7 CPU Execution** | **Yes** (Single Thread) | Register operations, integer/VFP arithmetic, and memory operations are deterministic on one thread. | Count retired guest instructions as the reference tick for event injection. |
| **Touch & User Input** | **No** | Touch coordinates, phases, timestamps, and multi-touch order arrive asynchronously. | Record `(tick, touch_id, phase, point_x, point_y)` and inject at exactly that tick as in Section 4.3. |
| **Clocks & Timers** | **No** | Wall-clock reads and host timer scheduling vary. | Use virtual time derived from retired instructions and deterministic timer ordering. |
| **RNG & Entropy** | **No** | `arc4random()` is host-seeded; `rand()` has guest-controlled state; entropy reads can vary. | Use a session-seeded deterministic PRNG for `arc4random()` and a deterministic PRNG for `rand()` with its existing default seed of 1 and guest `srand()` reseeding. Fix the algorithms and seed mapping for a compatible record/replay implementation. Record every `/dev/urandom` read and `SecRandomCopyBytes` result and returned bytes as `EVENT_RNG_VAL`; replay returns those results without host entropy. |
| **Thread Scheduling & Race Conditions** | **No** | Host scheduling, mutex acquisition, and memory contention vary. | Run guest threads cooperatively in fixed round-robin instruction quanta; record mutex acquisition order. |
| **File System & Directory Order** | **Partially** | Directory order and file timestamps depend on the host. | Record the initial sandbox checksum, sort `readdir` alphabetically, and freeze timestamps to session start. Replay first recomputes the checksum and fails before executing any guest instruction on a mismatch; it then serves every file read, directory listing, and stat result from `EVENT_FS_READ` and never reads the host sandbox for guest-visible state. |
| **Host Graphics & OpenGL ES** | **No** | Driver capabilities, GPU operations, and readbacks can vary. | Require a deterministic renderer in recording and replay, with identical guest-visible capabilities and deterministic readbacks. Hash a fixed normalized framebuffer format at each checkpoint; unsupported graphics operations fail the deterministic session rather than fall back to host rendering. |
| **Network & IPC** | **No** | Socket, DNS, HTTP, and IPC results depend on external peers. | Record each boundary result and its stream identity, or force the operation offline with a recorded synthetic error. Replay uses the journal without contacting peers. |

<!-- /coderabbit-ai-generated:v1 -->

---

## 3. Event Journal Schema (`hvmrec/v1`)

<!-- coderabbit-ai-generated:v1 -->

<!-- coderabbit-ai-generated-section:v1:ba21106b9333ee3c7547b69b0c80945af753629b8176d01394ac039d33b90db6 -->

The recording artifact is a streaming binary file (with an optional JSON human-readable inspection mode) designated `hvmrec/v1`.

All multi-byte integers in headers, event frames, and structured payloads are encoded little-endian. Write and read each field individually in declaration order, with no padding; the C declarations describe field widths and order, not native struct layout.

<!-- /coderabbit-ai-generated:v1 -->

<!-- coderabbit-ai-generated:v1 -->

<!-- coderabbit-ai-generated-section:v1:18fdffc239ce869b5e4fc12e0e5b3e15d0403718a61535c0f538a242ce286507 -->

### 3.1 Header (Fixed 64 bytes)
```c
struct HVMRecordHeader {
    char magic[8];             // "HVMREC01"
    uint32_t schemaVersion;    // 1
    uint32_t hostPageSize;     // 4096 or 16384; replay requires the same value
    uint64_t initialRNGSeed;   // Session seed for arc4random
    uint64_t sessionStartTime; // Virtual epoch in microseconds
    uint8_t  appSha256[32];    // Exact SHA-256 of the executed binary/IPA
};
```

Replay compares `hostPageSize` with the replay host's page size, and `appSha256` with the executed binary, before running any guest instruction. A mismatch is a deterministic startup failure (`replay-incompatible-host` / `replay-wrong-app`); the replayer does not emulate another page size, because page-granular behavior (`mmap`, `mprotect`, fault addresses) would differ.

<!-- /coderabbit-ai-generated:v1 -->

### 3.2 Event Packet Format
Every non-deterministic boundary crossing emits an event frame:
```c
struct HVMRecordEvent {
    uint64_t instructionTick;  // Monotonic guest instruction count
    uint32_t threadId;         // Guest thread identifier
    uint16_t eventKind;        // Event type tag
    uint16_t payloadSize;      // Length of following payload bytes
    // uint8_t payload[payloadSize];
};
```

<!-- coderabbit-ai-generated:v1 -->

<!-- coderabbit-ai-generated-section:v1:f2b8bac029bab8d01a5f8675e633161f845b42735116b8740bb44859bb7171ec -->

Payloads of at most 65,535 bytes use one ordinary event frame. For a larger logical payload, write one `EVENT_PAYLOAD_START` (0x09) frame containing, in order, `uint16_t originalEventKind`, `uint64_t payloadId`, and `uint64_t totalPayloadSize`, followed immediately by one or more `EVENT_PAYLOAD_CHUNK` (0x0A) frames. Each chunk payload contains `uint64_t payloadId`, `uint64_t offset`, and 1 to 65,519 data bytes. `payloadId` uniquely identifies this logical payload within the journal and is distinct from a network/IPC stream ID. All frames in the sequence have the logical event's tick and thread ID. Chunks are contiguous and uninterleaved, starting at offset zero and continuing in increasing offset order until exactly `totalPayloadSize` bytes are present. The reader rejects missing, duplicate, overlapping, out-of-order, or excess chunks, nested framing kinds, and totals at or below 65,535. It reconstructs and dispatches the original event atomically at the start frame's position in journal order. This framing applies equally to filesystem and network/IPC payloads.

### 3.3 Event Kinds
1. `EVENT_INPUT_TOUCH` (0x01): Touch ID, phase, normalized coordinates $(x, y)$.
2. `EVENT_TIME_TICK` (0x02): Advanced virtual time step (`mach_absolute_time`, monotonic tick).
3. `EVENT_RNG_VAL` (0x03): The API or entropy-read operation, requested length, return status/error, and exact returned bytes for each `/dev/urandom` read or `SecRandomCopyBytes` call. Replay checks the operation and request, then returns the recorded status and bytes.
4. `EVENT_FS_READ` (0x04): Data read from external/host file descriptors or directory listings.
5. `EVENT_THREAD_YIELD` (0x05): Cooperative scheduling barrier and quantum boundary.
6. `EVENT_FRAME_PRESENT` (0x06): Frame presentation boundary and framebuffer hash checkpoint.
7. `EVENT_MUTEX_SYNC` (0x07): Mutex operation, stable lock ID, result, and acquisition sequence number for each successful lock acquisition. The frame's thread ID identifies the caller. A lock ID identifies one mutex lifetime, including a generation when a guest address is reused; successful acquisitions on each lock have consecutive sequence numbers. Replay grants acquisitions only to the recorded thread in that order and reproduces recorded operation results.
8. `EVENT_NETWORK_IPC` (0x08): Operation, stable stream ID, call parameters, return value/error, and exact returned bytes or other output for every external network or IPC call, including connection, send, receive, close, and DNS operations. Assign an ID to each stream or standalone request in recording order and never reuse it in the session. Replay verifies the operation, stream and parameters, returns the recorded result, and performs no external call.
9. `EVENT_PAYLOAD_START` (0x09) and `EVENT_PAYLOAD_CHUNK` (0x0A): Oversized logical-event framing as specified in Section 3.2.

### 3.4 Structured Payload Layouts
Fields are written in the order listed, little-endian, with no padding. `bytes` fields are a `uint32_t length` followed by that many bytes; `string` fields are `bytes` holding UTF-8 with no terminator. `status` is an `int32_t` (0 = success, otherwise the guest-visible errno or `OSStatus` the call returned).

| Event | Payload fields |
| :--- | :--- |
| `EVENT_RNG_VAL` | `uint8_t operation` (0 = `/dev/urandom` read, 1 = `SecRandomCopyBytes`), `uint32_t requestedLength`, `int32_t status`, `bytes data` |
| `EVENT_MUTEX_SYNC` | `uint8_t operation` (0 = lock, 1 = trylock, 2 = unlock, 3 = destroy), `uint64_t lockId`, `uint32_t generation`, `uint64_t acquisitionSequence` (0 for operations that do not acquire), `int32_t status` |
| `EVENT_NETWORK_IPC` | `uint8_t operation` (0 = connect, 1 = send, 2 = receive, 3 = close, 4 = DNS, 5 = other), `uint64_t streamId`, `bytes parameters`, `int32_t status`, `bytes output` |
| `EVENT_FS_READ` | `uint8_t operation` (0 = read, 1 = directory listing, 2 = stat), `string path`, `uint64_t offset`, `uint32_t requestedLength`, `int32_t status`, `bytes data` |

`lockId` is assigned in recording order and `generation` increments when a guest address is reused for a different mutex lifetime. `bytes parameters` holds the call's arguments in the order the guest passed them, each encoded as a `uint64_t`, and the reader checks them for equality. Oversized `data` or `output` uses the payload framing of Section 3.2.

<!-- /coderabbit-ai-generated:v1 -->

---

## 4. Subsystem Interception Design

<!-- coderabbit-ai-generated:v1 -->

<!-- coderabbit-ai-generated-section:v1:c4a108a08a269cc7ef33b38854406cd42dd0d0db246ca2fd53bfe431440e4d30 -->

### 4.1 Virtual Monotonic Clock & Timer Wheel
- In record/replay mode, host calls to `mach_absolute_time()`, `clock_gettime(CLOCK_MONOTONIC)`, and `gettimeofday()` do not query the host kernel.
- `time()`, `clock()`, `CFAbsoluteTimeGetCurrent`, `[NSDate date]`, and `CACurrentMediaTime()` are derived from the same virtual clock (`time()` from the session start time plus virtual seconds, `clock()` from virtual CPU time); none calls `std::time` or `std::clock` on the host.
- `sleep()`, `usleep()`, `nanosleep()`, and timed waits do not block the host: the calling thread becomes ineligible until virtual time reaches its deadline, and the scheduler advances virtual time to the earliest deadline when every thread is waiting.
- Instead, time advances as a function of retired instructions:
  $$\Delta t_{\text{virtual}} = \frac{\Delta \text{guestInstructions}}{\text{ClockRateMIPS}}$$
- `NSRunLoop` and `CFRunLoopTimer` timers use a priority queue ordered by trigger tick, then deterministic insertion order. Recording and replay split execution at a timer or journal event tick so neither is delivered late.
- At a block boundary with both due timers and journal events, enqueue all due asynchronous events at the journal head first, in file order, before running a timer callback. Then run the earliest due timer; repeat this check after each callback and at every boundary reached within a callback, before executing another guest instruction. Enqueuing an event does not itself execute guest instructions. Synchronous journal events are consumed only by their corresponding calls, including calls made by timer callbacks; never skip the journal head. Events with the same `instructionTick` are consumed or dispatched in journal file order. An unexpected call or an event that cannot be consumed at its recorded tick is a replay divergence.

- `CADisplayLink` callbacks (and the frame presents they drive) run on a fixed virtual schedule: a 60 Hz display, one callback per 1/60 s of virtual time, `timestamp` and `targetTimestamp` taken from that schedule, and no frame dropping. The host display's refresh rate never reaches the guest.

### 4.2 Deterministic Cooperative Multithreading
- Multithreaded execution is the largest hurdle to bit-for-bit replay on modern multicore hosts.
- To eliminate host pthread scheduling races:
  - Guest threads are scheduled in a cooperative round-robin loop.
  - Each thread executes a fixed instruction quantum $Q$ (e.g., $Q = 100{,}000$ instructions) or until it encounters a blocking syscall / mutex lock.
  - Mutex lock acquisitions are recorded in sequence order. During replay, lock acquisitions are granted only in the recorded order, preventing inversion.
  - The cooperative scheduler also owns the dispatch worker pool in both recording and replay. Assign jobs a monotonic submission order and workers stable creation-order IDs. Select eligible jobs in submission order while respecting queue width, barriers, and suspension; assign each asynchronous pool job to the lowest-ID idle worker. Synchronous dispatch runs on the caller once its queue grants access. If no worker is idle, create a worker in deterministic creation order up to the pool limit of 64; at the limit, leave the job pending until a worker is available. Wake the selected worker explicitly; never let host wakeup order choose a job.
  - Retire workers idle for 30 seconds of virtual time in stable worker-ID order, retaining at least four workers, instead of using host timeouts. Worker lifecycle, queue transitions, and guest callbacks executed by pool jobs are governed by the same cooperative round-robin scheduler and quantum as other guest threads. Host background work may affect guest-visible state only through ordered, recorded boundaries.

### 4.3 Input & Touch Dispatch
- During recording, touch events generated by UI interaction or automation scripts (`--tap`, `--drag`) are serialized with the active `instructionTick`.
- During replay, the guest engine checks the journal head before each block and splits execution when the next event tick falls inside the block. At exactly the recorded `instructionTick`, it dispatches the touch into `UIApplication`'s event queue, following the timer precedence and journal file ordering in Section 4.1. Advancing past an undelivered event is a divergence.

<!-- /coderabbit-ai-generated:v1 -->

---

<!-- coderabbit-ai-generated:v1 -->

<!-- coderabbit-ai-generated-section:v1:94798051e0bd8521bdda63c61b5f032f92255eab61cdbe32c464edfd57fe79cf -->

## 5. Test Lab Verification Case (`TL-Replay`) & Acceptance Criteria

To satisfy the **T7 Done When** milestone:
1. **Fixture**: A new Test Lab case `TL-Replay` (`tests/ios/apps/testlab/replay_fixture.m`) designed to execute for 60 seconds:
   - Maintains an active `CADisplayLink` / frame animation loop.
   - Dispatches background calculation tasks to worker threads via `dispatch_async`.
   - Simulates periodic touch interactions (drags, buttons, gesture recognizers).
   - Utilizes `arc4random()`, `mach_absolute_time()`, and file persistence.
2. **Oracle & Verification Protocol**: The record report contains the output event log, retired instruction count, final heap checksum, and frame hashes using the same representation as the replay reports.
   ```bash
   # Run 1: Record 60-second interactive session
   hvm run TestLab.ipa --case TL-Replay --record session.hvmrec --run-for 60 --out report_record.json

   # Run 2: Replay run A from recording
   hvm run TestLab.ipa --case TL-Replay --replay session.hvmrec --out report_replay_a.json

   # Run 3: Replay run B from recording
   hvm run TestLab.ipa --case TL-Replay --replay session.hvmrec --out report_replay_b.json
   ```
3. **Acceptance Invariants**:
   - Compare each replay independently against `report_record.json`: the output event log and total retired guest instructions must match the recording exactly.
   - Each replay's final memory heap checksum and frame hashes at second 60 must match the recording byte-for-byte.
   - `report_replay_a.json` and `report_replay_b.json` must also be identical.

<!-- /coderabbit-ai-generated:v1 -->

---

## 6. Staged Implementation Roadmap

- **Phase 1 (This Document)**: Architecture, schema specification, determinism audit, and peer review.
- **Phase 2 (Virtual Time & Input Journaling)**: Implement `hvmrec/v1` serializer, decoupled virtual clock, and single-threaded touch injection in `hvm`.
- **Phase 3 (Deterministic Scheduler)**: Implement cooperative thread quantum slicing to guarantee identical thread interleave.
- **Phase 4 (Test Lab Integration & CI Gate)**: Land `TL-Replay` in `tools/hvm_testlab.py` and enforce deterministic replay in CI.
