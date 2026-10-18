#include "Engine.hpp"
#include "Decoder.hpp"
#include <csetjmp>
#include <csignal>
#include <cstring>

namespace Hydrogen::CPU {

// -------------------------------------------------------------- IR hooks ---

Block* hvmEnterBlock(Cpu* c, uint32_t key) { return c->engine->enterBlock(c, key); }
void   hvmCallHle(Cpu* c, uint32_t index)  { c->engine->callHle(c, index); }

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

void faultHandler(int sig, siginfo_t* si, void* uctx) {
    FaultScope* s = t_scope;
    if (s && s->arena->containsHost(si->si_addr)) {
        s->addr = s->arena->guestAddr(si->si_addr);
        siglongjmp(s->jb, 1);
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

// ---------------------------------------------------------------- engine ---

Engine::Engine(Memory::GuestArena& mem) : mem_(mem), fast_(kFastSize, FastEntry{0xFFFFFFFFu, nullptr}) {
    installFaultHandler();
}
Engine::~Engine() = default;

uint32_t Engine::registerHle(std::string name, HleFn fn) {
    std::lock_guard<std::mutex> lk(mu_);
    const uint32_t idx = uint32_t(hle_.size());
    if (idx >= Memory::GuestArena::kHleSize / 4) throw std::runtime_error("HydrogenVM: HLE table full");
    hle_.push_back({std::move(name), std::move(fn)});
    return Memory::GuestArena::kHleBase + idx * 4;
}

static inline size_t fastIndex(uint32_t key) { return ((key >> 1) ^ (key >> 15)) & (16384 - 1); }

Block* Engine::enterBlock(Cpu* c, uint32_t key) {
    (void)c;
    FastEntry& fe = fast_[fastIndex(key)];
    if (fe.key == key) return fe.block;
    Block* b = lookupOrDecode(key);
    fe.key = key; fe.block = b;
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
    std::fill(fast_.begin(), fast_.end(), FastEntry{0xFFFFFFFFu, nullptr});
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
        bb.exitIndirect(kLR, true);
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
        if (r == DecodeResult::EndNeedsFallthrough || bb.insns() >= BlockBuilder::kMaxInsns ||
            (pc & (Memory::GuestArena::kGuestPageSize - 1)) == 0) {
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
        if (r != DecodeResult::Continue) {
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
        return cpu.exit;
    }
    uint8_t* const base = mem_.base();
    while (cpu.exit == Exit::None) {
        const uint32_t key = (cpu.r[kPC] & (cpu.thumb ? ~1u : ~3u)) | (cpu.thumb & 1);
        Block* b = enterBlock(&cpu, key);
        if (!b) break;
        cpu.block_pc = key & ~1u;
        const Op* first = b->ops.data();
        first->fn(&cpu, first, base);
    }
    t_scope = prev;
    return cpu.exit;
}

} // namespace Hydrogen::CPU
