// HydrogenVM — execution engine: block cache, dispatcher, HLE table,
// host→guest calls.
//
// Threading: any number of host threads may run guest code at once, each
// with its own Cpu. Decoding is serialised by a mutex; block lookups go through
// a per-Cpu cache, and block links are published with atomics. flushCache()
// and invalidateRange() must only be called while no guest code is running.
#pragma once
#include "Cpu.hpp"
#include "../memory/GuestArena.hpp"
#include <atomic>
#include <condition_variable>
#include <functional>
#include <initializer_list>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>

namespace Hydrogen::CPU {

// Thrown by Engine::callGuest when the called guest code doesn't return
// normally (fault, undefined instruction, stop request, …). Caught by the
// engine around every HLE call and turned into a Cpu exit, so it unwinds
// whole nested host/guest call chains back to the outermost run().
struct GuestFault : std::runtime_error {
    Exit     exit;
    uint32_t info;
    uint32_t pc;
    uint32_t thumb;
    GuestFault(Exit e, uint32_t i, uint32_t p, uint32_t t, const std::string& what)
        : std::runtime_error(what), exit(e), info(i), pc(p), thumb(t) {}
};

// Registers of the innermost guest frame that failed, captured before
// callGuest unwinds (so crash reports show where the guest really died).
struct FaultSnapshot {
    bool valid = false;
    uint32_t r[16] = {};
    uint32_t thumb = 0, cpsr = 0;
    Exit exit = Exit::None;
    uint32_t info = 0;
};

class Engine {
public:
    using HleFn = std::function<void(Cpu&)>;

    explicit Engine(Memory::GuestArena& mem);
    ~Engine();

    Memory::GuestArena& memory() { return mem_; }

    // Register a host function. Returns the guest address that, when branched
    // to (in either ARM or Thumb state), calls `fn` and then returns to LR.
    // `fn` may call hleJump() to continue somewhere other than LR, set
    // `cpu.exit` to stop, or throw (reported as Exit::HostError).
    uint32_t registerHle(std::string name, HleFn fn);
    const std::string& hleName(uint32_t index) const { return hle_[index].name; }
    const HleFn& hleFunction(uint32_t index) const { return hle_[index].fn; }
    bool isHleAddress(uint32_t addr) const {
        return addr >= Memory::GuestArena::kHleBase &&
               (addr - Memory::GuestArena::kHleBase) / 4 < hleCount_.load(std::memory_order_acquire);
    }
    uint32_t hleIndex(uint32_t addr) const { return (addr - Memory::GuestArena::kHleBase) / 4; }

    // From inside an HLE function: continue at `target` (bit0 = Thumb) instead
    // of returning to LR. Registers are passed through unchanged.
    static void hleJump(Cpu& c, uint32_t target) { c.r[kTmp3] = target; }

    // Run guest code on `cpu` starting at cpu.r[15] (cpu.thumb selects state)
    // until something exits or `budget` guest instructions have retired.
    Exit run(Cpu& cpu, int64_t budget = INT64_MAX);

    // Call a guest function from host code (typically from inside an HLE
    // function) on the same Cpu, AAPCS/iOS style: the first four 32-bit
    // arguments in r0-r3, the rest on the stack. Returns r0 | r1 << 32. The
    // Cpu's state is saved and restored around the call. Throws GuestFault if
    // the callee doesn't return normally.
    uint64_t callGuest(Cpu& cpu, uint32_t fn, std::initializer_list<uint32_t> args);
    uint64_t callGuest(Cpu& cpu, uint32_t fn, const uint32_t* args, size_t nargs);

    // Ask every Cpu running on this engine to stop at its next time-slice
    // boundary (Engine::run returns Exit::Halt; callGuest throws).
    void requestStop() { stop_.store(true, std::memory_order_relaxed); }
    bool stopRequested() const { return stop_.load(std::memory_order_relaxed); }
    // Time slice used by callGuest and by runToExit between stop checks.
    static constexpr int64_t kSlice = 2'000'000;

    // Thread suspension (thread_suspend / thread_resume). suspend() returns once
    // the target has stopped running guest code; it stays stopped until resume().
    void suspend(Cpu& target);
    void resume(Cpu& target);
    // Marks `c` as running guest code, parking first while it is suspended.
    void enterGuest(Cpu& c) {
        c.in_guest.store(true, std::memory_order_seq_cst);
        if (__builtin_expect(c.suspend_count.load(std::memory_order_seq_cst) != 0, 0)) park(c);
    }

    // Run until an exit other than Budget (checking for stop requests).
    Exit runToExit(Cpu& cpu);

    // Discard all decoded blocks. Must not be called while any Cpu is running.
    void flushCache();
    // Discard blocks overlapping [addr, addr+size). Same restriction.
    void invalidateRange(uint32_t addr, uint32_t size);

    // Called from IR handlers.
    Block* enterBlock(Cpu* c, uint32_t key);
    void   callHle(Cpu* c, uint32_t index);

    // Per host thread; Process clears it before running and reads it after a failure.
    static FaultSnapshot& lastFault();
    static void captureFault(const Cpu& c, Exit e);

    // Details for the most recent Exit::HostError (per host thread).
    static const std::string& lastHostError();
    static void setLastHostError(std::string s);

    static void recordLogLineAsyncSafe(const char* data, size_t len);

    struct Stats { uint64_t blocks_decoded = 0, guest_insns_decoded = 0, ops_emitted = 0; };
    Stats stats() const { std::lock_guard<std::mutex> lk(mu_); return stats_; }

private:
    void park(Cpu& c);
    std::mutex suspendMu_;
    std::condition_variable suspendCv_;
    Block* lookupOrDecode(uint32_t key);
    std::unique_ptr<Block> decodeBlock(uint32_t key);
    void decodeArmBlock(Block& b, uint32_t pc);
    void decodeThumbBlock(Block& b, uint32_t pc);

    static constexpr size_t kMaxHle = Memory::GuestArena::kHleSize / 4;

    Memory::GuestArena& mem_;
    std::unordered_map<uint32_t, std::unique_ptr<Block>> blocks_;
    std::atomic<uint64_t> gen_{0};       // bumped by flushCache; invalidates per-Cpu caches
    mutable std::mutex mu_;

    struct HleEntry { std::string name; HleFn fn; };
    std::vector<HleEntry> hle_;          // fixed capacity, never reallocated
    std::atomic<uint32_t> hleCount_{0};
    uint32_t returnStub_ = 0;            // LR used by callGuest
    std::atomic<bool> stop_{false};
    Stats stats_;
};

} // namespace Hydrogen::CPU
