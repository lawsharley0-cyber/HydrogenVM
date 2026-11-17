// HydrogenVM — Linux signals for Android games.
//
// Handlers are kept here in Linux numbers (1-64) with Linux sa_flags; SIGSEGV / SIGBUS (and the
// other signals Darwin also has) are passed on to the iOS sigaction too, which is where fault
// delivery (libc/System.cpp) finds the handler.
//
// Signals sent to a thread (pthread_kill, tkill, tgkill, raise) are delivered for real: the
// target runs the handler on its own host thread at its next safepoint — the end of a time
// slice, the return from a host function, or inside a host wait (mutex, condition, semaphore,
// futex, sleep). Runtimes stop the world this way: Boehm GC (Unity's Mono) sends SIGPWR to
// every thread and waits for each to answer from its handler, which then sits in sigsuspend
// until SIGXCPU. Each thread has its own blocked-signal mask (pthread_sigmask, sigprocmask,
// sigsuspend).
#include "runtime/android/Android.hpp"
#include "runtime/android/BionicInternal.hpp"
#include "runtime/Support.hpp"
#include "core/cpu/Engine.hpp"
#include <array>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <mutex>
#include <string>
#include <unordered_map>

namespace Hydrogen::Runtime::Android {
namespace {

constexpr int kSigKill = 9, kSigStop = 19, kMaxSig = 64;
constexpr uint32_t kSaSiginfo = 4, kSaNodefer = 0x40000000u, kSaResethand = 0x80000000u;

int darwinSignal(int s) {
    static const int kMap[32] = {0, 1, 2, 3, 4, 5, 6, 10, 8, 9, 30, 11, 31, 13, 14, 15,
                                 0, 20, 19, 17, 18, 21, 22, 16, 24, 25, 26, 27, 28, 23, 0, 12};
    return s > 0 && s < 32 ? kMap[s] : 0;
}
uint32_t darwinFlags(uint32_t f) {
    uint32_t d = 0;
    if (f & 1) d |= 8;              // SA_NOCLDSTOP
    if (f & 2) d |= 0x20;           // SA_NOCLDWAIT
    if (f & 4) d |= 0x40;           // SA_SIGINFO
    if (f & 0x08000000u) d |= 1;    // SA_ONSTACK
    if (f & 0x10000000u) d |= 2;    // SA_RESTART
    if (f & 0x40000000u) d |= 0x10; // SA_NODEFER
    if (f & 0x80000000u) d |= 4;    // SA_RESETHAND
    return d;
}
// A Linux sigset bit: signal n is bit n-1 (bionic's ARM sigset_t is one 32-bit word, so
// pending and blocked keep 64 bits but only the low 32 are visible to the game).
constexpr uint64_t bit(int sig) { return uint64_t(1) << (sig - 1); }
constexpr uint64_t kUnblockable = bit(kSigKill) | bit(kSigStop);

struct Signals {
    explicit Signals(Process&) {}
    struct Act { uint32_t handler = 0, mask = 0, flags = 0, restorer = 0; };
    struct Thread { uint64_t pending = 0, blocked = 0; };
    std::mutex mu;
    std::condition_variable cv;
    std::array<Act, kMaxSig + 1> acts{};
    std::unordered_map<uint32_t, Thread> threads;   // GuestThread id
};

Signals& sigs(Process& p) { return p.service<Signals>(); }

// Marks the thread for delivery if anything pending is unblocked. Caller holds s.mu.
void kick(Process& p, uint32_t id, Signals::Thread& t) {
    if (!(t.pending & ~t.blocked)) return;
    if (GuestThread* g = p.findThreadById(id)) {
        g->cpu.async_pending.store(1, std::memory_order_release);
        if (g->cpu.in_guest.load(std::memory_order_acquire)) g->cpu.budget = 0;   // end its time slice
    }
}

// Runs one signal's handler on the current thread with a Linux ARM siginfo / ucontext built
// from its registers (the guest state when the signal came in).
void runHandler(Process& p, CPU::Cpu& c, int sig, const Signals::Act& a, uint64_t blockedBefore) {
    auto& m = p.mem();
    const uint32_t ucl = (c.r[CPU::kSP] - 256 - 768) & ~15u;
    const uint32_t sil = (ucl - 128) & ~15u;
    const uint32_t spl = (sil - 16) & ~15u;
    std::memset(m.host(sil), 0, ucl + 768 - sil);
    m.write<uint32_t>(sil + 0, uint32_t(sig));
    m.write<int32_t>(sil + 8, -6);                              // SI_TKILL
    m.write<uint32_t>(sil + 12, 1000);                          // si_pid
    const uint32_t sc = ucl + 20;
    for (int i = 0; i < 16; ++i) m.write<uint32_t>(sc + 12 + 4 * i, c.r[i]);
    m.write<uint32_t>(sc + 76, c.cpsr() | (c.thumb ? 0x20u : 0u));
    m.write<uint32_t>(ucl + 104, uint32_t(blockedBefore));     // uc_sigmask
    const uint32_t savedSp = c.r[CPU::kSP];
    c.r[CPU::kSP] = spl;
    struct RestoreSp { CPU::Cpu& c; uint32_t sp; ~RestoreSp() { c.r[CPU::kSP] = sp; } } restore{c, savedSp};
    (void)restore;
    if (a.flags & kSaSiginfo) p.engine().callGuest(c, a.handler, {uint32_t(sig), sil, ucl});
    else p.engine().callGuest(c, a.handler, {uint32_t(sig)});
}

// The safepoint hook: delivers every pending, unblocked signal of the current thread.
void deliverPending(Process& p, CPU::Cpu& c) {
    GuestThread& gt = p.currentThread();
    if (&gt.cpu != &c) { c.async_pending.store(0); return; }
    auto& s = sigs(p);
    for (;;) {
        int sig = 0;
        Signals::Act a;
        uint64_t before = 0;
        {
            std::lock_guard<std::mutex> lk(s.mu);
            Signals::Thread& t = s.threads[gt.id];
            const uint64_t ready = t.pending & ~t.blocked;
            if (!ready) { c.async_pending.store(0, std::memory_order_release); return; }
            sig = __builtin_ctzll(ready) + 1;
            t.pending &= ~bit(sig);
            a = s.acts[size_t(sig)];
            if (a.handler <= 1) continue;   // ignored, or default (nothing here needs a default action)
            before = t.blocked;
            t.blocked |= uint64_t(a.mask);
            if (!(a.flags & kSaNodefer)) t.blocked |= bit(sig);
            t.blocked &= ~kUnblockable;
            if (a.flags & kSaResethand) s.acts[size_t(sig)].handler = 0;
        }
        struct Unmask {
            Signals& s; uint32_t id; uint64_t before;
            ~Unmask() { std::lock_guard<std::mutex> lk(s.mu); s.threads[id].blocked = before; }
        } unmask{s, gt.id, before};
        runHandler(p, c, sig, a, before);
    }
}

// Sends `sig` to guest thread `id`. Returns 0 or a Linux errno.
int sendTo(Process& p, uint32_t id, int sig) {
    if (sig < 0 || sig > kMaxSig) return 22;
    if (!p.findThreadById(id)) return 3;   // ESRCH
    if (sig == 0) return 0;
    auto& s = sigs(p);
    std::lock_guard<std::mutex> lk(s.mu);
    const Signals::Act& a = s.acts[size_t(sig)];
    if (a.handler == 1) return 0;          // SIG_IGN
    if (a.handler == 0) {
        p.reportDefaulted("signal " + std::to_string(sig) + " with no handler: not delivered");
        return 0;
    }
    Signals::Thread& t = s.threads[id];
    t.pending |= bit(sig);
    kick(p, id, t);
    s.cv.notify_all();
    return 0;
}

uint32_t tidOf(GuestThread& t) { return 1000 + t.id; }

// ------------------------------------------------------------ the calls ---

int32_t g_sigaction(Ctx& x, int32_t sig, GuestPtr act, GuestPtr old) {
    if (sig <= 0 || sig > kMaxSig || ((sig == kSigKill || sig == kSigStop) && act)) { setLinuxErrno(x, 22); return -1; }
    auto& s = sigs(x.proc);
    Signals::Act prev, next;
    {
        std::lock_guard<std::mutex> lk(s.mu);
        prev = s.acts[size_t(sig)];
        if (act) {
            next = {x.mem.read<uint32_t>(act), x.mem.read<uint32_t>(act + 4), x.mem.read<uint32_t>(act + 8),
                    x.mem.read<uint32_t>(act + 12)};
            s.acts[size_t(sig)] = next;
        }
    }
    if (old) {
        x.mem.write<uint32_t>(old, prev.handler);
        x.mem.write<uint32_t>(old + 4, prev.mask);
        x.mem.write<uint32_t>(old + 8, prev.flags);
        x.mem.write<uint32_t>(old + 12, prev.restorer);
    }
    // Faults are delivered by the iOS side: keep its table in step (Darwin numbers and flags).
    if (act) {
        if (const int d = darwinSignal(sig)) {
            static const char key = 0;
            uint32_t& scratch = x.proc.threadSlot(&key);
            if (!scratch) scratch = x.proc.allocStatic(16);
            x.mem.write<uint32_t>(scratch, next.handler);
            x.mem.write<uint32_t>(scratch + 4, 0);
            x.mem.write<uint32_t>(scratch + 8, darwinFlags(next.flags));
            callIos(x, "_sigaction", {uint32_t(d), scratch, 0});
        }
    }
    return 0;
}
uint32_t g_signal(Ctx& x, int32_t sig, GuestPtr handler) {
    if (sig <= 0 || sig > kMaxSig || sig == kSigKill || sig == kSigStop) { setLinuxErrno(x, 22); return 0xFFFFFFFFu; }
    static const char key = 0;
    uint32_t& scratch = x.proc.threadSlot(&key);
    if (!scratch) scratch = x.proc.allocStatic(32);
    x.mem.write<uint32_t>(scratch, handler);
    x.mem.write<uint32_t>(scratch + 4, 0);
    x.mem.write<uint32_t>(scratch + 8, 0x10000000u);   // SA_RESTART, as bionic's signal()
    x.mem.write<uint32_t>(scratch + 12, 0);
    if (g_sigaction(x, sig, scratch, scratch + 16) != 0) return 0xFFFFFFFFu;
    return x.mem.read<uint32_t>(scratch + 16);
}

// A signal to this thread: a handler runs before the call returns to the game (at the
// return from this host function). Default fatal signals go to iOS, which ends the game.
int32_t raiseSelf(Ctx& x, int32_t sig) {
    if (sig < 0 || sig > kMaxSig) { setLinuxErrno(x, 22); return -1; }
    if (sig == 0) return 0;
    uint32_t handler;
    {
        auto& s = sigs(x.proc);
        std::lock_guard<std::mutex> lk(s.mu);
        handler = s.acts[size_t(sig)].handler;
    }
    if (handler == 0 && darwinSignal(sig)) return int32_t(callIos(x, "_raise", {uint32_t(darwinSignal(sig))}));
    const int e = sendTo(x.proc, x.proc.currentThread().id, sig);
    if (e) { setLinuxErrno(x, e); return -1; }
    return 0;
}
int32_t g_raise(Ctx& x, int32_t sig) { return raiseSelf(x, sig); }
int32_t g_kill(Ctx& x, int32_t pid, int32_t sig) {
    if (pid == 0 || pid == 1000 || pid == -1 || pid == int32_t(callIos(x, "_getpid", {}))) return raiseSelf(x, sig);
    setLinuxErrno(x, 3);   // ESRCH: no other processes
    return -1;
}
int32_t g_pthreadKill(Ctx& x, uint32_t thread, int32_t sig) {
    GuestThread* t = x.proc.findThread(thread);
    if (!t) return 3;
    if (t == &x.proc.currentThread()) { const int32_t r = raiseSelf(x, sig); return r ? 22 : 0; }
    return sendTo(x.proc, t->id, sig);
}
int32_t g_tkill(Ctx& x, int32_t tid, int32_t sig) {
    if (tid == int32_t(tidOf(x.proc.currentThread()))) return raiseSelf(x, sig);
    const int e = tid > 1000 ? sendTo(x.proc, uint32_t(tid - 1000), sig) : 3;
    if (e) { setLinuxErrno(x, e); return -1; }
    return 0;
}
int32_t g_tgkill(Ctx& x, int32_t, int32_t tid, int32_t sig) { return g_tkill(x, tid, sig); }

// Mask changes: SIG_BLOCK 0, SIG_UNBLOCK 1, SIG_SETMASK 2. Returns 0 or a Linux errno.
int changeMask(Ctx& x, int32_t how, GuestPtr set, GuestPtr old) {
    if (set && (how < 0 || how > 2)) return 22;
    auto& s = sigs(x.proc);
    GuestThread& gt = x.proc.currentThread();
    std::lock_guard<std::mutex> lk(s.mu);
    Signals::Thread& t = s.threads[gt.id];
    if (old) x.mem.write<uint32_t>(old, uint32_t(t.blocked));
    if (set) {
        const uint64_t v = x.mem.read<uint32_t>(set);
        if (how == 0) t.blocked |= v;
        else if (how == 1) t.blocked &= ~v;
        else t.blocked = (t.blocked & ~0xFFFFFFFFull) | v;
        t.blocked &= ~kUnblockable;
        kick(x.proc, gt.id, t);
    }
    return 0;
}
int32_t g_pthreadSigmask(Ctx& x, int32_t how, GuestPtr set, GuestPtr old) { return changeMask(x, how, set, old); }
int32_t g_sigprocmask(Ctx& x, int32_t how, GuestPtr set, GuestPtr old) {
    if (const int e = changeMask(x, how, set, old)) { setLinuxErrno(x, e); return -1; }
    return 0;
}
int32_t g_sigpending(Ctx& x, GuestPtr set) {
    auto& s = sigs(x.proc);
    std::lock_guard<std::mutex> lk(s.mu);
    const Signals::Thread& t = s.threads[x.proc.currentThread().id];
    x.mem.write<uint32_t>(set, uint32_t(t.pending & t.blocked));
    return 0;
}
// Waits with `mask` blocked until a signal comes, runs its handler, and returns EINTR.
int32_t g_sigsuspend(Ctx& x, GuestPtr maskPtr) {
    auto& s = sigs(x.proc);
    GuestThread& gt = x.proc.currentThread();
    const uint64_t mask = (maskPtr ? x.mem.read<uint32_t>(maskPtr) : 0) & ~kUnblockable;
    uint64_t saved;
    {
        std::unique_lock<std::mutex> lk(s.mu);
        Signals::Thread& t = s.threads[gt.id];
        saved = t.blocked;
        t.blocked = (t.blocked & ~0xFFFFFFFFull) | mask;
        BlockedScope blocked(x.proc, "sigsuspend");
        while (!(s.threads[gt.id].pending & ~s.threads[gt.id].blocked)) {
            s.cv.wait_for(lk, std::chrono::milliseconds(20));
            if (x.proc.stopping()) { s.threads[gt.id].blocked = saved; throw ProcessExit(0, "stopping"); }
        }
    }
    deliverPending(x.proc, x.cpu);
    {
        std::lock_guard<std::mutex> lk(s.mu);
        Signals::Thread& t = s.threads[gt.id];
        t.blocked = saved;
        kick(x.proc, gt.id, t);   // anything the old mask lets through comes on return
    }
    setLinuxErrno(x, 4);   // EINTR
    return -1;
}
int32_t g_sigemptyset(Ctx& x, GuestPtr set) { x.mem.write<uint32_t>(set, 0); return 0; }
int32_t g_sigfillset(Ctx& x, GuestPtr set) { x.mem.write<uint32_t>(set, 0xFFFFFFFFu); return 0; }
int32_t g_sigaddset(Ctx& x, GuestPtr set, int32_t sig) {
    if (sig <= 0 || sig > 32) { setLinuxErrno(x, 22); return -1; }
    x.mem.write<uint32_t>(set, x.mem.read<uint32_t>(set) | uint32_t(bit(sig)));
    return 0;
}
int32_t g_sigdelset(Ctx& x, GuestPtr set, int32_t sig) {
    if (sig <= 0 || sig > 32) { setLinuxErrno(x, 22); return -1; }
    x.mem.write<uint32_t>(set, x.mem.read<uint32_t>(set) & ~uint32_t(bit(sig)));
    return 0;
}
int32_t g_sigismember(Ctx& x, GuestPtr set, int32_t sig) {
    if (sig <= 0 || sig > 32) { setLinuxErrno(x, 22); return -1; }
    return (x.mem.read<uint32_t>(set) & uint32_t(bit(sig))) ? 1 : 0;
}

} // namespace

int sendSignalToTid(Process& p, CPU::Cpu& self, uint32_t tid, int sig) {
    GuestThread& me = p.currentThread();
    if (&me.cpu == &self && tid == 1000 + me.id) {
        Ctx x{p, self, p.mem()};
        return g_tkill(x, int32_t(tid), sig) == 0 ? 0 : 22;
    }
    return tid > 1000 ? sendTo(p, tid - 1000, sig) : 3;
}

void registerSignals(Process& p) {
#define R(F, NAME, WHAT) supportDef<&F>(p, std::string("android:") + NAME, SupportStatus::Real, WHAT)
    R(g_sigaction, "sigaction", "Linux numbers and flags; faults delivered by the iOS side");
    R(g_signal, "signal", "sigaction with SA_RESTART");
    R(g_signal, "bsd_signal", "sigaction with SA_RESTART");
    R(g_raise, "raise", "handler runs before the call returns");
    R(g_kill, "kill", "this process only");
    R(g_pthreadKill, "pthread_kill", "delivered at the thread's next safepoint");
    R(g_tkill, "tkill", "delivered at the thread's next safepoint");
    R(g_tgkill, "tgkill", "delivered at the thread's next safepoint");
    R(g_pthreadSigmask, "pthread_sigmask", "per-thread Linux mask");
    R(g_sigprocmask, "sigprocmask", "per-thread Linux mask");
    R(g_sigpending, "sigpending", "per-thread pending set");
    R(g_sigsuspend, "sigsuspend", "waits for a signal, runs its handler");
    R(g_sigemptyset, "sigemptyset", "one-word Linux set");
    R(g_sigfillset, "sigfillset", "one-word Linux set");
    R(g_sigaddset, "sigaddset", "one-word Linux set");
    R(g_sigdelset, "sigdelset", "one-word Linux set");
    R(g_sigismember, "sigismember", "one-word Linux set");
#undef R
    p.engine().setSafepointHook([&p](CPU::Cpu& c) { deliverPending(p, c); });
}

} // namespace Hydrogen::Runtime::Android
