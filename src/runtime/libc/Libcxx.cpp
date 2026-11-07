#include "runtime/Process.hpp"
#include "runtime/Support.hpp"
#include <chrono>
#include <algorithm>
#include <cerrno>
#include <cstring>
#include <functional>
#include <mutex>
#include <string>
#include <stdexcept>
#include <thread>

namespace Hydrogen::Runtime {

void registerLibcxxString(Process& p);

namespace {

uint32_t tld_get(Ctx& x) {
    auto& proc = x.proc;
    // One lock so concurrent first calls create exactly one pthread key.
    static std::mutex tldMutex;
    std::lock_guard<std::mutex> tldLock(tldMutex);

    auto addr = proc.lookupSymbol("__hvm_libcxx_tld_key");
    if (addr) {
        x.cpu.r[0] = *addr;
        return *addr;
    }

    // Allocate 4 bytes in static memory to hold the pthread_key_t
    uint32_t key_addr = proc.allocStatic(4);

    // Call _pthread_key_create
    auto key_create_fn = proc.hostFunctionFor("_pthread_key_create");
    if (!key_create_fn) {
        throw std::runtime_error("_pthread_key_create not found");
    }

    x.cpu.r[0] = key_addr;
    x.cpu.r[1] = 0; // NULL destructor

    key_create_fn(x.cpu); // execute key create
    if (x.cpu.r[0] != 0) {
        throw std::runtime_error(std::string("std::thread local data: pthread_key_create failed: ") + std::strerror(int(x.cpu.r[0])));
    }

    proc.defineData("__hvm_libcxx_tld_key", key_addr);
    x.cpu.r[0] = key_addr;
    return key_addr;
}

uint32_t ts_ctor(Ctx& x, GuestPtr self) {
    return self;
}

void ts_dtor(Ctx& x, GuestPtr self) {
}

void t_dtor(Ctx& x, GuestPtr self) {
    // In libc++, destroying a joinable thread calls std::terminate().
    // std::thread contains pthread_t at offset 0.
    uint32_t th = x.mem.read<uint32_t>(self);
    if (th != 0) {
        throw std::runtime_error("std::terminate() called");
    }
}

[[noreturn]] void thread_error(const char* op, int err) {
    // libc++ throws std::system_error here; the host reports it as a runtime error carrying the errno text.
    throw std::runtime_error(std::string("std::thread::") + op + " failed: " + std::strerror(err));
}

void t_join(Ctx& x, GuestPtr self) {
    uint32_t th = x.mem.read<uint32_t>(self);
    if (th == 0) thread_error("join", EINVAL);
    auto join_fn = x.proc.hostFunctionFor("_pthread_join");
    if (!join_fn) thread_error("join", ENOSYS);
    x.cpu.r[0] = th;
    x.cpu.r[1] = 0;
    join_fn(x.cpu);
    const uint32_t ret = x.cpu.r[0];
    if (ret != 0) thread_error("join", int(ret));
    x.mem.write<uint32_t>(self, 0);
}

void t_detach(Ctx& x, GuestPtr self) {
    uint32_t th = x.mem.read<uint32_t>(self);
    if (th == 0) thread_error("detach", EINVAL);
    auto detach_fn = x.proc.hostFunctionFor("_pthread_detach");
    if (!detach_fn) thread_error("detach", ENOSYS);
    x.cpu.r[0] = th;
    detach_fn(x.cpu);
    const uint32_t ret = x.cpu.r[0];
    if (ret != 0) thread_error("detach", int(ret));
    x.mem.write<uint32_t>(self, 0);
}

// A missing pthread implementation is a runtime error, never a silent no-op.
std::function<void(CPU::Cpu&)> requiredHost(Ctx& x, const char* symbol) {
    auto fn = x.proc.hostFunctionFor(symbol);
    if (!fn) throw std::runtime_error(std::string("libc++ shim: missing host function ") + symbol);
    return fn;
}

uint32_t callHost1(Ctx& x, const char* symbol, GuestPtr a) {
    auto fn = requiredHost(x, symbol);
    x.cpu.r[0] = a;
    fn(x.cpu);
    return x.cpu.r[0];
}

void m_lock(Ctx& x, GuestPtr self) {
    const uint32_t rc = callHost1(x, "_pthread_mutex_lock", self);
    if (rc != 0) thread_error("mutex::lock", int(rc));
}

void m_unlock(Ctx& x, GuestPtr self) {
    const uint32_t rc = callHost1(x, "_pthread_mutex_unlock", self);
    if (rc != 0) thread_error("mutex::unlock", int(rc));
}

void m_dtor(Ctx& x, GuestPtr self) {
    callHost1(x, "_pthread_mutex_destroy", self);
}

void cv_notify_one(Ctx& x, GuestPtr self) {
    callHost1(x, "_pthread_cond_signal", self);
}

void cv_notify_all(Ctx& x, GuestPtr self) {
    callHost1(x, "_pthread_cond_broadcast", self);
}

void cv_wait(Ctx& x, GuestPtr self, GuestPtr lock) {
    // lock is unique_lock<mutex>, offset 0 is mutex pointer
    uint32_t mtx = x.mem.read<uint32_t>(lock);
    auto fn = requiredHost(x, "_pthread_cond_wait");
    x.cpu.r[0] = self;
    x.cpu.r[1] = mtx;
    fn(x.cpu);
}

void cv_dtor(Ctx& x, GuestPtr self) {
    callHost1(x, "_pthread_cond_destroy", self);
}

// condition_variable::__do_timed_wait(unique_lock<mutex>&, time_point<system_clock, nanoseconds>):
// wait_for/wait_until end here. The deadline arrives in r2:r3 (Darwin's 32-bit ABI does not
// pair 64-bit arguments). A stub returned at once, so Minecraft PE's Semaphore::wait spun.
void cv_timedWait(Ctx& x) {
    const uint32_t self = x.cpu.r[0], lock = x.cpu.r[1];
    const int64_t ns = int64_t((uint64_t(x.cpu.r[3]) << 32) | x.cpu.r[2]);
    const uint32_t mtx = x.mem.read<uint32_t>(lock);
    const uint32_t ts = x.proc.heap().allocZeroed(8);
    const int64_t secs = ns <= 0 ? 0 : std::min<int64_t>(ns / 1000000000, INT32_MAX);
    x.mem.write<uint32_t>(ts, uint32_t(secs));
    x.mem.write<uint32_t>(ts + 4, ns <= 0 ? 0 : uint32_t(ns % 1000000000));
    auto fn = requiredHost(x, "_pthread_cond_timedwait");
    x.cpu.r[0] = self; x.cpu.r[1] = mtx; x.cpu.r[2] = ts;
    fn(x.cpu);   // ETIMEDOUT is not an error here: the caller checks the clock itself
    x.proc.heap().free(ts);
}

// std::recursive_mutex is a pthread mutex of type PTHREAD_MUTEX_RECURSIVE (2 on Darwin).
void rm_ctor(Ctx& x, GuestPtr self) {
    const uint32_t attr = x.proc.heap().allocZeroed(16);
    callHost1(x, "_pthread_mutexattr_init", attr);
    auto settype = requiredHost(x, "_pthread_mutexattr_settype");
    x.cpu.r[0] = attr; x.cpu.r[1] = 2;
    settype(x.cpu);
    auto init = requiredHost(x, "_pthread_mutex_init");
    x.cpu.r[0] = self; x.cpu.r[1] = attr;
    init(x.cpu);
    const uint32_t rc = x.cpu.r[0];
    callHost1(x, "_pthread_mutexattr_destroy", attr);
    x.proc.heap().free(attr);
    if (rc != 0) thread_error("recursive_mutex", int(rc));
}

unsigned hardwareConcurrency(Ctx&) { return 2; }   // the dual-core devices these games target

// std::__shared_weak_count: { vtable, __shared_owners_, __shared_weak_owners_ }, counts
// stored as "owners - 1". vtable: [0] D1, [1] D0, [2] __on_zero_shared,
// [3] __get_deleter, [4] __on_zero_shared_weak. Atomic on the guest words, since guest
// threads run on host threads.
int32_t* ownerWord(Ctx& x, GuestPtr self, uint32_t offset) {
    return reinterpret_cast<int32_t*>(x.mem.host(self + offset));
}
void callVirtual(Ctx& x, GuestPtr self, uint32_t slot) {
    const uint32_t vtable = x.mem.read<uint32_t>(self);
    x.proc.callGuest(x.cpu, x.mem.read<uint32_t>(vtable + slot * 4), {self});
}
void sw_addShared(Ctx& x, GuestPtr self) { __atomic_add_fetch(ownerWord(x, self, 4), 1, __ATOMIC_RELAXED); }
void sw_addWeak(Ctx& x, GuestPtr self) { __atomic_add_fetch(ownerWord(x, self, 8), 1, __ATOMIC_RELAXED); }
void sw_releaseWeak(Ctx& x, GuestPtr self) {
    // As libc++: a zero count means this is the last reference, so skip the decrement.
    if (__atomic_load_n(ownerWord(x, self, 8), __ATOMIC_ACQUIRE) == 0 ||
        __atomic_sub_fetch(ownerWord(x, self, 8), 1, __ATOMIC_ACQ_REL) == -1)
        callVirtual(x, self, 4);
}
void sw_releaseShared(Ctx& x, GuestPtr self) {
    if (__atomic_sub_fetch(ownerWord(x, self, 4), 1, __ATOMIC_ACQ_REL) == -1) {
        callVirtual(x, self, 2);
        sw_releaseWeak(x, self);
    }
}
uint32_t sw_lock(Ctx& x, GuestPtr self) {
    int32_t* owners = ownerWord(x, self, 4);
    int32_t seen = __atomic_load_n(owners, __ATOMIC_RELAXED);
    while (seen != -1)
        if (__atomic_compare_exchange_n(owners, &seen, seen + 1, true, __ATOMIC_ACQ_REL, __ATOMIC_RELAXED)) return self;
    return 0;
}
uint32_t sw_getDeleter(Ctx&, GuestPtr, GuestPtr) { return 0; }   // the base class has no deleter
void sw_dtor(Ctx&, GuestPtr) {}

void sys_clock_now(Ctx& x) {
    // Darwin libc++ system_clock counts microseconds (steady_clock counts nanoseconds).
    auto t = std::chrono::system_clock::now();
    int64_t us = std::chrono::duration_cast<std::chrono::microseconds>(t.time_since_epoch()).count();
    x.cpu.r[0] = uint32_t(us);
    x.cpu.r[1] = uint32_t(us >> 32);
}

void steady_clock_now(Ctx& x) {
    auto t = std::chrono::steady_clock::now();
    int64_t ns = std::chrono::duration_cast<std::chrono::nanoseconds>(t.time_since_epoch()).count();
    x.cpu.r[0] = uint32_t(ns);
    x.cpu.r[1] = uint32_t(ns >> 32);
}

void sleep_for(Ctx& x, GuestPtr ns_ptr) {
    int64_t ns = x.mem.read<int64_t>(ns_ptr);
    // Sleep in short slices so a stop request (app exit, kill) interrupts long sleeps.
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::nanoseconds(ns > 0 ? ns : 0);
    for (;;) {
        if (x.proc.stopping()) throw ProcessExit(0, "stopping");
        const auto now = std::chrono::steady_clock::now();
        if (now >= deadline) break;
        std::this_thread::sleep_for(std::min<std::chrono::steady_clock::duration>(deadline - now, std::chrono::milliseconds(10)));
    }
}

// std::__1::__next_prime(size_t): the smallest prime >= n, which unordered_map and
// unordered_set use as their bucket count. A stub returning 0 left Minecraft PE with
// zero buckets in a static initializer and it crashed before its first frame. size_t is
// 32-bit here; above the largest 32-bit prime libc++ throws overflow_error.
uint32_t nextPrime(Ctx&, uint32_t n) {
    if (n <= 2) return n == 0 ? 0 : 2;   // libc++'s table starts 0, 2, 3, 5: 0 stays 0, 1 becomes 2
    if (n > 4294967291u) throw std::runtime_error("std::overflow_error: __next_prime overflow");
    auto prime = [](uint32_t v) {
        if (v % 2 == 0) return v == 2;
        for (uint32_t d = 3; uint64_t(d) * d <= v; d += 2)
            if (v % d == 0) return false;
        return true;
    };
    while (!prime(n)) ++n;
    return n;
}

} // namespace

void registerLibcxx(Process& p) {
    p.def<&nextPrime>("__ZNSt3__112__next_primeEm");
    registerLibcxxString(p);
    p.def<&tld_get>("__ZNSt3__119__thread_local_dataEv");

    p.def<&ts_ctor>("__ZNSt3__115__thread_structC1Ev");
    supportDef<&ts_dtor>(p, "__ZNSt3__115__thread_structD1Ev", SupportStatus::Stub,
                         "Thread-struct destructor does nothing");

    p.def<&t_dtor>("__ZNSt3__16threadD1Ev");
    p.def<&t_join>("__ZNSt3__16thread4joinEv");
    p.def<&t_detach>("__ZNSt3__16thread6detachEv");

    p.def<&m_lock>("__ZNSt3__15mutex4lockEv");
    p.def<&m_unlock>("__ZNSt3__15mutex6unlockEv");
    p.def<&m_dtor>("__ZNSt3__15mutexD1Ev");

    p.def<&cv_notify_one>("__ZNSt3__118condition_variable10notify_oneEv");
    p.def<&cv_notify_all>("__ZNSt3__118condition_variable10notify_allEv");
    p.def<&cv_wait>("__ZNSt3__118condition_variable4waitERNS_11unique_lockINS_5mutexEEE");
    p.def<&cv_dtor>("__ZNSt3__118condition_variableD1Ev");
    p.defineFunction("__ZNSt3__118condition_variable15__do_timed_waitERNS_11unique_lockINS_5mutexEEENS_6chrono10time_pointINS5_12system_clockENS5_8durationIxNS_5ratioILx1ELx1000000000EEEEEEE",
                     [](Ctx& x) { cv_timedWait(x); });
    p.def<&rm_ctor>("__ZNSt3__115recursive_mutexC1Ev");
    p.def<&rm_ctor>("__ZNSt3__115recursive_mutexC2Ev");
    p.def<&m_lock>("__ZNSt3__115recursive_mutex4lockEv");
    p.def<&m_unlock>("__ZNSt3__115recursive_mutex6unlockEv");
    p.def<&m_dtor>("__ZNSt3__115recursive_mutexD1Ev");
    p.def<&m_dtor>("__ZNSt3__115recursive_mutexD2Ev");
    p.def<&hardwareConcurrency>("__ZNSt3__16thread20hardware_concurrencyEv");
    p.def<&sw_addShared>("__ZNSt3__119__shared_weak_count12__add_sharedEv");
    p.def<&sw_releaseShared>("__ZNSt3__119__shared_weak_count16__release_sharedEv");
    p.def<&sw_addWeak>("__ZNSt3__119__shared_weak_count10__add_weakEv");
    p.def<&sw_releaseWeak>("__ZNSt3__119__shared_weak_count14__release_weakEv");
    p.def<&sw_lock>("__ZNSt3__119__shared_weak_count4lockEv");
    p.def<&sw_getDeleter>("__ZNKSt3__119__shared_weak_count13__get_deleterERKSt9type_info");
    p.def<&sw_dtor>("__ZNSt3__119__shared_weak_countD2Ev");
    p.def<&sw_dtor>("__ZNSt3__115__shared_countD2Ev");

    p.def<&sys_clock_now>("__ZNSt3__16chrono12system_clock3nowEv");
    p.def<&steady_clock_now>("__ZNSt3__16chrono12steady_clock3nowEv");

    p.def<&sleep_for>("__ZNSt3__111this_thread9sleep_forERKNS_6chrono8durationIxNS_5ratioILx1ELx1000000000EEEEE");
}

} // namespace Hydrogen::Runtime
