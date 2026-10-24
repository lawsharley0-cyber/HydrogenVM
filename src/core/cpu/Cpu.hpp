// HydrogenVM — guest CPU state and the threaded-IR data structures.
//
// Execution model (see docs/ARCHITECTURE.md for the reasoning):
//
//  * Guest code is decoded lazily, one basic block at a time, the first time
//    execution reaches it ("decode on first run"). Nothing is decoded ahead of
//    time, so literal pools / jump tables / ARM-vs-Thumb ambiguity are never a
//    problem: we only decode what the guest actually jumps to, in the mode it
//    jumps in.
//  * A block is a flat array of Ops. Each Op holds a pointer to a small C++
//    handler that does its work and then tail-calls (op+1)->fn with
//    [[clang::musttail]]. The handlers are ordinary, signed, ahead-of-time
//    compiled code; the Ops are plain data. No executable memory is ever
//    created, so this runs under iOS W^X without JIT entitlements.
//  * Blocks end with an exit Op that either chains straight into the next block
//    (cached pointer = "block linking") or returns to the dispatcher when the
//    time-slice budget runs out, so the guest can always be interrupted.
#pragma once
#include <atomic>
#include <cstdint>
#include <cstddef>
#include <memory>
#include <vector>

// preserve_none (no callee-saved registers) removes the push/pop that every
// handler otherwise pays. It needs LLVM 19 for AArch64; Apple clang numbers
// its releases differently (Apple clang 17 = LLVM 19).
#if defined(__apple_build_version__)
#  define HVM_LLVM_HAS_PRESERVE_NONE (__clang_major__ >= 17)
#elif defined(__clang__)
#  define HVM_LLVM_HAS_PRESERVE_NONE (__clang_major__ >= 19)
#else
#  define HVM_LLVM_HAS_PRESERVE_NONE 0
#endif
#if defined(__clang__) && __has_attribute(preserve_none) && (defined(__x86_64__) || defined(__aarch64__)) && HVM_LLVM_HAS_PRESERVE_NONE && !defined(HVM_NO_PRESERVE_NONE)
#  define HVM_HOT_ABI __attribute__((preserve_none))
#else
#  define HVM_HOT_ABI
#endif

#if defined(__clang__)
#  define HVM_MUSTTAIL [[clang::musttail]]
#else
#  define HVM_MUSTTAIL
#endif

namespace Hydrogen::CPU {

struct Cpu;
struct Op;
struct Block;
class Engine;

using Handler = void (HVM_HOT_ABI*)(Cpu* c, const Op* op, uint8_t* mem);

// Register file indices. 0-15 are the architectural registers; 16+ are
// scratch registers the decoder uses (e.g. to materialise PC-relative values).
enum : uint8_t {
    kSP = 13, kLR = 14, kPC = 15,
    kTmp0 = 16, kTmp1 = 17, kTmp2 = 18, kTmp3 = 19,
    kNumRegs = 20,
    kDTmp0 = 32,   // scratch D register (Q16 = D32:D33, Q17 = D34:D35)
};

// NZCV live in bits 31..28, exactly as in the APSR.
enum : uint32_t {
    kFlagN = 1u << 31, kFlagZ = 1u << 30, kFlagC = 1u << 29, kFlagV = 1u << 28,
    kFlagMaskNZCV = 0xF0000000u,
};

enum class Exit : uint32_t {
    None = 0,
    Budget,        // time slice used up; resume at r[15]
    Halt,          // a host (HLE) function asked to stop
    Svc,           // SVC #info executed; r[15] = next instruction
    Undefined,     // undecodable / unimplemented instruction at info
    Breakpoint,    // BKPT / UDF trap at info
    MemFault,      // host fault on guest address `info`
    Return,        // a host→guest call (Engine::callGuest) returned
    HostError,     // a host (HLE) function failed; Engine::lastHostError() has details
};

// Per-thread direct-mapped cache in front of the engine's block map. `first`
// is the block's first op, so indirect branches can jump without touching the Block.
struct FastEntry { uint32_t key; Block* block; const Op* first; };
inline constexpr size_t kFastSize = 1u << 14;
inline size_t fastIndex(uint32_t key) { return ((key >> 1) ^ (key >> 15)) & (kFastSize - 1); }

struct alignas(64) Cpu {
    Cpu();
    ~Cpu();
    uint32_t r[kNumRegs] = {};
    uint32_t nzcv  = 0;        // N Z C V in bits 31..28
    uint32_t q     = 0;        // sticky saturation flag (APSR.Q), 0/1
    uint32_t ge    = 0;        // APSR.GE in bits 19..16
    uint32_t thumb = 0;        // current instruction set state (T bit)

    // VFP / NEON register file. D0-D31 alias S0-S63 / Q0-Q15.
    // D32-D35 (Q16-Q17) are decoder scratch registers (e.g. broadcast scalars).
    alignas(16) uint64_t d[36] = {};
    uint32_t fpscr = 0;

