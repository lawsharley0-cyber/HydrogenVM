// HydrogenVM — pthreads, OSAtomic and spin locks.
//
// Guest threads are real host threads (see Process::startThread). Guest
// mutexes, condition variables and rwlocks are host objects keyed by the
// guest address of the pthread_*_t, created on first use so statically
// initialised ones (PTHREAD_MUTEX_INITIALIZER) just work. All waits poll the
// process stop flag so quitting an app never hangs on a guest lock.
#include "runtime/Process.hpp"
#include "MachPorts.hpp"
#include <cerrno>
#include <chrono>
#include <thread>
#include <unordered_set>

namespace Hydrogen::Runtime {
namespace {

using namespace std::chrono_literals;

// Darwin errno values (guest code compares against these, whatever the host is).
constexpr int D_EPERM = 1, D_ESRCH = 3, D_EBUSY = 16, D_EDEADLK = 11, D_ETIMEDOUT = 60;

constexpr uint32_t kSigMutexNormal = 0x32AAABA7, kSigMutexErrorCheck = 0x32AAABA1, kSigMutexRecursive = 0x32AAABA2;
constexpr int kNormal = 0, kErrorCheck = 1, kRecursive = 2;

struct GuestMutex {
    std::mutex m;
    std::condition_variable cv;
    uint32_t owner = 0;
    int count = 0;
    int type = kNormal;
};

struct GuestCond {
    std::mutex m;
    std::condition_variable cv;
    uint64_t gen = 0;
};

struct GuestRwlock {
    std::mutex m;
    std::condition_variable cv;
    int readers = 0;
    uint32_t writer = 0;
};

struct Threads {
    explicit Threads(Process&) {}
    std::mutex mu;
    std::unordered_map<uint32_t, std::unique_ptr<GuestMutex>> mutexes;
    std::unordered_map<uint32_t, std::unique_ptr<GuestCond>> conds;
    std::unordered_map<uint32_t, std::unique_ptr<GuestRwlock>> rwlocks;
    std::mutex onceMu;
    std::condition_variable onceCv;
    std::unordered_map<uint32_t, int> once;   // 1 running, 2 done
    uint32_t nextKey = 0x100;                 // low keys are reserved by the system
    std::unordered_map<uint32_t, uint32_t> keyDestructors;
    std::unordered_set<uint32_t> cancelled, cancelDisabled;
};

Threads& T(Ctx& x) { return x.proc.service<Threads>(); }
inline uint32_t tid(Ctx& x) { return x.proc.currentThread().id; }
void t_exit(Ctx&, GuestPtr);
bool cancelPending(Ctx& x) {
    auto& state = T(x); std::lock_guard lock(state.mu);
    const auto self = x.proc.currentThread().self;
    return state.cancelled.count(self) && !state.cancelDisabled.count(self);
}
void t_testcancel(Ctx& x) { if (cancelPending(x)) t_exit(x, 0xffffffffu); }
inline void checkStop(Ctx& x) { if (x.proc.stopping()) throw ProcessExit(0, "stopping"); }

GuestMutex& mutexFor(Ctx& x, uint32_t addr) {
    auto& t = T(x);
    std::lock_guard<std::mutex> lk(t.mu);
    auto& p = t.mutexes[addr];
    if (!p) {
        p = std::make_unique<GuestMutex>();
        const uint32_t sig = x.mem.read<uint32_t>(addr);
        if (sig == kSigMutexRecursive) p->type = kRecursive;
        else if (sig == kSigMutexErrorCheck) p->type = kErrorCheck;
    }
    return *p;
}

int lockMutex(Ctx& x, GuestMutex& m, bool tryOnly) {
    const uint32_t me = tid(x);
    std::unique_lock<std::mutex> lk(m.m);
    if (m.owner == me) {
        if (m.type == kRecursive) { ++m.count; return 0; }
        if (m.type == kErrorCheck || tryOnly) return tryOnly ? D_EBUSY : D_EDEADLK;
    }
    if (m.owner && tryOnly) return D_EBUSY;
    while (m.owner) { m.cv.wait_for(lk, 50ms); if (m.owner) checkStop(x); }
    m.owner = me;
    m.count = 1;
    return 0;
}
int unlockMutex(Ctx& x, GuestMutex& m) {
    std::lock_guard<std::mutex> lk(m.m);
    if (m.owner != tid(x)) return D_EPERM;
    if (--m.count == 0) { m.owner = 0; m.cv.notify_one(); }
    return 0;
}

// ---------------------------------------------------------------- mutex ---

int32_t pm_init(Ctx& x, GuestPtr mu, GuestPtr attr) {
    auto& t = T(x);
    auto m = std::make_unique<GuestMutex>();
    if (attr) {
        const uint32_t type = x.mem.read<uint32_t>(attr + 4);
        m->type = type == 2 ? kRecursive : type == 1 ? kErrorCheck : kNormal;
    }
    x.mem.write<uint32_t>(mu, m->type == kRecursive ? kSigMutexRecursive : kSigMutexNormal);
    std::lock_guard<std::mutex> lk(t.mu);
    t.mutexes[mu] = std::move(m);
    return 0;
}
int32_t pm_lock(Ctx& x, GuestPtr mu) { return lockMutex(x, mutexFor(x, mu), false); }
int32_t pm_trylock(Ctx& x, GuestPtr mu) { return lockMutex(x, mutexFor(x, mu), true); }
int32_t pm_unlock(Ctx& x, GuestPtr mu) { return unlockMutex(x, mutexFor(x, mu)); }
int32_t pm_destroy(Ctx& x, GuestPtr mu) {
    auto& t = T(x);
    std::lock_guard<std::mutex> lk(t.mu);
    t.mutexes.erase(mu);
    return 0;
}
int32_t pma_init(Ctx& x, GuestPtr a) { x.mem.write<uint32_t>(a, 0x4D555458); x.mem.write<uint32_t>(a + 4, 0); return 0; }
int32_t pma_settype(Ctx& x, GuestPtr a, int32_t type) { x.mem.write<uint32_t>(a + 4, uint32_t(type)); return 0; }
int32_t pma_gettype(Ctx& x, GuestPtr a, GuestPtr out) { x.mem.write<uint32_t>(out, x.mem.read<uint32_t>(a + 4)); return 0; }
int32_t pma_ok(Ctx&, GuestPtr) { return 0; }
int32_t pma_ok2(Ctx&, GuestPtr, int32_t) { return 0; }

// ------------------------------------------------------------- condvars ---

GuestCond& condFor(Ctx& x, uint32_t addr) {
    auto& t = T(x);
    std::lock_guard<std::mutex> lk(t.mu);
    auto& p = t.conds[addr];
    if (!p) p = std::make_unique<GuestCond>();
    return *p;
}

// deadline: 0 = none, else nanoseconds since the Unix epoch.
int condWait(Ctx& x, GuestPtr cv, GuestPtr mu, int64_t deadlineNs) {
    t_testcancel(x);
    GuestCond& c = condFor(x, cv);
    GuestMutex& m = mutexFor(x, mu);
    uint64_t gen;
    { std::lock_guard<std::mutex> lk(c.m); gen = c.gen; }
    // Fully release the mutex (it may be recursively held).
    int saved;
    {
        std::lock_guard<std::mutex> lk(m.m);
        if (m.owner != tid(x)) return D_EPERM;
        saved = m.count;
        m.owner = 0; m.count = 0;
        m.cv.notify_one();
    }
    int result = 0;
    {
        std::unique_lock<std::mutex> lk(c.m);
        while (c.gen == gen) {
            c.cv.wait_for(lk, 50ms);
            if (c.gen != gen) break;
            if (x.proc.stopping() || cancelPending(x)) { result = -1; break; }
            if (deadlineNs) {
                const int64_t now = std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
                if (now >= deadlineNs) { result = D_ETIMEDOUT; break; }
            }
        }
    }
    lockMutex(x, m, false);
    { std::lock_guard<std::mutex> lk(m.m); m.count = saved; }
    t_testcancel(x); // POSIX requires the mutex to be reacquired before cancellation.
    if (result < 0) checkStop(x);
    return result;
}

int32_t pc_init(Ctx& x, GuestPtr cv, GuestPtr) { condFor(x, cv); x.mem.write<uint32_t>(cv, 0x3CB0B1BB); return 0; }
int32_t pc_wait(Ctx& x, GuestPtr cv, GuestPtr mu) { return condWait(x, cv, mu, 0); }
int32_t pc_timedwait(Ctx& x, GuestPtr cv, GuestPtr mu, GuestPtr ts) {
    const int64_t s = x.mem.read<int32_t>(ts), ns = x.mem.read<int32_t>(ts + 4);
    return condWait(x, cv, mu, s * 1000000000LL + ns + 1);
}
int32_t pc_timedwait_rel(Ctx& x, GuestPtr cv, GuestPtr mu, GuestPtr ts) {
    const int64_t s = x.mem.read<int32_t>(ts), ns = x.mem.read<int32_t>(ts + 4);
    const int64_t now = std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
    return condWait(x, cv, mu, now + s * 1000000000LL + ns + 1);
}
int32_t pc_signal(Ctx& x, GuestPtr cv) { GuestCond& c = condFor(x, cv); { std::lock_guard<std::mutex> lk(c.m); ++c.gen; } c.cv.notify_all(); return 0; }
int32_t pc_destroy(Ctx& x, GuestPtr cv) { auto& t = T(x); std::lock_guard<std::mutex> lk(t.mu); t.conds.erase(cv); return 0; }
int32_t pca_ok(Ctx&, GuestPtr) { return 0; }

// --------------------------------------------------------------- rwlock ---

GuestRwlock& rwFor(Ctx& x, uint32_t addr) {
    auto& t = T(x);
    std::lock_guard<std::mutex> lk(t.mu);
    auto& p = t.rwlocks[addr];
    if (!p) p = std::make_unique<GuestRwlock>();
    return *p;
}
int32_t rw_init(Ctx& x, GuestPtr rw, GuestPtr) { rwFor(x, rw); return 0; }
int32_t rw_rdlock(Ctx& x, GuestPtr rw) {
    GuestRwlock& l = rwFor(x, rw);
    std::unique_lock<std::mutex> lk(l.m);
    while (l.writer) { l.cv.wait_for(lk, 50ms); if (l.writer) checkStop(x); }
    ++l.readers;
    return 0;
}
int32_t rw_tryrdlock(Ctx& x, GuestPtr rw) { GuestRwlock& l = rwFor(x, rw); std::lock_guard<std::mutex> lk(l.m); if (l.writer) return D_EBUSY; ++l.readers; return 0; }
int32_t rw_wrlock(Ctx& x, GuestPtr rw) {
    GuestRwlock& l = rwFor(x, rw);
    std::unique_lock<std::mutex> lk(l.m);
    while (l.writer || l.readers) { l.cv.wait_for(lk, 50ms); if (l.writer || l.readers) checkStop(x); }
    l.writer = tid(x);
    return 0;
}
int32_t rw_trywrlock(Ctx& x, GuestPtr rw) { GuestRwlock& l = rwFor(x, rw); std::lock_guard<std::mutex> lk(l.m); if (l.writer || l.readers) return D_EBUSY; l.writer = tid(x); return 0; }
int32_t rw_unlock(Ctx& x, GuestPtr rw) {
    GuestRwlock& l = rwFor(x, rw);
    std::lock_guard<std::mutex> lk(l.m);
    if (l.writer == tid(x)) l.writer = 0;
    else if (l.readers) --l.readers;
    l.cv.notify_all();
    return 0;
}
int32_t rw_destroy(Ctx& x, GuestPtr rw) { auto& t = T(x); std::lock_guard<std::mutex> lk(t.mu); t.rwlocks.erase(rw); return 0; }

// ----------------------------------------------------------------- once ---

int32_t p_once(Ctx& x, GuestPtr once, GuestPtr fn) {
    auto& t = T(x);
    {
        std::unique_lock<std::mutex> lk(t.onceMu);
        // The guest word after the signature is our "done" flag, so a
        // pthread_once_t that's reused after being reset behaves correctly.
        if (x.mem.read<uint32_t>(once + 4) == 2) return 0;
        while (t.once[once] == 1) { t.onceCv.wait_for(lk, 50ms); checkStop(x); }
        if (x.mem.read<uint32_t>(once + 4) == 2) return 0;
        t.once[once] = 1;
    }
    x.proc.callGuest(x.cpu, fn, {});
    {
        std::lock_guard<std::mutex> lk(t.onceMu);
        t.once[once] = 2;
        x.mem.write<uint32_t>(once + 4, 2);
    }
    t.onceCv.notify_all();
    return 0;
}

// ----------------------------------------------------------------- keys ---

int32_t k_create(Ctx& x, GuestPtr out, GuestPtr dtor) {
    auto& t = T(x);
    std::lock_guard<std::mutex> lk(t.mu);
    const uint32_t k = t.nextKey++;
    t.keyDestructors[k] = dtor;
    x.mem.write<uint32_t>(out, k);
    return 0;
}
int32_t k_delete(Ctx& x, uint32_t key) { auto& t = T(x); std::lock_guard<std::mutex> lk(t.mu); t.keyDestructors.erase(key); return 0; }
uint32_t k_get(Ctx& x, uint32_t key) {
    auto& tsd = x.proc.currentThread().tsd;
    auto it = tsd.find(key);
    return it == tsd.end() ? 0 : it->second;
}
int32_t k_set(Ctx& x, uint32_t key, GuestPtr v) { x.proc.currentThread().tsd[key] = v; return 0; }

// -------------------------------------------------------------- threads ---

// Our pthread_attr_t layout: sig, stacksize, detachstate.
int32_t pa_init(Ctx& x, GuestPtr a) { x.mem.write<uint32_t>(a, 0x54484441); x.mem.write<uint32_t>(a + 4, 512u << 10); x.mem.write<uint32_t>(a + 8, 1); return 0; }
int32_t pa_destroy(Ctx&, GuestPtr) { return 0; }
int32_t pa_setstacksize(Ctx& x, GuestPtr a, uint32_t s) { x.mem.write<uint32_t>(a + 4, s); return 0; }
int32_t pa_getstacksize(Ctx& x, GuestPtr a, GuestPtr out) { x.mem.write<uint32_t>(out, x.mem.read<uint32_t>(a + 4)); return 0; }
int32_t pa_setdetachstate(Ctx& x, GuestPtr a, int32_t s) { x.mem.write<uint32_t>(a + 8, uint32_t(s)); return 0; }
int32_t pa_getdetachstate(Ctx& x, GuestPtr a, GuestPtr out) { x.mem.write<uint32_t>(out, x.mem.read<uint32_t>(a + 8)); return 0; }
int32_t pa_ignore2(Ctx&, GuestPtr, GuestPtr) { return 0; }
int32_t pa_ignore_int(Ctx&, GuestPtr, int32_t) { return 0; }

int32_t t_create(Ctx& x, GuestPtr out, GuestPtr attr, GuestPtr fn, GuestPtr arg) {
    uint32_t stack = 512u << 10;
    bool detached = false;
    if (attr && x.mem.read<uint32_t>(attr) == 0x54484441) {
        stack = x.mem.read<uint32_t>(attr + 4);
        detached = x.mem.read<uint32_t>(attr + 8) == 2;   // PTHREAD_CREATE_DETACHED
    }
    const uint32_t self = x.proc.startThread(fn, arg, stack, detached);
    if (out) x.mem.write<uint32_t>(out, self);
    return 0;
}
int32_t t_join(Ctx& x, uint32_t th, GuestPtr ret) {
    uint32_t r = 0;
    if (!x.proc.joinThread(th, &r)) return D_ESRCH;
    if (ret) x.mem.write<uint32_t>(ret, r);
    return 0;
}
int32_t t_cancel(Ctx& x, uint32_t self) {
    auto* thread = x.proc.findThread(self);
    if (!thread || thread->finished) return D_ESRCH;
    auto& state = T(x); std::lock_guard lock(state.mu);
    state.cancelled.insert(self); return 0;
}
int32_t t_cancelstate(Ctx& x, int32_t disabled, GuestPtr old) {
    if (disabled != 0 && disabled != 1) return 22;
    auto& state = T(x); std::lock_guard lock(state.mu);
    const auto self = x.proc.currentThread().self;
    if (old) x.mem.write<uint32_t>(old, state.cancelDisabled.count(self) ? 1 : 0);
    if (disabled) state.cancelDisabled.insert(self); else state.cancelDisabled.erase(self);
    return 0;
}
int32_t t_canceltype(Ctx& x, int32_t type, GuestPtr old) {
    // Cancellation is deferred at testcancel and condition waits; asynchronous host unwinding is unsafe.
    if (type != 0) return type == 1 ? 45 : 22;
    if (old) x.mem.write<uint32_t>(old, 0);
    return 0;
}
int32_t t_detach(Ctx& x, uint32_t th) { x.proc.detachThread(th); return 0; }
uint32_t t_self(Ctx& x) { return x.proc.currentThread().self; }
int32_t t_equal(Ctx&, uint32_t a, uint32_t b) { return a == b; }
int32_t t_main_np(Ctx& x) { return &x.proc.currentThread() == &x.proc.mainThread(); }
int32_t t_setname_np(Ctx&, GuestPtr) { return 0; }
int32_t t_kill(Ctx&, uint32_t, int32_t) { return 0; }
int32_t t_sigmask(Ctx&, int32_t, GuestPtr, GuestPtr) { return 0; }
int32_t t_getschedparam(Ctx& x, uint32_t, GuestPtr policy, GuestPtr param) {
    if (policy) x.mem.write<uint32_t>(policy, 1);
    if (param) x.mem.write<uint32_t>(param, 31);
    return 0;
}
int32_t t_setschedparam(Ctx&, uint32_t, int32_t, GuestPtr) { return 0; }
int32_t sched_prio(Ctx&, int32_t) { return 31; }
int32_t sched_yield_(Ctx&) { std::this_thread::yield(); return 0; }
uint32_t t_mach_thread_self(Ctx& x) { return acquireMachSendRight(x.proc, x.proc.currentThread().id + 0x1000); }
uint32_t t_pthread_mach_thread_np(Ctx& x, uint32_t th) {
    GuestThread* t = x.proc.findThread(th);
    if (!t) return 0;
    const uint32_t port = t->id + 0x1000;
    // This API borrows the pthread's port rather than adding a user reference.
    if (!hasMachSendRight(x.proc, port)) acquireMachSendRight(x.proc, port);
    return port;
}
int32_t t_pthread_threadid_np(Ctx& x, uint32_t th, GuestPtr outId) {
    if (!outId) return EINVAL;
    GuestThread* t = th ? x.proc.findThread(th) : &x.proc.currentThread();
    const uint64_t id = t ? uint64_t(t->id) : uint64_t(x.proc.currentThread().id);
    x.mem.write<uint64_t>(outId, id);
    return 0;
}

// Mach thread control (garbage collectors stop the world with these).
constexpr int32_t kKernSuccess = 0, kKernInvalidArgument = 4;
GuestThread* byPort(Ctx& x, uint32_t port) { return port >= 0x1000 && hasMachSendRight(x.proc, port) ? x.proc.findThreadById(port - 0x1000) : nullptr; }
uint32_t t_pthread_from_mach_thread_np(Ctx& x, uint32_t port) {
    GuestThread* t = byPort(x, port);
    return t ? t->self : 0;
}
int32_t mt_suspend(Ctx& x, uint32_t port) {
    GuestThread* t = byPort(x, port);
    if (!t) return kKernInvalidArgument;
    if (t == &x.proc.currentThread() || t->finished) return kKernSuccess;   // never park ourselves inside a host call; finished thread is already stopped
    x.proc.engine().suspend(t->cpu);
    return kKernSuccess;
}
int32_t mt_resume(Ctx& x, uint32_t port) {
    GuestThread* t = byPort(x, port);
    if (!t) return kKernInvalidArgument;
    if (t != &x.proc.currentThread() && !t->finished) x.proc.engine().resume(t->cpu);
    return kKernSuccess;
}
// thread_get_state(thread, flavor, state, count*): ARM_THREAD_STATE (1) = r0-r12, sp, lr, pc, cpsr;
// ARM_VFP_STATE (2) = 64 words + fpscr; ARM_EXCEPTION_STATE (3) = 3 words.
int32_t mt_getState(Ctx& x, uint32_t port, int32_t flavor, GuestPtr st, GuestPtr countPtr) {
    GuestThread* t = byPort(x, port);
    if (!t) return kKernInvalidArgument;
    const CPU::Cpu& c = t->cpu;
    const uint32_t have = x.mem.read<uint32_t>(countPtr);
    if (flavor == 1) {
        if (have < 17) return kKernInvalidArgument;
        for (uint32_t i = 0; i < 16; ++i) x.mem.write<uint32_t>(st + i * 4, c.r[i]);
        x.mem.write<uint32_t>(st + 60, c.r[CPU::kPC] | (c.thumb ? 1 : 0));
        x.mem.write<uint32_t>(st + 64, c.cpsr());
        x.mem.write<uint32_t>(countPtr, 17);
        return kKernSuccess;
    }
    if (flavor == 2) {
        if (have < 33) return kKernInvalidArgument;
        const uint32_t n = std::min<uint32_t>(have, 65);
        for (uint32_t i = 0; i + 1 < n; ++i) x.mem.write<uint32_t>(st + i * 4, uint32_t(c.d[i / 2] >> ((i & 1) * 32)));
        x.mem.write<uint32_t>(st + (n - 1) * 4, c.fpscr);
        x.mem.write<uint32_t>(countPtr, n);
        return kKernSuccess;
    }
    if (flavor == 3) {
        if (have < 3) return kKernInvalidArgument;
        for (uint32_t i = 0; i < 3; ++i) x.mem.write<uint32_t>(st + i * 4, 0);
        x.mem.write<uint32_t>(countPtr, 3);
        return kKernSuccess;
    }
    return kKernInvalidArgument;
}
// thread_set_state for ARM_THREAD_STATE: only meaningful on a suspended thread.
int32_t mt_setState(Ctx& x, uint32_t port, int32_t flavor, GuestPtr st, uint32_t count) {
    GuestThread* t = byPort(x, port);
    if (!t || flavor != 1 || count < 17) return kKernInvalidArgument;
    for (uint32_t i = 0; i < 15; ++i) t->cpu.r[i] = x.mem.read<uint32_t>(st + i * 4);
    const uint32_t pc = x.mem.read<uint32_t>(st + 60), cpsr = x.mem.read<uint32_t>(st + 64);
    t->cpu.thumb = ((cpsr >> 5) & 1) | (pc & 1);
    t->cpu.r[CPU::kPC] = pc & ~1u;
    t->cpu.setApsrFlags(cpsr);
    return kKernSuccess;
}
// thread_info(thread, THREAD_BASIC_INFO=3, info, count*): mostly "running, not suspended".
// task_threads(task, &list, &count): a kernel-allocated array of thread ports.
int32_t mt_taskThreads(Ctx& x, uint32_t task, GuestPtr listOut, GuestPtr countOut) {
    if (task != kTaskPort || !hasMachSendRight(x.proc, task)) return kKernInvalidArgument;
    const std::vector<uint32_t> ids = x.proc.liveThreadIds();
    const uint32_t arr = x.proc.heap().alloc(uint32_t(ids.size()) * 4 + 4);
    for (uint32_t i = 0; i < ids.size(); ++i) x.mem.write<uint32_t>(arr + i * 4, acquireMachSendRight(x.proc, ids[i] + 0x1000));
    x.mem.write<uint32_t>(listOut, arr);
    x.mem.write<uint32_t>(countOut, uint32_t(ids.size()));
    return kKernSuccess;
}
int32_t mt_info(Ctx& x, uint32_t port, int32_t flavor, GuestPtr info, GuestPtr countPtr) {
    GuestThread* t = byPort(x, port);
    if (!t) return kKernInvalidArgument;
    if (flavor != 3) return kKernInvalidArgument;
    const uint32_t n = std::min<uint32_t>(x.mem.read<uint32_t>(countPtr), 10);
    for (uint32_t i = 0; i < n; ++i) x.mem.write<uint32_t>(info + i * 4, 0);
    // {user_time[2], system_time[2], cpu_usage, policy, run_state, flags, suspend_count, sleep_time}
    if (n > 6) x.mem.write<uint32_t>(info + 24, t->cpu.suspend_count.load() ? 2 : 1);   // TH_STATE_STOPPED / RUNNING
    if (n > 8) x.mem.write<uint32_t>(info + 32, t->cpu.suspend_count.load());
    x.mem.write<uint32_t>(countPtr, n);
    return kKernSuccess;
}

struct ThreadExit : std::runtime_error { ThreadExit() : std::runtime_error("pthread_exit") {} };
void t_exit(Ctx& x, GuestPtr value) {
    GuestThread& t = x.proc.currentThread();
    t.retval = value;
    if (&t == &x.proc.mainThread()) x.proc.exit(0, "main thread called pthread_exit");
    t.pthreadExited = true;
    throw ThreadExit();
}

// ------------------------------------------------------------- OSAtomic ---

template <typename T> T* A(Ctx& x, GuestPtr p) { return reinterpret_cast<T*>(x.mem.host(p)); }

int32_t oa_add32(Ctx& x, int32_t v, GuestPtr p) { return __atomic_add_fetch(A<int32_t>(x, p), v, __ATOMIC_SEQ_CST); }
int32_t oa_inc32(Ctx& x, GuestPtr p) { return __atomic_add_fetch(A<int32_t>(x, p), 1, __ATOMIC_SEQ_CST); }
int32_t oa_dec32(Ctx& x, GuestPtr p) { return __atomic_sub_fetch(A<int32_t>(x, p), 1, __ATOMIC_SEQ_CST); }
int32_t oa_or32(Ctx& x, uint32_t v, GuestPtr p) { return int32_t(__atomic_or_fetch(A<uint32_t>(x, p), v, __ATOMIC_SEQ_CST)); }
int32_t oa_and32(Ctx& x, uint32_t v, GuestPtr p) { return int32_t(__atomic_and_fetch(A<uint32_t>(x, p), v, __ATOMIC_SEQ_CST)); }
int32_t oa_xor32(Ctx& x, uint32_t v, GuestPtr p) { return int32_t(__atomic_xor_fetch(A<uint32_t>(x, p), v, __ATOMIC_SEQ_CST)); }
int32_t oa_or32o(Ctx& x, uint32_t v, GuestPtr p) { return int32_t(__atomic_fetch_or(A<uint32_t>(x, p), v, __ATOMIC_SEQ_CST)); }
int32_t oa_and32o(Ctx& x, uint32_t v, GuestPtr p) { return int32_t(__atomic_fetch_and(A<uint32_t>(x, p), v, __ATOMIC_SEQ_CST)); }
int32_t oa_xor32o(Ctx& x, uint32_t v, GuestPtr p) { return int32_t(__atomic_fetch_xor(A<uint32_t>(x, p), v, __ATOMIC_SEQ_CST)); }
bool oa_cas32(Ctx& x, int32_t o, int32_t n, GuestPtr p) { return __atomic_compare_exchange_n(A<int32_t>(x, p), &o, n, false, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST); }
int64_t oa_add64(Ctx& x, int64_t v, GuestPtr p) { return __atomic_add_fetch(A<int64_t>(x, p), v, __ATOMIC_SEQ_CST); }
int64_t oa_inc64(Ctx& x, GuestPtr p) { return __atomic_add_fetch(A<int64_t>(x, p), 1, __ATOMIC_SEQ_CST); }
int64_t oa_dec64(Ctx& x, GuestPtr p) { return __atomic_sub_fetch(A<int64_t>(x, p), 1, __ATOMIC_SEQ_CST); }
bool oa_cas64(Ctx& x, int64_t o, int64_t n, GuestPtr p) { return __atomic_compare_exchange_n(A<int64_t>(x, p), &o, n, false, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST); }
bool oa_testset(Ctx& x, uint32_t n, GuestPtr p) {
    uint8_t* b = A<uint8_t>(x, p + (n >> 3));
    const uint8_t mask = uint8_t(0x80 >> (n & 7));
    return (__atomic_fetch_or(b, mask, __ATOMIC_SEQ_CST) & mask) != 0;
}
bool oa_testclear(Ctx& x, uint32_t n, GuestPtr p) {
    uint8_t* b = A<uint8_t>(x, p + (n >> 3));
    const uint8_t mask = uint8_t(0x80 >> (n & 7));
    return (__atomic_fetch_and(b, uint8_t(~mask), __ATOMIC_SEQ_CST) & mask) != 0;
}
void oa_barrier(Ctx&) { __atomic_thread_fence(__ATOMIC_SEQ_CST); }

void spin_lock(Ctx& x, GuestPtr p) {
    int32_t* l = A<int32_t>(x, p);
    for (uint32_t spins = 0;; ++spins) {
        int32_t z = 0;
        if (__atomic_compare_exchange_n(l, &z, -1, false, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED)) return;
        if (spins > 64) { std::this_thread::sleep_for(50us); if ((spins & 1023) == 0) checkStop(x); }
        else std::this_thread::yield();
    }
}
bool spin_try(Ctx& x, GuestPtr p) { int32_t z = 0; return __atomic_compare_exchange_n(A<int32_t>(x, p), &z, -1, false, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED); }
void spin_unlock(Ctx& x, GuestPtr p) { __atomic_store_n(A<int32_t>(x, p), 0, __ATOMIC_RELEASE); }

} // namespace

void registerLibcPthread(Process& p) {
    p.def<&t_cancel>("_pthread_cancel");
    p.def<&t_testcancel>("_pthread_testcancel");
    p.def<&t_cancelstate>("_pthread_setcancelstate");
    p.def<&t_canceltype>("_pthread_setcanceltype");
    p.def<&pm_init>("_pthread_mutex_init");
    p.def<&pm_lock>("_pthread_mutex_lock");
    p.def<&pm_trylock>("_pthread_mutex_trylock");
    p.def<&pm_unlock>("_pthread_mutex_unlock");
    p.def<&pm_destroy>("_pthread_mutex_destroy");
    p.def<&pma_init>("_pthread_mutexattr_init");
    p.def<&pma_settype>("_pthread_mutexattr_settype");
    p.def<&pma_gettype>("_pthread_mutexattr_gettype");
    p.def<&pma_ok>("_pthread_mutexattr_destroy");
    p.def<&pma_ok2>("_pthread_mutexattr_setpshared");
    p.def<&pma_ok2>("_pthread_mutexattr_setprotocol");
    p.def<&pc_init>("_pthread_cond_init");
    p.def<&pc_wait>("_pthread_cond_wait");
    p.def<&pc_timedwait>("_pthread_cond_timedwait");
    p.def<&pc_timedwait_rel>("_pthread_cond_timedwait_relative_np");
    p.def<&pc_signal>("_pthread_cond_signal");
    p.def<&pc_signal>("_pthread_cond_broadcast");
    p.def<&pc_destroy>("_pthread_cond_destroy");
    p.def<&pca_ok>("_pthread_condattr_init");
    p.def<&pca_ok>("_pthread_condattr_destroy");
    p.def<&rw_init>("_pthread_rwlock_init");
    p.def<&rw_rdlock>("_pthread_rwlock_rdlock");
    p.def<&rw_tryrdlock>("_pthread_rwlock_tryrdlock");
    p.def<&rw_wrlock>("_pthread_rwlock_wrlock");
    p.def<&rw_trywrlock>("_pthread_rwlock_trywrlock");
    p.def<&rw_unlock>("_pthread_rwlock_unlock");
    p.def<&rw_destroy>("_pthread_rwlock_destroy");
    p.def<&p_once>("_pthread_once");
    p.def<&k_create>("_pthread_key_create");
    p.def<&k_delete>("_pthread_key_delete");
    p.def<&k_get>("_pthread_getspecific");
    p.def<&k_set>("_pthread_setspecific");
    p.def<&pa_init>("_pthread_attr_init");
    p.def<&pa_destroy>("_pthread_attr_destroy");
    p.def<&pa_setstacksize>("_pthread_attr_setstacksize");
    p.def<&pa_getstacksize>("_pthread_attr_getstacksize");
    p.def<&pa_setdetachstate>("_pthread_attr_setdetachstate");
    p.def<&pa_getdetachstate>("_pthread_attr_getdetachstate");
    p.def<&pa_ignore2>("_pthread_attr_setschedparam");
    p.def<&pa_ignore2>("_pthread_attr_getschedparam");
    p.def<&pa_ignore_int>("_pthread_attr_setschedpolicy");
    p.def<&pa_ignore_int>("_pthread_attr_setscope");
    p.def<&pa_ignore_int>("_pthread_attr_setinheritsched");
    p.def<&t_create>("_pthread_create");
    p.def<&t_join>("_pthread_join");
    p.def<&t_detach>("_pthread_detach");
    p.def<&t_self>("_pthread_self");
    p.def<&t_equal>("_pthread_equal");
    p.def<&t_main_np>("_pthread_main_np");
    p.def<&t_setname_np>("_pthread_setname_np");
    p.def<&t_kill>("_pthread_kill");
    p.def<&t_sigmask>("_pthread_sigmask");
    p.def<&t_getschedparam>("_pthread_getschedparam");
    p.def<&t_setschedparam>("_pthread_setschedparam");
    p.def<&sched_prio>("_sched_get_priority_min");
    p.def<&sched_prio>("_sched_get_priority_max");
    p.def<&sched_yield_>("_sched_yield");
    p.def<&sched_yield_>("_pthread_yield_np");
    p.def<&t_mach_thread_self>("_mach_thread_self");
    p.def<&mt_suspend>("_thread_suspend");
    p.def<&mt_resume>("_thread_resume");
    p.def<&mt_getState>("_thread_get_state");
    p.def<&mt_setState>("_thread_set_state");
    p.def<&mt_info>("_thread_info");
    p.def<&mt_taskThreads>("_task_threads");
    p.def<&t_pthread_mach_thread_np>("_pthread_mach_thread_np");
    p.def<&t_pthread_from_mach_thread_np>("_pthread_from_mach_thread_np");
    p.def<&t_pthread_threadid_np>("_pthread_threadid_np");
    p.def<&t_exit>("_pthread_exit");
    p.def<&oa_add32>("_OSAtomicAdd32");
    p.def<&oa_add32>("_OSAtomicAdd32Barrier");
    p.def<&oa_inc32>("_OSAtomicIncrement32");
    p.def<&oa_inc32>("_OSAtomicIncrement32Barrier");
    p.def<&oa_dec32>("_OSAtomicDecrement32");
    p.def<&oa_dec32>("_OSAtomicDecrement32Barrier");
    p.def<&oa_or32>("_OSAtomicOr32");
    p.def<&oa_or32>("_OSAtomicOr32Barrier");
    p.def<&oa_and32>("_OSAtomicAnd32");
    p.def<&oa_and32>("_OSAtomicAnd32Barrier");
    p.def<&oa_xor32>("_OSAtomicXor32");
    p.def<&oa_xor32>("_OSAtomicXor32Barrier");
    p.def<&oa_or32o>("_OSAtomicOr32Orig");
    p.def<&oa_or32o>("_OSAtomicOr32OrigBarrier");
    p.def<&oa_and32o>("_OSAtomicAnd32Orig");
    p.def<&oa_and32o>("_OSAtomicAnd32OrigBarrier");
    p.def<&oa_xor32o>("_OSAtomicXor32Orig");
    p.def<&oa_xor32o>("_OSAtomicXor32OrigBarrier");
    for (const char* n : {"_OSAtomicCompareAndSwap32", "_OSAtomicCompareAndSwap32Barrier", "_OSAtomicCompareAndSwapInt",
                          "_OSAtomicCompareAndSwapIntBarrier", "_OSAtomicCompareAndSwapLong", "_OSAtomicCompareAndSwapLongBarrier",
                          "_OSAtomicCompareAndSwapPtr", "_OSAtomicCompareAndSwapPtrBarrier"})
        p.def<&oa_cas32>(n);
    p.def<&oa_add64>("_OSAtomicAdd64");
    p.def<&oa_add64>("_OSAtomicAdd64Barrier");
    p.def<&oa_inc64>("_OSAtomicIncrement64");
    p.def<&oa_inc64>("_OSAtomicIncrement64Barrier");
    p.def<&oa_dec64>("_OSAtomicDecrement64");
    p.def<&oa_dec64>("_OSAtomicDecrement64Barrier");
    p.def<&oa_cas64>("_OSAtomicCompareAndSwap64");
    p.def<&oa_cas64>("_OSAtomicCompareAndSwap64Barrier");
    p.def<&oa_testset>("_OSAtomicTestAndSet");
    p.def<&oa_testset>("_OSAtomicTestAndSetBarrier");
    p.def<&oa_testclear>("_OSAtomicTestAndClear");
    p.def<&oa_testclear>("_OSAtomicTestAndClearBarrier");
    p.def<&oa_barrier>("_OSMemoryBarrier");
    p.def<&spin_lock>("_OSSpinLockLock");
    p.def<&spin_try>("_OSSpinLockTry");
    p.def<&spin_unlock>("_OSSpinLockUnlock");
    p.def<&spin_lock>("__spin_lock");
    p.def<&spin_unlock>("__spin_unlock");
}

} // namespace Hydrogen::Runtime
