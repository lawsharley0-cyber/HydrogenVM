// HydrogenVM — the parts of Android's C library (bionic, as of Android 2.x–4.x) that differ
// from iOS's. See Android.hpp.
//
// Everything not defined here binds to the iOS function of the same name. What is here:
//   * data with a bionic layout: __sF (84-byte FILEs), _ctype_ and the case tables, __page_size;
//   * errno: iOS functions store Darwin values; __errno() hands the game a separate cell and
//     copies a new Darwin value into it, translated, each time the game asks for it;
//   * calls whose arguments or structs differ: open/fcntl flags, stat and dirent layouts,
//     32-bit off_t (lseek, ftruncate, mmap), sysconf names, clock ids, mmap flags;
//   * pthread types that are one word on Android (mutex attributes, once), the attribute
//     detach-state values, unnamed semaphores, and error codes returned by pthread functions;
//   * old bionic atomics, Android logging and the unwinder's exception-index lookup.
#include "runtime/android/Android.hpp"
#include "runtime/android/Etc1.hpp"
#include "runtime/android/BionicInternal.hpp"
#include "runtime/Process.hpp"
#include "runtime/Support.hpp"
#include "runtime/libc/Format.hpp"
#include "core/cpu/Engine.hpp"
#include <cerrno>
#include <cmath>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <mutex>
#include <sys/stat.h>
#include <thread>
#include <unistd.h>
#include <unordered_map>
#include <set>
#include <cstdlib>

