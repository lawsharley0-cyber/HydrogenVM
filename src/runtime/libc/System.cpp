// HydrogenVM — process/system interfaces: errno, stack protector, dyld,
// sysctl/uname, file descriptors and directories, mmap, signals, setjmp,
// a little of Mach.
#include <sys/ioctl.h>
#include <atomic>
#include <condition_variable>
#include "runtime/Process.hpp"
#include "MachPorts.hpp"
#include "Network.hpp"
#include "DarwinErrno.hpp"
#include <sys/socket.h>
#include <sys/file.h>
#include <sys/uio.h>
#include <cerrno>
#include <csignal>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <filesystem>
#include <mutex>
#include <random>
#include <sys/stat.h>
#include <iconv.h>
#include <mutex>
#include <unordered_map>
#include <unordered_set>
#include <unistd.h>

namespace Hydrogen::Runtime {

void registerStartupMach(Process& p);
void flushGuestStdio(Process& p);

namespace {

inline void setErrno(Ctx& x, int e) { x.mem.write<uint32_t>(x.proc.currentThread().errnoAddr, uint32_t(e)); }
template <typename T> T fail(Ctx& x, T v = T(-1)) { setErrno(x, errno); return v; }

template <typename T> T failD(Ctx& x, T v = T(-1)) { setErrno(x, darwinErrno(errno)); return v; }

// ------------------------------------------------------- process basics ---

uint32_t s_error(Ctx& x) { return x.proc.currentThread().errnoAddr; }
void s_stack_chk_fail(Ctx& x) {
    flushGuestStdio(x.proc);
    throw std::runtime_error("stack buffer overflow detected (__stack_chk_fail) in " + x.proc.describeAddress(x.cpu.r[CPU::kLR]));
}
int32_t s_NSGetExecutablePath(Ctx& x, GuestPtr buf, GuestPtr sizep) {
    const std::string& p = x.proc.guestExecutablePath();
    const uint32_t size = x.mem.read<uint32_t>(sizep);
    if (size < p.size() + 1) { x.mem.write<uint32_t>(sizep, uint32_t(p.size() + 1)); return -1; }
    x.mem.copyIn(buf, p.c_str(), p.size() + 1);
    return 0;
}
uint32_t cellOf(Ctx& x, const char* name, uint32_t value) {
    if (auto a = x.proc.lookupSymbol(name)) return *a;
    const uint32_t c = x.proc.defineDataBlock(name, 4);
    x.mem.write<uint32_t>(c, value);
    return c;
}
uint32_t s_NSGetArgc(Ctx& x) { return cellOf(x, "__hvm_argc_cell", x.proc.argc()); }
uint32_t s_NSGetArgv(Ctx& x) { return cellOf(x, "__hvm_argv_cell", x.proc.argvAddr()); }
uint32_t s_NSGetEnviron(Ctx& x) { return cellOf(x, "__hvm_environ_cell", x.proc.envpAddr()); }
uint32_t s_NSGetProgname(Ctx& x) {
    const std::string name = std::filesystem::path(x.proc.executablePath()).filename().string();
    return cellOf(x, "__hvm_progname_cell", x.proc.internCString(name));
}
uint32_t s_NSGetMachExecuteHeader(Ctx& x) { return x.proc.image().headerAddr; }

uint32_t s_getprogname(Ctx& x) {
    auto name = std::filesystem::path(x.proc.executablePath()).filename().string();
    if (name.empty()) name = std::filesystem::path(x.proc.bundlePath()).stem().string();
    return x.proc.internCString(name);
}
uint32_t s_strsignal(Ctx& x, int32_t signal) {
    // Darwin numbering differs from the Linux host at SIGBUS, SIGSYS, SIGCHLD, etc.
    static const char* names[] = {"Signal 0", "Hangup", "Interrupt", "Quit", "Illegal instruction", "Trace/BPT trap", "Abort trap", "EMT trap", "Floating point exception", "Killed", "Bus error", "Segmentation fault", "Bad system call", "Broken pipe", "Alarm clock", "Terminated", "Urgent I/O condition", "Suspended (signal)", "Suspended", "Continued", "Child exited", "Stopped (tty input)", "Stopped (tty output)", "I/O possible", "Cputime limit exceeded", "Filesize limit exceeded", "Virtual timer expired", "Profiling timer expired", "Window size changes", "Information request", "User defined signal 1", "User defined signal 2"};
    return x.proc.internCString(signal >= 0 && signal < 32 ? names[signal] : "Unknown signal: " + std::to_string(signal));
}

int32_t s_getpid(Ctx&) { return 1234; }
int32_t s_syscall(Ctx& x, int32_t num, VarArgs) {
    switch (num) {
        case 20: // SYS_getpid
            return s_getpid(x);
        case 286: // SYS_gettid
            setErrno(x, 3); // ESRCH
            return -1;
        case 372: // thread_selfid
            return int32_t(x.proc.currentThread().id);
        default: {
            static std::mutex mu;
            static std::unordered_set<int32_t> warned;
            {
                std::lock_guard<std::mutex> lk(mu);
                if (warned.insert(num).second) {
                    x.proc.logf(LogLevel::Warning, "syscall(%d) is not implemented", num);
                }
            }
            setErrno(x, 78); // ENOSYS
            return -1;
        }
    }
}
int32_t s_getppid(Ctx&) { return 1; }
uint32_t s_getuid(Ctx&) { return 501; }
// Darwin 32-bit struct passwd (40 bytes): name, passwd, uid, gid, change, class,
// gecos, dir, shell, expire. The one user is "mobile" (501), home = the app's home.
constexpr uint32_t kPasswdSize = 40;
GuestPtr s_getpwuid(Ctx& x, uint32_t) {
    static const char key = 0;
    uint32_t& addr = x.proc.processSlot(&key);
    if (!addr) {
        addr = x.proc.allocStatic(kPasswdSize);
        const uint32_t name = x.proc.internCString("mobile");
        const uint32_t empty = x.proc.internCString("");
        const uint32_t gecos = x.proc.internCString("Mobile User");
        const uint32_t dir  = x.proc.internCString(x.proc.guestHomePath());
        const uint32_t sh   = x.proc.internCString("/bin/sh");
        x.mem.write<uint32_t>(addr + 0, name);  // pw_name
        x.mem.write<uint32_t>(addr + 4, empty); // pw_passwd
        x.mem.write<uint32_t>(addr + 8, 501);   // pw_uid
        x.mem.write<uint32_t>(addr + 12, 501);  // pw_gid
        x.mem.write<uint32_t>(addr + 16, 0);    // pw_change
        x.mem.write<uint32_t>(addr + 20, empty);// pw_class
        x.mem.write<uint32_t>(addr + 24, gecos);// pw_gecos
        x.mem.write<uint32_t>(addr + 28, dir);  // pw_dir
        x.mem.write<uint32_t>(addr + 32, sh);   // pw_shell
        x.mem.write<uint32_t>(addr + 36, 0);    // pw_expire
    }
    return addr;
}
GuestPtr s_getpwnam(Ctx& x, GuestPtr name) {
    return name && x.proc.readCString(name) == "mobile" ? s_getpwuid(x, 501) : 0;
}
// getpw*_r(key, struct passwd* pwd, char* buf, size_t len, struct passwd** result):
// the strings stay in process-lifetime storage, so only the struct is copied.
int32_t pwCopy(Ctx& x, bool found, GuestPtr pwd, GuestPtr result) {
    if (result) x.mem.write<uint32_t>(result, 0);
    if (!found) return 0;   // no such user: 0 with *result = NULL
    if (!pwd) return 22;    // EINVAL
    x.mem.copyIn(pwd, x.mem.host(s_getpwuid(x, 501)), kPasswdSize);
    if (result) x.mem.write<uint32_t>(result, pwd);
    return 0;
}
int32_t s_getpwuid_r(Ctx& x, uint32_t uid, GuestPtr pwd, GuestPtr, uint32_t, GuestPtr result) {
    return pwCopy(x, uid == 501, pwd, result);
}
int32_t s_getpwnam_r(Ctx& x, GuestPtr name, GuestPtr pwd, GuestPtr, uint32_t, GuestPtr result) {
    return pwCopy(x, name && x.proc.readCString(name) == "mobile", pwd, result);
}

// iconv, forwarded to the host's iconv. Guest handles index a per-process table.
struct Iconvs {
    explicit Iconvs(Process&) {}
    ~Iconvs() { for (auto& [h, cd] : open) iconv_close(cd); }
    std::mutex mu;
    std::unordered_map<uint32_t, iconv_t> open;
    uint32_t next = 1;
};
uint32_t s_iconv_open(Ctx& x, GuestPtr to, GuestPtr from) {
    const iconv_t cd = iconv_open(x.proc.readCString(to).c_str(), x.proc.readCString(from).c_str());
    if (cd == iconv_t(-1)) { setErrno(x, darwinErrno(errno)); return 0xFFFFFFFFu; }
    auto& t = x.proc.service<Iconvs>();
    std::lock_guard<std::mutex> lk(t.mu);
    const uint32_t h = t.next++;
    t.open[h] = cd;
    return h;
}
int32_t s_iconv_close(Ctx& x, uint32_t h) {
    auto& t = x.proc.service<Iconvs>();
    std::lock_guard<std::mutex> lk(t.mu);
    auto it = t.open.find(h);
    if (it == t.open.end()) { setErrno(x, 9 /* EBADF */); return -1; }
    iconv_close(it->second);
    t.open.erase(it);
    return 0;
}
// size_t iconv(cd, char** inbuf, size_t* inleft, char** outbuf, size_t* outleft)
uint32_t s_iconv(Ctx& x, uint32_t h, GuestPtr inbuf, GuestPtr inleft, GuestPtr outbuf, GuestPtr outleft) {
    iconv_t cd;
    {
        auto& t = x.proc.service<Iconvs>();
        std::lock_guard<std::mutex> lk(t.mu);
        auto it = t.open.find(h);
        if (it == t.open.end()) { setErrno(x, 9 /* EBADF */); return 0xFFFFFFFFu; }
        cd = it->second;
    }
    const uint32_t inPtr = inbuf ? x.mem.read<uint32_t>(inbuf) : 0;
    const uint32_t outPtr = outbuf ? x.mem.read<uint32_t>(outbuf) : 0;
    size_t inN = inleft ? x.mem.read<uint32_t>(inleft) : 0;
    size_t outN = outleft ? x.mem.read<uint32_t>(outleft) : 0;
    char* in = inPtr ? reinterpret_cast<char*>(x.mem.host(inPtr)) : nullptr;
    char* out = outPtr ? reinterpret_cast<char*>(x.mem.host(outPtr)) : nullptr;
    char* in0 = in;
    char* out0 = out;
    const size_t r = iconv(cd, in ? &in : nullptr, in ? &inN : nullptr, out ? &out : nullptr, out ? &outN : nullptr);
    const int err = errno;
    if (inbuf && in) x.mem.write<uint32_t>(inbuf, inPtr + uint32_t(in - in0));
    if (outbuf && out) x.mem.write<uint32_t>(outbuf, outPtr + uint32_t(out - out0));
    if (inleft && in) x.mem.write<uint32_t>(inleft, uint32_t(inN));
    if (outleft && out) x.mem.write<uint32_t>(outleft, uint32_t(outN));
    if (r == size_t(-1)) { setErrno(x, darwinErrno(err)); return 0xFFFFFFFFu; }
    return uint32_t(r);
}
GuestPtr s_basename(Ctx& x, GuestPtr p) {
    if (!p) return x.proc.internCString(".");
    std::string s = x.proc.readCString(p);
    while (s.size() > 1 && s.back() == '/') s.pop_back();
    const size_t slash = s.rfind('/');
    if (slash != std::string::npos) s = s.substr(slash + 1);
    return x.proc.internCString(s.empty() ? "." : s);
}
int32_t s_ioctl(Ctx& x, int32_t fd, uint32_t req, GuestPtr arg) {
    // Sockets: FIONBIO (non-blocking) and FIONREAD (bytes waiting) matter to network code.
    if (isNetworkSocket(x.proc, fd) && arg) {
        if (req == 0x8004667e) {
            const int flags = ::fcntl(fd, F_GETFL);
            if (flags < 0) return failD<int32_t>(x);
            const bool on = x.mem.read<uint32_t>(arg) != 0;
            return ::fcntl(fd, F_SETFL, on ? (flags | O_NONBLOCK) : (flags & ~O_NONBLOCK)) < 0 ? failD<int32_t>(x) : 0;
        }
        if (req == 0x4004667f) {
            int n = 0;
            if (::ioctl(fd, FIONREAD, &n) < 0) return failD<int32_t>(x);
            x.mem.write<int32_t>(arg, n);
            return 0;
        }
    }
    return 0;
}
int32_t s_zero(Ctx&) { return 0; }
int32_t s_isatty(Ctx&, int32_t) { return 0; }
int32_t s_getpagesize(Ctx&) { return 4096; }
int32_t s_sysconf(Ctx&, int32_t name) {
    switch (name) {
        case 29: return 4096;          // _SC_PAGESIZE
        case 57: case 58: return 2;    // _SC_NPROCESSORS_CONF / ONLN
        case 200: return 262144;       // _SC_PHYS_PAGES (1 GB)
        case 5: return 256;            // _SC_OPEN_MAX
        case 3: return 100;            // _SC_CLK_TCK
        default: return -1;
    }
}
int32_t s_getrlimit(Ctx& x, int32_t, GuestPtr rl) {
    for (int i = 0; i < 4; ++i) x.mem.write<uint32_t>(rl + uint32_t(i) * 4, i % 2 ? 0x7FFFFFFF : 0xFFFFFFFF);
    x.mem.write<uint64_t>(rl, 8u << 20); x.mem.write<uint64_t>(rl + 8, 0x7FFFFFFFFFFFFFFFull);
    return 0;
}
int32_t s_ok2(Ctx&, int32_t, GuestPtr) { return 0; }
int32_t s_getrusage(Ctx& x, int32_t, GuestPtr ru) { std::memset(x.mem.host(ru), 0, 72); return 0; }

// ------------------------------------------------------------ sysctl ---

struct SysInfo { std::string machine, osrelease = "13.0.0", osversion = "10B329", hostname = "iPhone"; };
SysInfo sysInfo(Ctx& x) { SysInfo s; s.machine = x.proc.config().deviceModel; return s; }

int32_t writeSysctl(Ctx& x, GuestPtr oldp, GuestPtr oldlenp, const void* data, uint32_t len) {
    if (!oldlenp) return 0;
    if (!oldp) { x.mem.write<uint32_t>(oldlenp, len); return 0; }
    const uint32_t cap = x.mem.read<uint32_t>(oldlenp);
    if (cap < len) { x.mem.copyIn(oldp, data, cap); x.mem.write<uint32_t>(oldlenp, cap); setErrno(x, ENOMEM); return -1; }
    x.mem.copyIn(oldp, data, len);
    x.mem.write<uint32_t>(oldlenp, len);
    return 0;
}
int32_t sysctlValue(Ctx& x, const std::string& name, GuestPtr oldp, GuestPtr oldlenp) {
    const SysInfo si = sysInfo(x);
    auto str = [&](const std::string& s) { return writeSysctl(x, oldp, oldlenp, s.c_str(), uint32_t(s.size() + 1)); };
    auto u32 = [&](uint32_t v) { return writeSysctl(x, oldp, oldlenp, &v, 4); };
    auto u64 = [&](uint64_t v) { return writeSysctl(x, oldp, oldlenp, &v, 8); };
    if (name == "hw.machine") return str(si.machine);
    if (name == "hw.model") return str("N41AP");
    if (name == "hw.ncpu" || name == "hw.activecpu" || name == "hw.physicalcpu" || name == "hw.logicalcpu") return u32(2);
    if (name == "hw.physmem" || name == "hw.usermem") return u32(1u << 30);
    if (name == "hw.memsize") return u64(1ull << 30);
    if (name == "hw.pagesize") return u32(4096);
    if (name == "hw.cpufrequency" || name == "hw.cpufrequency_max") return u32(1300000000);
    if (name == "hw.busfrequency") return u32(250000000);
    if (name == "hw.cputype") return u32(12);
    if (name == "hw.cpusubtype") return u32(11);
    if (name == "hw.cachelinesize") return u32(64);
    if (name == "hw.l1dcachesize" || name == "hw.l1icachesize") return u32(32768);
    if (name == "hw.l2cachesize") return u32(1u << 20);
    if (name == "hw.optional.neon") return u32(1);
    if (name == "kern.osrelease") return str(si.osrelease);
    if (name == "kern.osversion") return str(si.osversion);
    if (name == "kern.ostype") return str("Darwin");
    if (name == "kern.hostname") return str(si.hostname);
    if (name == "kern.version") return str("Darwin Kernel Version 13.0.0: HydrogenVM; root:xnu/RELEASE_ARM_S5L8950X");
    if (name == "kern.boottime") { const uint32_t tv[2] = {uint32_t(std::time(nullptr) - 86400), 0}; return writeSysctl(x, oldp, oldlenp, tv, 8); }
    if (name == "kern.maxproc") return u32(512);
    setErrno(x, ENOENT);
    return -1;
}
int32_t s_sysctlbyname(Ctx& x, GuestPtr name, GuestPtr oldp, GuestPtr oldlenp, GuestPtr, uint32_t) {
    return sysctlValue(x, x.proc.readCString(name), oldp, oldlenp);
}
int32_t s_sysctl(Ctx& x, GuestPtr mib, uint32_t n, GuestPtr oldp, GuestPtr oldlenp, GuestPtr, uint32_t) {
    if (n < 2) { setErrno(x, EINVAL); return -1; }
    const uint32_t a = x.mem.read<uint32_t>(mib), b = x.mem.read<uint32_t>(mib + 4);
    if (a == 6) {   // CTL_HW
        static const std::unordered_map<uint32_t, const char*> hw = {
            {1, "hw.machine"}, {2, "hw.model"}, {3, "hw.ncpu"}, {5, "hw.physmem"}, {6, "hw.usermem"}, {7, "hw.pagesize"},
            {15, "hw.cpufrequency"}, {14, "hw.busfrequency"}, {24, "hw.memsize"}, {25, "hw.activecpu"}};
        auto it = hw.find(b);
        if (it != hw.end()) return sysctlValue(x, it->second, oldp, oldlenp);
    } else if (a == 1) {   // CTL_KERN
        static const std::unordered_map<uint32_t, const char*> kern = {
            {1, "kern.ostype"}, {2, "kern.osrelease"}, {4, "kern.version"}, {6, "kern.maxproc"}, {10, "kern.hostname"},
            {21, "kern.boottime"}, {65, "kern.osversion"}};
        auto it = kern.find(b);
        if (it != kern.end()) return sysctlValue(x, it->second, oldp, oldlenp);
        if (b == 14) {   // KERN_PROC: some apps check P_TRACED for debuggers; report a plain process
            std::vector<uint8_t> zeros(492, 0);
            return writeSysctl(x, oldp, oldlenp, zeros.data(), uint32_t(zeros.size()));
        }
    } else if (a == 4 && b == 17 && n >= 6 && x.mem.read<uint32_t>(mib + 12) == 18 && x.mem.read<uint32_t>(mib + 16) == 3) {
        // CTL_NET / AF_ROUTE / 0 / AF_LINK / NET_RT_IFLIST / index: one if_msghdr (112 bytes on
        // 32-bit Darwin) followed by the interface's sockaddr_dl. en0 has the MAC address iOS 7+
        // reports to apps (02:00:00:00:00:00); lo0 has none.
        const uint32_t index = x.mem.read<uint32_t>(mib + 20);
        if (index != 1 && index != 2) { setErrno(x, ENXIO); return -1; }
        const bool en0 = index == 2;
        uint8_t msg[112 + 20] = {};
        const uint16_t len = sizeof msg;
        std::memcpy(msg, &len, 2);
        msg[2] = 5;                     // RTM_VERSION
        msg[3] = 0xe;                   // RTM_IFINFO
        const uint32_t addrs = 0x10, flags = en0 ? 0x8863 : 0x8049;   // RTA_IFP; UP|BROADCAST|RUNNING|SIMPLEX|MULTICAST or loopback
        std::memcpy(msg + 4, &addrs, 4);
        std::memcpy(msg + 8, &flags, 4);
        const uint16_t idx = uint16_t(index);
        std::memcpy(msg + 12, &idx, 2);
        msg[16] = en0 ? 6 : 24;          // ifi_type: IFT_ETHER / IFT_LOOP
        msg[19] = en0 ? 6 : 0;           // ifi_addrlen
        uint8_t* sdl = msg + 112;
        sdl[0] = 20; sdl[1] = 18;        // sdl_len, AF_LINK
        std::memcpy(sdl + 2, &idx, 2);
        sdl[4] = msg[16];                // sdl_type
        sdl[5] = 3;                      // sdl_nlen
        sdl[6] = en0 ? 6 : 0;            // sdl_alen
        std::memcpy(sdl + 8, en0 ? "en0" : "lo0", 3);
        if (en0) sdl[11] = 0x02;         // 02:00:00:00:00:00
        return writeSysctl(x, oldp, oldlenp, msg, sizeof msg);
    }
    setErrno(x, ENOENT);
    return -1;
}
int32_t s_uname(Ctx& x, GuestPtr u) {
    const SysInfo si = sysInfo(x);
    const std::string f[5] = {"Darwin", si.hostname, si.osrelease, "Darwin Kernel Version 13.0.0: HydrogenVM", si.machine};
    for (int i = 0; i < 5; ++i) {
        std::string s = f[i].substr(0, 255);
        x.mem.copyIn(u + uint32_t(i) * 256, s.c_str(), s.size() + 1);
    }
    return 0;
}

// ---------------------------------------------------------------- dyld ---

uint32_t d_image_count(Ctx&) { return 1; }
uint32_t d_image_header(Ctx& x, uint32_t i) { return i == 0 ? x.proc.image().headerAddr : 0; }
uint32_t d_image_name(Ctx& x, uint32_t i) { return i == 0 ? x.proc.internCString(x.proc.guestExecutablePath()) : 0; }
int32_t d_image_slide(Ctx& x, uint32_t i) { return i == 0 ? int32_t(x.proc.image().slide) : 0; }
void d_register_add_image(Ctx& x, GuestPtr fn) { x.proc.callGuest(x.cpu, fn, {x.proc.image().headerAddr, x.proc.image().slide}); }
void d_register_remove_image(Ctx&, GuestPtr) {}
int32_t d_version(Ctx&, GuestPtr) { return -1; }
uint32_t d_dlopen(Ctx& x, GuestPtr path, int32_t mode) {
    // Like dyld: a NULL path returns the magic RTLD_DEFAULT (-2), or RTLD_MAIN_ONLY (-5)
    // with RTLD_FIRST. Unity's Mono only consults its registered P/Invoke symbols
    // (mono_dl_register_symbol) when the handle is RTLD_DEFAULT.
    if (!path) return (mode & 0x100) ? 0xFFFFFFFBu : 0xFFFFFFFEu;
    const std::string p = x.proc.readCString(path);
    x.proc.logf(LogLevel::Debug, "dlopen(%s)", p.c_str());
    return 0x7E000000u | (uint32_t(std::hash<std::string>{}(p)) & 0xFFF0);   // fake handle; dlsym searches everything
}
uint32_t d_dlsym(Ctx& x, uint32_t, GuestPtr name) {
    const std::string n = "_" + x.proc.readCString(name);
    if (auto a = x.proc.lookupSymbol(n)) return *a;
    // The app's own exported functions (Unity/Mono P/Invoke into plugins linked
    // into the executable, e.g. "InitEveryplay"); symbolsByAddr keeps one name per
    // address and misses aliases.
    if (auto it = x.proc.image().exports.find(n); it != x.proc.image().exports.end()) return it->second;
    for (const auto& [addr, sym] : x.proc.image().symbolsByAddr)
        if (sym == n) return addr;
    x.proc.logf(LogLevel::Warning, "dlsym: %s not found", n.c_str() + 1);
    return 0;
}
uint32_t d_local_arch_info(Ctx& x) {
    static const char slotKey = 0;
    uint32_t& a = x.proc.processSlot(&slotKey);
    if (!a) {
        a = x.proc.allocStatic(20);
        x.mem.write<uint32_t>(a + 0, x.proc.internCString("armv7"));
        x.mem.write<uint32_t>(a + 4, 12);   // CPU_TYPE_ARM
        x.mem.write<uint32_t>(a + 8, 9);    // CPU_SUBTYPE_ARM_V7
        x.mem.write<uint32_t>(a + 12, 1);   // NX_LittleEndian
        x.mem.write<uint32_t>(a + 16, x.proc.internCString("arm v7"));
    }
    return a;
}
uint32_t d_arch_from_cputype(Ctx& x, uint32_t, uint32_t) { return d_local_arch_info(x); }
int32_t d_dlclose(Ctx&, uint32_t) { return 0; }
uint32_t d_dlerror(Ctx&) { return 0; }
int32_t d_dladdr(Ctx& x, GuestPtr addr, GuestPtr info) {
    const auto& img = x.proc.image();
    if (addr < img.lowAddr || addr >= img.highAddr) return 0;
    x.mem.write<uint32_t>(info, x.proc.internCString(x.proc.guestExecutablePath()));
    x.mem.write<uint32_t>(info + 4, img.headerAddr);
    const std::string sym = img.symbolicate(addr & ~1u);
    const std::string name = sym.substr(0, sym.find(" + "));
    x.mem.write<uint32_t>(info + 8, name.empty() ? 0 : x.proc.internCString(name.substr(name[0] == '_' ? 1 : 0)));
    uint32_t saddr = 0;
    for (auto& [a, n] : img.symbolsByAddr) if (n == name) { saddr = a; break; }
    x.mem.write<uint32_t>(info + 12, saddr);
    return 1;
}
// Old binaries: crt1's __dyld_func_lookup(name, &fn).
int32_t d_func_lookup(Ctx& x, GuestPtr name, GuestPtr out) {
    const std::string n = x.proc.readCString(name);
    x.proc.logf(LogLevel::Debug, "dyld_func_lookup(%s)", n.c_str());
    x.mem.write<uint32_t>(out, *x.proc.lookupSymbol("__hvm_noop"));
    return 1;
}
void d_lazy_entry(Ctx&) { throw std::runtime_error("lazy binding helper reached (all imports are bound at load time)"); }
void s_noop(Ctx&) {}

// ----------------------------------------------------- files (POSIX fds) ---

struct OpenFds {
    explicit OpenFds(Process&) {}
    std::mutex mu;
    struct FdTrack { uint64_t fileHandle = 0; int64_t bytesRead = 0; };
    std::unordered_map<int32_t, FdTrack> tracks;
};

int hostOpenFlags(uint32_t f) {
    int h = int(f & 3);
    if (f & 0x4) h |= O_NONBLOCK;
    if (f & 0x8) h |= O_APPEND;
    if (f & 0x200) h |= O_CREAT;
    if (f & 0x400) h |= O_TRUNC;
    if (f & 0x800) h |= O_EXCL;
    if (f & 0x100000) h |= O_DIRECTORY;
    return h | O_CLOEXEC;
}
int32_t f_open(Ctx& x, GuestPtr path, uint32_t flags, VarArgs va) {
    const uint32_t mode = (flags & 0x200) ? va.args.word() : 0;
    const std::string gp = x.proc.readCString(path);
    const std::string hp = x.proc.hostPath(gp);
    const int fd = ::open(hp.c_str(), hostOpenFlags(flags), mode & 0777);
    if (fd < 0) {
        x.proc.recordFileOpen(gp, "open", errno, -1, 0, false);
        return failD<int32_t>(x);
    }
    int64_t sz = -1;
    struct stat st;
    if (::fstat(fd, &st) == 0) sz = st.st_size;
    const uint64_t h = x.proc.recordFileOpen(gp, "open", 0, sz, 0, true);
    auto& ofs = x.proc.service<OpenFds>();
    std::lock_guard<std::mutex> lk(ofs.mu);
    ofs.tracks[fd] = {h, 0};
    return fd;
}
int32_t f_close(Ctx& x, int32_t fd) {
    if (fd >= 0 && fd <= 2) return 0;
    {
        auto& ofs = x.proc.service<OpenFds>();
        std::lock_guard<std::mutex> lk(ofs.mu);
        auto it = ofs.tracks.find(fd);
        if (it != ofs.tracks.end()) {
            x.proc.recordFileClose(it->second.fileHandle);
            ofs.tracks.erase(it);
        }
    }
    const int tracked = closeNetworkSocket(x.proc, fd);
    return (tracked == -2 ? ::close(fd) : tracked) < 0 ? failD<int32_t>(x) : 0;
}
int32_t f_read(Ctx& x, int32_t fd, GuestPtr buf, uint32_t n) {
    if (fd == 0) return 0;
    if (isNetworkSocket(x.proc, fd)) return networkRead(x, fd, buf, n);
    const ssize_t r = ::read(fd, x.mem.host(buf), n);
    if (r > 0) {
        auto& ofs = x.proc.service<OpenFds>();
        std::lock_guard<std::mutex> lk(ofs.mu);
        auto it = ofs.tracks.find(fd);
        if (it != ofs.tracks.end()) {
            it->second.bytesRead += r;
            x.proc.updateFileRead(it->second.fileHandle, it->second.bytesRead);
        }
    }
    return r < 0 ? failD<int32_t>(x) : int32_t(r);
}
int32_t f_write(Ctx& x, int32_t fd, GuestPtr buf, uint32_t n) {
    if (fd == 1 || fd == 2) {
        x.proc.log(fd == 1 ? LogLevel::GuestStdout : LogLevel::GuestStderr, std::string(reinterpret_cast<const char*>(x.mem.host(buf)), n));
        return int32_t(n);
    }
    if (isNetworkSocket(x.proc, fd)) return networkWrite(x, fd, buf, n);
    const ssize_t r = ::write(fd, x.mem.host(buf), n);
    return r < 0 ? failD<int32_t>(x) : int32_t(r);
}
int32_t f_writev(Ctx& x, int32_t fd, GuestPtr vectors, int32_t count) {
    auto readable = [&](uint32_t address, uint32_t size) {
        if (!size) return true;
        const uint64_t end = uint64_t(address) + size;
        if (end > (1ull << 32)) return false;
        for (uint64_t page = address & ~4095u; page < end; page += 4096)
            if (!(x.mem.protAt(uint32_t(page)) & Memory::kProtRead)) return false;
        return true;
    };
    if (count < 0 || count > 1024) { setErrno(x, 22); return -1; }
    if (!readable(vectors, uint32_t(count) * 8)) { setErrno(x, 14); return -1; }
    std::vector<iovec> native;
    uint64_t total = 0;
    for (int i = 0; i < count; ++i) {
        const uint32_t base = x.mem.read<uint32_t>(vectors + i * 8), size = x.mem.read<uint32_t>(vectors + i * 8 + 4);
        if (!readable(base, size)) { setErrno(x, 14); return -1; }
        total += size;
        if (total > 0x7fffffff) { setErrno(x, 22); return -1; }
        native.push_back({x.mem.host(base), size});
    }
    if (fd == 1 || fd == 2 || isNetworkSocket(x.proc, fd)) {
        int32_t written = 0;
        for (int i = 0; i < count; ++i) {
            if (!native[i].iov_len) continue;
            const int32_t n = f_write(x, fd, x.mem.read<uint32_t>(vectors + i * 8), uint32_t(native[i].iov_len));
            if (n < 0) return written ? written : n;
            written += n;
            if (uint32_t(n) < native[i].iov_len) break;
        }
        return written;
    }
    const auto result = ::writev(fd, native.data(), count);
    return result < 0 ? failD<int32_t>(x) : int32_t(result);
}
int32_t f_flock(Ctx& x, int32_t fd, int32_t operation) {
    if ((operation & ~15) || !((operation & 3) == 1 || (operation & 3) == 2 || (operation & 8))) { setErrno(x, 22); return -1; }
    return ::flock(fd, operation) < 0 ? failD<int32_t>(x) : 0;
}

int32_t f_pread(Ctx& x, int32_t fd, GuestPtr buf, uint32_t n, int64_t off) {
    const ssize_t r = ::pread(fd, x.mem.host(buf), n, off_t(off));
    return r < 0 ? failD<int32_t>(x) : int32_t(r);
}
int32_t f_pwrite(Ctx& x, int32_t fd, GuestPtr buf, uint32_t n, int64_t off) {
    const ssize_t r = ::pwrite(fd, x.mem.host(buf), n, off_t(off));
    return r < 0 ? failD<int32_t>(x) : int32_t(r);
}
int64_t f_lseek(Ctx& x, int32_t fd, int64_t off, int32_t whence) {
    const off_t r = ::lseek(fd, off_t(off), whence);
    return r < 0 ? failD<int64_t>(x) : int64_t(r);
}
int32_t f_fcntl(Ctx& x, int32_t fd, int32_t cmd, VarArgs va) {
    const uint32_t arg = va.args.word();
    switch (cmd) {
        case 1: { int r = ::fcntl(fd, F_GETFD); return r < 0 ? failD<int32_t>(x) : (r & FD_CLOEXEC ? 1 : 0); }
        case 2: return ::fcntl(fd, F_SETFD, arg & 1 ? FD_CLOEXEC : 0) < 0 ? failD<int32_t>(x) : 0;
        case 3: {
            int r = ::fcntl(fd, F_GETFL);
            return r < 0 ? failD<int32_t>(x) : (r & O_ACCMODE) | (r & O_NONBLOCK ? 4 : 0) | (r & O_APPEND ? 8 : 0);
        }
        case 4: {
            if (arg & ~15u) { setErrno(x, 22); return -1; }
            int flags = ::fcntl(fd, F_GETFL);
            if (flags < 0) return failD<int32_t>(x);
            flags = (flags & ~(O_NONBLOCK | O_APPEND)) | (arg & 4 ? O_NONBLOCK : 0) | (arg & 8 ? O_APPEND : 0);
            return ::fcntl(fd, F_SETFL, flags) < 0 ? failD<int32_t>(x) : 0;
        }
        case 48: case 51: return 0;                   // F_NOCACHE / F_FULLFSYNC
        default: (void)fd; (void)arg; setErrno(x, EINVAL); return -1;
    }
}
int32_t f_fsync(Ctx&, int32_t fd) { if (fd > 2) ::fsync(fd); return 0; }
int32_t f_ftruncate(Ctx& x, int32_t fd, int64_t len) { return ::ftruncate(fd, off_t(len)) < 0 ? failD<int32_t>(x) : 0; }
int32_t f_dup(Ctx& x, int32_t fd) {
    const int r = ::dup(fd);
    if (r >= 0) duplicateNetworkSocket(x.proc, fd, r);
    return r < 0 ? failD<int32_t>(x) : r;
}

// Darwin (64-bit inode) struct stat, 4-byte-aligned 64-bit fields: 108 bytes.
void storeStat(Ctx& x, GuestPtr out, const struct stat& st) {
    std::memset(x.mem.host(out), 0, 108);
    uint16_t mode = uint16_t(st.st_mode & 0xFFF);
    if (S_ISDIR(st.st_mode)) mode |= 0040000;
    else if (S_ISREG(st.st_mode)) mode |= 0100000;
    else if (S_ISLNK(st.st_mode)) mode |= 0120000;
    else if (S_ISCHR(st.st_mode)) mode |= 0020000;
    else if (S_ISFIFO(st.st_mode)) mode |= 0010000;
    x.mem.write<int32_t>(out + 0, int32_t(st.st_dev));
    x.mem.write<uint16_t>(out + 4, mode);
    x.mem.write<uint16_t>(out + 6, uint16_t(st.st_nlink));
    x.mem.write<uint64_t>(out + 8, uint64_t(st.st_ino));
    x.mem.write<uint32_t>(out + 16, 501);
    x.mem.write<uint32_t>(out + 20, 501);
    auto ts = [&](uint32_t off, const timespec& t) { x.mem.write<int32_t>(out + off, int32_t(t.tv_sec)); x.mem.write<int32_t>(out + off + 4, int32_t(t.tv_nsec)); };
#ifdef __APPLE__
    ts(28, st.st_atimespec); ts(36, st.st_mtimespec); ts(44, st.st_ctimespec); ts(52, st.st_birthtimespec);
#else
    ts(28, st.st_atim); ts(36, st.st_mtim); ts(44, st.st_ctim); ts(52, st.st_mtim);
#endif
    x.mem.write<int64_t>(out + 60, int64_t(st.st_size));
    x.mem.write<int64_t>(out + 68, int64_t(st.st_blocks));
    x.mem.write<int32_t>(out + 76, int32_t(st.st_blksize));
}
int32_t f_stat(Ctx& x, GuestPtr path, GuestPtr out) {
    struct stat st;
    if (::stat(x.proc.hostPath(x.proc.readCString(path)).c_str(), &st) < 0) return failD<int32_t>(x);
    storeStat(x, out, st);
    return 0;
}
int32_t f_lstat(Ctx& x, GuestPtr path, GuestPtr out) {
    struct stat st;
    if (::lstat(x.proc.hostPath(x.proc.readCString(path)).c_str(), &st) < 0) return failD<int32_t>(x);
    storeStat(x, out, st);
    return 0;
}
int32_t f_fstat(Ctx& x, int32_t fd, GuestPtr out) {
    struct stat st;
    if (fd <= 2) { std::memset(&st, 0, sizeof st); st.st_mode = S_IFCHR | 0666; storeStat(x, out, st); return 0; }
    if (::fstat(fd, &st) < 0) return failD<int32_t>(x);
    storeStat(x, out, st);
    return 0;
}
int32_t f_access(Ctx& x, GuestPtr path, int32_t mode) { return ::access(x.proc.hostPath(x.proc.readCString(path)).c_str(), mode) < 0 ? failD<int32_t>(x) : 0; }
int32_t f_mkdir(Ctx& x, GuestPtr path, uint32_t mode) { return ::mkdir(x.proc.hostPath(x.proc.readCString(path)).c_str(), mode & 0777) < 0 ? failD<int32_t>(x) : 0; }
int32_t f_rmdir(Ctx& x, GuestPtr path) { return ::rmdir(x.proc.hostPath(x.proc.readCString(path)).c_str()) < 0 ? failD<int32_t>(x) : 0; }
int32_t f_unlink(Ctx& x, GuestPtr path) { return ::unlink(x.proc.hostPath(x.proc.readCString(path)).c_str()) < 0 ? failD<int32_t>(x) : 0; }
int32_t f_chdir(Ctx& x, GuestPtr path) { return ::chdir(x.proc.hostPath(x.proc.readCString(path)).c_str()) < 0 ? failD<int32_t>(x) : 0; }
uint32_t f_getcwd(Ctx& x, GuestPtr buf, uint32_t size) {
    char tmp[4096];
    if (!::getcwd(tmp, sizeof tmp)) return failD<uint32_t>(x, 0);
    const std::string s = x.proc.guestPath(tmp);
    if (!buf) return x.proc.allocCString(s);
    if (size < s.size() + 1) { setErrno(x, ERANGE); return 0; }
    x.mem.copyIn(buf, s.c_str(), s.size() + 1);
    return buf;
}
int32_t f_chmod(Ctx&, GuestPtr, uint32_t) { return 0; }
int32_t f_utimes(Ctx&, GuestPtr, GuestPtr) { return 0; }
int32_t f_utime(Ctx&, GuestPtr, GuestPtr) { return 0; }

struct Dirs {
    explicit Dirs(Process&) {}
    std::mutex mu;
    struct D { DIR* d; uint32_t entry; };
    std::unordered_map<uint32_t, D> open;
};
uint32_t f_opendir(Ctx& x, GuestPtr path) {
    DIR* d = ::opendir(x.proc.hostPath(x.proc.readCString(path)).c_str());
    if (!d) return failD<uint32_t>(x, 0);
    const uint32_t h = x.proc.heap().allocZeroed(64);
    const uint32_t entry = x.proc.heap().allocZeroed(1048);
    auto& ds = x.proc.service<Dirs>();
    std::lock_guard<std::mutex> lk(ds.mu);
    ds.open[h] = {d, entry};
    return h;
}
// struct dirent (64-bit inode): ino 8, seekoff 8, reclen 2, namlen 2, type 1, name[1024]
uint32_t f_readdir(Ctx& x, GuestPtr h) {
    auto& ds = x.proc.service<Dirs>();
    std::lock_guard<std::mutex> lk(ds.mu);
    auto it = ds.open.find(h);
    if (it == ds.open.end()) return 0;
    dirent* e = ::readdir(it->second.d);
    if (!e) return 0;
    const uint32_t o = it->second.entry;
    const size_t n = std::min<size_t>(std::strlen(e->d_name), 1023);
    x.mem.write<uint64_t>(o, uint64_t(e->d_ino));
    x.mem.write<uint64_t>(o + 8, 0);
    x.mem.write<uint16_t>(o + 16, uint16_t(21 + n + 1));
    x.mem.write<uint16_t>(o + 18, uint16_t(n));
    x.mem.write<uint8_t>(o + 20, e->d_type);
    x.mem.copyIn(o + 21, e->d_name, n);
    x.mem.write<uint8_t>(o + 21 + uint32_t(n), 0);
    return o;
}
int32_t f_closedir(Ctx& x, GuestPtr h) {
    auto& ds = x.proc.service<Dirs>();
    uint32_t entry = 0;
    {
        std::lock_guard<std::mutex> lk(ds.mu);
        auto it = ds.open.find(h);
        if (it == ds.open.end()) return -1;
        ::closedir(it->second.d);
        entry = it->second.entry;
        ds.open.erase(it);
    }
    x.proc.heap().free(entry);
    x.proc.heap().free(h);
    return 0;
}

// ------------------------------------------------------------------ mmap ---

uint32_t m_mmap(Ctx& x, GuestPtr addr, uint32_t len, int32_t prot, uint32_t flags, int32_t fd, int64_t off) {
    if (!len) { setErrno(x, EINVAL); return 0xFFFFFFFFu; }
    const uint32_t sz = (len + 4095u) & ~4095u;
    constexpr uint32_t kMapFixed = 0x0010;
    if (flags & kMapFixed) {
        if (!addr || (addr & 4095u) || (uint64_t(addr) + sz > 0xFFFFFFFFull)) {
            setErrno(x, EINVAL);
            return 0xFFFFFFFFu;
        }
        x.mem.map(addr, sz, Memory::kProtRW);
        if (!(flags & 0x1000) && fd >= 0) {
            const ssize_t r = ::pread(fd, x.mem.host(addr), len, off_t(off));
            if (r < 0) return failD<uint32_t>(x, 0xFFFFFFFFu);
        } else if (prot != 0) {
            std::memset(x.mem.host(addr), 0, sz);
        }
        return addr;
    }
    const uint32_t align = len >= 65536u ? 65536u : 4096u;
    const uint32_t a = x.proc.heap().allocAligned(sz ? sz : 4096u, align);
    if (!a) {
        setErrno(x, ENOMEM);
        return 0xFFFFFFFFu;
    }
    std::memset(x.mem.host(a), 0, sz ? sz : 4096u);
    if (!(flags & 0x1000) && fd >= 0) {   // file mapping: read the contents (private copy)
        const ssize_t r = ::pread(fd, x.mem.host(a), len, off_t(off));
        if (r < 0) { x.proc.heap().free(a); return failD<uint32_t>(x, 0xFFFFFFFFu); }
    }
    return a;
}
int32_t m_munmap(Ctx& x, GuestPtr a, uint32_t len) {
    const uint32_t bsz = x.proc.heap().blockSize(a);
    if (bsz && len >= bsz) x.proc.heap().free(a);
    return 0;
}
int32_t m_ok3(Ctx&, GuestPtr, uint32_t, int32_t) { return 0; }
int32_t m_ok2(Ctx&, GuestPtr, uint32_t) { return 0; }

// --------------------------------------------------------------- signals ---

// Guest signal handlers. Only faults are delivered: when guest code faults on a
// memory access and the app installed a SIGSEGV/SIGBUS handler (Mono does, to turn
// null dereferences in managed code into NullReferenceException), the handler runs
// on the faulting thread with a Darwin ARM siginfo/ucontext, and execution resumes
// from the registers it leaves in the context.
struct GuestSignals {
    explicit GuestSignals(Process&) {}
    struct Act { uint32_t handler = 0, mask = 0, flags = 0; };
    std::mutex mu;
    Act acts[32];
};
GuestSignals::Act signalAction(Process& p, int sig) {
    auto& s = p.service<GuestSignals>();
    std::lock_guard<std::mutex> lk(s.mu);
    return sig > 0 && sig < 32 ? s.acts[sig] : GuestSignals::Act{};
}
// struct sigaction (Darwin user): handler, sa_mask, sa_flags (12 bytes).
int32_t g_sigaction(Ctx& x, int32_t sig, GuestPtr act, GuestPtr old) {
    if (sig <= 0 || sig >= 32) { setErrno(x, 22); return -1; }
    auto& s = x.proc.service<GuestSignals>();
    std::lock_guard<std::mutex> lk(s.mu);
    if (old) {
        x.mem.write<uint32_t>(old, s.acts[sig].handler);
        x.mem.write<uint32_t>(old + 4, s.acts[sig].mask);
        x.mem.write<uint32_t>(old + 8, s.acts[sig].flags);
    }
    if (act) s.acts[sig] = {x.mem.read<uint32_t>(act), x.mem.read<uint32_t>(act + 4), x.mem.read<uint32_t>(act + 8)};
    return 0;
}
uint32_t g_signal(Ctx& x, int32_t sig, GuestPtr handler) {
    if (sig <= 0 || sig >= 32) { setErrno(x, 22); return 0xFFFFFFFFu; }
    auto& s = x.proc.service<GuestSignals>();
    std::lock_guard<std::mutex> lk(s.mu);
    const uint32_t prev = s.acts[sig].handler;
    s.acts[sig] = {handler, 0, 0};
    return prev;
}
// Runs the app's SIGSEGV (or SIGBUS) handler for a fault on `c`; true when the
// handler returned and the thread continues from the context it left.
bool deliverGuestFault(Process& p, CPU::Cpu& c) {
    thread_local int depth = 0;
    thread_local uint32_t lastPc = 0, lastAddr = 0, repeats = 0;
    int sig = 11;
    GuestSignals::Act a = signalAction(p, 11);
    if (a.handler <= 1) { sig = 10; a = signalAction(p, 10); }
    if (a.handler <= 1 || depth > 0) return false;
    const uint32_t addr = c.exit_info, pc = c.r[CPU::kPC];
    if (pc == lastPc && addr == lastAddr) { if (++repeats > 8) return false; }   // the handler isn't fixing it
    else { lastPc = pc; lastAddr = addr; repeats = 0; }
    auto& m = p.mem();
    // Frame below the red zone: mcontext (340), ucontext (32), siginfo (64).
    const uint32_t mc = (c.r[CPU::kSP] - 128 - 340) & ~15u;
    const uint32_t uc = (mc - 32) & ~15u;
    const uint32_t si = (uc - 64) & ~15u;
    const uint32_t sp = (si - 16) & ~15u;
    std::memset(m.host(si), 0, mc + 340 - si);
    // siginfo: signo, errno, code (SEGV_MAPERR / BUS_ADRERR), pid, uid, status, addr.
    m.write<uint32_t>(si + 0, uint32_t(sig));
    m.write<uint32_t>(si + 8, sig == 11 ? 1u : 2u);
    m.write<uint32_t>(si + 12, 1234);
    m.write<uint32_t>(si + 16, 501);
    m.write<uint32_t>(si + 24, addr);
    // ucontext: onstack, sigmask, stack{sp,size,flags}, link, mcsize, mcontext.
    m.write<uint32_t>(uc + 24, 340);
    m.write<uint32_t>(uc + 28, mc);
    // mcontext: exception state {exception, fsr, far}, thread state {r0-r12, sp, lr, pc, cpsr}, VFP {d0-d31, fpscr}.
    m.write<uint32_t>(mc + 8, addr);
    for (int i = 0; i < 16; ++i) m.write<uint32_t>(mc + 12 + 4 * i, c.r[i]);
    m.write<uint32_t>(mc + 12 + 64, c.cpsr());
    std::memcpy(m.host(mc + 80), c.d, 256);
    m.write<uint32_t>(mc + 80 + 256, c.fpscr);
    p.logf(LogLevel::Info, "Signal %d to the app's handler for a fault at 0x%08x (pc 0x%08x)", sig, addr, pc);
    p.perf().faultsToApp++;
    // Where the first few happened (Mono raises these routinely, so not every time).
    static std::atomic<int> traced{0};
    if (traced.fetch_add(1) < 3) p.logf(LogLevel::Info, "  fault backtrace:\n%s", p.backtrace(c, 8).c_str());
    const uint32_t savedSp = c.r[CPU::kSP];
    c.r[CPU::kSP] = sp;
    ++depth;
    try {
        p.engine().callGuest(c, a.handler, {uint32_t(sig), si, uc});
    } catch (...) {
        --depth;
        c.r[CPU::kSP] = savedSp;
        return false;
    }
    --depth;
    for (int i = 0; i < 16; ++i) c.r[i] = m.read<uint32_t>(mc + 12 + 4 * i);
    const uint32_t cpsr = m.read<uint32_t>(mc + 12 + 64);
    c.setApsrFlags(cpsr);
    c.thumb = ((cpsr >> 5) & 1) | (c.r[CPU::kPC] & 1);
    c.r[CPU::kPC] &= ~1u;
    std::memcpy(c.d, m.host(mc + 80), 256);
    c.fpscr = m.read<uint32_t>(mc + 80 + 256);
    c.exit = CPU::Exit::None;
    c.exit_info = 0;
    return true;
}
int32_t g_raise(Ctx& x, int32_t sig) {
    flushGuestStdio(x.proc);
    throw std::runtime_error("the app raised signal " + std::to_string(sig) + (sig == 6 ? " (SIGABRT)" : ""));
}
int32_t g_kill(Ctx& x, int32_t pid, int32_t sig) { if (sig == 0) return 0; if (pid == 1234 || pid == 0) return g_raise(x, sig); setErrno(x, 1); return -1; }
int32_t g_sigset(Ctx& x, GuestPtr set) { if (set) x.mem.write<uint32_t>(set, 0); return 0; }
int32_t g_sigsetop(Ctx&, GuestPtr, int32_t) { return 0; }
int32_t g_sigprocmask(Ctx&, int32_t, GuestPtr, GuestPtr) { return 0; }
int32_t g_sigaltstack(Ctx& x, GuestPtr, GuestPtr oss) {
    if (oss) { x.mem.write<uint32_t>(oss, 0); x.mem.write<uint32_t>(oss + 4, 0); x.mem.write<uint32_t>(oss + 8, 2); }
    return 0;
}

// ---------------------------------------------------------------- setjmp ---
// jmp_buf: r4-r11, sp, lr, d8-d15 (16 words), fpscr = 27 words.

int32_t j_setjmp(Ctx& x, GuestPtr buf) {
    for (int i = 0; i < 8; ++i) x.mem.write<uint32_t>(buf + uint32_t(i) * 4, x.cpu.r[4 + i]);
    x.mem.write<uint32_t>(buf + 32, x.cpu.r[CPU::kSP]);
    x.mem.write<uint32_t>(buf + 36, x.cpu.r[CPU::kLR]);
    for (int i = 0; i < 8; ++i) x.mem.write<uint64_t>(buf + 40 + uint32_t(i) * 8, x.cpu.d[8 + i]);
    x.mem.write<uint32_t>(buf + 104, x.cpu.fpscr);
    return 0;
}
void j_longjmp(Ctx& x, GuestPtr buf, int32_t val) {
    for (int i = 0; i < 8; ++i) x.cpu.r[4 + i] = x.mem.read<uint32_t>(buf + uint32_t(i) * 4);
    x.cpu.r[CPU::kSP] = x.mem.read<uint32_t>(buf + 32);
    const uint32_t lr = x.mem.read<uint32_t>(buf + 36);
    for (int i = 0; i < 8; ++i) x.cpu.d[8 + i] = x.mem.read<uint64_t>(buf + 40 + uint32_t(i) * 8);
    x.cpu.fpscr = x.mem.read<uint32_t>(buf + 104);
    x.cpu.r[0] = val ? uint32_t(val) : 1;
    CPU::Engine::hleJump(x.cpu, lr);
}
int32_t j_sigsetjmp(Ctx& x, GuestPtr buf, int32_t) { return j_setjmp(x, buf); }

// ----------------------------------------------------------------- mach ---

uint32_t k_task_self(Ctx&) { return kTaskPort; }
uint32_t k_host_self(Ctx& x) { return acquireMachSendRight(x.proc, kHostPort); }
int32_t k_port_mod_refs(Ctx& x, uint32_t task, uint32_t name, uint32_t right, int32_t delta) {
    return modifyMachSendRefs(x.proc, task, name, right, delta);
}
int32_t k_port_deallocate(Ctx& x, uint32_t task, uint32_t name) {
    return modifyMachSendRefs(x.proc, task, name, 0, -1);
}
int32_t k_port_get_refs(Ctx& x, uint32_t task, uint32_t name, uint32_t right, GuestPtr out) {
    const int32_t rc = modifyMachSendRefs(x.proc, task, name, right, 0);
    if (!rc) x.mem.write<uint32_t>(out, machSendRefs(x.proc, name));
    return rc;
}
bool vm_range(Ctx& x, uint32_t addr, uint32_t size, uint8_t access) {
    if (!size) return true;
    const uint64_t end = uint64_t(addr) + size;
    if (end > Memory::GuestArena::kArenaSize) return false;
    for (uint64_t page = uint64_t(addr) & ~4095ull; page < end; page += 4096)
        if ((x.mem.protAt(uint32_t(page)) & access) != access) return false;
    return true;
}
int32_t k_vm_read_overwrite(Ctx& x, uint32_t task, uint32_t src, uint32_t size, uint32_t dst, GuestPtr outSize) {
    if (task != kTaskPort || !hasMachSendRight(x.proc, task)) return 16;
    if (!vm_range(x, outSize, 4, Memory::kProtWrite)) return 1;
    x.mem.write<uint32_t>(outSize, 0);
    if (!vm_range(x, src, size, Memory::kProtRead) || !vm_range(x, dst, size, Memory::kProtWrite)) return 1;
    std::memmove(x.mem.host(dst), x.mem.host(src), size);
    x.mem.write<uint32_t>(outSize, size);
    return 0;
}
int32_t k_make_memory_entry(Ctx& x, uint32_t task, GuestPtr size, uint64_t offset, uint32_t permission, GuestPtr handle, uint32_t parent) {
    if (task != kTaskPort || !hasMachSendRight(x.proc, task)) return 16;
    if (!vm_range(x, size, 8, Memory::kProtRead) || !vm_range(x, handle, 4, Memory::kProtWrite)) return 1;
    x.mem.write<uint32_t>(handle, 0);
    // There is no Mach memory-object/IPC backend. Callers can recover from a
    // capability failure; do not fabricate a port or a successful mapping.
    return 46; // KERN_NOT_SUPPORTED
}
int32_t k_task_info(Ctx& x, uint32_t, uint32_t flavor, GuestPtr out, GuestPtr count) {
    const uint32_t n = count ? x.mem.read<uint32_t>(count) : 0;
    const uint32_t maxWords = std::min<uint32_t>(n, 10u);
    if (out && maxWords) std::memset(x.mem.host(out), 0, maxWords * 4);
    if (out && flavor == 5 /*TASK_BASIC_INFO*/ && maxWords >= 5) {
        const auto st = x.proc.heap().stats();
        x.mem.write<uint32_t>(out + 4, uint32_t(st.committed + (16u << 20)));   // resident_size
        x.mem.write<uint32_t>(out + 8, uint32_t(st.committed + (64u << 20)));   // virtual_size
    }
    if (count) x.mem.write<uint32_t>(count, maxWords);
    return 0;
}
int32_t k_host_page_size(Ctx& x, uint32_t, GuestPtr out) { if (out) x.mem.write<uint32_t>(out, 4096); return 0; }
int32_t k_host_statistics(Ctx& x, uint32_t, uint32_t, GuestPtr out, GuestPtr count) {
    const uint32_t n = count ? x.mem.read<uint32_t>(count) : 0;
    const uint32_t maxWords = std::min<uint32_t>(n, 15u);
    if (out && maxWords) std::memset(x.mem.host(out), 0, maxWords * 4);
    if (out && maxWords >= 2) { x.mem.write<uint32_t>(out, 65536); x.mem.write<uint32_t>(out + 4, 32768); }   // free / active pages
    if (count) x.mem.write<uint32_t>(count, maxWords);
    return 0;
}
int32_t k_host_info(Ctx& x, uint32_t, uint32_t flavor, GuestPtr out, GuestPtr count) {
    const uint32_t n = count ? x.mem.read<uint32_t>(count) : 0;
    const uint32_t maxWords = std::min<uint32_t>(n, 12u);
    if (out && maxWords) std::memset(x.mem.host(out), 0, maxWords * 4);
    if (out && flavor == 1 /*HOST_BASIC_INFO*/ && maxWords >= 5) {
        x.mem.write<uint32_t>(out + 0, 2);           // max_cpus
        x.mem.write<uint32_t>(out + 4, 2);           // avail_cpus
        x.mem.write<uint32_t>(out + 8, 512u << 20);  // memory_size
        x.mem.write<uint32_t>(out + 12, 12);         // cpu_type (CPU_TYPE_ARM)
        x.mem.write<uint32_t>(out + 16, 9);          // cpu_subtype (CPU_SUBTYPE_ARM_V7)
    }
    if (count) x.mem.write<uint32_t>(count, maxWords);
    return 0;
}
int32_t k_vm_allocate(Ctx& x, uint32_t, GuestPtr out, uint32_t size, int32_t) {
    const uint32_t a = x.proc.heap().allocAligned((size + 4095) & ~4095u, 4096);
    if (!a) return 3;   // KERN_NO_SPACE
    std::memset(x.mem.host(a), 0, (size + 4095) & ~4095u);
    x.mem.write<uint32_t>(out, a);
    return 0;
}
int32_t k_vm_deallocate(Ctx& x, uint32_t, GuestPtr a, uint32_t) { if (x.proc.heap().blockSize(a)) x.proc.heap().free(a); return 0; }
int32_t k_vm_remap(Ctx& x, uint32_t, GuestPtr targetAddrPtr, uint32_t size, uint32_t) {
    const uint32_t sp = x.cpu.r[CPU::kSP];
    const int32_t flags = x.mem.read<int32_t>(sp + 0);
    const GuestPtr srcAddr = x.mem.read<uint32_t>(sp + 8);
    const GuestPtr curProtPtr = x.mem.read<uint32_t>(sp + 16);
    const GuestPtr maxProtPtr = x.mem.read<uint32_t>(sp + 20);
    const uint32_t alignedSize = (size + 4095u) & ~4095u;
    uint32_t dst = targetAddrPtr ? x.mem.read<uint32_t>(targetAddrPtr) : 0;
    if ((flags & 1 /* VM_FLAGS_ANYWHERE */) != 0 || !dst || !x.mem.isMapped(dst)) {
        dst = x.proc.heap().allocAligned(alignedSize ? alignedSize : 4096u, 4096u);
        if (!dst) return 3;   // KERN_NO_SPACE
    }
    if (alignedSize) {
        x.mem.protect(dst, alignedSize, Memory::kProtRead | Memory::kProtWrite | Memory::kProtExec);
        if (srcAddr && size) std::memcpy(x.mem.host(dst), x.mem.host(srcAddr), size);
    }
    if (targetAddrPtr) x.mem.write<uint32_t>(targetAddrPtr, dst);
    if (curProtPtr) x.mem.write<uint32_t>(curProtPtr, 7);
    if (maxProtPtr) x.mem.write<uint32_t>(maxProtPtr, 7);
    return 0;
}
int32_t k_zero4(Ctx&, uint32_t, uint32_t, uint32_t, uint32_t) { return 0; }
int32_t k_mach_wait_until(Ctx& x, uint64_t deadline) {
    if (x.proc.stopping()) throw ProcessExit(0, "stopping");
    const uint64_t now = uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());
    if (deadline > now) {
        const uint64_t diffNs = deadline - now;
        if (diffNs >= 1000000ULL) {
            const uint64_t sleepNs = std::min<uint64_t>(diffNs, 50000000ULL);
            std::this_thread::sleep_for(std::chrono::nanoseconds(sleepNs));
        } else {
            std::this_thread::yield();
        }
    } else {
        std::this_thread::yield();
    }
    return 0;
}

// Named POSIX semaphores have opaque handles, not arbitrary guest addresses.
// Keep names and open handles separate: unlink removes a name, whereas close
// only releases that handle; another open handle remains usable.
struct NamedSemaphore {
    std::mutex mu;
    std::condition_variable changed;
    uint32_t value = 0;
};
struct NamedSemaphoreTable {
    explicit NamedSemaphoreTable(Process&) {}
    std::mutex mu;
    std::unordered_map<std::string, std::shared_ptr<NamedSemaphore>> names;
    std::unordered_map<uint32_t, std::shared_ptr<NamedSemaphore>> handles;
};
uint32_t sys_sem_open(Ctx& x, GuestPtr name, int32_t flags, uint32_t, uint32_t value) {
    if (!name) { setErrno(x, EINVAL); return UINT32_MAX; }
    const std::string key = x.proc.readCString(name);
    if (key.size() < 2 || key[0] != '/' || key.find('/', 1) != std::string::npos) {
        setErrno(x, EINVAL); return UINT32_MAX;
    }
    constexpr int32_t kDarwinCreate = 0x0200, kDarwinExcl = 0x0800;
    auto& table = x.proc.service<NamedSemaphoreTable>();
    std::lock_guard lock(table.mu);
    auto it = table.names.find(key);
    if (it == table.names.end()) {
        if (!(flags & kDarwinCreate)) { setErrno(x, ENOENT); return UINT32_MAX; }
        auto sem = std::make_shared<NamedSemaphore>();
        sem->value = value;
        it = table.names.emplace(key, std::move(sem)).first;
    } else if ((flags & (kDarwinCreate | kDarwinExcl)) == (kDarwinCreate | kDarwinExcl)) {
        setErrno(x, EEXIST); return UINT32_MAX;
    }
    const uint32_t handle = x.proc.heap().allocZeroed(4);
    table.handles.emplace(handle, it->second);
    return handle;
}
int32_t sys_sem_close(Ctx& x, uint32_t handle) {
    auto& table = x.proc.service<NamedSemaphoreTable>();
    std::lock_guard lock(table.mu);
    auto it = table.handles.find(handle);
    if (it == table.handles.end()) { setErrno(x, EINVAL); return -1; }
    table.handles.erase(it);
    x.proc.heap().free(handle);
    return 0;
}
int32_t sys_sem_unlink(Ctx& x, GuestPtr name) {
    if (!name) { setErrno(x, EINVAL); return -1; }
    auto& table = x.proc.service<NamedSemaphoreTable>();
    std::lock_guard lock(table.mu);
    if (!table.names.erase(x.proc.readCString(name))) { setErrno(x, ENOENT); return -1; }
    return 0;
}
std::shared_ptr<NamedSemaphore> openNamed(Ctx& x, uint32_t handle) {
    auto& table = x.proc.service<NamedSemaphoreTable>();
    std::lock_guard lock(table.mu);
    auto it = table.handles.find(handle);
    return it == table.handles.end() ? nullptr : it->second;
}
int32_t sys_sem_post(Ctx& x, uint32_t handle) {
    auto sem = openNamed(x, handle);
    if (!sem) { setErrno(x, EINVAL); return -1; }
    { std::lock_guard lock(sem->mu); if (sem->value == UINT32_MAX) { setErrno(x, darwinErrno(EOVERFLOW)); return -1; } ++sem->value; }
    sem->changed.notify_one();
    return 0;
}
int32_t sys_sem_trywait(Ctx& x, uint32_t handle) {
    auto sem = openNamed(x, handle);
    if (!sem) { setErrno(x, EINVAL); return -1; }
    std::lock_guard lock(sem->mu);
    if (!sem->value) { setErrno(x, darwinErrno(EAGAIN)); return -1; }
    --sem->value;
    return 0;
}
int32_t sys_sem_wait(Ctx& x, uint32_t handle) {
    auto sem = openNamed(x, handle);
    if (!sem) { setErrno(x, EINVAL); return -1; }
    std::unique_lock lock(sem->mu);
    sem->changed.wait(lock, [&] { return sem->value != 0; });
    --sem->value;
    return 0;
}

} // namespace

