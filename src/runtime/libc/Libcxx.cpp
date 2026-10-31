#include "runtime/Process.hpp"
#include <chrono>
#include <stdexcept>
#include <thread>

namespace Hydrogen::Runtime {

namespace {

uint32_t tld_get(Ctx& x) {
    auto& proc = x.proc;

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

uint32_t t_join(Ctx& x, GuestPtr self) {
    uint32_t th = x.mem.read<uint32_t>(self);
    if (th == 0) {
        return 22; // EINVAL
    }
    auto join_fn = x.proc.hostFunctionFor("_pthread_join");
    if (join_fn) {
        x.cpu.r[0] = th;
        x.cpu.r[1] = 0;
        join_fn(x.cpu);
        uint32_t ret = x.cpu.r[0];
        if (ret == 0) x.mem.write<uint32_t>(self, 0);
        return ret;
    }
    return 3; // ESRCH or 38 ENOSYS
}

void t_detach(Ctx& x, GuestPtr self) {
    uint32_t th = x.mem.read<uint32_t>(self);
    if (th == 0) {
        x.cpu.r[0] = 22; // EINVAL
        return;
    }
    auto detach_fn = x.proc.hostFunctionFor("_pthread_detach");
    if (detach_fn) {
        x.cpu.r[0] = th;
        detach_fn(x.cpu);
        if (x.cpu.r[0] == 0) x.mem.write<uint32_t>(self, 0);
    } else {
        x.cpu.r[0] = 3;
    }
}

uint32_t m_lock(Ctx& x, GuestPtr self) {
    auto lock_fn = x.proc.hostFunctionFor("_pthread_mutex_lock");
    if (lock_fn) {
        x.cpu.r[0] = self;
        lock_fn(x.cpu);
        return x.cpu.r[0];
    }
    return 0;
}

uint32_t m_unlock(Ctx& x, GuestPtr self) {
    auto unlock_fn = x.proc.hostFunctionFor("_pthread_mutex_unlock");
    if (unlock_fn) {
        x.cpu.r[0] = self;
        unlock_fn(x.cpu);
        return x.cpu.r[0];
    }
    return 0;
}

void m_dtor(Ctx& x, GuestPtr self) {
    auto dtor_fn = x.proc.hostFunctionFor("_pthread_mutex_destroy");
    if (dtor_fn) {
        x.cpu.r[0] = self;
        dtor_fn(x.cpu);
    }
}

void cv_notify_one(Ctx& x, GuestPtr self) {
    auto fn = x.proc.hostFunctionFor("_pthread_cond_signal");
    if (fn) {
        x.cpu.r[0] = self;
        fn(x.cpu);
    }
}

void cv_notify_all(Ctx& x, GuestPtr self) {
    auto fn = x.proc.hostFunctionFor("_pthread_cond_broadcast");
    if (fn) {
        x.cpu.r[0] = self;
        fn(x.cpu);
    }
}

void cv_wait(Ctx& x, GuestPtr self, GuestPtr lock) {
    // lock is unique_lock<mutex>, offset 0 is mutex pointer
    uint32_t mtx = x.mem.read<uint32_t>(lock);
    auto fn = x.proc.hostFunctionFor("_pthread_cond_wait");
    if (fn) {
        x.cpu.r[0] = self;
        x.cpu.r[1] = mtx;
        fn(x.cpu);
    }
}

void cv_dtor(Ctx& x, GuestPtr self) {
    auto fn = x.proc.hostFunctionFor("_pthread_cond_destroy");
    if (fn) {
        x.cpu.r[0] = self;
        fn(x.cpu);
    }
}

void sys_clock_now(Ctx& x) {
    auto t = std::chrono::system_clock::now();
    int64_t ns = std::chrono::duration_cast<std::chrono::nanoseconds>(t.time_since_epoch()).count();
    x.cpu.r[0] = uint32_t(ns);
    x.cpu.r[1] = uint32_t(ns >> 32);
}

void steady_clock_now(Ctx& x) {
    auto t = std::chrono::steady_clock::now();
    int64_t ns = std::chrono::duration_cast<std::chrono::nanoseconds>(t.time_since_epoch()).count();
    x.cpu.r[0] = uint32_t(ns);
    x.cpu.r[1] = uint32_t(ns >> 32);
}

void sleep_for(Ctx& x, GuestPtr ns_ptr) {
    int64_t ns = x.mem.read<int64_t>(ns_ptr);
    if (ns > 0) {
        std::this_thread::sleep_for(std::chrono::nanoseconds(ns));
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
