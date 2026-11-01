#include "Engine.hpp"
#include "Decoder.hpp"
#include <chrono>
#include <csetjmp>
#include <csignal>
#include <cstring>
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
    const Memory::GuestArena* arena;
    uint32_t addr;
};
thread_local FaultScope* t_scope = nullptr;

struct sigaction g_oldSegv, g_oldBus;
std::once_flag g_installOnce;

// ------------------------------------ preallocated thread & log tracking ---

constexpr size_t kMaxCpus = 128;
static std::atomic<Cpu*> g_activeCpus[kMaxCpus]{};

void registerCpu(Cpu* c) {
    for (size_t i = 0; i < kMaxCpus; ++i) {
        Cpu* expected = nullptr;
        if (g_activeCpus[i].compare_exchange_strong(expected, c, std::memory_order_acq_rel)) {
            return;
        }
    }
}

void unregisterCpu(Cpu* c) {
    for (size_t i = 0; i < kMaxCpus; ++i) {
        Cpu* expected = c;
        if (g_activeCpus[i].compare_exchange_strong(expected, nullptr, std::memory_order_acq_rel)) {
            return;
        }
    }
}

constexpr size_t kLogRingCap = 200;
constexpr size_t kLogLineCap = 256;

struct LogRingBuffer {
    char lines[kLogRingCap][kLogLineCap];
    size_t lengths[kLogRingCap];
    std::atomic<uint64_t> writeIdx{0};
};

static LogRingBuffer g_logRing;

void recordLogLineAsyncSafeImpl(const char* data, size_t len) {
    const uint64_t idx = g_logRing.writeIdx.fetch_add(1, std::memory_order_relaxed);
    const size_t slot = idx % kLogRingCap;
    const size_t copyLen = len < (kLogLineCap - 1) ? len : (kLogLineCap - 1);
    std::memcpy(g_logRing.lines[slot], data, copyLen);
    g_logRing.lines[slot][copyLen] = '\0';
    g_logRing.lengths[slot] = copyLen;
}

// Async-signal-safe I/O primitives using raw write(STDERR_FILENO).
static std::atomic<int> g_faultFd{STDERR_FILENO};
static void safeWrite(const char* str, size_t len) {
    const int fd = g_faultFd.load(std::memory_order_relaxed);
    while (len > 0) {
        ssize_t r = ::write(fd, str, len);
        if (r <= 0) break;
        str += r;
        len -= r;
    }
}

