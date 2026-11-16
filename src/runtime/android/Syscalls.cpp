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
            for (;;) {
                if (auto it = f.wakes.find(addr); it != f.wakes.end() && it->second) {
                    if (--it->second == 0) f.wakes.erase(it);
                    return 0;
                }
                if (p.mem().read<uint32_t>(addr) != val) return 0;   // the word changed: a wake we may have missed
                if (p.stopping()) throw ProcessExit(0, "stopping");
                if (timed && clock::now() >= due) return -kETIMEDOUT;
                f.cv.wait_for(lk, std::chrono::milliseconds(20));
            }
        }
        case 1: case 10: {            // FUTEX_WAKE, FUTEX_WAKE_BITSET
            {
                std::lock_guard<std::mutex> lk(f.mu);
                f.wakes[addr] += std::min<uint32_t>(val, 64);
            }
            f.cv.notify_all();
            return int32_t(std::min<uint32_t>(val, 64));
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
    eng.invalidateRange(start, end - start);
    for (CPU::Cpu* c : held) eng.resume(*c);
    return 0;
}

// The call: number and arguments as the kernel takes them. Returns r0.
int32_t linuxSyscall(Process& p, CPU::Cpu& c, uint32_t nr, const uint32_t a[6]) {
    Ctx x{p, c, p.mem()};
    switch (nr) {
        case 0xF0002: return cacheflush(p, c, a[0], a[1]);                 // ARM cacheflush
        case 0xF0005: c.tpidrurw = a[0]; return 0;                         // ARM set_tls
        case 224: return int32_t(1000 + p.currentThread().id);            // gettid
        case 20: return 1000;                                              // getpid
        case 240: return futex(p, a[0], a[1], a[2], a[3], a[4]);           // futex
        case 158: std::this_thread::yield(); return 0;                     // sched_yield
        case 263: {                                                        // clock_gettime
            const int32_t r = int32_t(callIos(x, "android:clock_gettime", {a[0], a[1]}));
            return r < 0 ? -kEINVAL : 0;
        }
        case 78: return int32_t(callIos(x, "_gettimeofday", {a[0], a[1]}));   // gettimeofday
        case 238: {                                                        // tkill
            if (a[1] == 0) return 0;
            p.reportDefaulted("tkill by system call (signal " + std::to_string(a[1]) + ") ignored");
            return 0;
        }
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
