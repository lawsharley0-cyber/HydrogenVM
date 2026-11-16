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
#include <map>
#include <mutex>
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
int32_t p_tkill(Ctx& x, int32_t tid, int32_t sig) {
    if (sig == 0) return 0;
    if (tid == p_gettid(x)) return int32_t(callIos(x, "_raise", {uint32_t(sig)}));
    x.proc.reportDefaulted("tkill of another thread (signal " + std::to_string(sig) + ") ignored");
    return 0;
}
int32_t p_sigsuspend(Ctx& x, GuestPtr) {
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    if (x.proc.stopping()) throw ProcessExit(0, "stopping");
    setLinuxErrno(x, 4);   // EINTR
    return -1;
}
uint32_t p_bsdSignal(Ctx& x, int32_t sig, GuestPtr handler) { return uint32_t(callIos(x, "_signal", {uint32_t(sig), handler})); }
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
    const int64_t real = duration_cast<nanoseconds>(system_clock::now().time_since_epoch()).count() + (due - mono);
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
    R(f_fgetpos, "fgetpos", "ftell (32-bit fpos_t)");
    R(f_fsetpos, "fsetpos", "fseek (32-bit fpos_t)");
    R(f_fchmod, "fchmod", "host fchmod");
    A(f_fchown, "fchown", "one user: accepted");
    R(f_truncate, "truncate", "host truncate");
    A(f_umask, "umask", "fixed 022");
    A(f_rewinddir, "rewinddir", "not rewound");
    R(p_getauxval, "getauxval", "ARMv7 NEON/VFPv3 hardware caps, no ARMv8 crypto");
    R(p_gettid, "gettid", "guest thread id");
    A(p_getresuid, "getresuid", "one app user");
    A(p_ok3, "setresuid", "accepted");
    A(p_eperm, "ptrace", "not traced: EPERM");
    A(p_eacces, "execve", "no child processes: EACCES");
    A(p_eacces, "execl", "no child processes: EACCES");
    A(p_epollCreate, "epoll_create", "no epoll: ENOSYS");
    A(p_epollCtl, "epoll_ctl", "no epoll: EBADF");
    A(p_epollWait, "epoll_wait", "no epoll: EBADF");
    A(p_mremap, "mremap", "ENOMEM (callers fall back to a new mapping)");
    A(p_tkill, "tkill", "own thread: raise; others ignored");
    A(p_sigsuspend, "sigsuspend", "short sleep, then EINTR");
    A(p_ok3, "setitimer", "accepted, no timer signal");
    R(p_bsdSignal, "bsd_signal", "signal");
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
