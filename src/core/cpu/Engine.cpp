#include "Engine.hpp"
#include "Decoder.hpp"
#include <chrono>
#include <csetjmp>
#include <csignal>
#include <cstring>
#include <cerrno>
#include <unistd.h>   // write() in the host-fault diagnostics

namespace Hydrogen::CPU {

// -------------------------------------------------------------- IR hooks ---

Block* hvmEnterBlock(Cpu* c, uint32_t key) { return c->engine->enterBlock(c, key); }
void   hvmCallHle(Cpu* c, uint32_t index)  { c->engine->callHle(c, index); }

#ifdef HVM_PROFILE_PAIRS
}  // namespace Hydrogen::CPU
#include <cstdio>
#include <unordered_map>
namespace Hydrogen::CPU {
namespace {
struct PairProfile {
    std::unordered_map<uint64_t, uint64_t> n;   // key: (a low 32 bits of fn ptr offsets)
    std::unordered_map<uintptr_t, uintptr_t> ids;
    ~PairProfile() {
        FILE* f = std::fopen(getenv("HVM_PAIRPROF") ? getenv("HVM_PAIRPROF") : "pairprof.txt", "w");
        if (!f) return;
        for (auto& [k, v] : n) std::fprintf(f, "%llx %llx %llu\n", (unsigned long long)(k >> 32), (unsigned long long)(k & 0xFFFFFFFF), (unsigned long long)v);
        std::fclose(f);
    }
};
PairProfile g_pairs;
uintptr_t g_base = reinterpret_cast<uintptr_t>(&hvmEnterBlock);
}
void hvmProfilePair(Handler a, Handler b) {
    const uint64_t ka = uint32_t(reinterpret_cast<uintptr_t>(a) - g_base + 0x80000000u);
    const uint64_t kb = uint32_t(reinterpret_cast<uintptr_t>(b) - g_base + 0x80000000u);
    ++g_pairs.n[(ka << 32) | kb];
}
#endif

// ----------------------------------------------------------- fault guard ---
//
// Guest loads/stores are plain host memory accesses into the arena. An access
// to an uncommitted page raises SIGSEGV/SIGBUS; if the faulting address is in
// the arena of the thread's running engine we unwind back to Engine::run with
// siglongjmp. All IR handlers are tail calls, so there are no C++ frames with
// destructors between the fault and run() to skip.


namespace {

struct FaultScope {
    sigjmp_buf jb;
    uintptr_t arenaBase;
    uint32_t addr;
};
thread_local FaultScope* t_scope = nullptr;

struct sigaction g_oldSegv, g_oldBus;
std::once_flag g_installOnce;

// ------------------------------------ preallocated thread & log tracking ---

constexpr size_t kMaxCpus = 128;
static HostFaultThreadState g_threads[kMaxCpus];
static std::atomic<uint64_t> g_threadGeneration{0};
static std::atomic<uint32_t> g_untrackedCpus{0};
constexpr size_t kMaxArenas = 32;
static std::atomic<uintptr_t> g_arenas[kMaxArenas]{};
static_assert(std::atomic<uintptr_t>::is_always_lock_free);
static_assert(std::atomic<uint64_t>::is_always_lock_free);
static_assert(std::atomic<uint32_t>::is_always_lock_free);
static_assert(std::atomic<bool>::is_always_lock_free);
static_assert(std::atomic<int>::is_always_lock_free);

void registerCpu(Cpu* c) {
    for (size_t i = 0; i < kMaxCpus; ++i) {
        bool expected = false;
        auto& s = g_threads[i];
        if (s.claimed.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) {
            s.active.store(false, std::memory_order_relaxed);
            s.generation.store(g_threadGeneration.fetch_add(1) + 1, std::memory_order_relaxed);
            c->fault_state = &s;
            return;
        }
    }
    g_untrackedCpus.fetch_add(1, std::memory_order_relaxed);
}

void unregisterCpu(Cpu* c) {
    if (!c->fault_state) { g_untrackedCpus.fetch_sub(1, std::memory_order_relaxed); return; }
    c->fault_state->active.store(false, std::memory_order_release);
    c->fault_state->claimed.store(false, std::memory_order_release);
}

constexpr size_t kLogRingCap = 200;
constexpr size_t kLogLineCap = 256;

struct LogRingBuffer {
    struct Entry {
        std::atomic_flag busy = ATOMIC_FLAG_INIT;
        char line[kLogLineCap]{};
        size_t length = 0;
        uint64_t sequence = 0;
        bool valid = false;
    } entries[kLogRingCap];
    std::atomic<uint64_t> writeIdx{0};
};

static LogRingBuffer g_logRing;

void recordLogLineAsyncSafeImpl(const char* data, size_t len) {
    const uint64_t idx = g_logRing.writeIdx.fetch_add(1, std::memory_order_relaxed);
    auto& entry = g_logRing.entries[idx % kLogRingCap];
    // Both writers and the signal reader try once. A signal interrupting a
    // writer must never wait for that same writer to resume.
    if (entry.busy.test_and_set(std::memory_order_acquire)) return;
    if (entry.valid && entry.sequence > idx) { entry.busy.clear(std::memory_order_release); return; }
    const size_t copyLen = len < (kLogLineCap - 1) ? len : (kLogLineCap - 1);
    for (size_t i = 0; i < copyLen; ++i) entry.line[i] = data[i];
    entry.length = copyLen;
    entry.sequence = idx;
    entry.valid = true;
    entry.busy.clear(std::memory_order_release);
}

// Async-signal-safe I/O primitives using raw write(STDERR_FILENO).
static std::atomic<int> g_faultFd{STDERR_FILENO};
static void safeWrite(const char* str, size_t len) {
    const int fd = g_faultFd.load(std::memory_order_relaxed);
    while (len > 0) {
        ssize_t r = ::write(fd, str, len);
        if (r < 0 && errno == EINTR) continue;
        if (r <= 0) break;
        str += r;
        len -= r;
    }
}

static void safeWriteStr(const char* str) {
    if (!str) return;
    size_t len = 0;
    while (str[len]) ++len;
    safeWrite(str, len);
}

static void safeWriteHex(uint64_t val, int width = 8) {
    static const char hexChars[] = "0123456789abcdef";
    char buf[32];
    int pos = sizeof(buf);
    buf[--pos] = '\0';
    if (val == 0) {
        buf[--pos] = '0';
    } else {
        while (val > 0 && pos > 0) {
            buf[--pos] = hexChars[val & 0xF];
            val >>= 4;
        }
    }
    while ((sizeof(buf) - 1 - pos) < (size_t)width && pos > 0) {
        buf[--pos] = '0';
    }
    safeWriteStr(buf + pos);
}

static void safeWriteDec(uint64_t val) {
    char buf[32];
    int pos = sizeof(buf);
    buf[--pos] = '\0';
    if (val == 0) {
        buf[--pos] = '0';
    } else {
        while (val > 0 && pos > 0) {
            buf[--pos] = '0' + (val % 10);
            val /= 10;
        }
    }
    safeWriteStr(buf + pos);
}

static std::atomic<bool> g_faultDiagnosticActive{false};

void faultHandler(int sig, siginfo_t* si, void* uctx) {
    FaultScope* s = t_scope;
    const uintptr_t address = reinterpret_cast<uintptr_t>(si ? si->si_addr : nullptr);
    if (s && address >= s->arenaBase && uint64_t(address - s->arenaBase) < Memory::GuestArena::kArenaSize) {
        s->addr = uint32_t(address - s->arenaBase);
        siglongjmp(s->jb, 1);
    }

    // Host SIGSEGV/SIGBUS fault: print async-signal-safe diagnostic report.
    if (!g_faultDiagnosticActive.exchange(true, std::memory_order_acq_rel)) {
        safeWriteStr("\n=== HVM HOST FAULT DIAGNOSTICS ===\nSignal: ");
        safeWriteStr(sig == SIGSEGV ? "SIGSEGV (Segmentation Fault)" : sig == SIGBUS ? "SIGBUS (Bus Error)" : "OTHER");
        safeWriteStr("\nHost Fault Address (si_addr): 0x");
        safeWriteHex(reinterpret_cast<uintptr_t>(si ? si->si_addr : nullptr), 16);

        bool anyArena = false, inside = false;
        for (auto& item : g_arenas) {
            const uintptr_t base = item.load(std::memory_order_acquire);
            if (!base) continue;
            anyArena = true;
            safeWriteStr("\nGuest Arena Base: 0x");
            safeWriteHex(base, 16);
            if (address >= base && uint64_t(address - base) < Memory::GuestArena::kArenaSize) {
                safeWriteStr("\nGuest Arena Offset: 0x");
                safeWriteHex(address - base, 8);
                inside = true;
            }
        }
        if (!anyArena) safeWriteStr("\nGuest arena unavailable (no registered engine)");
        else if (!inside) safeWriteStr("\nFault address outside registered guest arenas");
        safeWriteStr("\n\n--- Guest Threads State ---\nRegisters sampled at block entry/HLE boundary; samples may differ in time.\n");

        for (size_t i = 0; i < kMaxCpus; ++i) {
            auto& state = g_threads[i];
            if (!state.active.load(std::memory_order_acquire)) continue;
            const auto generation = state.generation.load(std::memory_order_acquire);
            const auto pc = state.pc.load(std::memory_order_relaxed);
            const auto lr = state.lr.load(std::memory_order_relaxed);
            const auto block = state.block.load(std::memory_order_relaxed);
            const auto arena = state.arena.load(std::memory_order_relaxed);
            const bool inGuest = state.inGuest.load(std::memory_order_relaxed);
            char name[sizeof state.hleName]{};
            uint32_t hleIndex = 0xFFFFFFFFu;
            bool haveName = false;
            if (!state.nameBusy.test_and_set(std::memory_order_acquire)) {
                if (state.hleGeneration == generation) {
                    for (size_t j = 0; j < sizeof name; ++j) name[j] = state.hleName[j];
                    hleIndex = state.hleIndex;
                    haveName = true;
                }
                state.nameBusy.clear(std::memory_order_release);
            }
            if (!state.active.load(std::memory_order_acquire) ||
                state.generation.load(std::memory_order_acquire) != generation) continue;
            safeWriteStr("Thread slot #");
            safeWriteDec(i);
            safeWriteStr(" generation=");
            safeWriteDec(generation);
            safeWriteStr(": PC=0x");
            safeWriteHex(pc, 8);
            safeWriteStr(" LR=0x");
            safeWriteHex(lr, 8);
            safeWriteStr(" block_pc=0x");
            safeWriteHex(block, 8);
            safeWriteStr(" arena=0x");
            safeWriteHex(arena, 16);
            safeWriteStr(" in_guest=");
            safeWriteStr(inGuest ? "true" : "false");
            if (haveName) {
                safeWriteStr(" current_hle=0x");
                safeWriteHex(hleIndex, 8);
                safeWriteStr(" (");
                safeWriteStr(name);
                safeWriteStr(")");
            } else safeWriteStr(" current_hle=unavailable (publication interrupted)");
            safeWriteStr("\n");
        }
        safeWriteStr("Untracked CPUs (registry capacity 128): ");
        safeWriteDec(g_untrackedCpus.load(std::memory_order_relaxed));
        safeWriteStr("\n");

        safeWriteStr("\n--- Recent Logs (last 200) ---\n");
        const uint64_t totalWritten = g_logRing.writeIdx.load(std::memory_order_relaxed);
        const uint64_t count = totalWritten < kLogRingCap ? totalWritten : kLogRingCap;
        const uint64_t startIdx = totalWritten > count ? (totalWritten - count) : 0;
        uint64_t unavailable = 0;
        for (uint64_t i = startIdx; i < totalWritten; ++i) {
            auto& entry = g_logRing.entries[i % kLogRingCap];
            if (entry.busy.test_and_set(std::memory_order_acquire)) { ++unavailable; continue; }
            if (entry.valid && entry.sequence == i) {
                safeWrite(entry.line, entry.length);
                safeWriteStr("\n");
            } else ++unavailable;
            entry.busy.clear(std::memory_order_release);
        }
        safeWriteStr("Unavailable log entries (concurrent publication): ");
        safeWriteDec(unavailable);
        safeWriteStr("\n");
        safeWriteStr("=== END HOST FAULT DIAGNOSTICS ===\n\n");
    }

    // Not ours: chain to the previous handler (or default).
    struct sigaction& old = sig == SIGBUS ? g_oldBus : g_oldSegv;
    if (old.sa_flags & SA_SIGINFO) { if (old.sa_sigaction) { old.sa_sigaction(sig, si, uctx); return; } }
    else if (old.sa_handler != SIG_DFL && old.sa_handler != SIG_IGN) { old.sa_handler(sig); return; }
    signal(sig, SIG_DFL);
    raise(sig);
}

void installFaultHandler() {
    std::call_once(g_installOnce, [] {
        struct sigaction sa;
        std::memset(&sa, 0, sizeof sa);
        sa.sa_sigaction = faultHandler;
        sa.sa_flags = SA_SIGINFO | SA_ONSTACK | SA_NODEFER;
        sigemptyset(&sa.sa_mask);
        sigaction(SIGSEGV, &sa, &g_oldSegv);
        sigaction(SIGBUS, &sa, &g_oldBus);
    });
}

} // namespace

Cpu::Cpu() { registerCpu(this); }
Cpu::~Cpu() { unregisterCpu(this); delete call_ring.load(std::memory_order_relaxed); }

void Engine::recordLogLineAsyncSafe(const char* data, size_t len) {
    recordLogLineAsyncSafeImpl(data, len);
}
void Engine::setFaultReportFd(int fd) { g_faultFd.store(fd >= 0 ? fd : STDERR_FILENO, std::memory_order_relaxed); }

// ---------------------------------------------------------------- engine ---

Engine::Engine(Memory::GuestArena& mem) : mem_(mem), hle_(kMaxHle) {
    installFaultHandler();
    returnStub_ = registerHle("__hvm_return_to_host", [](Cpu& c) { c.exit = Exit::Return; });
    for (size_t i = 0; i < kMaxArenas; ++i) {
        uintptr_t expected = 0;
        if (g_arenas[i].compare_exchange_strong(expected, reinterpret_cast<uintptr_t>(mem_.base()), std::memory_order_release)) {
            faultArenaSlot_ = i;
            break;
        }
    }
}
Engine::~Engine() {
    if (faultArenaSlot_ < kMaxArenas) g_arenas[faultArenaSlot_].store(0, std::memory_order_release);
}

void Engine::recordHleState(Cpu& c, uint32_t index) {
    auto* state = c.fault_state;
    if (!state) return;
    c.recordFaultRegisters(c.r[kPC]);
    state->inGuest.store(false, std::memory_order_relaxed);
    if (state->nameBusy.test_and_set(std::memory_order_acquire)) return;
    const char* name = index < hleCount_.load(std::memory_order_acquire) ? hle_[index].name.c_str() : "none";
    size_t n = 0;
    while (n + 1 < sizeof state->hleName && name[n]) { state->hleName[n] = name[n]; ++n; }
    state->hleName[n] = '\0';
    state->hleIndex = index;
    state->hleGeneration = state->generation.load(std::memory_order_relaxed);
    state->nameBusy.clear(std::memory_order_release);
}

uint32_t Engine::registerHle(std::string name, HleFn fn) {
    std::lock_guard<std::mutex> lk(mu_);
    const uint32_t idx = hleCount_.load(std::memory_order_relaxed);
    if (idx >= kMaxHle) throw std::runtime_error("HydrogenVM: HLE table full");
    hle_[idx] = {std::move(name), std::move(fn)};
    hleCount_.store(idx + 1, std::memory_order_release);
    return Memory::GuestArena::kHleBase + idx * 4;
}

namespace {
thread_local std::string t_hostError;
thread_local FaultSnapshot t_fault;
}
FaultSnapshot& Engine::lastFault() { return t_fault; }
void Engine::captureFault(const Cpu& c, Exit e) {
    if (t_fault.valid) return;
    t_fault.valid = true;
    std::memcpy(t_fault.r, c.r, sizeof t_fault.r);
    t_fault.thumb = c.thumb;
    t_fault.cpsr = c.cpsr();
    t_fault.exit = e;
    t_fault.info = c.exit_info;
}
const std::string& Engine::lastHostError() { return t_hostError; }
void Engine::setLastHostError(std::string s) { t_hostError = std::move(s); }

void Engine::callHle(Cpu* c, uint32_t index) {
    Diag::ThreadCallRing* ring = c->call_ring.load(std::memory_order_relaxed);
    if (__builtin_expect(!ring, 0)) {
        ring = new Diag::ThreadCallRing();
        c->call_ring.store(ring, std::memory_order_release);
    }
    ring->push(Diag::CallKind::Hle, index);
    c->in_guest.store(false, std::memory_order_release);
    const uint32_t outerHle = c->current_hle.exchange(index, std::memory_order_relaxed);
    recordHleState(*c, index);
    struct Reenter {
        Engine* e; Cpu* c; uint32_t outer;
        ~Reenter() { c->current_hle.store(outer, std::memory_order_relaxed); e->recordHleState(*c, outer); e->enterGuest(*c); }
    } reenter{this, c, outerHle};
    try {
        hle_[index].fn(*c);
    } catch (const GuestFault& f) {
        // A nested host→guest call failed: propagate the original exit.
        c->exit = f.exit;
        c->exit_info = f.info;
        c->r[kPC] = f.pc;
        c->thumb = f.thumb;
        if (f.exit == Exit::HostError) t_hostError = f.what();
    } catch (const std::exception& e) {
        c->exit = Exit::HostError;
        c->exit_info = index;
        t_hostError = hle_[index].name + ": " + e.what();
    }
}


Block* Engine::enterBlock(Cpu* c, uint32_t key) {
    const uint64_t g = gen_.load(std::memory_order_acquire);
    if (__builtin_expect(c->fast_gen != g, 0)) {
        if (!c->fast) c->fast.reset(new FastEntry[kFastSize]);
        for (size_t i = 0; i < kFastSize; ++i) c->fast[i] = {0xFFFFFFFFu, nullptr, nullptr};
        c->fast_gen = g;
    }
    FastEntry& fe = c->fast[fastIndex(key)];
    if (fe.key == key) return fe.block;
    Block* b = lookupOrDecode(key);
    fe.key = key; fe.block = b; fe.first = b ? b->ops.data() : nullptr;
    return b;
}

Block* Engine::lookupOrDecode(uint32_t key) {
    // HLE addresses are the same entry regardless of the interworking bit.
    if (isHleAddress(key & ~1u)) key &= ~1u;
    std::lock_guard<std::mutex> lk(mu_);
    auto it = blocks_.find(key);
    if (it != blocks_.end()) return it->second.get();
    auto b = decodeBlock(key);
    Block* raw = b.get();
    ++stats_.blocks_decoded;
    stats_.guest_insns_decoded += raw->guest_insns;
    stats_.ops_emitted += raw->ops.size();
    blocks_.emplace(key, std::move(b));
    return raw;
}

void Engine::flushCache() {
    std::lock_guard<std::mutex> lk(mu_);
    blocks_.clear();
    gen_.fetch_add(1, std::memory_order_acq_rel);
}

void Engine::invalidateRange(uint32_t addr, uint32_t size) {
    // Blocks are linked to each other by raw pointer, so dropping any block
    // means dropping every link into it. Simplest correct approach: flush all if
    // anything overlaps. (Self-modifying code is rare on iOS.)
    bool hit = false;
    {
        std::lock_guard<std::mutex> lk(mu_);
        const uint64_t lo = addr, hi = uint64_t(addr) + size;
        for (auto& [k, b] : blocks_) {
            const uint64_t bl = k & ~1u, bh = b->end;
            if (bl < hi && bh > lo) { hit = true; break; }
        }
    }
    if (hit) flushCache();
}

std::unique_ptr<Block> Engine::decodeBlock(uint32_t key) {
    auto b = std::make_unique<Block>();
    b->key = key;
    const uint32_t pc = key & ~1u;
    if (isHleAddress(pc)) {
        BlockBuilder bb(*b, pc, false);
        bb.beginInsn(pc);
        bb.emit(&H_Hle, 0, 0, 0, 0, (pc - Memory::GuestArena::kHleBase) / 4);
        bb.exitIndirect(kTmp3, true);
        bb.finish(pc + 4, false);
        return b;
    }
    if (key & 1) decodeThumbBlock(*b, pc);
    else decodeArmBlock(*b, pc & ~3u);
    return b;
}

void Engine::decodeArmBlock(Block& b, uint32_t pc) {
    BlockBuilder bb(b, pc, false);
    for (;;) {
        bb.beginInsn(pc);
        if (!mem_.isMapped(pc)) {
            bb.exitSimple(&H_FetchFault);
            bb.finish(pc, false);
            return;
        }
        const uint32_t insn = mem_.read<uint32_t>(pc);
        const DecodeResult r = decodeArm(bb, insn);
        pc += 4;
        if (r == DecodeResult::End) { bb.finish(pc, false); return; }
        // A conditional exit (B<c>, BX<c> LR, LDM<c> {..pc}, …) is a side exit:
        // when it isn't taken execution simply continues in this block.
        if (bb.insns() >= BlockBuilder::kMaxInsns || (pc & (Memory::GuestArena::kGuestPageSize - 1)) == 0) {
            // Ending blocks at guest page boundaries keeps invalidation simple.
            bb.finish(pc, true);
            return;
        }
    }
}

void Engine::decodeThumbBlock(Block& b, uint32_t pc) {
    BlockBuilder bb(b, pc, true);
    uint32_t itstate = 0;   // ITSTATE[7:0]: firstcond[3:1]:mask... as in the ARM ARM
    for (;;) {
        bb.beginInsn(pc);
        if (!mem_.isMapped(pc)) {
            bb.exitSimple(&H_FetchFault);
            bb.finish(pc, false);
            return;
        }
        const uint16_t hw1 = mem_.read<uint16_t>(pc);
        uint16_t hw2 = 0;
        if (thumbIs32(hw1)) {
            if (!mem_.isMapped(pc + 2)) {
                bb.exitSimple(&H_FetchFault);
                bb.finish(pc, false);
                return;
            }
            hw2 = mem_.read<uint16_t>(pc + 2);
        }
        uint32_t len = 2;
        DecodeResult r;
        const bool inIt = (itstate & 0xF) != 0;
        const uint32_t cond = inIt ? (itstate >> 4) : 14;

        if (!inIt && (hw1 & 0xFF00) == 0xBF00 && (hw1 & 0xF) != 0) {
            // IT <firstcond>, <mask>: consumed here; conditions are applied
            // statically to the following instructions.
            itstate = hw1 & 0xFF;
            r = DecodeResult::Continue;
        } else {
            r = decodeThumb(bb, hw1, hw2, cond, inIt, len);
            if (inIt) {
                if ((itstate & 7) == 0) itstate = 0;
                else itstate = (itstate & 0xE0) | ((itstate << 1) & 0x1F);
            }
        }
        pc += len;
        if (r == DecodeResult::End && (itstate & 0xF) == 0) { bb.finish(pc, false); return; }
        // Conditional exits (B<c>, CBZ/CBNZ, or the last instruction of an IT
        // block such as BXEQ LR / POPNE {pc}) are side exits: the not-taken
        // path keeps decoding into this block.
        const bool sideExit = r == DecodeResult::EndNeedsFallthrough && (itstate & 0xF) == 0;
        if (r != DecodeResult::Continue && !sideExit) {
            // A branch / PC write inside an IT block must be its last
            // instruction; anything else is UNPREDICTABLE. Either way, stop here.
            bb.finish(pc, r == DecodeResult::EndNeedsFallthrough || (itstate & 0xF) != 0);
            return;
        }
        if ((itstate & 0xF) == 0 &&
            (bb.insns() >= BlockBuilder::kMaxInsns || (pc & (Memory::GuestArena::kGuestPageSize - 1)) == 0 ||
             ((pc + 2) & (Memory::GuestArena::kGuestPageSize - 1)) == 0)) {
            bb.finish(pc, true);
            return;
        }
    }
}

// --------------------------------------------------------------- run loop ---

Exit Engine::run(Cpu& cpu, int64_t budget) {
    cpu.engine = this;
    if (cpu.fault_state) {
        cpu.fault_state->arena.store(reinterpret_cast<uintptr_t>(mem_.base()), std::memory_order_relaxed);
        recordHleState(cpu, cpu.current_hle.load(std::memory_order_relaxed));
        cpu.fault_state->active.store(true, std::memory_order_release);
    }
    cpu.budget = budget;
    const int64_t startBudget = budget;
    cpu.exit = Exit::None;
    cpu.exit_info = 0;

    FaultScope scope;
    scope.arenaBase = reinterpret_cast<uintptr_t>(mem_.base());
    scope.addr = 0;
    FaultScope* prev = t_scope;
    // Restore on C++ exceptions too: a subsequent host fault must not follow
    // a scope pointer into the abandoned Engine::run stack frame.
    struct RestoreScope { FaultScope* prev; ~RestoreScope() { t_scope = prev; } } restore{prev};
    t_scope = &scope;
    if (sigsetjmp(scope.jb, 1)) {
        t_scope = prev;
        cpu.exit = Exit::MemFault;
        cpu.exit_info = scope.addr;
        cpu.r[kPC] = cpu.block_pc;   // precise PC within the block isn't tracked
        cpu.insn_count += uint64_t(startBudget - cpu.budget);
        cpu.in_guest.store(false, std::memory_order_release);
        cpu.recordFaultRegisters(cpu.r[kPC]);
        if (cpu.fault_state) cpu.fault_state->inGuest.store(false, std::memory_order_relaxed);
        return cpu.exit;
    }
    uint8_t* const base = mem_.base();
    enterGuest(cpu);
    while (cpu.exit == Exit::None) {
        const uint32_t key = (cpu.r[kPC] & (cpu.thumb ? ~1u : ~3u)) | (cpu.thumb & 1);
        Block* b = enterBlock(&cpu, key);
        if (!b) break;
        cpu.block_pc = key & ~1u;
        cpu.recordFaultRegisters(cpu.block_pc);
        const Op* first = b->ops.data();
        first->fn(&cpu, first, base);
    }
    t_scope = prev;
    cpu.in_guest.store(false, std::memory_order_release);
    // Handlers only decrement the budget; the retired count falls out of it.
    cpu.insn_count += uint64_t(startBudget - cpu.budget);
    cpu.recordFaultRegisters(cpu.r[kPC]);
    if (cpu.fault_state) cpu.fault_state->inGuest.store(false, std::memory_order_relaxed);
    return cpu.exit;
}

void Engine::park(Cpu& c) {
    c.in_guest.store(false, std::memory_order_seq_cst);
    c.recordFaultRegisters(c.r[kPC]);
    if (c.fault_state) c.fault_state->inGuest.store(false, std::memory_order_relaxed);
    std::unique_lock<std::mutex> lk(suspendMu_);
    suspendCv_.notify_all();
    suspendCv_.wait(lk, [&] { return c.suspend_count.load() == 0 || stopRequested(); });
    lk.unlock();
    c.in_guest.store(true, std::memory_order_seq_cst);
    if (c.fault_state) c.fault_state->inGuest.store(true, std::memory_order_relaxed);
}

void Engine::suspend(Cpu& t) {
    t.suspend_count.fetch_add(1, std::memory_order_seq_cst);
    // Wait until it is out of guest code. Shrinking its budget makes a running
    // thread reach the end of its time slice (a safepoint) promptly.
    std::unique_lock<std::mutex> lk(suspendMu_);
    while (t.in_guest.load(std::memory_order_seq_cst) && !stopRequested()) {
        t.budget = 0;
        suspendCv_.wait_for(lk, std::chrono::microseconds(200));
    }
}

void Engine::resume(Cpu& t) {
    {
        std::lock_guard<std::mutex> lk(suspendMu_);
        uint32_t n = t.suspend_count.load();
        while (n && !t.suspend_count.compare_exchange_weak(n, n - 1)) {}
    }
    suspendCv_.notify_all();
}

// Debugging: HVM_TRACE_BLOCKS=lo-hi (hex) logs every block entered in that range
// with r0-r3 (single-steps blocks, so it is slow).
static bool traceRange(uint32_t& lo, uint32_t& hi) {
    static const char* e = std::getenv("HVM_TRACE_BLOCKS");
    static uint32_t l = 0, h = 0;
    static bool parsed = false;
    if (e && !parsed) { std::sscanf(e, "%x-%x", &l, &h); parsed = true; }
    lo = l; hi = h;
    return e != nullptr;
}

Exit Engine::runToExit(Cpu& cpu) {
    uint32_t lo, hi;
    if (traceRange(lo, hi)) {
        for (;;) {
            if (stopRequested()) { cpu.exit = Exit::Halt; return Exit::Halt; }
            const uint32_t pc = cpu.r[kPC];
            if (pc >= lo && pc < hi)
                std::fprintf(stderr, "[blk] %08x lr=%08x r0=%08x r1=%08x r2=%08x r3=%08x r4=%08x r5=%08x r6=%08x\n", pc | (cpu.thumb & 1), cpu.r[14],
                             cpu.r[0], cpu.r[1], cpu.r[2], cpu.r[3], cpu.r[4], cpu.r[5], cpu.r[6]);
            const Exit e = run(cpu, 1);
            if (e == Exit::MemFault && faultHook_ && faultHook_(cpu, e)) continue;
            if (e != Exit::Budget) return e;
        }
    }
    for (;;) {
        if (stopRequested()) { cpu.exit = Exit::Halt; return Exit::Halt; }
        const Exit e = run(cpu, kSlice);
        if (e == Exit::MemFault && faultHook_ && faultHook_(cpu, e)) continue;
        if (e != Exit::Budget) return e;
    }
}

uint64_t Engine::callGuest(Cpu& c, uint32_t fn, std::initializer_list<uint32_t> args) {
    return callGuest(c, fn, args.begin(), args.size());
}

uint64_t Engine::callGuest(Cpu& c, uint32_t fn, const uint32_t* args, size_t nargs) {
    // Save everything the interrupted context can observe.
    uint32_t saved[kNumRegs];
    std::memcpy(saved, c.r, sizeof saved);
    const uint32_t nzcv = c.nzcv, q = c.q, ge = c.ge, thumb = c.thumb, blockPc = c.block_pc;
    const int64_t budget = c.budget;
    const Exit exitSaved = c.exit;
    const uint32_t infoSaved = c.exit_info;
    const uint32_t boundarySaved = c.call_boundary_sp;
    struct RestoreBoundary { Cpu& c; uint32_t v; ~RestoreBoundary() { c.call_boundary_sp = v; } } restoreBoundary{c, boundarySaved};

    uint32_t sp = c.r[kSP] & ~7u;
    c.call_boundary_sp = sp;
    if (nargs > 4) {
        sp = (sp - uint32_t(nargs - 4) * 4) & ~7u;
        for (size_t i = 4; i < nargs; ++i) mem_.write<uint32_t>(sp + uint32_t(i - 4) * 4, args[i]);
    }
    for (size_t i = 0; i < 4; ++i) c.r[i] = i < nargs ? args[i] : 0;
    c.r[kSP] = sp;
    c.r[kLR] = returnStub_;
    c.r[kPC] = fn & ~1u;
    c.thumb = fn & 1;

    const Exit e = runToExit(c);
    const uint64_t result = (uint64_t(c.r[1]) << 32) | c.r[0];
    if (e != Exit::Return) {
        captureFault(c, e);
        const uint32_t info = c.exit_info, pc = c.r[kPC], th = c.thumb;
        std::memcpy(c.r, saved, sizeof saved);
        c.nzcv = nzcv; c.q = q; c.ge = ge; c.thumb = thumb; c.block_pc = blockPc; c.budget = budget;
        c.exit = exitSaved; c.exit_info = infoSaved;
        throw GuestFault(e, info, pc, th, e == Exit::HostError ? t_hostError : "guest call did not return");
    }
    std::memcpy(c.r, saved, sizeof saved);
    c.nzcv = nzcv; c.q = q; c.ge = ge; c.thumb = thumb; c.block_pc = blockPc; c.budget = budget;
    c.exit = exitSaved; c.exit_info = infoSaved;
    return result;
}

} // namespace Hydrogen::CPU