void registerLibcSystem(Process& p) {
    registerStartupMach(p);
    p.def<&s_getprogname>("_getprogname");
    p.def<&s_strsignal>("_strsignal");
    p.def<&f_writev>("_writev");
    p.def<&f_flock>("_flock");
    std::random_device rd;
    const uint32_t guard = p.defineDataBlock("___stack_chk_guard", 4);
    p.mem().write<uint32_t>(guard, (rd() & 0xFFFFFF00u) | 0x0A);
    p.def<&s_error>("___error");
    p.def<&s_stack_chk_fail>("___stack_chk_fail");
    p.def<&s_noop>("__hvm_noop");
    p.def<&d_lazy_entry>("__hvm_dyld_lazy_entry");
    p.def<&d_func_lookup>("__hvm_dyld_func_lookup");
    p.def<&d_func_lookup>("__dyld_func_lookup");
    p.def<&s_noop>("dyld_stub_binder");
    p.def<&s_NSGetExecutablePath>("__NSGetExecutablePath");
    p.def<&s_NSGetArgc>("__NSGetArgc");
    p.def<&s_NSGetArgv>("__NSGetArgv");
    p.def<&s_NSGetEnviron>("__NSGetEnviron");
    p.def<&s_NSGetProgname>("__NSGetProgname");
    p.def<&s_NSGetMachExecuteHeader>("__NSGetMachExecuteHeader");
    p.def<&s_getpid>("_getpid");
    p.def<&s_syscall>("_syscall");
    p.def<&s_getppid>("_getppid");
    p.def<&s_getuid>("_getuid");
    p.def<&s_getuid>("_geteuid");
    p.def<&s_getuid>("_getgid");
    p.def<&s_getuid>("_getegid");
    p.def<&s_getpwuid>("_getpwuid");
    p.def<&s_getpwnam>("_getpwnam");
    p.def<&s_getpwuid_r>("_getpwuid_r");
    p.def<&s_getpwnam_r>("_getpwnam_r");
    for (const char* n : {"_iconv_open", "_libiconv_open"}) p.def<&s_iconv_open>(n);
    for (const char* n : {"_iconv", "_libiconv"}) p.def<&s_iconv>(n);
    for (const char* n : {"_iconv_close", "_libiconv_close"}) p.def<&s_iconv_close>(n);
    p.def<&s_basename>("_basename");
    p.def<&s_ioctl>("_ioctl");
    p.def<&s_zero>("_tcgetattr");
    p.def<&s_zero>("_tcsetattr");
    p.def<&s_zero>("_issetugid");
    p.def<&s_isatty>("_isatty");
    p.def<&s_getpagesize>("_getpagesize");
    p.def<&s_sysconf>("_sysconf");
    p.def<&s_getrlimit>("_getrlimit");
    p.def<&s_ok2>("_setrlimit");
    p.def<&sys_sem_open>("_sem_open");
    p.def<&sys_sem_close>("_sem_close");
    p.def<&sys_sem_unlink>("_sem_unlink");
    p.def<&sys_sem_post>("_sem_post");
    p.def<&sys_sem_trywait>("_sem_trywait");
    p.def<&sys_sem_wait>("_sem_wait");
    p.def<&s_getrusage>("_getrusage");
    p.def<&s_sysctl>("_sysctl");
    p.def<&s_sysctlbyname>("_sysctlbyname");
    p.def<&s_uname>("_uname");
    p.def<&d_image_count>("__dyld_image_count");
    p.def<&d_image_header>("__dyld_get_image_header");
    p.def<&d_image_name>("__dyld_get_image_name");
    p.def<&d_image_slide>("__dyld_get_image_vmaddr_slide");
    p.def<&d_register_add_image>("__dyld_register_func_for_add_image");
    p.def<&d_register_remove_image>("__dyld_register_func_for_remove_image");
    p.def<&d_version>("_NSVersionOfRunTimeLibrary");
    p.def<&d_version>("_NSVersionOfLinkTimeLibrary");
    p.def<&d_dlopen>("_dlopen");
    p.def<&d_dlsym>("_dlsym");
    p.def<&d_dlclose>("_dlclose");
    p.def<&d_dlerror>("_dlerror");
    p.def<&d_dladdr>("_dladdr");
    p.def<&d_local_arch_info>("_NXGetLocalArchInfo");
    p.def<&d_arch_from_cputype>("_NXGetArchInfoFromCpuType");
    p.def<&f_open>("_open");
    p.def<&f_close>("_close");
    p.def<&f_read>("_read");
    p.def<&f_write>("_write");
    p.def<&f_pread>("_pread");
    p.def<&f_pwrite>("_pwrite");
    p.def<&f_lseek>("_lseek");
    p.def<&f_fcntl>("_fcntl");
    p.def<&f_fsync>("_fsync");
    p.def<&f_ftruncate>("_ftruncate");
    p.def<&f_dup>("_dup");
    // iOS armv7 apps fill the 108-byte (64-bit inode) struct stat through the plain
    // symbols too: switching _stat/_fstat to the 96-byte legacy layout broke Unity,
    // Mono and Supercell file checks on the device ("mscorlib.dll not found").
    for (const char* n : {"_stat", "_stat$INODE64", "_stat64"}) p.def<&f_stat>(n);
    for (const char* n : {"_lstat", "_lstat$INODE64", "_lstat64"}) p.def<&f_lstat>(n);
    for (const char* n : {"_fstat", "_fstat$INODE64", "_fstat64"}) p.def<&f_fstat>(n);
    p.def<&f_access>("_access");
    p.def<&f_mkdir>("_mkdir");
    p.def<&f_rmdir>("_rmdir");
    p.def<&f_unlink>("_unlink");
    p.def<&f_chdir>("_chdir");
    p.def<&f_getcwd>("_getcwd");
    p.def<&f_chmod>("_chmod");
    p.def<&f_utimes>("_utimes");
    p.def<&f_utime>("_utime");
    for (const char* n : {"_opendir", "_opendir$INODE64", "_opendir$INODE64$UNIX2003", "_opendir$UNIX2003"}) p.def<&f_opendir>(n);
    for (const char* n : {"_readdir", "_readdir$INODE64"}) p.def<&f_readdir>(n);
    for (const char* n : {"_closedir", "_closedir$UNIX2003"}) p.def<&f_closedir>(n);
    p.def<&m_mmap>("_mmap");
    p.def<&m_munmap>("_munmap");
    p.def<&m_ok3>("_mprotect");
    p.def<&m_ok3>("_madvise");
    p.def<&m_ok3>("_msync");
    p.def<&m_ok2>("_mlock");
    p.def<&m_ok2>("_munlock");
    p.def<&g_signal>("_signal");
    p.engine().setFaultHook([&p](CPU::Cpu& c, CPU::Exit) { return deliverGuestFault(p, c); });
    p.def<&g_sigaction>("_sigaction");
    p.def<&g_raise>("_raise");
    p.def<&g_kill>("_kill");
    p.def<&g_sigset>("_sigemptyset");
    p.def<&g_sigset>("_sigfillset");
    p.def<&g_sigsetop>("_sigaddset");
    p.def<&g_sigsetop>("_sigdelset");
    p.def<&g_sigprocmask>("_sigprocmask");
    p.def<&g_sigaltstack>("_sigaltstack");
    p.def<&j_setjmp>("_setjmp");
    p.def<&j_setjmp>("__setjmp");
    p.def<&j_sigsetjmp>("_sigsetjmp");
    p.def<&j_longjmp>("_longjmp");
    p.def<&j_longjmp>("__longjmp");
    p.def<&j_longjmp>("_siglongjmp");
    p.def<&k_task_self>("_mach_task_self");
    p.defineData("_mach_task_self_", [&] { const uint32_t c = p.allocStatic(4); p.mem().write<uint32_t>(c, 0x103); return c; }());
    p.def<&k_host_self>("_mach_host_self");
    p.def<&k_task_info>("_task_info");
    p.def<&k_host_page_size>("_host_page_size");
    p.def<&k_host_statistics>("_host_statistics");
    p.def<&k_host_info>("_host_info");
    p.def<&k_vm_allocate>("_vm_allocate");
    p.def<&k_vm_deallocate>("_vm_deallocate");
    p.def<&k_vm_remap>("_vm_remap");
    p.def<&k_port_deallocate>("_mach_port_deallocate");
    p.def<&k_port_mod_refs>("_mach_port_mod_refs");
    p.def<&k_port_get_refs>("_mach_port_get_refs");
    p.def<&k_vm_read_overwrite>("_vm_read_overwrite");
    p.def<&k_make_memory_entry>("_mach_make_memory_entry_64");
    p.def<&k_zero4>("_thread_policy_set");
    p.def<&k_zero4>("_thread_policy_get");
    p.def<&k_mach_wait_until>("_mach_wait_until");
    p.defineData("_vm_page_size", [&] { const uint32_t c = p.allocStatic(4); p.mem().write<uint32_t>(c, 4096); return c; }());
    p.defineData("_vm_page_mask", [&] { const uint32_t c = p.allocStatic(4); p.mem().write<uint32_t>(c, 4095); return c; }());
    p.defineData("_vm_page_shift", [&] { const uint32_t c = p.allocStatic(4); p.mem().write<uint32_t>(c, 12); return c; }());
    p.defineData("_NDR_record", [&] { const uint32_t c = p.allocStatic(8); p.mem().write<uint64_t>(c, 0); return c; }());
}

} // namespace Hydrogen::Runtime
