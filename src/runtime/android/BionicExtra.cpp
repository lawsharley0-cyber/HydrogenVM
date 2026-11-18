// HydrogenVM — the rest of bionic that Android games import and iOS has no function of the same
// name for: ARM EABI memory helpers, assertions, wide-character I/O and classification, the
// auxiliary vector, system properties, SHA-1, pthread cleanup handlers, and calls that have no
// meaning here (process control, user ids). Measured with `hvm android-imports` over the games
// brought up so far; see Android.hpp.
#include "runtime/android/Android.hpp"
#include "runtime/android/BionicInternal.hpp"
#include "runtime/Support.hpp"
#include "core/cpu/Engine.hpp"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <ctime>
#include <cwctype>
#include <array>
#include <map>
#include <mutex>
#include <random>
#include <cstdlib>
#include <string>
#include <sys/stat.h>
#include <thread>
#include <unistd.h>
#include <vector>

namespace Hydrogen::Runtime::Android {
namespace {

// ------------------------------------------------------------ ARM EABI memory ---
// __aeabi_memset takes (dest, n, c), unlike memset; memclr is memset to 0.

void a_memcpy(Ctx& x, GuestPtr d, GuestPtr s, uint32_t n) { if (n) std::memmove(x.mem.host(d), x.mem.host(s), n); }
void a_memset(Ctx& x, GuestPtr d, uint32_t n, int32_t c) { if (n) std::memset(x.mem.host(d), c & 0xFF, n); }
void a_memclr(Ctx& x, GuestPtr d, uint32_t n) { if (n) std::memset(x.mem.host(d), 0, n); }

GuestPtr s_memmem(Ctx& x, GuestPtr hay, uint32_t hn, GuestPtr needle, uint32_t nn) {
    if (!nn) return hay;
    if (nn > hn) return 0;
    const uint8_t* h = x.mem.host(hay);
    const uint8_t* n = x.mem.host(needle);
    for (uint32_t i = 0; i + nn <= hn; ++i)
        if (h[i] == n[0] && !std::memcmp(h + i, n, nn)) return hay + i;
    return 0;
}
GuestPtr s_memrchr(Ctx& x, GuestPtr s, int32_t c, uint32_t n) {
    const uint8_t* p = x.mem.host(s);
    for (uint32_t i = n; i-- > 0;) if (p[i] == uint8_t(c)) return s + i;
    return 0;
}
GuestPtr s_memalign(Ctx& x, uint32_t align, uint32_t size) {
    if (align < 8) align = 8;
    if (align & (align - 1)) { setLinuxErrno(x, 22); return 0; }
    const uint32_t p = x.proc.heap().allocAligned(size ? size : 1, align);
    if (!p) setLinuxErrno(x, 12);
    return p;
}
uint32_t s_usableSize(Ctx& x, GuestPtr p) { return p ? uint32_t(callIos(x, "_malloc_size", {p})) : 0; }

// ------------------------------------------------------------------ asserts ---

void s_assert(Ctx& x, GuestPtr file, int32_t line, GuestPtr msg) {
    throw std::runtime_error("assertion failed: " + x.proc.readCString(msg, 1024) + " (" + x.proc.readCString(file, 512) + ":" +
                             std::to_string(line) + ")");
}
void s_assert2(Ctx& x, GuestPtr file, int32_t line, GuestPtr fn, GuestPtr msg) {
    throw std::runtime_error("assertion failed: " + x.proc.readCString(msg, 1024) + " in " + x.proc.readCString(fn, 512) + " (" +
                             x.proc.readCString(file, 512) + ":" + std::to_string(line) + ")");
}

// ------------------------------------------------------------------- math ---

int32_t m_isfinite(Ctx&, double v) { return std::isfinite(v); }
int32_t m_isfinitef(Ctx&, float v) { return std::isfinite(v); }
int32_t m_isnan(Ctx&, double v) { return std::isnan(v); }
int32_t m_signbit(Ctx&, double v) { return std::signbit(v); }
float m_nextafterf(Ctx&, float a, float b) { return std::nextafter(a, b); }
double m_scalbnl(Ctx&, double v, int32_t e) { return std::scalbn(v, e); }   // long double is double on ARM
double m_remquo(Ctx& x, double a, double b, GuestPtr q) {
    int quo = 0;
    const double r = std::remquo(a, b, &quo);
    if (q) x.mem.write<int32_t>(q, quo);
    return r;
}

// --------------------------------------------------------- wide characters ---
// wchar_t is 32 bits on Android, as on the host.

int32_t w_isblank(Ctx&, int32_t c) { return c == ' ' || c == '\t'; }
int32_t w_iswcntrl(Ctx&, int32_t c) { return std::iswcntrl(wint_t(c)) != 0; }
int32_t w_iswlower(Ctx&, int32_t c) { return std::iswlower(wint_t(c)) != 0; }
int32_t w_iswupper(Ctx&, int32_t c) { return std::iswupper(wint_t(c)) != 0; }
int32_t w_iswprint(Ctx&, int32_t c) { return std::iswprint(wint_t(c)) != 0; }
int32_t w_iswpunct(Ctx&, int32_t c) { return std::iswpunct(wint_t(c)) != 0; }
int32_t w_iswxdigit(Ctx&, int32_t c) { return std::iswxdigit(wint_t(c)) != 0; }
// wctype_t: an index into this list (0 is invalid).
const char* const kClasses[] = {"", "alnum", "alpha", "blank", "cntrl", "digit", "graph",
                                "lower", "print", "punct", "space", "upper", "xdigit"};
uint32_t w_wctype(Ctx& x, GuestPtr name) {
    const std::string n = x.proc.readCString(name, 32);
    for (uint32_t i = 1; i < std::size(kClasses); ++i) if (n == kClasses[i]) return i;
    return 0;
}
int32_t w_iswctype(Ctx&, int32_t c, uint32_t t) {
    const wint_t w = wint_t(c);
    switch (t) {
        case 1: return std::iswalnum(w) != 0;  case 2: return std::iswalpha(w) != 0;
        case 3: return c == ' ' || c == '\t'; case 4: return std::iswcntrl(w) != 0;
        case 5: return std::iswdigit(w) != 0;  case 6: return std::iswgraph(w) != 0;
        case 7: return std::iswlower(w) != 0;  case 8: return std::iswprint(w) != 0;
        case 9: return std::iswpunct(w) != 0;  case 10: return std::iswspace(w) != 0;
        case 11: return std::iswupper(w) != 0; case 12: return std::iswxdigit(w) != 0;
        default: return 0;
    }
}
// Wide stdio through the byte functions, UTF-8 encoded.
int32_t w_getwc(Ctx& x, GuestPtr fp) {
    const int32_t c = int32_t(callIos(x, "_fgetc", {fp}));
    if (c < 0x80) return c;   // EOF (-1) or ASCII
    const int extra = c >= 0xF0 ? 3 : c >= 0xE0 ? 2 : c >= 0xC0 ? 1 : 0;
    int32_t v = c & (0x3F >> extra);
    for (int i = 0; i < extra; ++i) {
        const int32_t n = int32_t(callIos(x, "_fgetc", {fp}));
        if (n < 0) return -1;
        v = (v << 6) | (n & 0x3F);
    }
    return v;
}
int32_t w_putwc(Ctx& x, int32_t c, GuestPtr fp) {
    char b[4];
    int n = 0;
    const uint32_t u = uint32_t(c);
    if (u < 0x80) b[n++] = char(u);
    else if (u < 0x800) { b[n++] = char(0xC0 | (u >> 6)); b[n++] = char(0x80 | (u & 63)); }
    else if (u < 0x10000) { b[n++] = char(0xE0 | (u >> 12)); b[n++] = char(0x80 | ((u >> 6) & 63)); b[n++] = char(0x80 | (u & 63)); }
    else { b[n++] = char(0xF0 | (u >> 18)); b[n++] = char(0x80 | ((u >> 12) & 63)); b[n++] = char(0x80 | ((u >> 6) & 63)); b[n++] = char(0x80 | (u & 63)); }
    for (int i = 0; i < n; ++i) if (int32_t(callIos(x, "_fputc", {uint32_t(uint8_t(b[i])), fp})) < 0) return -1;
    return c;
}
int32_t w_ungetwc(Ctx& x, int32_t c, GuestPtr fp) {
    if (c < 0 || c >= 0x80) return -1;   // only one byte can be pushed back
    return int32_t(callIos(x, "_ungetc", {uint32_t(c), fp}));
}
// wcsftime: strftime into bytes, then widened.
uint32_t w_wcsftime(Ctx& x, GuestPtr out, uint32_t max, GuestPtr fmt, GuestPtr tm) {
    std::string f;
    for (uint32_t a = fmt;; a += 4) {
        const uint32_t c = x.mem.read<uint32_t>(a);
        if (!c || f.size() > 512) break;
        f.push_back(c < 0x80 ? char(c) : '?');
    }
    const uint32_t fbuf = x.proc.allocCString(f);
    const uint32_t buf = x.proc.heap().alloc(1024);
    const uint32_t n = uint32_t(callIos(x, "_strftime", {buf, 1024, fbuf, tm}));
    x.proc.heap().free(fbuf);
    uint32_t written = 0;
    if (n && n < max) {
        for (uint32_t i = 0; i < n; ++i) x.mem.write<uint32_t>(out + i * 4, x.mem.read<uint8_t>(buf + i));
        x.mem.write<uint32_t>(out + n * 4, 0);
        written = n;
    }
    x.proc.heap().free(buf);
    return written;
}

// --------------------------------------------------------------- files ---

int32_t f_fgetpos(Ctx& x, GuestPtr fp, GuestPtr pos) {
    const int32_t r = int32_t(callIos(x, "_ftell", {fp}));
    if (r < 0) return -1;
    x.mem.write<int32_t>(pos, r);   // fpos_t is a 32-bit off_t
    return 0;
}
int32_t f_fsetpos(Ctx& x, GuestPtr fp, GuestPtr pos) {
    return int32_t(callIos(x, "_fseek", {fp, x.mem.read<uint32_t>(pos), 0}));
}
int32_t f_fchmod(Ctx& x, int32_t fd, uint32_t mode) { return ::fchmod(fd, mode & 07777) < 0 ? failHost<int32_t>(x) : 0; }
int32_t f_fchown(Ctx&, int32_t, uint32_t, uint32_t) { return 0; }   // one user
int32_t f_truncate(Ctx& x, GuestPtr path, int32_t len) {
    return ::truncate(x.proc.hostPath(x.proc.readCString(path)).c_str(), len) < 0 ? failHost<int32_t>(x) : 0;
}
uint32_t f_umask(Ctx&, uint32_t) { return 022; }
void f_rewinddir(Ctx& x, GuestPtr dir) { (void)x; (void)dir; x.proc.reportDefaulted("rewinddir (directory not rewound)"); }

// ----------------------------------------------------------- the process ---

// AT_HWCAP: what Android's kernel reports for an ARMv7 CPU with NEON and VFPv3, which the
// emulated CPU implements. AT_HWCAP2 (ARMv8 crypto: AES, PMULL, SHA) is empty — libraries
// that check it (OpenSSL) then stay off those instructions.
uint32_t p_getauxval(Ctx&, uint32_t type) {
    switch (type) {
        case 6: return 4096;                     // AT_PAGESZ
        case 16: return 1 | 2 | 4 | 16 | 64 | 128 | 4096 | 8192 | 32768;   // SWP HALF THUMB FAST_MULT VFP EDSP NEON VFPv3 TLS
        case 26: return 0;                       // AT_HWCAP2
        default: return 0;
    }
}
int32_t p_gettid(Ctx& x) { return int32_t(1000 + x.proc.currentThread().id); }
int32_t p_getresuid(Ctx& x, GuestPtr r, GuestPtr e, GuestPtr s) {
    for (GuestPtr p : {r, e, s}) if (p) x.mem.write<uint32_t>(p, 10000);
    return 0;
}
int32_t p_ok3(Ctx&, uint32_t, uint32_t, uint32_t) { return 0; }
int32_t p_eperm(Ctx& x, uint32_t, uint32_t, uint32_t, uint32_t) { setLinuxErrno(x, 1); return -1; }
int32_t p_eacces(Ctx& x, GuestPtr, GuestPtr, GuestPtr) { setLinuxErrno(x, 13); return -1; }
int32_t p_enosys(Ctx& x) { setLinuxErrno(x, 38); return -1; }
int32_t p_epollCreate(Ctx& x, int32_t) { setLinuxErrno(x, 38); return -1; }
int32_t p_epollCtl(Ctx& x, int32_t, int32_t, int32_t, GuestPtr) { setLinuxErrno(x, 9); return -1; }
int32_t p_epollWait(Ctx& x, int32_t, GuestPtr, int32_t, int32_t) { setLinuxErrno(x, 9); return -1; }
uint32_t p_mremap(Ctx& x, GuestPtr, uint32_t, uint32_t, uint32_t) { setLinuxErrno(x, 12); return 0xFFFFFFFFu; }
int32_t p_clockGetres(Ctx& x, int32_t, GuestPtr ts) {
    if (ts) { x.mem.write<int32_t>(ts, 0); x.mem.write<int32_t>(ts + 4, 1); }
    return 0;
}
uint32_t p_null(Ctx&, GuestPtr) { return 0; }
int32_t p_getnameinfo(Ctx&, GuestPtr, uint32_t, GuestPtr, uint32_t, GuestPtr, uint32_t, int32_t) { return 8; }   // EAI_NONAME
void p_noop(Ctx&) {}

// System properties: the few games read, as a device of the API level we report.
int32_t p_propertyGet(Ctx& x, GuestPtr name, GuestPtr value) {
    static const std::map<std::string, std::string> kProps = {
        {"ro.build.version.sdk", "19"}, {"ro.build.version.release", "4.4.4"},
        {"ro.product.model", "HydrogenVM"}, {"ro.product.manufacturer", "HydrogenVM"},
        {"ro.product.brand", "HydrogenVM"}, {"ro.product.device", "hydrogenvm"},
        {"ro.product.name", "hydrogenvm"}, {"ro.product.cpu.abi", "armeabi-v7a"},
        {"ro.product.cpu.abi2", "armeabi"}, {"ro.hardware", "hydrogenvm"},
        {"ro.board.platform", "hydrogenvm"}, {"ro.build.fingerprint", "HydrogenVM/hydrogenvm/hydrogenvm:4.4.4/KTU84P/1:user/release-keys"},
        {"ro.kernel.qemu", "0"}, {"ro.debuggable", "0"}, {"ro.secure", "1"},
    };
    const std::string n = x.proc.readCString(name, 128);
    auto it = kProps.find(n);
    const std::string v = it == kProps.end() ? std::string() : it->second;
    x.mem.copyIn(value, v.c_str(), v.size() + 1);
    return int32_t(v.size());
}

// pthread_cond_timedwait_monotonic_np: the deadline is on CLOCK_MONOTONIC; turned into the
// realtime deadline pthread_cond_timedwait takes.
int32_t p_condTimedwaitMonotonic(Ctx& x, GuestPtr cond, GuestPtr mutex, GuestPtr abstime) {
    using namespace std::chrono;
    const int64_t due = int64_t(x.mem.read<int32_t>(abstime)) * 1'000'000'000 + x.mem.read<int32_t>(abstime + 4);
    const int64_t mono = duration_cast<nanoseconds>(steady_clock::now().time_since_epoch()).count();
    const int64_t real = x.proc.wallNowNs() + (due - mono);
    static const char key = 0;
    uint32_t& ts = x.proc.threadSlot(&key);
    if (!ts) ts = x.proc.allocStatic(8);
    x.mem.write<int32_t>(ts, int32_t(real / 1'000'000'000));
    x.mem.write<int32_t>(ts + 4, int32_t(real % 1'000'000'000));
    return linuxErrno(int32_t(callIos(x, "_pthread_cond_timedwait", {cond, mutex, ts})));
}

// __pthread_cleanup_push / pop: bionic's struct is {prev, routine, arg}; the list is per thread.
uint32_t& cleanupTop(Ctx& x) {
    static const char key = 0;
    return x.proc.threadSlot(&key);
}
void p_cleanupPush(Ctx& x, GuestPtr c, GuestPtr routine, GuestPtr arg) {
    x.mem.write<uint32_t>(c, cleanupTop(x));
    x.mem.write<uint32_t>(c + 4, routine);
    x.mem.write<uint32_t>(c + 8, arg);
    cleanupTop(x) = c;
}
void p_cleanupPop(Ctx& x, GuestPtr c, int32_t execute) {
    cleanupTop(x) = x.mem.read<uint32_t>(c);
    if (execute) x.proc.callGuest(x.cpu, x.mem.read<uint32_t>(c + 4), {x.mem.read<uint32_t>(c + 8)});
}

// C++ exception helpers that libstdc++ (gnustl) provides on Android. A library with its own
// unwinder calls them from its personality routine: cleanup always proceeds; HydrogenVM's
// C++ runtime does the type matching for exceptions it threw, so a foreign exception matches
// no handler here.
int32_t x_beginCleanup(Ctx&, GuestPtr) { return 1; }
int32_t x_typeMatch(Ctx&, GuestPtr, GuestPtr, int32_t, GuestPtr) { return 0; }   // ctm_failed

// dl_unwind_find_exidx: the exception index of whichever loaded library holds `pc`.
uint32_t x_findExidx(Ctx& x, uint32_t pc, GuestPtr count) {
    return uint32_t(callIos(x, "android:__gnu_Unwind_Find_exidx", {pc, count}));
}

// ------------------------------------------------------------------ SHA-1 ---
// bionic's SHA1_CTX: state[5], count[2] (bits, little word first), buffer[64].

uint32_t rol(uint32_t v, int n) { return (v << n) | (v >> (32 - n)); }
void sha1Block(uint32_t st[5], const uint8_t* b) {
    uint32_t w[80];
    for (int i = 0; i < 16; ++i) w[i] = uint32_t(b[i * 4]) << 24 | uint32_t(b[i * 4 + 1]) << 16 | uint32_t(b[i * 4 + 2]) << 8 | b[i * 4 + 3];
    for (int i = 16; i < 80; ++i) w[i] = rol(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
    uint32_t a = st[0], bb = st[1], c = st[2], d = st[3], e = st[4];
    for (int i = 0; i < 80; ++i) {
        uint32_t f, k;
        if (i < 20) { f = (bb & c) | (~bb & d); k = 0x5A827999; }
        else if (i < 40) { f = bb ^ c ^ d; k = 0x6ED9EBA1; }
        else if (i < 60) { f = (bb & c) | (bb & d) | (c & d); k = 0x8F1BBCDC; }
        else { f = bb ^ c ^ d; k = 0xCA62C1D6; }
        const uint32_t t = rol(a, 5) + f + e + k + w[i];
        e = d; d = c; c = rol(bb, 30); bb = a; a = t;
    }
    st[0] += a; st[1] += bb; st[2] += c; st[3] += d; st[4] += e;
}
void h_sha1Init(Ctx& x, GuestPtr ctx) {
    const uint32_t init[5] = {0x67452301, 0xEFCDAB89, 0x98BADCFE, 0x10325476, 0xC3D2E1F0};
    for (int i = 0; i < 5; ++i) x.mem.write<uint32_t>(ctx + uint32_t(i) * 4, init[i]);
    x.mem.write<uint32_t>(ctx + 20, 0);
    x.mem.write<uint32_t>(ctx + 24, 0);
}
void h_sha1Update(Ctx& x, GuestPtr ctx, GuestPtr data, uint32_t len) {
    uint32_t st[5];
    for (int i = 0; i < 5; ++i) st[i] = x.mem.read<uint32_t>(ctx + uint32_t(i) * 4);
    uint64_t bits = uint64_t(x.mem.read<uint32_t>(ctx + 24)) << 32 | x.mem.read<uint32_t>(ctx + 20);
    uint32_t have = uint32_t((bits / 8) % 64);
    uint8_t* buf = x.mem.host(ctx + 28);
    const uint8_t* in = x.mem.host(data);
    bits += uint64_t(len) * 8;
    for (uint32_t i = 0; i < len; ++i) {
        buf[have++] = in[i];
        if (have == 64) { sha1Block(st, buf); have = 0; }
    }
    for (int i = 0; i < 5; ++i) x.mem.write<uint32_t>(ctx + uint32_t(i) * 4, st[i]);
    x.mem.write<uint32_t>(ctx + 20, uint32_t(bits));
    x.mem.write<uint32_t>(ctx + 24, uint32_t(bits >> 32));
}
void h_sha1Final(Ctx& x, GuestPtr digest, GuestPtr ctx) {
    const uint64_t bits = uint64_t(x.mem.read<uint32_t>(ctx + 24)) << 32 | x.mem.read<uint32_t>(ctx + 20);
    const uint32_t pad = x.proc.heap().allocZeroed(72);
    x.mem.write<uint8_t>(pad, 0x80);
    const uint32_t have = uint32_t((bits / 8) % 64);
    h_sha1Update(x, ctx, pad, have < 56 ? 56 - have : 120 - have);
    for (int i = 0; i < 8; ++i) x.mem.write<uint8_t>(pad + uint32_t(i), uint8_t(bits >> (56 - i * 8)));
    h_sha1Update(x, ctx, pad, 8);
    x.proc.heap().free(pad);
    for (int i = 0; i < 5; ++i) {
        const uint32_t v = x.mem.read<uint32_t>(ctx + uint32_t(i) * 4);
        for (int k = 0; k < 4; ++k) x.mem.write<uint8_t>(digest + uint32_t(i * 4 + k), uint8_t(v >> (24 - k * 8)));
    }
}

// -------------------------------------------------- FORTIFY, math, locale, zlib ---

uint32_t c_strlenChk(Ctx& x, GuestPtr s, uint32_t) { return uint32_t(callIos(x, "_strlen", {s})); }
GuestPtr c_strchrChk(Ctx& x, GuestPtr s, int32_t c, uint32_t) { return uint32_t(callIos(x, "_strchr", {s, uint32_t(c)})); }
GuestPtr c_strrchrChk(Ctx& x, GuestPtr s, int32_t c, uint32_t) { return uint32_t(callIos(x, "_strrchr", {s, uint32_t(c)})); }
GuestPtr c_strncpyChk2(Ctx& x, GuestPtr d, GuestPtr s, uint32_t n, uint32_t, uint32_t) {
    return uint32_t(callIos(x, "_strncpy", {d, s, n}));
}
void c_fdSetChk(Ctx& x, int32_t fd, GuestPtr set, uint32_t setsize) {
    if (fd < 0 || !set || uint32_t(fd / 8) >= setsize) return;
    const uint32_t w = set + uint32_t(fd / 32) * 4;
    x.mem.write<uint32_t>(w, x.mem.read<uint32_t>(w) | (1u << (fd & 31)));
}
int32_t c_fdIssetChk(Ctx& x, int32_t fd, GuestPtr set, uint32_t setsize) {
    if (fd < 0 || !set || uint32_t(fd / 8) >= setsize) return 0;
    return (x.mem.read<uint32_t>(set + uint32_t(fd / 32) * 4) & (1u << (fd & 31))) ? 1 : 0;
}
int32_t c_readChk(Ctx& x, int32_t fd, GuestPtr buf, uint32_t count, uint32_t) {
    return int32_t(callIos(x, "_read", {uint32_t(fd), buf, count}));
}
int32_t c_writeChk(Ctx& x, int32_t fd, GuestPtr buf, uint32_t count, uint32_t) {
    return int32_t(callIos(x, "_write", {uint32_t(fd), buf, count}));
}
uint32_t c_freadChk(Ctx& x, GuestPtr buf, uint32_t size, uint32_t count, GuestPtr stream, uint32_t) {
    return uint32_t(callIos(x, "_fread", {buf, size, count, stream}));
}
uint32_t c_fwriteChk(Ctx& x, GuestPtr buf, uint32_t size, uint32_t count, GuestPtr stream, uint32_t) {
    return uint32_t(callIos(x, "_fwrite", {buf, size, count, stream}));
}
int32_t c_pollChk(Ctx& x, GuestPtr fds, uint32_t nfds, int32_t timeout, uint32_t) {
    return int32_t(callIos(x, "_poll", {fds, nfds, uint32_t(timeout)}));
}

void m_sincosf(Ctx& x, float a, GuestPtr sOut, GuestPtr cOut) {
    if (sOut) x.mem.write<float>(sOut, std::sin(a));
    if (cOut) x.mem.write<float>(cOut, std::cos(a));
}
void m_sincos(Ctx& x, double a, GuestPtr sOut, GuestPtr cOut) {
    if (sOut) x.mem.write<double>(sOut, std::sin(a));
    if (cOut) x.mem.write<double>(cOut, std::cos(a));
}
int64_t m_strtoimax(Ctx& x, GuestPtr s, GuestPtr endptr, int32_t base) {
    return int64_t(callIos(x, "_strtoll", {s, endptr, uint32_t(base)}));
}
uint64_t m_strtoumax(Ctx& x, GuestPtr s, GuestPtr endptr, int32_t base) {
    return uint64_t(callIos(x, "_strtoull", {s, endptr, uint32_t(base)}));
}
double m_strtold(Ctx& x, GuestPtr s, GuestPtr endptr) {
    const uint64_t bits = callIos(x, "_strtod", {s, endptr});
    double d = 0.0;
    std::memcpy(&d, &bits, sizeof d);
    return d;
}
double m_wcstold(Ctx& x, GuestPtr s, GuestPtr endptr) {
    std::string ascii;
    for (uint32_t a = s; a && ascii.size() < 256; a += 4) {
        const uint32_t c = x.mem.read<uint32_t>(a);
        if (!c) break;
        ascii.push_back(c < 0x80 ? char(c) : '?');
    }
    char* end = nullptr;
    const double v = std::strtod(ascii.c_str(), &end);
    if (endptr) x.mem.write<uint32_t>(endptr, s + uint32_t(end ? end - ascii.c_str() : 0) * 4);
    return v;
}

uint32_t l_mbCurMax(Ctx&) { return 4; }
uint32_t l_newlocale(Ctx&, int32_t, GuestPtr, uint32_t) { return 1; }
uint32_t l_uselocale(Ctx&, uint32_t loc) { return loc ? loc : 1; }
void l_freelocale(Ctx&, uint32_t) {}
int32_t l_iswalpha(Ctx&, int32_t c, uint32_t) { return std::iswalpha(wint_t(c)) != 0; }
int32_t l_iswblank(Ctx&, int32_t c, uint32_t) { return c == ' ' || c == '\t'; }
int32_t l_iswcntrl(Ctx&, int32_t c, uint32_t) { return std::iswcntrl(wint_t(c)) != 0; }
int32_t l_iswdigit(Ctx&, int32_t c, uint32_t) { return std::iswdigit(wint_t(c)) != 0; }
int32_t l_iswlower(Ctx&, int32_t c, uint32_t) { return std::iswlower(wint_t(c)) != 0; }
int32_t l_iswprint(Ctx&, int32_t c, uint32_t) { return std::iswprint(wint_t(c)) != 0; }
int32_t l_iswpunct(Ctx&, int32_t c, uint32_t) { return std::iswpunct(wint_t(c)) != 0; }
int32_t l_iswspace(Ctx&, int32_t c, uint32_t) { return std::iswspace(wint_t(c)) != 0; }
int32_t l_iswupper(Ctx&, int32_t c, uint32_t) { return std::iswupper(wint_t(c)) != 0; }
int32_t l_iswxdigit(Ctx&, int32_t c, uint32_t) { return std::iswxdigit(wint_t(c)) != 0; }
int32_t l_towlower(Ctx&, int32_t c, uint32_t) { return int32_t(std::towlower(wint_t(c))); }
int32_t l_towupper(Ctx&, int32_t c, uint32_t) { return int32_t(std::towupper(wint_t(c))); }
int32_t l_strcoll(Ctx& x, GuestPtr a, GuestPtr b, uint32_t) { return int32_t(callIos(x, "_strcmp", {a, b})); }
uint32_t l_strxfrm(Ctx& x, GuestPtr d, GuestPtr s, uint32_t n, uint32_t) { return uint32_t(callIos(x, "_strxfrm", {d, s, n})); }
uint32_t l_strftime(Ctx& x, GuestPtr s, uint32_t max, GuestPtr fmt, GuestPtr tm, uint32_t) {
    return uint32_t(callIos(x, "_strftime", {s, max, fmt, tm}));
}
int64_t l_strtoll(Ctx& x, GuestPtr s, GuestPtr endptr, int32_t base, uint32_t) { return m_strtoimax(x, s, endptr, base); }
uint64_t l_strtoull(Ctx& x, GuestPtr s, GuestPtr endptr, int32_t base, uint32_t) { return m_strtoumax(x, s, endptr, base); }
double l_strtold(Ctx& x, GuestPtr s, GuestPtr endptr, uint32_t) { return m_strtold(x, s, endptr); }
int32_t l_wcscoll(Ctx& x, GuestPtr a, GuestPtr b, uint32_t) { return int32_t(callIos(x, "_wcscmp", {a, b})); }
uint32_t l_wcsxfrm(Ctx& x, GuestPtr dst, GuestPtr src, uint32_t n, uint32_t) {
    uint32_t len = 0;
    while (src && x.mem.read<uint32_t>(src + len * 4) != 0) {
        if (dst && len + 1 < n) x.mem.write<uint32_t>(dst + len * 4, x.mem.read<uint32_t>(src + len * 4));
        ++len;
    }
    if (dst && n > 0) x.mem.write<uint32_t>(dst + std::min(len, n - 1) * 4, 0);
    return len;
}
int32_t w_wcwidth(Ctx&, int32_t c) { return c == 0 ? 0 : (c < 32 || c == 127) ? -1 : 1; }
int32_t w_mbsinit(Ctx& x, GuestPtr ps) { return !ps || x.mem.read<uint32_t>(ps) == 0 ? 1 : 0; }
uint32_t w_mbsnrtowcs(Ctx& x, GuestPtr dst, GuestPtr srcPtr, uint32_t nmc, uint32_t len, GuestPtr) {
    if (!srcPtr) return 0;
    GuestPtr s = x.mem.read<uint32_t>(srcPtr);
    if (!s) return 0;
    uint32_t used = 0, out = 0;
    while (used < nmc && (!dst || out < len)) {
        const uint8_t b0 = x.mem.read<uint8_t>(s + used);
        if (b0 == 0) {
            if (dst) { x.mem.write<uint32_t>(dst + out * 4, 0); x.mem.write<uint32_t>(srcPtr, 0); }
            return out;
        }
        uint32_t cp = b0, step = 1;
        if ((b0 & 0xE0) == 0xC0 && used + 1 < nmc) { cp = ((b0 & 0x1F) << 6) | (x.mem.read<uint8_t>(s + used + 1) & 0x3F); step = 2; }
        else if ((b0 & 0xF0) == 0xE0 && used + 2 < nmc) {
            cp = ((b0 & 0x0F) << 12) | ((x.mem.read<uint8_t>(s + used + 1) & 0x3F) << 6) | (x.mem.read<uint8_t>(s + used + 2) & 0x3F);
            step = 3;
        } else if ((b0 & 0xF8) == 0xF0 && used + 3 < nmc) {
            cp = ((b0 & 0x07) << 18) | ((x.mem.read<uint8_t>(s + used + 1) & 0x3F) << 12) |
                 ((x.mem.read<uint8_t>(s + used + 2) & 0x3F) << 6) | (x.mem.read<uint8_t>(s + used + 3) & 0x3F);
            step = 4;
        }
        if (dst) x.mem.write<uint32_t>(dst + out * 4, cp);
        used += step;
        ++out;
    }
    if (dst) x.mem.write<uint32_t>(srcPtr, s + used);
    return out;
}
uint32_t w_mbsrtowcs(Ctx& x, GuestPtr dst, GuestPtr srcPtr, uint32_t len, GuestPtr ps) {
    return w_mbsnrtowcs(x, dst, srcPtr, 0x7FFFFFFFu, len, ps);
}
uint32_t w_wcsnrtombs(Ctx& x, GuestPtr dst, GuestPtr srcPtr, uint32_t nwc, uint32_t len, GuestPtr) {
    if (!srcPtr) return 0;
    GuestPtr s = x.mem.read<uint32_t>(srcPtr);
    if (!s) return 0;
    uint32_t used = 0, out = 0;
    while (used < nwc) {
        const uint32_t cp = x.mem.read<uint32_t>(s + used * 4);
        if (cp == 0) {
            if (dst) {
                if (out < len) { x.mem.write<uint8_t>(dst + out, 0); x.mem.write<uint32_t>(srcPtr, 0); }
                else x.mem.write<uint32_t>(srcPtr, s + used * 4);
            }
            return out;
        }
        uint8_t utf8[4];
        uint32_t n = 0;
        if (cp < 0x80) { utf8[0] = uint8_t(cp); n = 1; }
        else if (cp < 0x800) { utf8[0] = uint8_t(0xC0 | (cp >> 6)); utf8[1] = uint8_t(0x80 | (cp & 0x3F)); n = 2; }
        else if (cp < 0x10000) {
            utf8[0] = uint8_t(0xE0 | (cp >> 12)); utf8[1] = uint8_t(0x80 | ((cp >> 6) & 0x3F)); utf8[2] = uint8_t(0x80 | (cp & 0x3F));
            n = 3;
        } else {
            utf8[0] = uint8_t(0xF0 | (cp >> 18)); utf8[1] = uint8_t(0x80 | ((cp >> 12) & 0x3F));
            utf8[2] = uint8_t(0x80 | ((cp >> 6) & 0x3F)); utf8[3] = uint8_t(0x80 | (cp & 0x3F));
            n = 4;
        }
        if (dst && out + n > len) break;
        if (dst) for (uint32_t i = 0; i < n; ++i) x.mem.write<uint8_t>(dst + out + i, utf8[i]);
        out += n;
        ++used;
    }
    if (dst) x.mem.write<uint32_t>(srcPtr, s + used * 4);
    return out;
}

int32_t f_fdatasync(Ctx& x, int32_t fd) { return ::fsync(fd) < 0 ? failHost<int32_t>(x) : 0; }
int32_t f_fchmodat(Ctx& x, int32_t, GuestPtr path, uint32_t mode, int32_t) {
    return ::chmod(x.proc.hostPath(x.proc.readCString(path)).c_str(), mode & 07777) < 0 ? failHost<int32_t>(x) : 0;
}
int32_t f_fstatfs(Ctx& x, int32_t fd, GuestPtr buf) {
    if (fd < 0 || !buf) { setLinuxErrno(x, 9); return -1; }
    std::memset(x.mem.host(buf), 0, 88);
    x.mem.write<uint32_t>(buf + 0, 0xEF53);      // EXT4_SUPER_MAGIC
    x.mem.write<uint32_t>(buf + 4, 4096);        // f_bsize
    x.mem.write<uint64_t>(buf + 8, 1048576);     // f_blocks
    x.mem.write<uint64_t>(buf + 16, 524288);     // f_bfree
    x.mem.write<uint64_t>(buf + 24, 524288);     // f_bavail
    x.mem.write<uint64_t>(buf + 32, 262144);     // f_files
    x.mem.write<uint64_t>(buf + 40, 131072);     // f_ffree
    x.mem.write<uint32_t>(buf + 56, 255);        // f_namelen
    x.mem.write<uint32_t>(buf + 60, 4096);       // f_frsize
    return 0;
}
int32_t f_link(Ctx& x, GuestPtr oldp, GuestPtr newp) {
    return ::link(x.proc.hostPath(x.proc.readCString(oldp)).c_str(),
                  x.proc.hostPath(x.proc.readCString(newp)).c_str()) < 0 ? failHost<int32_t>(x) : 0;
}
int32_t f_pathconf(Ctx&, GuestPtr, int32_t name) { return name == 3 ? 255 : name == 4 ? 4096 : 1; }
int32_t f_utimensat(Ctx&, int32_t, GuestPtr, GuestPtr, int32_t) { return 0; }
int32_t f_inotifyAddWatch(Ctx& x, int32_t, GuestPtr, uint32_t) { setLinuxErrno(x, 9); return -1; }

uint32_t z_gzdopen(Ctx&, int32_t, GuestPtr) { return 0; }
int32_t z_gzgetc(Ctx& x, uint32_t h) {
    static const char key = 0;
    uint32_t& slot = x.proc.threadSlot(&key);
    if (!slot) slot = x.proc.allocStatic(4);
    return int32_t(callIos(x, "_gzread", {h, slot, 1})) == 1 ? int32_t(x.mem.read<uint8_t>(slot)) : -1;
}
int32_t z_gzputc(Ctx& x, uint32_t h, int32_t c) {
    static const char key = 0;
    uint32_t& slot = x.proc.threadSlot(&key);
    if (!slot) slot = x.proc.allocStatic(4);
    x.mem.write<uint8_t>(slot, uint8_t(c));
    return int32_t(callIos(x, "_gzwrite", {h, slot, 1})) == 1 ? (c & 0xFF) : -1;
}
GuestPtr z_gzerror(Ctx& x, uint32_t, GuestPtr errnum) {
    if (errnum) x.mem.write<int32_t>(errnum, 0);
    return x.proc.internCString("");
}
GuestPtr z_zError(Ctx& x, int32_t err) { return x.proc.internCString(err == 0 ? "ok" : "zlib error"); }

int32_t p_ok1(Ctx&, uint32_t) { return 0; }
int32_t p_ok4(Ctx&, uint32_t, uint32_t, uint32_t, uint32_t) { return 0; }
void p_libcInit(Ctx&, GuestPtr, GuestPtr, GuestPtr, GuestPtr) {}
int32_t p_getentropy(Ctx& x, GuestPtr buf, uint32_t len) {
    if (!buf || len > 256) { setLinuxErrno(x, 5); return -1; }   // EIO
    static std::mutex mu;
    static std::mt19937_64 rng{std::random_device{}()};
    std::lock_guard<std::mutex> lk(mu);
    uint8_t* dst = x.mem.host(buf);
    for (uint32_t i = 0; i < len; ++i) dst[i] = uint8_t(rng() >> ((i & 7) * 8));
    if (len > 0 && dst[0] == 0) dst[0] = 1;
    return 0;
}

// Bionic <fenv.h> on 32-bit ARM: fenv_t and fexcept_t are 4-byte uint32_t (unlike Darwin's
// 16-byte fenv_t and 2-byte fexcept_t), and FE_DFL_ENV is ((const fenv_t*)-1).
constexpr uint32_t kFeFlags = 0x9F;
int32_t m_fegetenv(Ctx& x, GuestPtr env) {
    if (!env) return -1;
    x.mem.write<uint32_t>(env, x.cpu.fpscr);
    return 0;
}
int32_t m_fesetenv(Ctx& x, GuestPtr env) {
    x.cpu.fpscr = (!env || env == 0xFFFFFFFFu) ? 0u : x.mem.read<uint32_t>(env);
    return 0;
}
int32_t m_feholdexcept(Ctx& x, GuestPtr env) {
    if (!env) return -1;
    x.mem.write<uint32_t>(env, x.cpu.fpscr);
    x.cpu.fpscr &= ~kFeFlags;
    return 0;
}
int32_t m_feupdateenv(Ctx& x, GuestPtr env) {
    const uint32_t raised = x.cpu.fpscr & kFeFlags;
    m_fesetenv(x, env);
    x.cpu.fpscr |= raised;
    return 0;
}
int32_t m_fegetexceptflag(Ctx& x, GuestPtr out, int32_t mask) {
    if (!out) return -1;
    x.mem.write<uint32_t>(out, x.cpu.fpscr & uint32_t(mask) & kFeFlags);
    return 0;
}
int32_t m_fesetexceptflag(Ctx& x, GuestPtr in, int32_t mask) {
    if (!in) return -1;
    const uint32_t m = uint32_t(mask) & kFeFlags;
    x.cpu.fpscr = (x.cpu.fpscr & ~m) | (x.mem.read<uint32_t>(in) & m);
    return 0;
}

} // namespace

void registerBionicExtra(Process& p) {
#define R(F, NAME, WHAT) supportDef<&F>(p, std::string("android:") + NAME, SupportStatus::Real, WHAT)
#define A(F, NAME, WHAT) supportDef<&F>(p, std::string("android:") + NAME, SupportStatus::Approx, WHAT)
    for (const char* n : {"__aeabi_memcpy", "__aeabi_memcpy4", "__aeabi_memcpy8", "__aeabi_memmove", "__aeabi_memmove4", "__aeabi_memmove8"})
        R(a_memcpy, n, "ARM EABI copy (overlap-safe)");
    for (const char* n : {"__aeabi_memset", "__aeabi_memset4", "__aeabi_memset8"}) R(a_memset, n, "ARM EABI memset (dest, n, c)");
    for (const char* n : {"__aeabi_memclr", "__aeabi_memclr4", "__aeabi_memclr8"}) R(a_memclr, n, "ARM EABI clear");
    R(s_memmem, "memmem", "byte search");
    R(s_memrchr, "memrchr", "reverse byte search");
    R(s_memalign, "memalign", "aligned guest heap block");
    R(s_usableSize, "dlmalloc_usable_size", "the heap block's size");
    R(s_usableSize, "malloc_usable_size", "the heap block's size");
    R(s_assert, "__assert", "stops the game with the assertion");
    R(s_assert2, "__assert2", "stops the game with the assertion");
    R(m_isfinite, "__isfinite", "host isfinite");
    R(m_isfinitef, "__isfinitef", "host isfinite");
    R(m_isnan, "isnan", "host isnan");
    R(m_isnan, "__isnan", "host isnan");
    R(m_signbit, "__signbit", "host signbit");
    R(m_nextafterf, "nextafterf", "host nextafter");
    R(m_scalbnl, "scalbnl", "host scalbn (long double is double)");
    R(m_remquo, "remquo", "host remquo");
    R(m_sincosf, "sincosf", "host sin and cos (float)");
    R(m_sincos, "sincos", "host sin and cos (double)");
    R(m_fegetenv, "fegetenv", "4-byte Bionic fenv_t FPSCR save");
    R(m_fesetenv, "fesetenv", "4-byte Bionic fenv_t FPSCR restore (FE_DFL_ENV=-1)");
    R(m_feholdexcept, "feholdexcept", "4-byte Bionic fenv_t FPSCR save and clear flags");
    R(m_feupdateenv, "feupdateenv", "4-byte Bionic fenv_t FPSCR restore and re-raise flags");
    R(m_fegetexceptflag, "fegetexceptflag", "4-byte Bionic fexcept_t flags");
    R(m_fesetexceptflag, "fesetexceptflag", "4-byte Bionic fexcept_t flags");
    R(m_strtoimax, "strtoimax", "64-bit signed integer parse");
    R(m_strtoumax, "strtoumax", "64-bit unsigned integer parse");
    R(m_strtold, "strtold", "double parse (long double is double on ARM)");
    R(m_wcstold, "wcstold", "wide string double parse");
    R(c_strlenChk, "__strlen_chk", "FORTIFY strlen");
    R(c_strchrChk, "__strchr_chk", "FORTIFY strchr");
    R(c_strrchrChk, "__strrchr_chk", "FORTIFY strrchr");
    R(c_strncpyChk2, "__strncpy_chk2", "FORTIFY strncpy");
    R(c_fdSetChk, "__FD_SET_chk", "FORTIFY FD_SET");
    R(c_fdIssetChk, "__FD_ISSET_chk", "FORTIFY FD_ISSET");
    R(c_readChk, "__read_chk", "FORTIFY read");
    R(c_writeChk, "__write_chk", "FORTIFY write");
    R(c_freadChk, "__fread_chk", "FORTIFY fread");
    R(c_fwriteChk, "__fwrite_chk", "FORTIFY fwrite");
    R(c_pollChk, "__poll_chk", "FORTIFY poll");
    R(w_isblank, "isblank", "space or tab");
    R(w_iswcntrl, "iswcntrl", "host classification");
    R(w_iswlower, "iswlower", "host classification");
    R(w_iswupper, "iswupper", "host classification");
    R(w_iswprint, "iswprint", "host classification");
    R(w_iswpunct, "iswpunct", "host classification");
    R(w_iswxdigit, "iswxdigit", "host classification");
    R(w_wctype, "wctype", "class names to indices");
    R(w_iswctype, "iswctype", "host classification by class");
    R(w_getwc, "getwc", "UTF-8 through fgetc");
    R(w_getwc, "fgetwc", "UTF-8 through fgetc");
    R(w_putwc, "putwc", "UTF-8 through fputc");
    R(w_putwc, "fputwc", "UTF-8 through fputc");
    A(w_ungetwc, "ungetwc", "ASCII only");
    R(w_wcsftime, "wcsftime", "strftime, widened");
    R(w_wcwidth, "wcwidth", "display column width");
    R(w_mbsinit, "mbsinit", "multibyte conversion state check");
    R(w_mbsrtowcs, "mbsrtowcs", "UTF-8 to 32-bit wchar_t conversion");
    R(w_mbsnrtowcs, "mbsnrtowcs", "bounded UTF-8 to 32-bit wchar_t conversion");
    R(w_wcsnrtombs, "wcsnrtombs", "bounded 32-bit wchar_t to UTF-8 conversion");
    R(l_mbCurMax, "__ctype_get_mb_cur_max", "UTF-8 max 4 bytes per character");
    A(l_newlocale, "newlocale", "C.UTF-8 locale handle");
    A(l_uselocale, "uselocale", "C.UTF-8 locale handle");
    A(l_freelocale, "freelocale", "C.UTF-8 locale handle");
    R(l_iswalpha, "iswalpha_l", "host classification");
    R(l_iswblank, "iswblank_l", "space or tab");
    R(l_iswcntrl, "iswcntrl_l", "host classification");
    R(l_iswdigit, "iswdigit_l", "host classification");
    R(l_iswlower, "iswlower_l", "host classification");
    R(l_iswprint, "iswprint_l", "host classification");
    R(l_iswpunct, "iswpunct_l", "host classification");
    R(l_iswspace, "iswspace_l", "host classification");
    R(l_iswupper, "iswupper_l", "host classification");
    R(l_iswxdigit, "iswxdigit_l", "host classification");
    R(l_towlower, "towlower_l", "host towlower");
    R(l_towupper, "towupper_l", "host towupper");
    R(l_strcoll, "strcoll_l", "bytewise comparison");
    R(l_strxfrm, "strxfrm_l", "strxfrm");
    R(l_strftime, "strftime_l", "strftime");
    R(l_strtoll, "strtoll_l", "strtoll");
    R(l_strtoull, "strtoull_l", "strtoull");
    R(l_strtold, "strtold_l", "strtold");
    R(l_wcscoll, "wcscoll_l", "wide string comparison");
    R(l_wcsxfrm, "wcsxfrm_l", "wide string copy");
    R(f_fgetpos, "fgetpos", "ftell (32-bit fpos_t)");
    R(f_fsetpos, "fsetpos", "fseek (32-bit fpos_t)");
    R(f_fchmod, "fchmod", "host fchmod");
    R(f_fchmodat, "fchmodat", "host chmod");
    A(f_fchown, "fchown", "one user: accepted");
    R(f_fdatasync, "fdatasync", "host fsync");
    A(f_fstatfs, "fstatfs", "fixed ext4 filesystem stats");
    R(f_link, "link", "host link");
    A(f_pathconf, "pathconf", "NAME_MAX=255, PATH_MAX=4096");
    A(f_utimensat, "utimensat", "accepted");
    A(p_enosys, "inotify_init", "no inotify: ENOSYS");
    A(f_inotifyAddWatch, "inotify_add_watch", "no inotify: EBADF");
    R(f_truncate, "truncate", "host truncate");
    A(f_umask, "umask", "fixed 022");
    A(f_rewinddir, "rewinddir", "not rewound");
    A(z_gzdopen, "gzdopen", "not supported on raw fd: null");
    R(z_gzgetc, "gzgetc", "one byte through gzread");
    R(z_gzputc, "gzputc", "one byte through gzwrite");
    A(z_gzerror, "gzerror", "Z_OK");
    A(z_zError, "zError", "error string");
    R(p_getauxval, "getauxval", "ARMv7 NEON/VFPv3 hardware caps, no ARMv8 crypto");
    R(p_gettid, "gettid", "guest thread id");
    A(p_getresuid, "getresuid", "one app user");
    A(p_ok1, "setuid", "one app user: accepted");
    A(p_ok3, "setresuid", "accepted");
    A(p_ok3, "pthread_atfork", "no fork: accepted");
    A(p_ok4, "__register_atfork", "no fork: accepted");
    A(p_ok3, "__cxa_thread_atexit_impl", "accepted");
    A(p_libcInit, "__libc_init", "executable entry stub");
    R(p_getentropy, "getentropy", "host random bytes up to 256, EIO above");
    A(p_eperm, "ptrace", "not traced: EPERM");
    A(p_eacces, "execve", "no child processes: EACCES");
    A(p_eacces, "execvp", "no child processes: EACCES");
    A(p_eacces, "execl", "no child processes: EACCES");
    A(p_epollCreate, "epoll_create", "no epoll: ENOSYS");
    A(p_epollCtl, "epoll_ctl", "no epoll: EBADF");
    A(p_epollWait, "epoll_wait", "no epoll: EBADF");
    A(p_mremap, "mremap", "ENOMEM (callers fall back to a new mapping)");
    A(p_ok3, "setitimer", "accepted, no timer signal");
    A(p_clockGetres, "clock_getres", "1 ns");
    A(p_null, "getgrnam", "no groups");
    A(p_null, "getgrgid", "no groups");
    A(p_getnameinfo, "getnameinfo", "no Android sockets yet: EAI_NONAME");
    R(p_propertyGet, "__system_property_get", "a fixed Android 4.4 device");
    R(p_condTimedwaitMonotonic, "pthread_cond_timedwait_monotonic_np", "monotonic deadline to realtime");
    R(p_condTimedwaitMonotonic, "pthread_cond_timedwait_monotonic", "monotonic deadline to realtime");
    R(p_cleanupPush, "__pthread_cleanup_push", "per-thread handler list");
    R(p_cleanupPop, "__pthread_cleanup_pop", "per-thread handler list");
    A(x_beginCleanup, "__cxa_begin_cleanup", "cleanup always proceeds");
    A(x_typeMatch, "__cxa_type_match", "foreign exceptions match no handler");
    R(x_findExidx, "dl_unwind_find_exidx", "the loaded library's exception index");
    R(h_sha1Init, "SHA1Init", "host SHA-1");
    R(h_sha1Update, "SHA1Update", "host SHA-1");
    R(h_sha1Final, "SHA1Final", "host SHA-1");
    A(p_noop, "__google_potentially_blocking_region_begin", "no-op");
    A(p_noop, "__google_potentially_blocking_region_end", "no-op");
#undef R
#undef A
    // Data.
    {
        p.defineDataBlock("android:environ", 4);   // filled in at load, once the environment exists
        p.defineDataBlock("android:__data_start", 4);
        p.defineDataBlock("android:data_start", 4);
        const uint32_t herr = p.defineDataBlock("android:hvm_h_errno", 4);
        p.defineFunction("android:__get_h_errno", [herr](Ctx& x) { x.cpu.r[0] = herr; });
    }
}

} // namespace Hydrogen::Runtime::Android
