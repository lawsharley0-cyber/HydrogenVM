#include "runtime/Process.hpp"
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

} // namespace

void registerLibcxx(Process& p) {
    p.def<&tld_get>("__ZNSt3__119__thread_local_dataEv");

    p.def<&ts_ctor>("__ZNSt3__115__thread_structC1Ev");
    p.def<&ts_dtor>("__ZNSt3__115__thread_structD1Ev");

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

    p.def<&sys_clock_now>("__ZNSt3__16chrono12system_clock3nowEv");
    p.def<&steady_clock_now>("__ZNSt3__16chrono12steady_clock3nowEv");

    p.def<&sleep_for>("__ZNSt3__111this_thread9sleep_forERKNS_6chrono8durationIxNS_5ratioILx1ELx1000000000EEEEE");
}

} // namespace Hydrogen::Runtime
