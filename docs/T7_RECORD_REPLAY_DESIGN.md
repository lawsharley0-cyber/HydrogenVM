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

| Category | Deterministic Under HydrogenVM? | Non-Deterministic Sources & Gaps | Mitigation in Record/Replay |
| :--- | :---: | :--- | :--- |
| **ARMv7 CPU Execution** | **Yes** (Single Thread) | Pure register operations, integer/VFP arithmetic, and memory loads/stores execute deterministically within a single thread. | Base monotonic instruction counter (`guestInstructions`) provides the reference tick for event injection. |
| **Touch & User Input** | **No** | Tap coordinates, touch phases (`Began`, `Moved`, `Ended`), timestamps, and multi-touch sequence order arrive asynchronously from the UI. | Record touch events with `(tick, touch_id, phase, point_x, point_y)`. Inject during replay when `guestInstructions >= target_tick`. |
| **Clocks & Timers** | **No** | Host wall-clock reads (`mach_absolute_time`, `gettimeofday`, `clock_gettime`, `CFAbsoluteTimeGetCurrent`) vary across runs; timers fire based on host OS scheduling. | Decouple guest clock from host clock. Replace wall clock with virtual time stepped deterministically per instruction quantum / frame tick. |
| **RNG & Entropy** | **No** | `arc4random()`, `rand()`, `/dev/urandom`, and `SecRandomCopyBytes` consume host OS entropy. | Seed a deterministic PRNG at session start. Record the seed; replay reproduces identical pseudo-random sequences. |
| **Thread Scheduling & Race Conditions** | **No** | Guest threads run on host `std::thread` / pthreads. Host kernel scheduling, mutex acquisition order, and memory bus contention are non-deterministic. | Implement deterministic cooperative thread slicing during record/replay: threads execute in fixed instruction quanta (e.g. 100k instructions) in round-robin order. |
| **File System & Directory Order** | **Partially** | Directory traversal order (`opendir`/`readdir`) depends on host filesystem layout; file modification timestamps (`stat`) leak host time. | Record initial sandbox checksum; normalize `readdir` to alphabetical sort; freeze virtual file timestamps to session start. |
| **Host Graphics & OpenGL ES** | **No** | Host GL driver strings (`glGetString`), extension query addresses, and texture upload timing vary by driver/GPU. | Record driver capabilities in journal header; stub or capture asynchronous GPU readbacks; evaluate software rasterizer as deterministic reference. |
| **Network & IPC** | **No** | Sockets, DNS lookup, and HTTP responses are inherently external and non-reproducible. | Network responses are captured as discrete byte streams in the journal or forced offline with immediate synthetic errors. |

---

## 3. Event Journal Schema (`hvmrec/v1`)

The recording artifact is a streaming binary file (with an optional JSON human-readable inspection mode) designated `hvmrec/v1`.

### 3.1 Header (Fixed 64 bytes)
```c
struct HVMRecordHeader {
    char magic[8];             // "HVMREC01"
    uint32_t schemaVersion;    // 1
    uint32_t hostPageSize;     // 4096 or 16384
    uint64_t initialRNGSeed;   // Seed for arc4random/rand
    uint64_t sessionStartTime; // Virtual epoch in microseconds
    uint8_t  appSha256[32];    // Exact SHA-256 of the executed binary/IPA
};
```

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

### 3.3 Event Kinds
1. `EVENT_INPUT_TOUCH` (0x01): Touch ID, phase, normalized coordinates $(x, y)$.
2. `EVENT_TIME_TICK` (0x02): Advanced virtual time step (`mach_absolute_time`, monotonic tick).
3. `EVENT_RNG_VAL` (0x03): Entropy injection if unseeded dynamic entropy is requested.
4. `EVENT_FS_READ` (0x04): Data read from external/host file descriptors or directory listings.
5. `EVENT_THREAD_YIELD` (0x05): Cooperative scheduling barrier and quantum boundary.
6. `EVENT_FRAME_PRESENT` (0x06): Frame presentation boundary and framebuffer hash checkpoint.

---

## 4. Subsystem Interception Design

### 4.1 Virtual Monotonic Clock & Timer Wheel
- In record/replay mode, host calls to `mach_absolute_time()`, `clock_gettime(CLOCK_MONOTONIC)`, and `gettimeofday()` do not query the host kernel.
- Instead, time advances as a function of retired instructions:
  $$\Delta t_{\text{virtual}} = \frac{\Delta \text{guestInstructions}}{\text{ClockRateMIPS}}$$
- `NSRunLoop` and `CFRunLoopTimer` timers are inserted into an internal priority queue sorted by virtual tick. When the instruction count reaches the timer's trigger tick, the timer callback executes immediately before guest block dispatch.

### 4.2 Deterministic Cooperative Multithreading
- Multithreaded execution is the largest hurdle to bit-for-bit replay on modern multicore hosts.
- To eliminate host pthread scheduling races:
  - Guest threads are scheduled in a cooperative round-robin loop.
  - Each thread executes a fixed instruction quantum $Q$ (e.g., $Q = 100{,}000$ instructions) or until it encounters a blocking syscall / mutex lock.
  - Mutex lock acquisitions are recorded in sequence order. During replay, lock acquisitions are granted only in the recorded order, preventing inversion.

### 4.3 Input & Touch Dispatch
- During recording, touch events generated by UI interaction or automation scripts (`--tap`, `--drag`) are serialized with the active `instructionTick`.
- During replay, the guest engine checks the journal head before decoding each block. When `currentInstructionCount >= event.instructionTick`, the touch event is dispatched into `UIApplication`'s event queue.

---

## 5. Test Lab Verification Case (`TL-Replay`) & Acceptance Criteria

To satisfy the **T7 Done When** milestone:
1. **Fixture**: A new Test Lab case `TL-Replay` (`tests/ios/apps/testlab/replay_fixture.m`) designed to execute for 60 seconds:
   - Maintains an active `CADisplayLink` / frame animation loop.
   - Dispatches background calculation tasks to worker threads via `dispatch_async`.
   - Simulates periodic touch interactions (drags, buttons, gesture recognizers).
   - Utilizes `arc4random()`, `mach_absolute_time()`, and file persistence.
2. **Oracle & Verification Protocol**:
   ```bash
   # Run 1: Record 60-second interactive session
   hvm run TestLab.ipa --case TL-Replay --record session.hvmrec --run-for 60
   
   # Run 2: Replay run A from recording
   hvm run TestLab.ipa --case TL-Replay --replay session.hvmrec --out report_replay_a.json
   
   # Run 3: Replay run B from recording
   hvm run TestLab.ipa --case TL-Replay --replay session.hvmrec --out report_replay_b.json
   ```
3. **Acceptance Invariants**:
   - `report_replay_a.json` and `report_replay_b.json` must be identical.
   - Total retired guest instructions must match exactly: $\Delta \text{Instructions} = 0$.
   - Final memory heap checksum and frame hashes at second 60 must be byte-for-byte identical.

---

## 6. Staged Implementation Roadmap

- **Phase 1 (This Document)**: Architecture, schema specification, determinism audit, and peer review.
- **Phase 2 (Virtual Time & Input Journaling)**: Implement `hvmrec/v1` serializer, decoupled virtual clock, and single-threaded touch injection in `hvm`.
- **Phase 3 (Deterministic Scheduler)**: Implement cooperative thread quantum slicing to guarantee identical thread interleave.
- **Phase 4 (Test Lab Integration & CI Gate)**: Land `TL-Replay` in `tools/hvm_testlab.py` and enforce deterministic replay in CI.