static void safeWriteStr(const char* str) {
    if (!str) return;
    safeWrite(str, std::strlen(str));
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
    if (s && s->arena->containsHost(si->si_addr)) {
        s->addr = s->arena->guestAddr(si->si_addr);
        siglongjmp(s->jb, 1);
    }

    // Host SIGSEGV/SIGBUS fault: print async-signal-safe diagnostic report.
    if (!g_faultDiagnosticActive.exchange(true, std::memory_order_acq_rel)) {
        safeWriteStr("\n=== HVM HOST FAULT DIAGNOSTICS ===\nSignal: ");
        safeWriteStr(sig == SIGSEGV ? "SIGSEGV (Segmentation Fault)" : sig == SIGBUS ? "SIGBUS (Bus Error)" : "OTHER");
        safeWriteStr("\nHost Fault Address (si_addr): 0x");
        safeWriteHex(reinterpret_cast<uintptr_t>(si ? si->si_addr : nullptr), 16);

        if (s && s->arena) {
            if (s->arena->containsHost(si->si_addr)) {
                safeWriteStr("\nGuest Arena Offset: 0x");
                safeWriteHex(s->arena->guestAddr(si->si_addr), 8);
            } else {
                safeWriteStr("\nGuest Arena Base: 0x");
                safeWriteHex(reinterpret_cast<uintptr_t>(s->arena->base()), 16);
                safeWriteStr(" (fault address outside guest arena)");
            }
        }
        safeWriteStr("\n\n--- Guest Threads State ---\n");

        for (size_t i = 0; i < kMaxCpus; ++i) {
            Cpu* c = g_activeCpus[i].load(std::memory_order_relaxed);
            if (!c) continue;
            safeWriteStr("Thread slot #");
            safeWriteDec(i);
            safeWriteStr(": PC=0x");
            safeWriteHex(c->r[kPC], 8);
            safeWriteStr(" LR=0x");
            safeWriteHex(c->r[kLR], 8);
            safeWriteStr(" block_pc=0x");
            safeWriteHex(c->block_pc, 8);
            safeWriteStr(" in_guest=");
            safeWriteStr(c->in_guest.load(std::memory_order_relaxed) ? "true" : "false");
            safeWriteStr(" current_hle=0x");
            safeWriteHex(c->current_hle.load(std::memory_order_relaxed), 8);
            safeWriteStr("\n");
        }

        safeWriteStr("\n--- Recent Logs (last 200) ---\n");
        const uint64_t totalWritten = g_logRing.writeIdx.load(std::memory_order_relaxed);
        const uint64_t count = totalWritten < kLogRingCap ? totalWritten : kLogRingCap;
        const uint64_t startIdx = totalWritten > count ? (totalWritten - count) : 0;
        for (uint64_t i = startIdx; i < totalWritten; ++i) {
            const size_t slot = i % kLogRingCap;
            if (g_logRing.lengths[slot] > 0) {
                safeWrite(g_logRing.lines[slot], g_logRing.lengths[slot]);
                safeWriteStr("\n");
            }
        }
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
}
Engine::~Engine() = default;

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
    const uint32_t outerArg0 = c->progress_argument0, outerArg1 = c->progress_argument1;
    c->progress_argument0 = c->r[0]; c->progress_argument1 = c->r[1];
    c->publishProgress(false);
    struct Reenter {
        Engine* e; Cpu* c; uint32_t outer, a0, a1;
        ~Reenter() { c->current_hle.store(outer, std::memory_order_relaxed);
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
    cpu.budget = budget;
    cpu.progress_budget_start = budget;
    const int64_t startBudget = budget;
    cpu.exit = Exit::None;
    cpu.exit_info = 0;

    FaultScope scope;
    scope.arena = &mem_;
    scope.addr = 0;
    FaultScope* prev = t_scope;
    t_scope = &scope;
    if (sigsetjmp(scope.jb, 1)) {
        t_scope = prev;
        cpu.exit = Exit::MemFault;
        cpu.exit_info = scope.addr;
        cpu.r[kPC] = cpu.block_pc;   // precise PC within the block isn't tracked
        cpu.insn_count += uint64_t(startBudget - cpu.budget);
        cpu.in_guest.store(false, std::memory_order_release);
        cpu.progress_budget_start = cpu.budget;
        cpu.publishProgress(false);
        return cpu.exit;
    }
    uint8_t* const base = mem_.base();
    enterGuest(cpu);
    while (cpu.exit == Exit::None) {
        const uint32_t key = (cpu.r[kPC] & (cpu.thumb ? ~1u : ~3u)) | (cpu.thumb & 1);
        Block* b = enterBlock(&cpu, key);
        if (!b) break;
        cpu.block_pc = key & ~1u;
        const Op* first = b->ops.data();
        first->fn(&cpu, first, base);
    }
    t_scope = prev;
    cpu.in_guest.store(false, std::memory_order_release);
    // Handlers only decrement the budget; the retired count falls out of it.
    cpu.insn_count += uint64_t(startBudget - cpu.budget);
    cpu.progress_budget_start = cpu.budget;
    cpu.publishProgress(false);
    return cpu.exit;
}

void Engine::park(Cpu& c) {
    c.in_guest.store(false, std::memory_order_seq_cst);
    c.publishProgress(false);
    std::unique_lock<std::mutex> lk(suspendMu_);
    suspendCv_.notify_all();
    suspendCv_.wait(lk, [&] { return c.suspend_count.load() == 0 || stopRequested(); });
    lk.unlock();
    c.in_guest.store(true, std::memory_order_seq_cst);
    c.publishProgress(true);
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
    const int64_t progressBudgetStart = c.progress_budget_start;
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
        c.progress_budget_start = progressBudgetStart;
        c.exit = exitSaved; c.exit_info = infoSaved;
        c.publishProgress(false);
        throw GuestFault(e, info, pc, th, e == Exit::HostError ? t_hostError : "guest call did not return");
    }
    std::memcpy(c.r, saved, sizeof saved);
    c.nzcv = nzcv; c.q = q; c.ge = ge; c.thumb = thumb; c.block_pc = blockPc; c.budget = budget;
    c.progress_budget_start = progressBudgetStart;
    c.exit = exitSaved; c.exit_info = infoSaved;
    c.publishProgress(false);
    return result;
}

} // namespace Hydrogen::CPU
