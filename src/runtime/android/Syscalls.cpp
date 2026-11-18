// HydrogenVM — Linux system calls made directly by Android games: the `svc #0` instruction
// (r7 = number, r0-r6 arguments, r0 = result or -errno) and bionic's syscall() function.
//
// Runtimes that generate code (Mono's JIT in Unity games, LuaJIT, V8) call cacheflush after
// writing it; decoded blocks over that range are dropped, with the other guest threads held
// for the moment it takes. Engines also make a few calls by number rather than through libc
// (gettid, futex, clock_gettime). Everything else answers -ENOSYS, which callers handle.
#include "runtime/android/Android.hpp"
#include "runtime/android/BionicInternal.hpp"
#include "runtime/Support.hpp"
#include "core/cpu/Engine.hpp"
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#include <unordered_map>

namespace Hydrogen::Runtime::Android {
namespace {

constexpr int32_t kENOSYS = 38, kEAGAIN = 11, kETIMEDOUT = 110, kEINVAL = 22;

// futex: waiters per address, woken by FUTEX_WAKE or when the word changes.
struct Futexes {
    explicit Futexes(Process&) {}
    std::mutex mu;
    std::condition_variable cv;
    std::unordered_map<uint32_t, uint32_t> wakes;   // address -> wakes not yet taken
    std::unordered_map<uint32_t, uint32_t> waiters; // address -> threads waiting there
};

int32_t futex(Process& p, uint32_t addr, uint32_t op, uint32_t val, uint32_t timeout, uint32_t) {
    auto& f = p.service<Futexes>();
    const uint32_t cmd = op & 0x7F;   // without FUTEX_PRIVATE_FLAG (128) / CLOCK_REALTIME (256)
    switch (cmd) {
        case 0: case 9: {             // FUTEX_WAIT, FUTEX_WAIT_BITSET
            std::unique_lock<std::mutex> lk(f.mu);
            if (p.mem().read<uint32_t>(addr) != val) return -kEAGAIN;
            using clock = std::chrono::steady_clock;
            const bool timed = timeout != 0;
            clock::time_point due = clock::time_point::max();
            if (timed) {
                const int64_t ns = int64_t(p.mem().read<int32_t>(timeout)) * 1'000'000'000 + p.mem().read<int32_t>(timeout + 4);
                // WAIT takes a relative timeout; WAIT_BITSET an absolute monotonic one.
                due = cmd == 0 ? clock::now() + std::chrono::nanoseconds(ns) : clock::time_point(std::chrono::nanoseconds(ns));
            }
            BlockedScope blocked(p, "futex wait", addr);
            ++f.waiters[addr];
            struct Leave {
                Futexes& f; uint32_t addr;
                ~Leave() {   // owed wakes never outnumber the threads still waiting
                    uint32_t left = 0;
                    if (auto it = f.waiters.find(addr); it != f.waiters.end()) {
                        left = --it->second;
                        if (!left) f.waiters.erase(it);
                    }
                    if (auto it = f.wakes.find(addr); it != f.wakes.end() && it->second > left) {
                        if (left) it->second = left; else f.wakes.erase(it);
                    }
                }
            } leave{f, addr};
            for (;;) {
                if (auto it = f.wakes.find(addr); it != f.wakes.end() && it->second) {
                    if (--it->second == 0) f.wakes.erase(it);
                    return 0;
                }
                if (p.mem().read<uint32_t>(addr) != val) return 0;   // the word changed: a wake we may have missed
                if (p.stopping()) throw ProcessExit(0, "stopping");
                if (timed && clock::now() >= due) return -kETIMEDOUT;
                if (p.currentThread().cpu.async_pending.load(std::memory_order_acquire)) return -4;   // EINTR: the handler runs on return
                f.cv.wait_for(lk, std::chrono::milliseconds(20));
            }
        }
        case 1: case 10: {            // FUTEX_WAKE, FUTEX_WAKE_BITSET
            // Linux wakes at most `val` of the threads actually waiting and returns how many; a
            // wake nobody is waiting for is not saved up for a later wait.
            uint32_t woken = 0;
            {
                std::lock_guard<std::mutex> lk(f.mu);
                const auto w = f.waiters.find(addr);
                const uint32_t waiting = w == f.waiters.end() ? 0 : w->second;
                const auto pending = f.wakes.find(addr);
                const uint32_t owed = pending == f.wakes.end() ? 0 : pending->second;
                woken = waiting > owed ? std::min(val, waiting - owed) : 0;
                if (woken) f.wakes[addr] += woken;
            }
            if (woken) f.cv.notify_all();
            return int32_t(woken);
        }
        default:
            p.reportDefaulted("futex op " + std::to_string(op) + " -> ENOSYS");
            return -kENOSYS;
    }
}

// cacheflush(start, end, flags): new code at [start, end).
int32_t cacheflush(Process& p, CPU::Cpu& self, uint32_t start, uint32_t end) {
    if (end <= start) return 0;
    auto& eng = p.engine();
    if (!eng.hasBlocksIn(start, end - start)) return 0;   // never run: nothing decoded to drop
    // Code that ran before was rewritten: hold the other guest threads, drop the blocks, go on.
    std::vector<CPU::Cpu*> held;
    for (const uint32_t id : p.liveThreadIds()) {
        GuestThread* t = p.findThreadById(id);
        if (!t || &t->cpu == &self || t->finished) continue;
        eng.suspend(t->cpu);
        held.push_back(&t->cpu);
    }
    eng.retireRange(start, end - start);
    for (CPU::Cpu* c : held) eng.resume(*c);
    return 0;
}

// The call: number and arguments as the kernel takes them. Returns r0.
int32_t linuxSyscall(Process& p, CPU::Cpu& c, uint32_t nr, const uint32_t a[6]) {
    Ctx x{p, c, p.mem()};
    auto bionic = [&](const char* name, std::initializer_list<uint32_t> args) {
        const int32_t result = int32_t(callIos(x, name, args));
        return result == -1 ? -takeLinuxErrno(x) : result;
    };
    switch (nr) {
        case 0xF0002: return cacheflush(p, c, a[0], a[1]);                 // ARM cacheflush
        case 0xF0005: c.tpidrurw = a[0]; return 0;                         // ARM set_tls
        case 224: return int32_t(1000 + p.currentThread().id);            // gettid
        case 20: return 1000;                                              // getpid
        case 240: return futex(p, a[0], a[1], a[2], a[3], a[4]);           // futex
        case 158: std::this_thread::yield(); return 0;                     // sched_yield
        case 241: return 0;                                                // sched_setaffinity: any core is fine
        case 263: {                                                        // clock_gettime
            const int32_t r = int32_t(callIos(x, "android:clock_gettime", {a[0], a[1]}));
            return r < 0 ? -kEINVAL : 0;
        }
        case 78: return int32_t(callIos(x, "_gettimeofday", {a[0], a[1]}));   // gettimeofday
        case 238: return -sendSignalToTid(p, c, a[0], int(a[1]));          // tkill
        case 268: return -sendSignalToTid(p, c, a[1], int(a[2]));          // tgkill
        case 3: return bionic("android:read", {a[0], a[1], a[2]});
        case 4: return bionic("android:write", {a[0], a[1], a[2]});
        case 6: return bionic("android:close", {a[0]});
        case 41: return bionic("android:dup", {a[0]});
        case 63: return bionic("android:dup2", {a[0], a[1]});
        case 91: return bionic("android:munmap", {a[0], a[1]});
        case 168: return bionic("android:poll", {a[0], a[1], a[2]});
        case 180: return bionic("android:pread64", {a[0], a[1], a[2], a[3], a[4], a[5]});
        case 181: return bionic("android:pwrite64", {a[0], a[1], a[2], a[3], a[4], a[5]});
        case 187: return bionic("android:sendfile", {a[0], a[1], a[2], a[3]});
        case 192: return bionic("android:__mmap2", {a[0], a[1], a[2], a[3], a[4], a[5]});
        case 239: return bionic("android:sendfile64", {a[0], a[1], a[2], a[3]});
        case 250: return bionic("android:epoll_create", {a[0]});
        case 251: return bionic("android:epoll_ctl", {a[0], a[1], a[2], a[3]});
        case 252: return bionic("android:epoll_wait", {a[0], a[1], a[2], a[3]});
        case 281: return bionic("android:socket", {a[0], a[1], a[2]});
        case 282: return bionic("android:bind", {a[0], a[1], a[2]});
        case 283: return bionic("android:connect", {a[0], a[1], a[2]});
        case 284: return bionic("android:listen", {a[0], a[1]});
        case 285: return bionic("android:accept", {a[0], a[1], a[2]});
        case 286: return bionic("android:getsockname", {a[0], a[1], a[2]});
        case 287: return bionic("android:getpeername", {a[0], a[1], a[2]});
        case 288: return bionic("android:socketpair", {a[0], a[1], a[2], a[3]});
        case 289: return bionic("android:send", {a[0], a[1], a[2], a[3]});
        case 290: return bionic("android:sendto", {a[0], a[1], a[2], a[3], a[4], a[5]});
        case 291: return bionic("android:recv", {a[0], a[1], a[2], a[3]});
        case 292: return bionic("android:recvfrom", {a[0], a[1], a[2], a[3], a[4], a[5]});
        case 293: return bionic("android:shutdown", {a[0], a[1]});
        case 294: return bionic("android:setsockopt", {a[0], a[1], a[2], a[3], a[4]});
        case 295: return bionic("android:getsockopt", {a[0], a[1], a[2], a[3], a[4]});
        case 346: return bionic("android:epoll_pwait", {a[0], a[1], a[2], a[3], a[4]});
        case 350: return bionic("android:timerfd_create", {a[0], a[1]});
        case 351: return bionic("android:__eventfd", {a[0]});
        case 353: return bionic("android:timerfd_settime", {a[0], a[1], a[2], a[3]});
        case 354: return bionic("android:timerfd_gettime", {a[0], a[1]});
        case 356: return bionic("android:eventfd2", {a[0], a[1]});
        case 357: return bionic("android:epoll_create1", {a[0]});
        case 359: return bionic("android:pipe2", {a[0], a[1]});
        case 366: return bionic("android:accept4", {a[0], a[1], a[2], a[3]});
        case 172: return 0;                                                // prctl
        default:
            p.reportDefaulted("Linux system call " + std::to_string(nr) + " -> ENOSYS");
            return -kENOSYS;
    }
}

// bionic's syscall(number, ...): the same calls, returning -1 and errno on failure.
int32_t s_syscall(Ctx& x, uint32_t nr, VarArgs va) {
    uint32_t a[6];
    for (auto& v : a) v = va.args.word();
    const int32_t r = linuxSyscall(x.proc, x.cpu, nr, a);
    if (r < 0 && r > -4096) { setLinuxErrno(x, -r); return -1; }
    return r;
}

} // namespace

void registerSyscalls(Process& p) {
    supportDef<&s_syscall>(p, "android:syscall", SupportStatus::Real,
                           "Linux numbers: cacheflush, gettid, futex, clock_gettime, ...; others ENOSYS");
    p.engine().setSvcHandler([&p](CPU::Cpu& c, uint32_t) {
        const uint32_t a[6] = {c.r[0], c.r[1], c.r[2], c.r[3], c.r[4], c.r[5]};
        c.r[0] = uint32_t(linuxSyscall(p, c, c.r[7], a));
        return true;
    });
}

} // namespace Hydrogen::Runtime::Android
