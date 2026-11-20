#include "Engine.hpp"
#include "Decoder.hpp"
#include <algorithm>
#include <chrono>
#include <csetjmp>
#include <csignal>
#include <cstring>
#include <cerrno>
#include <unistd.h>   // write() in the host-fault diagnostics
#if defined(__APPLE__)
#include <sys/ucontext.h>   // <ucontext.h> needs _XOPEN_SOURCE on Apple
#include <mach/thread_status.h>
#else
#include <ucontext.h>
#endif

namespace Hydrogen::CPU {

// -------------------------------------------------------------- IR hooks ---

const Op* hvmEnterBlock(Cpu* c, uint32_t key) { return c->engine->enterBlock(c, key); }
void   hvmCallHle(Cpu* c, uint32_t index)  { c->engine->callHle(c, index); }
bool   hvmFastHle(Cpu* c, uint32_t index)  { return c->engine->tryFastHle(*c, index); }
void   hvmObserveEntry(Cpu* c, uint32_t address) { c->engine->notifyEntry(*c, address); }

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
    Cpu* cpu = nullptr;
    // Host general registers at the fault. The faulting handler holds its `const Op*` in one of
    // them; Engine::run picks the value that points at an op of the current block (precise PC).
    static constexpr int kMaxRegs = 32;
    uintptr_t regs[kMaxRegs];
    int nregs = 0;
    uintptr_t hostPc = 0;   // the faulting host instruction
};

// Copies the interrupted context's general registers (async-signal-safe: plain loads).
void captureRegisters(FaultScope* s, void* uctx) {
    s->nregs = 0;
    s->hostPc = 0;
    if (!uctx) return;
    auto* uc = static_cast<ucontext_t*>(uctx);
#if defined(__APPLE__) && defined(__aarch64__)
    // The accessor macros work whether or not the SDK makes fp/lr/pc opaque (arm64e).
    const auto& ss = uc->uc_mcontext->__ss;
    s->hostPc = uintptr_t(arm_thread_state64_get_pc(ss));
    for (int i = 0; i < 29; ++i) s->regs[s->nregs++] = uintptr_t(ss.__x[i]);
    s->regs[s->nregs++] = uintptr_t(arm_thread_state64_get_fp(ss));
    s->regs[s->nregs++] = uintptr_t(arm_thread_state64_get_lr(ss));
#elif defined(__APPLE__) && defined(__x86_64__)
    const auto& ss = uc->uc_mcontext->__ss;
    s->hostPc = uintptr_t(ss.__rip);
    for (uintptr_t v : {ss.__rax, ss.__rbx, ss.__rcx, ss.__rdx, ss.__rdi, ss.__rsi, ss.__rbp, ss.__r8, ss.__r9,
                        ss.__r10, ss.__r11, ss.__r12, ss.__r13, ss.__r14, ss.__r15})
        s->regs[s->nregs++] = v;
#elif defined(__linux__) && defined(__x86_64__)
    s->hostPc = uintptr_t(uc->uc_mcontext.gregs[REG_RIP]);
    for (int i = 0; i < NGREG && s->nregs < FaultScope::kMaxRegs; ++i)
        if (i != REG_RIP && i != REG_RSP && i != REG_EFL && i != REG_CSGSFS && i != REG_ERR && i != REG_TRAPNO &&
            i != REG_OLDMASK && i != REG_CR2)
            s->regs[s->nregs++] = uintptr_t(uc->uc_mcontext.gregs[i]);
#elif defined(__linux__) && defined(__aarch64__)
    s->hostPc = uintptr_t(uc->uc_mcontext.pc);
    for (int i = 0; i < 31 && s->nregs < FaultScope::kMaxRegs; ++i) s->regs[s->nregs++] = uintptr_t(uc->uc_mcontext.regs[i]);
#else
    (void)uc;
#endif
}
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
            s.cpu.store(c, std::memory_order_release);
            c->fault_state = &s;
            return;
        }
    }
    g_untrackedCpus.fetch_add(1, std::memory_order_relaxed);
}

