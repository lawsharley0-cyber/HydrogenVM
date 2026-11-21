// HydrogenVM — assorted POSIX and Mach calls that games import: readlink,
// Mach semaphores, file-system statistics, locale queries, thread stack
// queries, and harmless no-ops.
#include "core/cpu/Engine.hpp"
#include "runtime/Process.hpp"
#include "runtime/Support.hpp"
#include "runtime/libc/Format.hpp"
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <optional>
#include <string>
#include <unistd.h>
#include <unordered_map>

namespace Hydrogen::Runtime {
namespace {

void setErr(Ctx& x, int e) { x.mem.write<uint32_t>(x.proc.currentThread().errnoAddr, uint32_t(e)); }

// ssize_t readlink(const char* path, char* buf, size_t size)
int32_t p_readlink(Ctx& x, GuestPtr path, GuestPtr buf, uint32_t size) {
    char tmp[4096];
    const ssize_t n = ::readlink(x.proc.hostPath(x.proc.readCString(path)).c_str(), tmp, std::min<size_t>(size, sizeof tmp));
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
    std::optional<BlockedScope> blocked;
    if (sem->count <= 0 && timeout != 0) blocked.emplace(x.proc, "semaphore_wait / sem_wait", id);
    while (sem->count <= 0) {
        // Stopping: end the thread like the pthread waits do. Returning KERN_ABORTED
        // made Unity log "Failed to wait on a semaphore (errno:14)" in a loop, which
        // filled the session's log and hid the real error.
        if (x.proc.stopping()) throw ProcessExit(0, "stopping");
        if (clock::now() >= deadline) return KERN_OPERATION_TIMED_OUT;
        sem->cv.wait_until(lk, std::min(deadline, clock::now() + std::chrono::milliseconds(50)));
    }
    --sem->count;
    return KERN_SUCCESS;
}
int32_t m_semWait(Ctx& x, uint32_t id) { return semWait(x, id, -1); }

// ---- POSIX named semaphores (sem_open). iOS has no unnamed sem_init, so apps that want a
// counting semaphore across threads use these: Geometry Dash's worker threads wait in sem_wait.
// A sem_t* is a guest word holding one of the Mach semaphore handles above; one per name, so
// every sem_open of a name returns the same pointer, as on Darwin.
struct NamedSemaphores {
    explicit NamedSemaphores(Process&) {}
    std::mutex mu;
    std::unordered_map<std::string, GuestPtr> byName;      // name -> sem_t*
    std::unordered_map<GuestPtr, uint32_t> handle;          // sem_t* -> Mach semaphore id
    std::unordered_map<GuestPtr, int> opens;                // sem_open count not yet closed
};
constexpr GuestPtr kSemFailed = 0xFFFFFFFFu;
constexpr int32_t kOCreat = 0x200, kOExcl = 0x800;
// sem_t *sem_open(const char *name, int oflag, ... /* mode_t mode, unsigned value */)
GuestPtr p_semOpen(Ctx& x, GuestPtr namePtr, int32_t oflag, uint32_t /*mode*/, uint32_t value) {
    if (!namePtr) { setErr(x, 22); return kSemFailed; }
    const std::string name = x.proc.readCString(namePtr);
    if (name.empty() || name.size() > 31) { setErr(x, name.empty() ? 22 : 63); return kSemFailed; }   // EINVAL / ENAMETOOLONG (PSEMNAMLEN 31)
    auto& n = x.proc.service<NamedSemaphores>();
    std::lock_guard<std::mutex> lk(n.mu);
    if (auto it = n.byName.find(name); it != n.byName.end()) {
        if ((oflag & kOCreat) && (oflag & kOExcl)) { setErr(x, 17); return kSemFailed; }   // EEXIST
        ++n.opens[it->second];
        return it->second;
    }
    if (!(oflag & kOCreat)) { setErr(x, 2); return kSemFailed; }   // ENOENT
    if (value > 32767) { setErr(x, 22); return kSemFailed; }      // SEM_VALUE_MAX
    const GuestPtr cell = x.proc.heap().allocZeroed(4);
    if (!cell) { setErr(x, 28); return kSemFailed; }   // ENOSPC
    const GuestPtr idCell = x.proc.heap().allocZeroed(4);
    m_semCreate(x, 0, idCell, 0, int32_t(value));
    const uint32_t id = x.mem.read<uint32_t>(idCell);
    x.proc.heap().free(idCell);
    x.mem.write<uint32_t>(cell, id);
    n.byName[name] = cell;
    n.handle[cell] = id;
    n.opens[cell] = 1;
    return cell;
}
// The Mach handle behind a sem_t*, or 0 (EINVAL) for anything sem_open did not return.
uint32_t semHandle(Ctx& x, GuestPtr sem) {
    auto& n = x.proc.service<NamedSemaphores>();
    std::lock_guard<std::mutex> lk(n.mu);
    auto it = n.handle.find(sem);
    if (it == n.handle.end()) { setErr(x, 22); return 0; }
    return it->second;
}
int32_t p_semPost(Ctx& x, GuestPtr sem) {
    const uint32_t id = semHandle(x, sem);
    return id && m_semSignal(x, id) == KERN_SUCCESS ? 0 : -1;
}
int32_t p_semWait(Ctx& x, GuestPtr sem) {
    const uint32_t id = semHandle(x, sem);
    return id && semWait(x, id, -1) == KERN_SUCCESS ? 0 : -1;
}
int32_t p_semTryWait(Ctx& x, GuestPtr sem) {
    const uint32_t id = semHandle(x, sem);
    if (!id) return -1;
    if (semWait(x, id, 0) == KERN_SUCCESS) return 0;
    setErr(x, 35);   // EAGAIN
    return -1;
}
// Closing drops this process's reference; the semaphore and its count stay with the name
// (so a later sem_open of the name sees the same count) until sem_unlink.
int32_t p_semClose(Ctx& x, GuestPtr sem) {
    auto& n = x.proc.service<NamedSemaphores>();
    std::lock_guard<std::mutex> lk(n.mu);
    auto it = n.opens.find(sem);
    if (it == n.opens.end() || it->second <= 0) { setErr(x, 22); return -1; }
    --it->second;
    return 0;
}
int32_t p_semUnlink(Ctx& x, GuestPtr namePtr) {
    const std::string name = namePtr ? x.proc.readCString(namePtr) : std::string();
    auto& n = x.proc.service<NamedSemaphores>();
    std::lock_guard<std::mutex> lk(n.mu);
    auto it = n.byName.find(name);
    if (it == n.byName.end()) { setErr(x, 2); return -1; }
    n.byName.erase(it);   // open handles keep working; a new sem_open makes a new semaphore
    return 0;
}
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
    // f_fstypename[15] follows f_reserved2[2] at 60, then f_mntonname[90] at 75 and f_mntfromname[90] at 165.
    x.mem.copyIn(st + 60, "hfs", 4);
    x.mem.copyIn(st + 75, "/", 2);
    x.mem.copyIn(st + 165, "/dev/disk0s1s1", 15);
    return 0;
}
// int getfsstat(struct statfs* buf, int bufsize, int flags): one mounted file system (the app's
// sandbox volume), described as statfs describes it. A null buffer asks only for the count.
int32_t p_getfsstat(Ctx& x, GuestPtr buf, int32_t bufsize, int32_t) {
    if (!buf) return 1;
    if (bufsize < 272) return 0;
    p_statfs(x, 0, buf);
    return 1;
}
// int statvfs(const char*, struct statvfs*) — every field is 32 bits on iOS.
int32_t p_statvfs(Ctx& x, GuestPtr, GuestPtr st) {
    const uint32_t v[11] = {4096, 4096, 4000000, 2000000, 2000000, 1000000, 900000, 900000, 0, 0, 255};
    for (uint32_t i = 0; i < 11; ++i) x.mem.write<uint32_t>(st + i * 4, v[i]);
    return 0;
}
uint32_t p_alarm(Ctx&, uint32_t) { return 0; }

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
// Apple System Log: messages go to the app log (levels 0-3 as errors, 4 warnings, the rest info).
uint32_t asl_handle(Ctx& x) { static const char slotKey = 0; uint32_t& h = x.proc.processSlot(&slotKey); if (!h) h = x.proc.allocStatic(16); return h; }
uint32_t asl_open(Ctx& x, GuestPtr, GuestPtr, uint32_t) { return asl_handle(x); }
uint32_t asl_new(Ctx& x, uint32_t) { return x.proc.heap().allocZeroed(16); }
void asl_free(Ctx& x, uint32_t m) { if (m && m != asl_handle(x) && x.proc.heap().blockSize(m)) x.proc.heap().free(m); }
void asl_close(Ctx&, uint32_t) {}
int32_t asl_zero2(Ctx&, uint32_t, uint32_t) { return 0; }
int32_t asl_zero3(Ctx&, uint32_t, uint32_t, uint32_t) { return 0; }
void aslLog(Ctx& x, int32_t level, const std::string& msg) {
    x.proc.log(level <= 3 ? LogLevel::Error : level == 4 ? LogLevel::Warning : LogLevel::Info, "asl: " + msg);
}
int32_t asl_log(Ctx& x, uint32_t, uint32_t, int32_t level, GuestPtr fmt, VarArgs va) {
    if (fmt) aslLog(x, level, formatGuest(x.proc, x.proc.readCString(fmt), va.args));
    return 0;
}
int32_t asl_vlog(Ctx& x, uint32_t, uint32_t, int32_t level, GuestPtr fmt, GuestPtr vaList) {
    ArgReader a(x.mem, vaList);
    if (fmt) aslLog(x, level, formatGuest(x.proc, x.proc.readCString(fmt), a));
    return 0;
}
int32_t asl_send(Ctx&, uint32_t, uint32_t) { return 0; }
// Extended attributes: the backing filesystem may not support them, and apps use
// them only for flags like "exclude from iCloud backup". Accept and ignore.
int32_t x_setxattr(Ctx&, GuestPtr, GuestPtr, GuestPtr, uint32_t, uint32_t, int32_t) { return 0; }
int32_t x_getxattr(Ctx&, GuestPtr, GuestPtr, GuestPtr, uint32_t, uint32_t, int32_t) { return -1; }  // ENOATTR via -1
int32_t x_removexattr(Ctx&, GuestPtr, GuestPtr, int32_t) { return 0; }
int32_t x_listxattr(Ctx&, GuestPtr, GuestPtr, uint32_t, int32_t) { return 0; }
int32_t x_fsetxattr(Ctx&, int32_t, GuestPtr, GuestPtr, uint32_t, uint32_t, int32_t) { return 0; }
int32_t x_fgetxattr(Ctx&, int32_t, GuestPtr, GuestPtr, uint32_t, uint32_t, int32_t) { return -1; }
int32_t p_zero2(Ctx&, int32_t, int32_t) { return 0; }
int32_t p_zero3(Ctx&, int32_t, int32_t, int32_t) { return 0; }
void p_void2(Ctx&, GuestPtr, uint32_t) {}
void p_icacheInvalidate(Ctx& x, GuestPtr addr, uint32_t size) {
    x.proc.engine().retireRange(addr, size);
}

} // namespace

