// HydrogenVM — assorted POSIX and Mach calls that games import: readlink,
// Mach semaphores, file-system statistics, locale queries, thread stack
// queries, and harmless no-ops.
#include "core/cpu/Engine.hpp"
#include "runtime/Process.hpp"
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <unistd.h>
#include <unordered_map>

namespace Hydrogen::Runtime {
namespace {

void setErr(Ctx& x, int e) { x.mem.write<uint32_t>(x.proc.currentThread().errnoAddr, uint32_t(e)); }

// ssize_t readlink(const char* path, char* buf, size_t size)
int32_t p_readlink(Ctx& x, GuestPtr path, GuestPtr buf, uint32_t size) {
    char tmp[4096];
    const ssize_t n = ::readlink(x.proc.readCString(path).c_str(), tmp, std::min<size_t>(size, sizeof tmp));
    if (n < 0) { setErr(x, errno == ENOENT ? 2 : errno == EINVAL ? 22 : errno == EACCES ? 13 : 22); return -1; }
    x.mem.copyIn(buf, tmp, size_t(n));
    return int32_t(n);
}

int32_t p_getdtablesize(Ctx&) { return 256; }
int32_t p_gethostname(Ctx& x, GuestPtr buf, uint32_t len) {
    static const char kName[] = "iPhone";
    if (len) { x.mem.copyIn(buf, kName, std::min<size_t>(len, sizeof kName)); x.mem.write<uint8_t>(buf + len - 1, 0); }
    return 0;
}

// size_t pthread_get_stacksize_np(pthread_t) / void* pthread_get_stackaddr_np(pthread_t): the top of the stack.
uint32_t p_stacksize(Ctx& x, uint32_t self) {
    GuestThread* t = x.proc.findThread(self);
    return t && t->stackSize ? t->stackSize : 512u << 10;
}
uint32_t p_stackaddr(Ctx& x, uint32_t self) {
    GuestThread* t = x.proc.findThread(self);
    if (t && t->stackSize) return t->stackBase + t->stackSize;
    return (x.cpu.r[CPU::kSP] + 0xFFFFu) & ~0xFFFFu;
}

// ---- Mach semaphores (semaphore_t is a small integer handle)
struct Semaphores {
    explicit Semaphores(Process&) {}
    struct Sem { std::mutex mu; std::condition_variable cv; int32_t count = 0; };
    std::mutex mu;
    std::unordered_map<uint32_t, std::shared_ptr<Sem>> sems;
    uint32_t next = 0x5E000001;
    std::shared_ptr<Sem> get(uint32_t id) {
        std::lock_guard<std::mutex> lk(mu);
        auto it = sems.find(id);
        return it == sems.end() ? nullptr : it->second;
    }
};
constexpr int32_t KERN_SUCCESS = 0, KERN_INVALID_ARGUMENT = 4, KERN_ABORTED = 14, KERN_OPERATION_TIMED_OUT = 49;

int32_t m_semCreate(Ctx& x, uint32_t, GuestPtr out, int32_t, int32_t value) {
    auto& s = x.proc.service<Semaphores>();
    auto sem = std::make_shared<Semaphores::Sem>();
    sem->count = value;
    std::lock_guard<std::mutex> lk(s.mu);
    const uint32_t id = s.next++;
    s.sems[id] = sem;
    if (out) x.mem.write<uint32_t>(out, id);
    return KERN_SUCCESS;
}
int32_t m_semDestroy(Ctx& x, uint32_t, uint32_t id) {
    auto& s = x.proc.service<Semaphores>();
    std::lock_guard<std::mutex> lk(s.mu);
    return s.sems.erase(id) ? KERN_SUCCESS : KERN_INVALID_ARGUMENT;
}
int32_t m_semSignal(Ctx& x, uint32_t id) {
    auto sem = x.proc.service<Semaphores>().get(id);
    if (!sem) return KERN_INVALID_ARGUMENT;
    { std::lock_guard<std::mutex> lk(sem->mu); ++sem->count; }
    sem->cv.notify_one();
    return KERN_SUCCESS;
}
int32_t m_semSignalAll(Ctx& x, uint32_t id) {
    auto sem = x.proc.service<Semaphores>().get(id);
    if (!sem) return KERN_INVALID_ARGUMENT;
    { std::lock_guard<std::mutex> lk(sem->mu); if (sem->count < 0) sem->count = 0; ++sem->count; }
    sem->cv.notify_all();
    return KERN_SUCCESS;
}
// Waits until the count is positive (or the deadline passes, or the app is stopping).
int32_t semWait(Ctx& x, uint32_t id, double timeout) {
    auto sem = x.proc.service<Semaphores>().get(id);
    if (!sem) return KERN_INVALID_ARGUMENT;
    using clock = std::chrono::steady_clock;
    const auto deadline = timeout < 0 ? clock::time_point::max() : clock::now() + std::chrono::duration_cast<clock::duration>(std::chrono::duration<double>(timeout));
    std::unique_lock<std::mutex> lk(sem->mu);
    while (sem->count <= 0) {
        if (x.proc.stopping()) return KERN_ABORTED;
        if (clock::now() >= deadline) return KERN_OPERATION_TIMED_OUT;
        sem->cv.wait_until(lk, std::min(deadline, clock::now() + std::chrono::milliseconds(50)));
    }
    --sem->count;
    return KERN_SUCCESS;
}
int32_t m_semWait(Ctx& x, uint32_t id) { return semWait(x, id, -1); }
// semaphore_timedwait(semaphore_t, mach_timespec_t {unsigned sec; int nsec}) — the struct is passed in r1, r2.
int32_t m_semTimedWait(Ctx& x, uint32_t id, uint32_t sec, int32_t nsec) { return semWait(x, id, double(sec) + double(nsec) * 1e-9); }

// int statfs(const char*, struct statfs*) — 32-bit (non-INODE64) layout.
int32_t p_statfs(Ctx& x, GuestPtr, GuestPtr st) {
    for (uint32_t i = 0; i < 272; i += 4) x.mem.write<uint32_t>(st + i, 0);
    x.mem.write<uint32_t>(st + 4, 4096);          // f_bsize
    x.mem.write<uint32_t>(st + 8, 4096);          // f_iosize
    x.mem.write<uint32_t>(st + 12, 4000000);      // f_blocks (16 GB)
    x.mem.write<uint32_t>(st + 16, 2000000);      // f_bfree
    x.mem.write<uint32_t>(st + 20, 2000000);      // f_bavail
    x.mem.write<uint32_t>(st + 24, 1000000);      // f_files
    x.mem.write<uint32_t>(st + 28, 900000);       // f_ffree
    x.mem.copyIn(st + 64, "hfs", 4);              // f_fstypename
    return 0;
}
// int statvfs(const char*, struct statvfs*) — every field is 32 bits on iOS.
int32_t p_statvfs(Ctx& x, GuestPtr, GuestPtr st) {
    const uint32_t v[11] = {4096, 4096, 4000000, 2000000, 2000000, 1000000, 900000, 900000, 0, 0, 255};
    for (uint32_t i = 0; i < 11; ++i) x.mem.write<uint32_t>(st + i * 4, v[i]);
    return 0;
}

// Locale: everything is "C" / UTF-8.
struct LocaleData {
    explicit LocaleData(Process& p) {
        cName = p.allocCString("C");
        utf8 = p.allocCString("UTF-8");
        empty = p.allocCString("");
        dot = p.allocCString(".");
        // struct lconv: 10 char* then 8 chars (CHAR_MAX = "not available") then more chars.
        lconv = p.heap().allocZeroed(64);
        auto& m = p.mem();
        for (uint32_t i = 0; i < 10; ++i) m.write<uint32_t>(lconv + i * 4, i == 0 ? dot : empty);
        for (uint32_t i = 40; i < 64; ++i) m.write<uint8_t>(lconv + i, 127);
    }
    uint32_t cName, utf8, empty, dot, lconv;
};
uint32_t p_setlocale(Ctx& x, int32_t, GuestPtr) { return x.proc.service<LocaleData>().cName; }
uint32_t p_localeconv(Ctx& x) { return x.proc.service<LocaleData>().lconv; }
uint32_t p_nl_langinfo(Ctx& x, int32_t item) {
    auto& l = x.proc.service<LocaleData>();
    return item == 0 /* CODESET */ ? l.utf8 : l.empty;
}

int32_t p_zero(Ctx&) { return 0; }
int32_t p_zero2(Ctx&, int32_t, int32_t) { return 0; }
int32_t p_zero3(Ctx&, int32_t, int32_t, int32_t) { return 0; }
void p_void2(Ctx&, GuestPtr, uint32_t) {}

} // namespace

void registerLibcPosix(Process& p) {
    p.def<&p_readlink>("_readlink");
    p.def<&p_getdtablesize>("_getdtablesize");
    p.def<&p_gethostname>("_gethostname");
    p.def<&p_stacksize>("_pthread_get_stacksize_np");
    p.def<&p_stackaddr>("_pthread_get_stackaddr_np");
    p.def<&m_semCreate>("_semaphore_create");
    p.def<&m_semDestroy>("_semaphore_destroy");
    p.def<&m_semSignal>("_semaphore_signal");
    p.def<&m_semSignalAll>("_semaphore_signal_all");
    p.def<&m_semWait>("_semaphore_wait");
    p.def<&m_semTimedWait>("_semaphore_timedwait");
    p.def<&p_statfs>("_statfs");
    p.def<&p_statvfs>("_statvfs");
    p.def<&p_setlocale>("_setlocale");
    p.def<&p_localeconv>("_localeconv");
    p.def<&p_nl_langinfo>("_nl_langinfo");
    p.def<&p_zero3>("_getpriority");
    p.def<&p_zero3>("_setpriority");
    p.def<&p_void2>("_sys_icache_invalidate");
    p.def<&p_void2>("_sys_dcache_flush");
    (void)&p_zero; (void)&p_zero2;
}

} // namespace Hydrogen::Runtime