void unregisterCpu(Cpu* c) {
    if (!c->fault_state) { g_untrackedCpus.fetch_sub(1, std::memory_order_relaxed); return; }
    c->fault_state->active.store(false, std::memory_order_release);
    c->fault_state->cpu.store(nullptr, std::memory_order_release);
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
    // Host code (a library function, the loader, another host thread) touching a guest page whose
    // guest permissions the host page enforces: lift that page and retry, as the kernel and dyld
    // may write read-only user pages. Guest loads and stores are never lifted.
    const bool guestCode = s && s->cpu && s->cpu->in_guest.load(std::memory_order_relaxed);
    if (!guestCode && Memory::GuestArena::liftHostFault(address)) return;
    if (s && address >= s->arenaBase && uint64_t(address - s->arenaBase) < Memory::GuestArena::kArenaSize) {
        s->addr = uint32_t(address - s->arenaBase);
        if (guestCode) captureRegisters(s, uctx); else s->nregs = 0;
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
            auto pc = state.pc.load(std::memory_order_relaxed);
            auto lr = state.lr.load(std::memory_order_relaxed);
            auto block = state.block.load(std::memory_order_relaxed);
            // Running guest code: the block it is in now and its LR, read from its CPU (the
            // samples above are from the last time slice or library call).
            if (const Cpu* cc = state.cpu.load(std::memory_order_acquire); cc && state.inGuest.load(std::memory_order_relaxed)) {
                block = __atomic_load_n(&cc->block_pc, __ATOMIC_RELAXED);
                lr = __atomic_load_n(&cc->r[kLR], __ATOMIC_RELAXED);
                pc = block;
            }
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

void Engine::recordHleState(Cpu& c, uint32_t index, bool sampleRegisters) {
    auto* state = c.fault_state;
    if (!state) return;
    if (sampleRegisters) c.recordFaultRegisters(c.r[kPC]);
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

void Engine::observeEntry(uint32_t address, EntryObserver observer) {
    std::lock_guard<std::mutex> lk(mu_);
    if (!blocks_.empty()) throw std::logic_error("Entry observers must be configured before execution");
    if (!observer || isHleAddress(address & ~1u)) throw std::invalid_argument("Entry observer requires guest code and a callback");
    entryObservers_[address & ~1u] = std::move(observer);
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
void Engine::notifyEntry(Cpu& cpu, uint32_t address) noexcept {
    try {
        auto found = entryObservers_.find(address);
        if (found != entryObservers_.end()) found->second(address, cpu);
    } catch (const std::exception& error) {
        cpu.exit = Exit::HostError;
        cpu.r[kPC] = address;
        t_hostError = std::string("Entry observer: ") + error.what();
    } catch (...) {
        cpu.exit = Exit::HostError;
        cpu.r[kPC] = address;
        t_hostError = "Entry observer: unknown exception";
    }
}

void Engine::callHle(Cpu* c, uint32_t index) {
    Diag::ThreadCallRing* ring = c->call_ring.load(std::memory_order_relaxed);
    if (__builtin_expect(!ring, 0)) {
        ring = new Diag::ThreadCallRing();
        c->call_ring.store(ring, std::memory_order_release);
    }
    ring->push(Diag::CallKind::Hle, index);
    const uint32_t outerHle = c->current_hle.exchange(index, std::memory_order_relaxed);
    // Sample registers while in_guest is still set: once it clears a suspender may rewrite c->r[].
    recordHleState(*c, index);
    c->in_guest.store(false, std::memory_order_release);
    const uint32_t outerArg0 = c->progress_argument0, outerArg1 = c->progress_argument1;
    c->progress_argument0 = c->r[0]; c->progress_argument1 = c->r[1];
    c->publishProgress(false);
    struct Reenter {
        Engine* e; Cpu* c; uint32_t outer, a0, a1;
        ~Reenter() { c->current_hle.store(outer, std::memory_order_relaxed); e->recordHleState(*c, outer);
            c->progress_argument0 = a0; c->progress_argument1 = a1; e->enterGuest(*c); }
    } reenter{this, c, outerHle, outerArg0, outerArg1};
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
    if (__builtin_expect(c->async_pending.load(std::memory_order_acquire) != 0, 0) && c->exit == Exit::None) {
        try {
            runSafepoint(*c);
        } catch (const GuestFault& f) {
            c->exit = f.exit;
            c->exit_info = f.info;
            c->r[kPC] = f.pc;
            c->thumb = f.thumb;
            if (f.exit == Exit::HostError) t_hostError = f.what();
        } catch (const std::exception& e) {
            c->exit = Exit::HostError;
            c->exit_info = index;
            t_hostError = std::string("signal handler: ") + e.what();
        }
    }
}


const Op* Engine::enterBlock(Cpu* c, uint32_t key) {
    const uint64_t g = gen_.load(std::memory_order_acquire);
    if (__builtin_expect(c->fast_gen != g, 0)) {
        if (!c->fast) c->fast.reset(new FastEntry[kFastSize]);
        for (size_t i = 0; i < kFastSize; ++i) c->fast[i] = {nullptr, kFastEmpty};
        c->fast_gen = g;
    }
    FastEntry& fe = c->fast[fastIndex(key)];
    if (fe.key == key) return fe.first;
    Block* b = lookupOrDecode(key);
    // Never cache a miss: the handlers' fast path follows `first` without checking it.
    if (!b) return nullptr;
    const Op* first = b->ops.data();
    fe = {first, key};
    return first;
}

size_t Engine::loadBlockCache(const std::string& path) {
    std::unordered_map<uint32_t, CachedBlock> loaded;
    if (!loadBlockCacheFile(path, loaded)) return 0;
    std::lock_guard<std::mutex> lk(mu_);
    diskCache_ = std::move(loaded);
    diskCacheStats_.diskEntries = diskCache_.size();
    return diskCache_.size();
}

size_t Engine::saveBlockCache(const std::string& path) {
    std::vector<const Block*> snapshot;
    std::lock_guard<std::mutex> lk(mu_);
    snapshot.reserve(blocks_.size());
    for (const auto& [k, b] : blocks_) {
        const uint32_t pc = k & ~1u;
        if (isHleAddress(pc) || entryObservers_.count(pc)) continue;
        snapshot.push_back(b.get());
    }
    size_t saved = 0;
    if (saveBlockCacheFile(path, snapshot, mem_, saved)) {
        diskCacheStats_.diskSaved = saved;
    }
    return saved;
}

Block* Engine::lookupOrDecode(uint32_t key) {
    // HLE addresses are the same entry regardless of the interworking bit.
    if (isHleAddress(key & ~1u)) key &= ~1u;
    std::lock_guard<std::mutex> lk(mu_);
    auto it = blocks_.find(key);
    if (it != blocks_.end()) return it->second.get();
    std::unique_ptr<Block> b;
    bool fromDisk = false;
    const uint32_t pc = key & ~1u;
    if (!isHleAddress(pc) && !entryObservers_.count(pc)) {
        auto dit = diskCache_.find(key);
        if (dit != diskCache_.end()) {
            b = materializeCachedBlock(dit->second, mem_);
            diskCache_.erase(dit);
            if (b) {
                ++diskCacheStats_.diskHits;
                fromDisk = true;
            } else {
                ++diskCacheStats_.diskHashMismatches;
            }
        }
    }
    if (!b) b = decodeBlock(key);
    Block* raw = b.get();
    if (!fromDisk) {
        ++stats_.blocks_decoded;
        stats_.guest_insns_decoded += raw->guest_insns;
        stats_.ops_emitted += raw->ops.size();
    }
    blocks_.emplace(key, std::move(b));
    if (!isHleAddress(pc) && raw->end > pc)
        for (uint32_t pg = pc >> 12; pg <= (raw->end - 1) >> 12; ++pg) pageBlocks_[pg].push_back(key);
    return raw;
}

uint32_t Engine::faultPc(const Cpu& c, const uintptr_t* regs, int nregs, uintptr_t hostPc) {
    const uint32_t blockPc = c.block_pc;
    if (nregs <= 0 && !hostPc) return blockPc;
    std::lock_guard<std::mutex> lk(mu_);
    auto it = blocks_.find(blockPc | (c.thumb & 1u));
    if (it == blocks_.end() || it->second->ops.empty()) return blockPc;
    const std::vector<Op>& ops = it->second->ops;
    const size_t n = ops.size();
    // 1. Ops whose handler holds the faulting host instruction: the nearest handler entry at or
    //    below the host PC among this block's handlers (handlers are separate functions).
    uintptr_t bestFn = 0;
    for (const Op& o : ops) {
        const auto fn = reinterpret_cast<uintptr_t>(o.fn);
        if (fn <= hostPc && hostPc - fn < 0x4000 && fn > bestFn) bestFn = fn;
    }
    std::vector<size_t> byFn;
    if (bestFn)
        for (size_t i = 0; i < n; ++i) if (reinterpret_cast<uintptr_t>(ops[i].fn) == bestFn) byFn.push_back(i);
    // 2. Ops a host register points at (the handler's `op`, or `op + 1` loaded for the tail call).
    const auto lo = reinterpret_cast<uintptr_t>(ops.data()), hi = lo + n * sizeof(Op);
    std::vector<size_t> byReg;
    for (int i = 0; i < nregs; ++i) {
        const uintptr_t v = regs[i];
        if (v < lo || v > hi || (v - lo) % sizeof(Op)) continue;
        const size_t k = (v - lo) / sizeof(Op);
        if (k < n) byReg.push_back(k);
        if (k > 0) byReg.push_back(k - 1);
    }
    std::sort(byReg.begin(), byReg.end());
    byReg.erase(std::unique(byReg.begin(), byReg.end()), byReg.end());
    size_t pick = n;
    std::vector<size_t> both;
    for (size_t k : byFn) if (std::binary_search(byReg.begin(), byReg.end(), k)) both.push_back(k);
    // Both signals must agree. The handler alone decides only when no register names an op of
    // this block; when registers do but none matches, the fault was in a helper the handler called.
    if (both.size() == 1) pick = both[0];
    else if (byReg.empty() && byFn.size() == 1) pick = byFn[0];
    if (pick == n) return blockPc;
    // A fused pair runs ops[pick] and ops[pick + 1] in one handler; only memory ops fault, so when
    // just the second one touches memory, it is the faulting instruction.
    if (pick + 1 < n && pairMemoryParts(ops[pick].fn) == 2) ++pick;
    const uint32_t pc = ops[pick].pc;
    // Ops carry the address of their source instruction; it must lie inside this block.
    return pc >= blockPc && pc < it->second->end ? pc : blockPc;
}

void Engine::flushCache() {
    std::lock_guard<std::mutex> lk(mu_);
    // HLE blocks stay: they never change, and a thread blocked inside a host function is
    // still running the HLE block that called it.
    for (auto it = blocks_.begin(); it != blocks_.end();) {
        if (isHleAddress(uint32_t(it->first) & ~1u)) ++it;
        else it = blocks_.erase(it);
    }
    pageBlocks_.clear();
    diskCache_.clear();
    gen_.fetch_add(1, std::memory_order_acq_rel);
}

void Engine::resetFastCache(Cpu& c) {
    if (!c.fast) return;
    for (size_t i = 0; i < kFastSize; ++i) c.fast[i] = {nullptr, kFastEmpty};
    c.fast_gen = gen_.load(std::memory_order_acquire);
}

bool Engine::hasBlocksIn(uint32_t addr, uint32_t size) {
    if (!size) return false;
    std::lock_guard<std::mutex> lk(mu_);
    const uint64_t lo = addr, hi = uint64_t(addr) + size;
    for (uint64_t pg = lo >> 12; pg <= (hi - 1) >> 12; ++pg) {
        auto it = pageBlocks_.find(uint32_t(pg));
        if (it == pageBlocks_.end()) continue;
        for (const uint32_t key : it->second) {
            auto b = blocks_.find(key);
            if (b != blocks_.end() && (key & ~1u) < hi && b->second->end > lo) return true;
        }
    }
    return false;
}

void Engine::retireRange(uint32_t addr, uint32_t size) {
    if (!size) return;
    std::lock_guard<std::mutex> lk(mu_);
    const uint64_t lo = addr, hi = uint64_t(addr) + size;
    for (auto dit = diskCache_.begin(); dit != diskCache_.end();) {
        const uint64_t bl = dit->first & ~1u, bh = dit->second.end;
        if (bl < hi && bh > lo) dit = diskCache_.erase(dit);
        else ++dit;
    }
    for (uint64_t pg = lo >> 12; pg <= (hi - 1) >> 12; ++pg) {
        auto pit = pageBlocks_.find(uint32_t(pg));
        if (pit == pageBlocks_.end()) continue;
        auto& keys = pit->second;
        for (size_t i = 0; i < keys.size();) {
            auto it = blocks_.find(keys[i]);
            if (it == blocks_.end()) { keys[i] = keys.back(); keys.pop_back(); continue; }   // already gone
            const uint64_t bl = it->first & ~1u, bh = it->second->end;
            if (bl < hi && bh > lo) {
                std::unique_ptr<Block> b = std::move(it->second);
                blocks_.erase(it);
                Op& o = b->ops[0];
                o.fn = &H_Stale;
                o.imm = b->key;
                b->valid = false;
                retired_.push_back(std::move(b));   // links may still reach it
                keys[i] = keys.back();
                keys.pop_back();
                continue;
            }
            ++i;
        }
        if (keys.empty()) pageBlocks_.erase(pit);
    }
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
    if (entryObservers_.count(pc)) {
        Op probe{};
        probe.fn = &H_ObserveEntry; probe.pc = pc; probe.imm = pc;
        b->ops.insert(b->ops.begin(), probe);
    }
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
    FaultScope* prev = t_scope;
    const bool previousFaultInGuest = prev && cpu.fault_state &&
        cpu.fault_state->inGuest.load(std::memory_order_relaxed);
    cpu.engine = this;
    if (cpu.fault_state) {
        cpu.fault_state->arena.store(reinterpret_cast<uintptr_t>(mem_.base()), std::memory_order_relaxed);
        // Registers are sampled by enterGuest, after any pending suspension has been honoured.
        recordHleState(cpu, cpu.current_hle.load(std::memory_order_relaxed), false);
        cpu.fault_state->active.store(true, std::memory_order_release);
    }
    cpu.budget = budget;
    cpu.progress_budget_start = budget;
    const int64_t startBudget = budget;
    cpu.exit = Exit::None;
    cpu.exit_info = 0;

    FaultScope scope;
    scope.arenaBase = reinterpret_cast<uintptr_t>(mem_.base());
    scope.addr = 0;
    scope.cpu = &cpu;
    // Restore on C++ exceptions too: a subsequent host fault must not follow
    // a scope pointer into the abandoned Engine::run stack frame.
    struct RestoreScope {
        FaultScope* prev;
        HostFaultThreadState* state;
        bool inGuest;
        ~RestoreScope() {
            if (state) state->inGuest.store(inGuest, std::memory_order_relaxed);
            t_scope = prev;
        }
    } restore{prev, cpu.fault_state, previousFaultInGuest};
    t_scope = &scope;
    if (sigsetjmp(scope.jb, 1)) {
        t_scope = prev;
        cpu.exit = Exit::MemFault;
        cpu.exit_info = scope.addr;
        // The faulting instruction: the op of the current block named by the fault context, or the
        // block start when no register identifies one.
        cpu.r[kPC] = faultPc(cpu, scope.regs, scope.nregs, scope.hostPc);
        cpu.insn_count += uint64_t(startBudget - cpu.budget);
        // Sample the registers before releasing the CPU: once in_guest is false a suspender may rewrite them.
        cpu.recordFaultRegisters(cpu.r[kPC]);
        if (cpu.fault_state) cpu.fault_state->inGuest.store(false, std::memory_order_relaxed);
        cpu.in_guest.store(false, std::memory_order_release);
        cpu.progress_budget_start = cpu.budget;
        cpu.publishProgress(false);
        return cpu.exit;
    }
    uint8_t* const base = mem_.base();
    enterGuest(cpu);
    for (;;) {
        while (cpu.exit == Exit::None) {
            const uint32_t key = (cpu.r[kPC] & (cpu.thumb ? ~1u : ~3u)) | (cpu.thumb & 1);
            const Op* first = enterBlock(&cpu, key);
            if (!first) break;
            cpu.block_pc = key & ~1u;
            cpu.recordFaultRegisters(cpu.block_pc);
            first->fn(&cpu, first, base);
        }
        if (cpu.exit != Exit::Svc || !svcHandler_) break;
        cpu.exit = Exit::None;
        if (svcHandler_(cpu, cpu.exit_info)) continue;
        cpu.exit = Exit::Svc;
        break;
    }
    t_scope = prev;
    cpu.recordFaultRegisters(cpu.r[kPC]);   // before releasing the CPU to a suspender
    if (cpu.fault_state) cpu.fault_state->inGuest.store(false, std::memory_order_relaxed);
    cpu.in_guest.store(false, std::memory_order_release);
    // Handlers only decrement the budget; the retired count falls out of it.
    cpu.insn_count += uint64_t(startBudget - cpu.budget);
    cpu.progress_budget_start = cpu.budget;
    cpu.publishProgress(false);
    return cpu.exit;
}

void Engine::park(Cpu& c) {
    // Sample before announcing the park: once in_guest is false a suspender may rewrite c.r[].
    c.recordFaultRegisters(c.r[kPC]);
    if (c.fault_state) c.fault_state->inGuest.store(false, std::memory_order_relaxed);
    c.in_guest.store(false, std::memory_order_seq_cst);
    c.publishProgress(false);
    std::unique_lock<std::mutex> lk(suspendMu_);
    suspendCv_.notify_all();
    suspendCv_.wait(lk, [&] { return c.suspend_count.load() == 0 || stopRequested(); });
    // Published while still holding suspendMu_: a suspend() that starts after this either sees in_guest and waits for
    // it to clear, or has already raised suspend_count, which enterGuest re-checks.
    c.in_guest.store(true, std::memory_order_seq_cst);
    lk.unlock();
    // enterGuest samples the registers and publishes progress after this returns (post-resume registers).
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
        if (__builtin_expect(cpu.async_pending.load(std::memory_order_acquire) != 0, 0)) {
            try {
                runSafepoint(cpu);
            } catch (const GuestFault& f) {
                cpu.exit = f.exit; cpu.exit_info = f.info; cpu.r[kPC] = f.pc; cpu.thumb = f.thumb;
                return f.exit;
            }
        }
    }
}

uint64_t Engine::callGuest(Cpu& c, uint32_t fn, std::initializer_list<uint32_t> args) {
    return callGuest(c, fn, args.begin(), args.size());
}

uint64_t Engine::callGuest(Cpu& c, uint32_t fn, const uint32_t* args, size_t nargs) {
    if (nargs && !args) throw std::invalid_argument("guest call arguments are null");
    // Save everything the interrupted context can observe.
    uint32_t saved[kNumRegs];
    std::memcpy(saved, c.r, sizeof saved);
    const uint32_t nzcv = c.nzcv, q = c.q, ge = c.ge, thumb = c.thumb, blockPc = c.block_pc;
    const int64_t budget = c.budget;
    const int64_t progressBudgetStart = c.progress_budget_start;
    const uint64_t progressCarry = c.progress_carry;
    const Exit exitSaved = c.exit;
    const uint32_t infoSaved = c.exit_info;
    const uint32_t boundarySaved = c.call_boundary_sp;
    struct RestoreBoundary { Cpu& c; uint32_t v; ~RestoreBoundary() { c.call_boundary_sp = v; } } restoreBoundary{c, boundarySaved};

    uint32_t sp = c.r[kSP] & ~7u;
    c.call_boundary_sp = sp;
    if (nargs > 4) {
        auto stackFault = [&](uint32_t address, const char* reason) {
            const auto oldInfo = c.exit_info;
            c.exit_info = address;
            captureFault(c, Exit::MemFault);
            c.exit_info = oldInfo;
            throw GuestFault(Exit::MemFault, address, c.r[kPC], c.thumb, reason);
        };
        const size_t extra = nargs - 4;
        if (extra > UINT32_MAX / 4 || uint64_t(extra) * 4 > sp)
            stackFault(sp, "guest call argument stack overflows its address space");
        const size_t bytes = extra * 4;
        sp = (sp - uint32_t(bytes)) & ~7u;
        uint32_t fault = sp;
        // These writes happen before run() establishes a guest fault scope.
        // A bad guest stack must be a controlled GuestFault, not a host SIGSEGV.
        if (!mem_.copyInWritable(sp, args + 4, bytes, fault))
            stackFault(fault, "guest call argument stack is not writable");
    }
    for (size_t i = 0; i < 4; ++i) c.r[i] = i < nargs ? args[i] : 0;
    c.r[kSP] = sp;
    c.r[kLR] = returnStub_;
    c.r[kPC] = fn & ~1u;
    c.thumb = fn & 1;

    // After the stack-argument writes (they can fault, and a fault publishes before the carry is restored): the nested slices replace budget/progress_budget_start; keep the outer slice's pending count in the published total.
    c.progress_carry += uint64_t(c.progress_budget_start) - uint64_t(c.budget);
    const Exit e = runToExit(c);
    const uint64_t result = (uint64_t(c.r[1]) << 32) | c.r[0];
    if (e != Exit::Return) {
        captureFault(c, e);
        const uint32_t info = c.exit_info, pc = c.r[kPC], th = c.thumb;
        std::memcpy(c.r, saved, sizeof saved);
        c.nzcv = nzcv; c.q = q; c.ge = ge; c.thumb = thumb; c.block_pc = blockPc; c.budget = budget;
        c.progress_budget_start = progressBudgetStart;
        c.progress_carry = progressCarry;
        c.exit = exitSaved; c.exit_info = infoSaved;
        c.publishProgress(false);
        throw GuestFault(e, info, pc, th, e == Exit::HostError ? t_hostError : "guest call did not return");
    }
    std::memcpy(c.r, saved, sizeof saved);
    c.nzcv = nzcv; c.q = q; c.ge = ge; c.thumb = thumb; c.block_pc = blockPc; c.budget = budget;
    c.progress_budget_start = progressBudgetStart;
    c.progress_carry = progressCarry;
    c.exit = exitSaved; c.exit_info = infoSaved;
    c.publishProgress(false);
    return result;
}

} // namespace Hydrogen::CPU