    // User-visible thread ID registers (CP15 c13). 32-bit iOS keeps the
    // thread's TSD pointer in TPIDRURO and reads it with MRC p15,0,Rt,c13,c0,3.
    uint32_t tpidrurw = 0;
    uint32_t tpidruro = 0;

    // Exclusive monitor (LDREX/STREX). Implemented as compare-and-swap against
    // the value observed by LDREX, which stays correct when guest threads run
    // on real host threads.
    uint32_t excl_addr  = 0;
    uint64_t excl_value = 0;
    uint8_t  excl_size  = 0;   // 0 = monitor open/clear

    // Dispatcher bookkeeping.
    int64_t  budget     = 0;   // guest instructions left in this time slice
    Exit     exit       = Exit::None;
    uint32_t exit_info  = 0;
    uint32_t block_pc   = 0;   // start of the block currently executing (fault reports)
    uint64_t insn_count = 0;   // retired guest instructions (approximate, per block; updated when run() returns)
    Engine*  engine     = nullptr;
    void*    user       = nullptr;  // owner (guest thread object, test harness, ...)

    // Block lookup cache, private to the host thread running this Cpu, so guest
    // threads can run on real host threads without locking on every branch.
    std::unique_ptr<FastEntry[]> fast;
    uint64_t fast_gen = ~0ull;

    // Guest SP at the innermost host->guest call (Engine::callGuest). Guest
    // frames at or above it belong to code that host frames are waiting on,
    // so an exception unwinder must not land there.
    uint32_t call_boundary_sp = 0xFFFFFFFFu;

    // thread_suspend() support. `suspend_count` > 0 asks the thread to park at
    // its next safepoint (entering guest code: each time slice, and on return
    // from every host function). `in_guest` is false while the thread runs
    // host code or is parked, i.e. whenever its registers in r[] are a
    // complete, stable picture of its guest state.
    std::atomic<uint32_t> suspend_count{0};
    std::atomic<bool> in_guest{false};
    // Index of the host function this thread is in (outermost-latest), for the
    // guest sampling profiler. Not used for control flow.
    std::atomic<uint32_t> current_hle{0xFFFFFFFFu};

    uint32_t pc() const { return r[kPC]; }
    uint32_t cpsr() const { return nzcv | (q << 27) | ge | (thumb << 5) | 0x10 /*USR*/; }
    void setApsrFlags(uint32_t v) { nzcv = v & kFlagMaskNZCV; q = (v >> 27) & 1; ge = v & 0x000F0000u; }
};

// One IR instruction. 32 bytes, two per cache line.
struct alignas(32) Op {
    Handler  fn;
    uint8_t  a, b, c, d;      // operand fields (usually rd, rn, rm, rs/flags)
    uint32_t imm;
    uint32_t imm2;
    uint32_t pc;              // guest address of the source instruction
    mutable const Op* link;   // exit ops: first op of the cached successor block
};
static_assert(sizeof(Op) == 32, "Op should stay 32 bytes");

struct Block {
    uint32_t key = 0;          // guest start address | thumb bit
    uint32_t end = 0;          // guest address one past the last decoded instruction
    uint32_t guest_insns = 0;
    bool     valid = true;
    std::vector<Op> ops;
};

// Condition-code evaluation: kCondPass[cond] has bit `nzcv>>28` set if `cond` passes.
struct CondTable {
    uint16_t pass[16];
    constexpr CondTable() : pass{} {
        for (int c = 0; c < 16; ++c) {
            uint16_t m = 0;
            for (int f = 0; f < 16; ++f) {
                bool N = f & 8, Z = f & 4, C = f & 2, V = f & 1, r = false;
                switch (c >> 1) {
                    case 0: r = Z; break;
                    case 1: r = C; break;
                    case 2: r = N; break;
                    case 3: r = V; break;
                    case 4: r = C && !Z; break;
                    case 5: r = N == V; break;
                    case 6: r = !Z && (N == V); break;
                    case 7: r = true; break;
                }
                if ((c & 1) && c != 15) r = !r;
                if (r) m |= uint16_t(1u << f);
            }
            pass[c] = m;
        }
    }
};
inline constexpr CondTable kCond{};

inline bool condPasses(uint32_t cond, uint32_t nzcv) {
    return (kCond.pass[cond] >> (nzcv >> 28)) & 1;
}

// Which flags a condition reads (for dead-flag elimination).
inline constexpr uint8_t condReads(uint32_t cond) {
    switch (cond >> 1) {
        case 0: return 0x4;           // Z
        case 1: return 0x2;           // C
        case 2: return 0x8;           // N
        case 3: return 0x1;           // V
        case 4: return 0x6;           // C Z
        case 5: return 0x9;           // N V
        case 6: return 0xD;           // N Z V
        default: return 0;
    }
}

} // namespace Hydrogen::CPU