namespace Hydrogen::Runtime::Android {
using namespace std::chrono_literals;
namespace {

// ------------------------------------------------------------------ errno ---

} // namespace

// Darwin errno -> Linux errno. 1-34 agree except 11 (Darwin EDEADLK, Linux EAGAIN).
int linuxErrno(int d) {
    switch (d) {
        case 11: return 35;    // EDEADLK
        case 35: return 11;    // EAGAIN
        case 36: return 115; case 37: return 114; case 38: return 88; case 39: return 89;
        case 40: return 90; case 41: return 91; case 42: return 92; case 43: return 93;
        case 44: return 94; case 45: return 95; case 46: return 96; case 47: return 97;
        case 48: return 98; case 49: return 99; case 50: return 100; case 51: return 101;
        case 52: return 102; case 53: return 103; case 54: return 104; case 55: return 105;
        case 56: return 106; case 57: return 107; case 58: return 108; case 59: return 109;
        case 60: return 110; case 61: return 111; case 62: return 40; case 63: return 36;
        case 64: return 112; case 65: return 113; case 66: return 39; case 68: return 87;
        case 69: return 122; case 70: return 116; case 77: return 37; case 78: return 38;
        case 84: return 75; case 89: return 125; case 90: return 43; case 91: return 42;
        case 92: return 84;
        default: return d;
    }
}
// The host's errno as a Linux value.
int hostErrnoLinux(int e) {
#ifdef __APPLE__
    return linuxErrno(e);
#else
    return e;
#endif
}

// errno (Darwin, written by the iOS functions) for the calling thread.
uint32_t darwinErrnoAddr(Ctx& x) { return x.proc.currentThread().errnoAddr; }
uint32_t linuxErrnoCell(Ctx& x) {
    static const char key = 0;
    uint32_t& cell = x.proc.threadSlot(&key);
    if (!cell) cell = x.proc.allocStatic(4);
    return cell;
}
uint32_t b_errno(Ctx& x) {
    const uint32_t cell = linuxErrnoCell(x), d = darwinErrnoAddr(x);
    if (const uint32_t v = x.mem.read<uint32_t>(d)) {
        x.mem.write<uint32_t>(cell, uint32_t(linuxErrno(int(v))));
        x.mem.write<uint32_t>(d, 0);
    }
    return cell;
}
// Set errno to a Linux value (from a bionic function here).
void setLinuxErrno(Ctx& x, int e) {
    x.mem.write<uint32_t>(darwinErrnoAddr(x), 0);
    x.mem.write<uint32_t>(linuxErrnoCell(x), uint32_t(e));
}

// --------------------------------------------- calling the iOS functions ---

// Calls the iOS host function bound to `name` with `words` as its arguments (r0-r3, then
// the stack) and returns r0 (and r1 in the high word). The arguments are read with this
// process's ABI: a 64-bit one starts at an even word.
uint64_t callIos(Ctx& x, const char* name, std::initializer_list<uint32_t> words) {
    auto addr = x.proc.lookupSymbol(name);
    auto& eng = x.proc.engine();
    if (!addr || !eng.isHleAddress(*addr)) throw std::runtime_error(std::string("Android: no iOS function ") + name);
    CPU::Cpu& c = x.cpu;
    const uint32_t savedSp = c.r[CPU::kSP], savedLr = c.r[CPU::kLR];
    uint32_t saved[4];
    std::memcpy(saved, c.r, sizeof saved);
    const uint32_t* w = words.begin();
    const size_t n = words.size();
    for (size_t i = 0; i < 4; ++i) c.r[i] = i < n ? w[i] : 0;
    if (n > 4) {
        const uint32_t extra = uint32_t(n - 4);
        c.r[CPU::kSP] = (savedSp - extra * 4 - 16) & ~7u;
        for (uint32_t i = 0; i < extra; ++i) x.mem.write<uint32_t>(c.r[CPU::kSP] + i * 4, w[4 + i]);
    }
    eng.hleFunction(eng.hleIndex(*addr))(c);
    const uint64_t r = uint64_t(c.r[0]) | (uint64_t(c.r[1]) << 32);
    std::memcpy(c.r, saved, sizeof saved);
    c.r[CPU::kSP] = savedSp;
    c.r[CPU::kLR] = savedLr;
    return r;
}

namespace {

// ------------------------------------------------------------------- data ---

constexpr uint32_t kBionicFileSize = 84;

// OpenBSD-style ctype: _ctype_ points at 1 + 256 flag bytes (index 0 is EOF).
void defineCtype(Process& p) {
    constexpr uint8_t U = 0x01, L = 0x02, N = 0x04, S = 0x08, P = 0x10, C = 0x20, X = 0x40, B = 0x80;
    const uint32_t tab = p.allocStatic(257);
    const uint32_t lower = p.allocStatic(257 * 2, 2), upper = p.allocStatic(257 * 2, 2);
    p.mem().write<uint8_t>(tab, 0);
    p.mem().write<int16_t>(lower, -1);
    p.mem().write<int16_t>(upper, -1);
    for (int c = 0; c < 256; ++c) {
        uint8_t f = 0;
        if (c < 128) {
            if (c >= 'A' && c <= 'Z') f |= U;
            if (c >= 'a' && c <= 'z') f |= L;
            if (c >= '0' && c <= '9') f |= N;
            if (c == ' ' || (c >= '\t' && c <= '\r')) f |= S;
            if (c > 32 && c < 127 && !std::isalnum(c)) f |= P;
            if (c < 32 || c == 127) f |= C;
            if ((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F')) f |= X;
            if (c == ' ') f |= B;
        }
        p.mem().write<uint8_t>(tab + 1 + uint32_t(c), f);
        p.mem().write<int16_t>(lower + 2 + uint32_t(c) * 2, int16_t(c >= 'A' && c <= 'Z' ? c + 32 : c));
        p.mem().write<int16_t>(upper + 2 + uint32_t(c) * 2, int16_t(c >= 'a' && c <= 'z' ? c - 32 : c));
    }
    p.mem().write<uint32_t>(p.defineDataBlock("android:_ctype_", 4), tab);
    p.mem().write<uint32_t>(p.defineDataBlock("android:_tolower_tab_", 4), lower);
    p.mem().write<uint32_t>(p.defineDataBlock("android:_toupper_tab_", 4), upper);
}

// --------------------------------------------------------------- logging ---

const char* priorityName(int prio) {
    switch (prio) {
        case 2: return "V"; case 3: return "D"; case 4: return "I"; case 5: return "W";
        case 6: return "E"; case 7: return "F"; default: return "?";
    }
}
void androidLog(Ctx& x, int prio, GuestPtr tag, const std::string& msg) {
    std::string line = std::string(priorityName(prio)) + "/" + (tag ? x.proc.readCString(tag, 128) : std::string("?")) + ": " + msg;
    if (line.empty() || line.back() != '\n') line.push_back('\n');
    x.proc.log(prio >= 6 ? LogLevel::GuestStderr : LogLevel::GuestStdout, line);
}
int32_t b_logWrite(Ctx& x, int32_t prio, GuestPtr tag, GuestPtr text) {
    androidLog(x, prio, tag, text ? x.proc.readCString(text) : std::string());
    return 1;
}
int32_t b_logPrint(Ctx& x, int32_t prio, GuestPtr tag, GuestPtr fmt, VarArgs va) {
    androidLog(x, prio, tag, formatGuest(x.proc, x.proc.readCString(fmt), va.args));
    return 1;
}
int32_t b_logVprint(Ctx& x, int32_t prio, GuestPtr tag, GuestPtr fmt, VaList ap) {
    ArgReader a(x.mem, ap.ptr);
    androidLog(x, prio, tag, formatGuest(x.proc, x.proc.readCString(fmt), a));
    return 1;
}
void b_logAssert(Ctx& x, GuestPtr cond, GuestPtr tag, GuestPtr fmt, VarArgs va) {
    const std::string c = cond ? x.proc.readCString(cond, 256) : std::string();
    const std::string m = fmt ? formatGuest(x.proc, x.proc.readCString(fmt), va.args) : c;
    androidLog(x, 7, tag, m);
    throw std::runtime_error("android_log_assert: " + m);
}
void b_setAbortMessage(Ctx& x, GuestPtr msg) {
    if (msg) x.proc.logf(LogLevel::Warning, "android_set_abort_message: %s", x.proc.readCString(msg, 512).c_str());
}
void b_openlog(Ctx&, GuestPtr, int32_t, int32_t) {}
void b_closelog(Ctx&) {}
void b_syslog(Ctx& x, int32_t prio, GuestPtr fmt, VarArgs va) {
    if (!fmt) return;
    std::string line = formatGuest(x.proc, x.proc.readCString(fmt), va.args);
    if (line.empty() || line.back() != '\n') line.push_back('\n');
    x.proc.log((prio & 7) <= 3 ? LogLevel::GuestStderr : LogLevel::GuestStdout, line);
}

// ---------------------------------------------------------------- atomics ---

// Old bionic: __atomic_cmpxchg returns 0 when it swapped; the others return the old value.
int32_t b_atomicCmpxchg(Ctx& x, int32_t oldv, int32_t newv, GuestPtr ptr) {
    auto* p = reinterpret_cast<int32_t*>(x.mem.host(ptr));
    return __atomic_compare_exchange_n(p, &oldv, newv, false, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST) ? 0 : 1;
}
int32_t b_atomicSwap(Ctx& x, int32_t v, GuestPtr ptr) {
    return __atomic_exchange_n(reinterpret_cast<int32_t*>(x.mem.host(ptr)), v, __ATOMIC_SEQ_CST);
}
int32_t b_atomicInc(Ctx& x, GuestPtr ptr) { return __atomic_fetch_add(reinterpret_cast<int32_t*>(x.mem.host(ptr)), 1, __ATOMIC_SEQ_CST); }
int32_t b_atomicDec(Ctx& x, GuestPtr ptr) { return __atomic_fetch_sub(reinterpret_cast<int32_t*>(x.mem.host(ptr)), 1, __ATOMIC_SEQ_CST); }

// --------------------------------------------------------------- unwinder ---

uint32_t b_findExidx(Ctx& x, uint32_t pc, GuestPtr count) {
    auto& L = x.proc.service<Library>();
    std::lock_guard<std::recursive_mutex> lk(L.mu);
    const LoadedLib* lib = L.containing(pc & ~1u);
    if (!lib || !lib->so.exidx) return 0;
    if (count) x.mem.write<uint32_t>(count, lib->so.exidxCount);
    return lib->so.exidx;
}

// ------------------------------------------------------------------ files ---

uint32_t darwinOpenFlags(uint32_t f) {
    uint32_t d = f & 3;
    if (f & 0x40) d |= 0x200;        // O_CREAT
    if (f & 0x80) d |= 0x800;        // O_EXCL
    if (f & 0x100) d |= 0x20000;     // O_NOCTTY
    if (f & 0x200) d |= 0x400;       // O_TRUNC
    if (f & 0x400) d |= 0x8;         // O_APPEND
    if (f & 0x800) d |= 0x4;         // O_NONBLOCK
    if (f & 0x4000) d |= 0x100000;   // O_DIRECTORY (ARM)
    if (f & 0x8000) d |= 0x100;      // O_NOFOLLOW (ARM)
    if (f & 0x80000) d |= 0x1000000; // O_CLOEXEC
    if (f & 0x1000) d |= 0x80;       // O_SYNC
    return d;
}
int32_t b_open(Ctx& x, GuestPtr path, uint32_t flags, VarArgs va) {
    const uint32_t mode = (flags & 0x40) ? va.args.word() : 0;
    noteExternalPath(x.proc, x.proc.hostPath(x.proc.readCString(path)));
    return int32_t(callIos(x, "_open", {path, darwinOpenFlags(flags), mode}));
}
int32_t b_open2(Ctx& x, GuestPtr path, uint32_t flags) {
    noteExternalPath(x.proc, x.proc.hostPath(x.proc.readCString(path)));
    return int32_t(callIos(x, "_open", {path, darwinOpenFlags(flags), 0u}));
}
int32_t b_fcntl(Ctx& x, int32_t fd, int32_t cmd, VarArgs va) {
    const uint32_t arg = va.args.word();
    if (cmd == 3) {   // F_GETFL: Darwin flags back to Linux
        const int32_t d = int32_t(callIos(x, "_fcntl", {uint32_t(fd), 3u, 0u}));
        if (d < 0) return d;
        return (d & 3) | (d & 4 ? 0x800 : 0) | (d & 8 ? 0x400 : 0);
    }
    if (cmd == 4) return int32_t(callIos(x, "_fcntl", {uint32_t(fd), 4u, (arg & 0x800 ? 4u : 0u) | (arg & 0x400 ? 8u : 0u)}));
    return int32_t(callIos(x, "_fcntl", {uint32_t(fd), uint32_t(cmd), arg}));
}

// bionic (ARM) struct stat: the kernel's stat64, 104 bytes.
void storeStat(Ctx& x, GuestPtr out, const struct stat& st) {
    std::memset(x.mem.host(out), 0, 104);
    uint32_t mode = st.st_mode & 07777;
    if (S_ISDIR(st.st_mode)) mode |= 0040000;
    else if (S_ISREG(st.st_mode)) mode |= 0100000;
    else if (S_ISLNK(st.st_mode)) mode |= 0120000;
    else if (S_ISCHR(st.st_mode)) mode |= 0020000;
    else if (S_ISFIFO(st.st_mode)) mode |= 0010000;
    x.mem.write<uint64_t>(out + 0, uint64_t(st.st_dev));
    x.mem.write<uint32_t>(out + 12, uint32_t(st.st_ino));
    x.mem.write<uint32_t>(out + 16, mode);
    x.mem.write<uint32_t>(out + 20, uint32_t(st.st_nlink));
    x.mem.write<uint32_t>(out + 24, 10000);   // an app uid
    x.mem.write<uint32_t>(out + 28, 10000);
    x.mem.write<int64_t>(out + 48, int64_t(st.st_size));
    x.mem.write<uint32_t>(out + 56, uint32_t(st.st_blksize));
    x.mem.write<uint64_t>(out + 64, uint64_t(st.st_blocks));
#ifdef __APPLE__
    const timespec a = st.st_atimespec, m = st.st_mtimespec, c = st.st_ctimespec;
#else
    const timespec a = st.st_atim, m = st.st_mtim, c = st.st_ctim;
#endif
    x.mem.write<uint32_t>(out + 72, uint32_t(a.tv_sec)); x.mem.write<uint32_t>(out + 76, uint32_t(a.tv_nsec));
    x.mem.write<uint32_t>(out + 80, uint32_t(m.tv_sec)); x.mem.write<uint32_t>(out + 84, uint32_t(m.tv_nsec));
    x.mem.write<uint32_t>(out + 88, uint32_t(c.tv_sec)); x.mem.write<uint32_t>(out + 92, uint32_t(c.tv_nsec));
    x.mem.write<uint64_t>(out + 96, uint64_t(st.st_ino));
}
int32_t b_stat(Ctx& x, GuestPtr path, GuestPtr out) {
    struct stat st;
    const std::string gp = x.proc.readCString(path);
    noteExternalPath(x.proc, x.proc.hostPath(gp));
    if (::stat(x.proc.hostPath(gp).c_str(), &st) < 0) {
        const int e = errno;
        x.proc.recordFileOpen(gp, "stat", e, -1, 0, false);
        errno = e;
        return failHost<int32_t>(x);
    }
    storeStat(x, out, st);
    return 0;
}
int32_t b_lstat(Ctx& x, GuestPtr path, GuestPtr out) {
    struct stat st;
    if (::lstat(x.proc.hostPath(x.proc.readCString(path)).c_str(), &st) < 0) return failHost<int32_t>(x);
    storeStat(x, out, st);
    return 0;
}
int32_t b_fstat(Ctx& x, int32_t fd, GuestPtr out) {
    struct stat st;
    if (fd >= 0 && fd <= 2) { std::memset(&st, 0, sizeof st); st.st_mode = S_IFCHR | 0666; storeStat(x, out, st); return 0; }
    if (::fstat(fd, &st) < 0) return failHost<int32_t>(x);
    storeStat(x, out, st);
    return 0;
}
int32_t b_lseek(Ctx& x, int32_t fd, int32_t off, int32_t whence) {
    const int64_t r = int64_t(callIos(x, "_lseek", {uint32_t(fd), 0u, uint32_t(off), off < 0 ? 0xFFFFFFFFu : 0u, uint32_t(whence)}));
    if (r > INT32_MAX) { setLinuxErrno(x, 75); return -1; }   // EOVERFLOW
    return int32_t(r);
}
int32_t b_ftruncate(Ctx& x, int32_t fd, int32_t len) {
    return int32_t(callIos(x, "_ftruncate", {uint32_t(fd), 0u, uint32_t(len), len < 0 ? 0xFFFFFFFFu : 0u}));
}
uint32_t b_mmap(Ctx& x, GuestPtr addr, uint32_t len, int32_t prot, uint32_t flags, int32_t fd, int32_t off) {
    uint32_t d = flags & 0x13;                     // SHARED, PRIVATE, FIXED
    if (flags & 0x20) d |= 0x1000;                 // MAP_ANONYMOUS
    if (flags & 0x4000) d |= 0x40;                 // MAP_NORESERVE
    return uint32_t(callIos(x, "_mmap", {addr, len, uint32_t(prot), d, uint32_t(fd), 0u, uint32_t(off), off < 0 ? 0xFFFFFFFFu : 0u}));
}

// bionic struct dirent: d_ino 8, d_off 8, d_reclen 2, d_type 1, d_name[256]: 280 bytes.
struct Dirs {
    explicit Dirs(Process&) {}
    std::mutex mu;
    std::unordered_map<uint32_t, uint32_t> entry;   // guest DIR* -> its readdir buffer
};
uint32_t b_opendir(Ctx& x, GuestPtr path) {
    const uint32_t d = uint32_t(callIos(x, "_opendir", {path}));
    if (!d) return 0;
    auto& ds = x.proc.service<Dirs>();
    std::lock_guard<std::mutex> lk(ds.mu);
    ds.entry[d] = x.proc.heap().allocZeroed(280);
    return d;
}
// Reads the next entry through the iOS readdir (Darwin dirent) into the bionic layout at `out`.
bool nextEntry(Ctx& x, GuestPtr dir, GuestPtr out) {
    const uint32_t e = uint32_t(callIos(x, "_readdir", {dir}));
    if (!e) return false;
    const uint16_t namlen = std::min<uint16_t>(x.mem.read<uint16_t>(e + 18), 255);
    std::memset(x.mem.host(out), 0, 280);
    x.mem.write<uint64_t>(out, x.mem.read<uint64_t>(e));
    x.mem.write<uint16_t>(out + 16, uint16_t((19 + namlen + 1 + 7) & ~7));
    x.mem.write<uint8_t>(out + 18, x.mem.read<uint8_t>(e + 20));
    std::memcpy(x.mem.host(out + 19), x.mem.host(e + 21), namlen);
    return true;
}
uint32_t b_readdir(Ctx& x, GuestPtr dir) {
    uint32_t out;
    {
        auto& ds = x.proc.service<Dirs>();
        std::lock_guard<std::mutex> lk(ds.mu);
        auto it = ds.entry.find(dir);
        if (it == ds.entry.end()) return 0;
        out = it->second;
    }
    return nextEntry(x, dir, out) ? out : 0;
}
int32_t b_readdir_r(Ctx& x, GuestPtr dir, GuestPtr entry, GuestPtr result) {
    const bool ok = nextEntry(x, dir, entry);
    x.mem.write<uint32_t>(result, ok ? entry : 0);
    return 0;
}
int32_t b_closedir(Ctx& x, GuestPtr dir) {
    uint32_t buf = 0;
    {
        auto& ds = x.proc.service<Dirs>();
        std::lock_guard<std::mutex> lk(ds.mu);
        auto it = ds.entry.find(dir);
        if (it != ds.entry.end()) { buf = it->second; ds.entry.erase(it); }
    }
    if (buf) x.proc.heap().free(buf);
    return int32_t(callIos(x, "_closedir", {dir}));
}
int32_t b_alphasort(Ctx& x, GuestPtr a, GuestPtr b) {
    const GuestPtr ea = a ? x.mem.read<uint32_t>(a) : 0;
    const GuestPtr eb = b ? x.mem.read<uint32_t>(b) : 0;
    return std::strcmp(ea ? x.proc.readCString(ea + 19, 256).c_str() : "",
                       eb ? x.proc.readCString(eb + 19, 256).c_str() : "");
}
int32_t b_scandir(Ctx& x, GuestPtr path, GuestPtr namelistOut, GuestPtr filterFn, GuestPtr comparFn) {
    const uint32_t dir = b_opendir(x, path);
    if (!dir) return -1;
    const uint32_t scratch = x.proc.heap().allocZeroed(280);
    std::vector<uint32_t> entries;
    while (nextEntry(x, dir, scratch)) {
        if (filterFn && int32_t(x.proc.callGuest(x.cpu, filterFn, {scratch})) == 0) continue;
        const uint32_t item = uint32_t(callIos(x, "_malloc", {280}));
        if (!item) break;
        std::memcpy(x.mem.host(item), x.mem.host(scratch), 280);
        entries.push_back(item);
    }
    x.proc.heap().free(scratch);
    b_closedir(x, dir);
    if (comparFn && entries.size() > 1) {
        const uint32_t pair = x.proc.heap().alloc(8);
        std::sort(entries.begin(), entries.end(), [&](uint32_t a, uint32_t b) {
            x.mem.write<uint32_t>(pair, a);
            x.mem.write<uint32_t>(pair + 4, b);
            return int32_t(x.proc.callGuest(x.cpu, comparFn, {pair, pair + 4})) < 0;
        });
        x.proc.heap().free(pair);
    }
    const uint32_t list = uint32_t(callIos(x, "_malloc", {uint32_t(std::max<size_t>(entries.size(), 1) * 4)}));
    for (size_t i = 0; i < entries.size(); ++i) x.mem.write<uint32_t>(list + uint32_t(i) * 4, entries[i]);
    if (namelistOut) x.mem.write<uint32_t>(namelistOut, list);
    return int32_t(entries.size());
}

// ----------------------------------------------------------------- system ---

int32_t b_sysconf(Ctx& x, int32_t name) {
    switch (name) {
        case 0x27: case 0x28: return 4096;      // _SC_PAGESIZE / _SC_PAGE_SIZE
        case 0x60: case 0x61: return 2;         // _SC_NPROCESSORS_CONF / ONLN
        case 0x62: return 262144;               // _SC_PHYS_PAGES (1 GB)
        case 0x63: return 131072;               // _SC_AVPHYS_PAGES
        case 0x06: return 100;                  // _SC_CLK_TCK
        case 0x0b: return 1024;                 // _SC_OPEN_MAX
        default:
            x.proc.reportDefaulted("sysconf(" + std::to_string(name) + ")");
            setLinuxErrno(x, 22);
            return -1;
    }
}
int32_t b_clockGettime(Ctx& x, int32_t clk, GuestPtr ts) {
    uint32_t d = uint32_t(clk);
    switch (clk) {
        case 1: d = 6; break;     // CLOCK_MONOTONIC
        case 2: d = 12; break;    // CLOCK_PROCESS_CPUTIME_ID
        case 3: d = 16; break;    // CLOCK_THREAD_CPUTIME_ID
        case 4: d = 4; break;     // CLOCK_MONOTONIC_RAW
        default: break;
    }
    return int32_t(callIos(x, "_clock_gettime", {d, ts}));
}
int32_t b_noChildProcesses(Ctx& x) { setLinuxErrno(x, 11); return -1; }        // fork: EAGAIN
int32_t b_execv(Ctx& x, GuestPtr, GuestPtr) { setLinuxErrno(x, 13); return -1; }  // EACCES
int32_t b_waitpid(Ctx& x, int32_t, GuestPtr, int32_t) { setLinuxErrno(x, 10); return -1; }   // ECHILD
int32_t b_system(Ctx&, GuestPtr cmd) { return cmd ? -1 : 0; }   // no shell
int32_t b_prctl(Ctx&, int32_t, uint32_t, uint32_t, uint32_t, uint32_t) { return 0; }
// No sockets yet: the address and addrinfo layouts differ from iOS's. Games that need the
// network see it as unavailable.
int32_t b_socket(Ctx& x, int32_t, int32_t, int32_t) { setLinuxErrno(x, 97); return -1; }   // EAFNOSUPPORT
int32_t b_getaddrinfo(Ctx&, GuestPtr, GuestPtr, GuestPtr, GuestPtr res) { (void)res; return 8; }   // EAI_NONAME (bionic)

// ARM EABI: __aeabi_atexit(object, destructor, dso) is __cxa_atexit with the first two swapped.
int32_t b_aeabiAtexit(Ctx& x, GuestPtr obj, GuestPtr dtor, GuestPtr dso) {
    return int32_t(callIos(x, "___cxa_atexit", {dtor, obj, dso}));
}
int32_t b_isinf(Ctx&, double v) { return std::isinf(v) ? (v > 0 ? 1 : -1) : 0; }

// ---------------------------------------------------------------- pthread ---

// Mutex attributes are one int: the type.
int32_t b_mutexattrInit(Ctx& x, GuestPtr a) { x.mem.write<uint32_t>(a, 0); return 0; }
int32_t b_mutexattrDestroy(Ctx&, GuestPtr) { return 0; }
int32_t b_mutexattrSettype(Ctx& x, GuestPtr a, int32_t type) {
    if (type < 0 || type > 2) return 22;
    x.mem.write<uint32_t>(a, uint32_t(type));
    return 0;
}
int32_t b_mutexattrGettype(Ctx& x, GuestPtr a, GuestPtr out) { x.mem.write<uint32_t>(out, x.mem.read<uint32_t>(a)); return 0; }
int32_t b_mutexattrSetpshared(Ctx&, GuestPtr, int32_t) { return 0; }
int32_t b_mutexInit(Ctx& x, GuestPtr mu, GuestPtr attr) {
    static const char key = 0;
    uint32_t& scratch = x.proc.threadSlot(&key);
    if (!scratch) scratch = x.proc.allocStatic(16);
    uint32_t darwinAttr = 0;
    if (attr) {
        x.mem.write<uint32_t>(scratch, 0x4D555458);
        // Types: Linux NORMAL 0 / RECURSIVE 1 / ERRORCHECK 2, Darwin NORMAL 0 / ERRORCHECK 1 / RECURSIVE 2.
        static const uint32_t kType[3] = {0, 2, 1};
        x.mem.write<uint32_t>(scratch + 4, kType[x.mem.read<uint32_t>(attr) % 3]);
        darwinAttr = scratch;
    }
    return linuxErrno(int32_t(callIos(x, "_pthread_mutex_init", {mu, darwinAttr})));
}
// Detach state: Android JOINABLE 0 / DETACHED 1, Darwin 1 / 2.
int32_t b_attrSetdetachstate(Ctx& x, GuestPtr a, int32_t s) {
    if (s != 0 && s != 1) return 22;
    return linuxErrno(int32_t(callIos(x, "_pthread_attr_setdetachstate", {a, uint32_t(s + 1)})));
}
int32_t b_attrGetdetachstate(Ctx& x, GuestPtr a, GuestPtr out) {
    const int32_t r = int32_t(callIos(x, "_pthread_attr_getdetachstate", {a, out}));
    if (!r) x.mem.write<uint32_t>(out, x.mem.read<uint32_t>(out) == 2 ? 1 : 0);
    return linuxErrno(r);
}
// pthread_getattr_np: an attribute describing the thread's stack.
int32_t b_getattrNp(Ctx& x, uint32_t thread, GuestPtr a) {
    callIos(x, "_pthread_attr_init", {a});
    GuestThread* t = x.proc.findThread(thread);
    if (!t) t = &x.proc.currentThread();
    if (t->stackBase && t->stackSize) callIos(x, "_pthread_attr_setstack", {a, t->stackBase, t->stackSize});
    return 0;
}
// pthread_once: one word, 0 until the routine has run.
struct Once {
    explicit Once(Process&) {}
    std::mutex mu;
    std::condition_variable cv;
    std::unordered_map<uint32_t, bool> running;
};
int32_t b_once(Ctx& x, GuestPtr once, GuestPtr fn) {
    auto& o = x.proc.service<Once>();
    {
        std::unique_lock<std::mutex> lk(o.mu);
        if (x.mem.read<uint32_t>(once) != 0) return 0;
        while (o.running[once]) {
            o.cv.wait_for(lk, 50ms);
            if (x.proc.stopping()) throw ProcessExit(0, "stopping");
        }
        if (x.mem.read<uint32_t>(once) != 0) return 0;
        o.running[once] = true;
    }
    x.proc.callGuest(x.cpu, fn, {});
    {
        std::lock_guard<std::mutex> lk(o.mu);
        x.mem.write<uint32_t>(once, 1);
        o.running.erase(once);
    }
    o.cv.notify_all();
    return 0;
}

// Unnamed semaphores (sem_t is one word on Android; iOS has none).
struct Sems {
    explicit Sems(Process&) {}
    std::mutex mu;
    std::condition_variable cv;
    std::unordered_map<uint32_t, uint32_t> count;
};
int32_t b_semInit(Ctx& x, GuestPtr s, int32_t, uint32_t value) {
    auto& ss = x.proc.service<Sems>();
    std::lock_guard<std::mutex> lk(ss.mu);
    ss.count[s] = value;
    x.mem.write<uint32_t>(s, value);
    return 0;
}
int32_t b_semDestroy(Ctx& x, GuestPtr s) {
    auto& ss = x.proc.service<Sems>();
    std::lock_guard<std::mutex> lk(ss.mu);
    ss.count.erase(s);
    return 0;
}
int32_t b_semPost(Ctx& x, GuestPtr s) {
    auto& ss = x.proc.service<Sems>();
    {
        std::lock_guard<std::mutex> lk(ss.mu);
        x.mem.write<uint32_t>(s, ++ss.count[s]);
    }
    ss.cv.notify_all();
    return 0;
}
// Waits until the count is positive or `deadline` (0: forever; -1: don't wait).
int32_t semTake(Ctx& x, GuestPtr s, int64_t deadlineNs) {
    auto& ss = x.proc.service<Sems>();
    std::unique_lock<std::mutex> lk(ss.mu);
    if (!ss.count[s] && deadlineNs >= 0) {
        BlockedScope blocked(x.proc, "sem_wait", s);
        while (!ss.count[s]) {
            if (deadlineNs > 0) {
                const int64_t now = x.proc.wallNowNs();
                if (now >= deadlineNs) break;
            }
            ss.cv.wait_for(lk, 20ms);
            if (x.proc.stopping()) throw ProcessExit(0, "stopping");
            if (x.cpu.async_pending.load(std::memory_order_acquire)) {   // a signal: its handler runs here
                lk.unlock();
                x.proc.engine().runSafepoint(x.cpu);
                lk.lock();
            }
        }
    }
    if (!ss.count[s]) { lk.unlock(); setLinuxErrno(x, deadlineNs < 0 ? 11 : 110); return -1; }   // EAGAIN / ETIMEDOUT
    x.mem.write<uint32_t>(s, --ss.count[s]);
    return 0;
}
int32_t b_semWait(Ctx& x, GuestPtr s) { return semTake(x, s, 0); }
int32_t b_semTrywait(Ctx& x, GuestPtr s) { return semTake(x, s, -1); }
int32_t b_semTimedwait(Ctx& x, GuestPtr s, GuestPtr ts) {
    const int64_t ns = int64_t(x.mem.read<int32_t>(ts)) * 1'000'000'000 + x.mem.read<int32_t>(ts + 4);
    return semTake(x, s, std::max<int64_t>(ns, 1));
}
int32_t b_semGetvalue(Ctx& x, GuestPtr s, GuestPtr out) {
    auto& ss = x.proc.service<Sems>();
    std::lock_guard<std::mutex> lk(ss.mu);
    x.mem.write<uint32_t>(out, ss.count[s]);
    return 0;
}

// Functions that return an error number: the iOS one, with the number translated.
void defineErrnoReturning(Process& p, const char* name) {
    const std::string ios = std::string("_") + name;
    p.defineFunction(std::string("android:") + name, [ios](Ctx& x) {
        auto addr = x.proc.lookupSymbol(ios);
        auto& eng = x.proc.engine();
        if (!addr || !eng.isHleAddress(*addr)) throw std::runtime_error("Android: no iOS function " + ios);
        eng.hleFunction(eng.hleIndex(*addr))(x.cpu);
        x.cpu.r[0] = uint32_t(linuxErrno(int32_t(x.cpu.r[0])));
    });
}

// Compressed textures. ETC1 (what Android games ship) is decoded to RGB here, since Apple's
// GPUs and desktop drivers don't take it; other formats go to the driver as they are.
void traceFormat(Ctx& x, uint32_t fmt, int32_t w, int32_t h) {
    static const bool on = [] { const char* e = std::getenv("HVM_JNI_TRACE"); return e && *e && *e != '0'; }();
    if (!on) return;
    static std::mutex mu;
    static std::set<uint32_t> seen;
    std::lock_guard<std::mutex> lk(mu);
    if (seen.insert(fmt).second) x.proc.logf(LogLevel::Info, "GL compressed format 0x%04x (%dx%d)", fmt, w, h);
}
// Uploads decoded ETC1 as GL_RGB / GL_UNSIGNED_BYTE through `call` (glTexImage2D or
// glTexSubImage2D), with a byte-aligned unpack row for the duration.
template <typename F> void uploadEtc1(Ctx& x, int32_t w, int32_t h, int32_t size, GuestPtr data, F call) {
    if (w <= 0 || h <= 0 || !data || size_t(size) < etc1Size(w, h)) {
        call(GuestPtr(0));
        return;
    }
    const auto rgb = decodeEtc1(x.mem.host(data), w, h);
    const uint32_t buf = x.proc.heap().alloc(uint32_t(rgb.size()));
    if (!buf) throw std::runtime_error("ETC1: out of guest memory");
    x.mem.copyIn(buf, rgb.data(), rgb.size());
    static const char key = 0;
    uint32_t& cell = x.proc.threadSlot(&key);
    if (!cell) cell = x.proc.allocStatic(16);
    callIos(x, "_glGetIntegerv", {0x0CF5 /*UNPACK_ALIGNMENT*/, cell});
    const uint32_t savedAlign = x.mem.read<uint32_t>(cell);
    callIos(x, "_glPixelStorei", {0x0CF5, 1});
    call(buf);
    callIos(x, "_glPixelStorei", {0x0CF5, savedAlign ? savedAlign : 4});
    x.proc.heap().free(buf);
}
void b_glCompressedTexImage2D(Ctx& x, uint32_t target, int32_t level, uint32_t fmt, int32_t w, int32_t h, int32_t border,
                              int32_t size, GuestPtr data) {
    traceFormat(x, fmt, w, h);
    if (fmt == kGlEtc1Rgb8) {
        uploadEtc1(x, w, h, size, data, [&](GuestPtr pixels) {
            callIos(x, "_glTexImage2D", {target, uint32_t(level), 0x1907 /*RGB*/, uint32_t(w), uint32_t(h), uint32_t(border),
                                          0x1907, 0x1401 /*UNSIGNED_BYTE*/, pixels});
        });
        return;
    }
    callIos(x, "_glCompressedTexImage2D", {target, uint32_t(level), fmt, uint32_t(w), uint32_t(h), uint32_t(border), uint32_t(size), data});
}
void b_glCompressedTexSubImage2D(Ctx& x, uint32_t target, int32_t level, int32_t xo, int32_t yo, int32_t w, int32_t h,
                                 uint32_t fmt, int32_t size, GuestPtr data) {
    traceFormat(x, fmt, w, h);
    if (fmt == kGlEtc1Rgb8) {
        uploadEtc1(x, w, h, size, data, [&](GuestPtr pixels) {
            callIos(x, "_glTexSubImage2D", {target, uint32_t(level), uint32_t(xo), uint32_t(yo), uint32_t(w), uint32_t(h),
                                             0x1907, 0x1401, pixels});
        });
        return;
    }
    callIos(x, "_glCompressedTexSubImage2D", {target, uint32_t(level), uint32_t(xo), uint32_t(yo), uint32_t(w), uint32_t(h),
                                              fmt, uint32_t(size), data});
}

} // namespace

void registerBionic(Process& p) {
    registerSurfaceFunctions(p);
    registerBionicExtra(p);
    registerLibAndroid(p);
    registerOpenSLES(p);
    registerLibdl(p);
    registerEgl(p);
    registerSyscalls(p);
    registerSignals(p);
    registerFmodBridge(p);
    registerLooper(p);
    installProcFs(p);
    p.defineFunction("android:glCompressedTexImage2D", [](Ctx& x) { callWrapped<&b_glCompressedTexImage2D>(x); });
    p.defineFunction("android:glCompressedTexSubImage2D", [](Ctx& x) { callWrapped<&b_glCompressedTexSubImage2D>(x); });
    // Data.
    {
        const uint32_t sF = p.defineDataBlock("android:__sF", kBionicFileSize * 3);
        registerStdioStreams(p, sF, kBionicFileSize);
        p.mem().write<uint32_t>(p.defineDataBlock("android:stdin", 4), sF);
        p.mem().write<uint32_t>(p.defineDataBlock("android:stdout", 4), sF + kBionicFileSize);
        p.mem().write<uint32_t>(p.defineDataBlock("android:stderr", 4), sF + kBionicFileSize * 2);
    }
    defineCtype(p);
    p.mem().write<uint32_t>(p.defineDataBlock("android:__page_size", 4), 4096);
    p.defineDataBlock("android:__dso_handle", 4);
    p.defineDataBlock("android:timezone", 4);
    {
        const uint32_t names = p.defineDataBlock("android:tzname", 8);
        p.mem().write<uint32_t>(names, p.internCString("UTC"));
        p.mem().write<uint32_t>(names + 4, p.internCString("UTC"));
    }

#define B(F, NAME, WHAT) supportDef<&F>(p, std::string("android:") + NAME, SupportStatus::Real, WHAT)
#define BA(F, NAME, WHAT) supportDef<&F>(p, std::string("android:") + NAME, SupportStatus::Approx, WHAT)
    B(b_aeabiAtexit, "__aeabi_atexit", "the iOS __cxa_atexit, arguments swapped");
    B(b_isinf, "__isinf", "host isinf");
    B(b_errno, "__errno", "per-thread errno cell; Darwin values from iOS functions translated on read");
    B(b_logWrite, "__android_log_write", "to the game log");
    B(b_logPrint, "__android_log_print", "formatted to the game log");
    B(b_logVprint, "__android_log_vprint", "formatted to the game log");
    B(b_logAssert, "__android_log_assert", "logs the assertion and stops the game");
    B(b_setAbortMessage, "android_set_abort_message", "logs the abort message");
    BA(b_openlog, "openlog", "accepted; syslog writes to the game log");
    BA(b_closelog, "closelog", "accepted; syslog writes to the game log");
    B(b_syslog, "syslog", "formatted to the game log");
    B(b_atomicCmpxchg, "__atomic_cmpxchg", "host atomic compare-and-swap");
    B(b_atomicSwap, "__atomic_swap", "host atomic exchange");
    B(b_atomicInc, "__atomic_inc", "host atomic increment");
    B(b_atomicDec, "__atomic_dec", "host atomic decrement");
    B(b_findExidx, "__gnu_Unwind_Find_exidx", "the library's PT_ARM_EXIDX table");
    B(b_open, "open", "Linux open flags translated to the iOS open");
    B(b_open2, "__open_2", "FORTIFY 2-argument open");
    B(b_fcntl, "fcntl", "Linux status flags translated to the iOS fcntl");
    B(b_stat, "stat", "bionic struct stat");
    B(b_lstat, "lstat", "bionic struct stat");
    B(b_fstat, "fstat", "bionic struct stat");
    B(b_lseek, "lseek", "32-bit off_t");
    B(b_ftruncate, "ftruncate", "32-bit off_t");
    B(b_mmap, "mmap", "Linux flags and 32-bit offset translated to the iOS mmap");
    B(b_opendir, "opendir", "iOS opendir with a bionic dirent buffer");
    B(b_readdir, "readdir", "bionic struct dirent");
    B(b_readdir_r, "readdir_r", "bionic struct dirent");
    B(b_closedir, "closedir", "iOS closedir");
    B(b_alphasort, "alphasort", "bionic dirent name comparison");
    B(b_scandir, "scandir", "directory scan into guest-allocated bionic dirents");
    BA(b_sysconf, "sysconf", "bionic names for page size, processors, memory and clock ticks; others EINVAL");
    B(b_clockGettime, "clock_gettime", "Linux clock ids translated");
    BA(b_noChildProcesses, "fork", "no child processes: EAGAIN");
    BA(b_execv, "execv", "no child processes: EACCES");
    BA(b_waitpid, "waitpid", "no child processes: ECHILD");
    BA(b_system, "system", "no shell");
    BA(b_prctl, "prctl", "accepted and ignored");
    BA(b_socket, "socket", "no Android sockets yet: EAFNOSUPPORT");
    BA(b_getaddrinfo, "getaddrinfo", "no Android sockets yet: EAI_NONAME");
    B(b_mutexattrInit, "pthread_mutexattr_init", "one-word bionic attribute");
    B(b_mutexattrDestroy, "pthread_mutexattr_destroy", "one-word bionic attribute");
    B(b_mutexattrSettype, "pthread_mutexattr_settype", "one-word bionic attribute");
    B(b_mutexattrGettype, "pthread_mutexattr_gettype", "one-word bionic attribute");
    BA(b_mutexattrSetpshared, "pthread_mutexattr_setpshared", "one process: ignored");
    B(b_mutexInit, "pthread_mutex_init", "iOS mutex with the bionic attribute's type");
    B(b_attrSetdetachstate, "pthread_attr_setdetachstate", "bionic detach-state values");
    B(b_attrGetdetachstate, "pthread_attr_getdetachstate", "bionic detach-state values");
    B(b_getattrNp, "pthread_getattr_np", "the thread's stack");
    B(b_once, "pthread_once", "one-word bionic pthread_once_t");
    B(b_semInit, "sem_init", "host counting semaphore");
    B(b_semDestroy, "sem_destroy", "host counting semaphore");
    B(b_semPost, "sem_post", "host counting semaphore");
    B(b_semWait, "sem_wait", "host counting semaphore");
    B(b_semTrywait, "sem_trywait", "host counting semaphore");
    B(b_semTimedwait, "sem_timedwait", "host counting semaphore");
    B(b_semGetvalue, "sem_getvalue", "host counting semaphore");
#undef B
#undef BA
    for (const char* n : {"pthread_mutex_lock", "pthread_mutex_trylock", "pthread_mutex_unlock", "pthread_mutex_destroy",
                          "pthread_cond_init", "pthread_cond_destroy", "pthread_cond_wait", "pthread_cond_timedwait",
                          "pthread_cond_signal", "pthread_cond_broadcast", "pthread_create", "pthread_join",
                          "pthread_detach", "pthread_key_create", "pthread_key_delete", "pthread_setspecific",
                          "pthread_attr_init", "pthread_attr_destroy", "pthread_attr_setstacksize",
                          "pthread_attr_getstacksize", "pthread_attr_setstack", "pthread_attr_getstack",
                          "pthread_attr_setschedparam", "pthread_attr_getschedparam", "pthread_getschedparam",
                          "pthread_setschedparam"})
        if (p.lookupSymbol(std::string("_") + n)) defineErrnoReturning(p, n);
    registerNativePlaceholders(p);   // last: never replaces a real function
}

uint32_t resolveImport(Process& p, const std::string& name, bool weak) {
    if (auto a = p.lookupSymbol("android:" + name)) return *a;
    if (auto a = p.lookupSymbol("_" + name)) return *a;
    if (weak) return 0;
    if (uint32_t a = p.resolveMissingSymbol("_" + name, Process::MissingSymbolMode::Import)) return a;
    return p.makeMissingStub(name);
}

} // namespace Hydrogen::Runtime::Android