void registerLibcPosix(Process& p) {
    p.def<&p_readlink>("_readlink");
    p.def<&asl_open>("_asl_open");
    p.def<&asl_new>("_asl_new");
    p.def<&asl_free>("_asl_free");
    p.def<&asl_close>("_asl_close");
    p.def<&asl_log>("_asl_log");
    p.def<&asl_vlog>("_asl_vlog");
    p.def<&asl_send>("_asl_send");
    p.def<&asl_zero3>("_asl_set");
    p.def<&asl_zero2>("_asl_set_filter");
    p.def<&asl_zero3>("_asl_add_log_file");
    p.def<&asl_zero3>("_asl_add_output_file");
    p.def<&asl_zero2>("_asl_remove_log_file");
    p.def<&x_setxattr>("_setxattr");
    p.def<&x_getxattr>("_getxattr");
    p.def<&x_removexattr>("_removexattr");
    p.def<&x_listxattr>("_listxattr");
    p.def<&x_fsetxattr>("_fsetxattr");
    p.def<&x_fgetxattr>("_fgetxattr");
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
    supportDef<&p_semOpen>(p, "_sem_open", SupportStatus::Real, "process-local named counting semaphores over the Mach semaphore table");
    supportDef<&p_semPost>(p, "_sem_post", SupportStatus::Real, "named semaphore signal");
    supportDef<&p_semWait>(p, "_sem_wait", SupportStatus::Real, "named semaphore wait (blocks; ends with the app)");
    supportDef<&p_semTryWait>(p, "_sem_trywait", SupportStatus::Real, "named semaphore try-wait (EAGAIN at zero)");
    supportDef<&p_semClose>(p, "_sem_close", SupportStatus::Real, "drops one sem_open reference");
    supportDef<&p_semUnlink>(p, "_sem_unlink", SupportStatus::Real, "removes the name; open handles keep working");
    supportDef<&p_statfs>(p, "_statfs", SupportStatus::Approx, "fixed 16 GB hfs volume mounted at /, half free");
    supportDef<&p_getfsstat>(p, "_getfsstat", SupportStatus::Approx, "one mounted volume (the app sandbox), described as statfs describes it");
    p.def<&p_statvfs>("_statvfs");
    p.def<&p_alarm>("_alarm");
    p.def<&p_setlocale>("_setlocale");
    p.def<&p_localeconv>("_localeconv");
    p.def<&p_nl_langinfo>("_nl_langinfo");
    p.def<&p_zero3>("_getpriority");
    p.def<&p_zero3>("_setpriority");
    p.def<&p_icacheInvalidate>("_sys_icache_invalidate");
    p.def<&p_void2>("_sys_dcache_flush");
    (void)&p_zero; (void)&p_zero2;
}

} // namespace Hydrogen::Runtime
