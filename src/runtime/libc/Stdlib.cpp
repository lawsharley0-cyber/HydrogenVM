// HydrogenVM — <stdlib.h>: heap, conversions, sorting, random numbers,
// environment, process exit.
#include "runtime/Process.hpp"
#include <cerrno>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <random>
#include <fcntl.h>
#include <unistd.h>

namespace Hydrogen::Runtime {

void flushGuestStdio(Process& p);   // Stdio.cpp

namespace {

inline void setErrno(Ctx& x, int e) { x.mem.write<uint32_t>(x.proc.currentThread().errnoAddr, uint32_t(e)); }

// ------------------------------------------------------------------ heap ---

uint32_t m_malloc(Ctx& x, uint32_t n) { const uint32_t a = x.proc.heap().alloc(n); if (!a) setErrno(x, ENOMEM); return a; }
uint32_t m_calloc(Ctx& x, uint32_t n, uint32_t s) {
    const uint64_t total = uint64_t(n) * s;
    if (total > 0x7FFFFFFF) { setErrno(x, ENOMEM); return 0; }
    return x.proc.heap().allocZeroed(uint32_t(total));
}
uint32_t m_realloc(Ctx& x, uint32_t p, uint32_t n) { return x.proc.heap().realloc(p, n); }
uint32_t m_reallocf(Ctx& x, uint32_t p, uint32_t n) { const uint32_t r = x.proc.heap().realloc(p, n); if (!r && n) x.proc.heap().free(p); return r; }
void     m_free(Ctx& x, uint32_t p) { x.proc.heap().free(p); }
uint32_t m_valloc(Ctx& x, uint32_t n) { return x.proc.heap().allocAligned(n, 4096); }
int32_t  m_posix_memalign(Ctx& x, GuestPtr out, uint32_t align, uint32_t n) {
    const uint32_t a = x.proc.heap().allocAligned(n, align);
    if (!a) return ENOMEM;
    x.mem.write<uint32_t>(out, a);
    return 0;
}
uint32_t m_malloc_size(Ctx& x, uint32_t p) { return x.proc.heap().blockSize(p); }
uint32_t m_malloc_good_size(Ctx&, uint32_t n) { return (n + 15) & ~15u; }
uint32_t m_zone_size(Ctx& x, uint32_t, uint32_t p) { return x.proc.heap().blockSize(p); }
uint32_t m_zone_malloc(Ctx& x, uint32_t, uint32_t n) { return x.proc.heap().alloc(n); }
uint32_t m_zone_calloc(Ctx& x, uint32_t, uint32_t n, uint32_t s) { return m_calloc(x, n, s); }
uint32_t m_zone_realloc(Ctx& x, uint32_t, uint32_t p, uint32_t n) { return x.proc.heap().realloc(p, n); }
void     m_zone_free(Ctx& x, uint32_t, uint32_t p) { x.proc.heap().free(p); }
void     m_zone_free_definite(Ctx& x, uint32_t, uint32_t p, uint32_t) { x.proc.heap().free(p); }
uint32_t m_zone_memalign(Ctx& x, uint32_t, uint32_t align, uint32_t n) { return x.proc.heap().allocAligned(n, align); }
uint32_t m_default_zone(Ctx& x);
uint32_t m_zone_from_ptr(Ctx& x, uint32_t p) { return x.proc.heap().blockSize(p) ? m_default_zone(x) : 0; }
// Custom zones all map onto the single guest heap.
uint32_t m_create_zone(Ctx& x, uint32_t, uint32_t) { return m_default_zone(x); }
void m_destroy_zone(Ctx&, uint32_t) {}
void m_set_zone_name(Ctx&, uint32_t, GuestPtr) {}
uint32_t m_zone_name(Ctx& x, uint32_t) { static const char slotKey = 0; uint32_t& s = x.proc.processSlot(&slotKey); if (!s) s = x.proc.allocCString("DefaultMallocZone"); return s; }
uint32_t m_zone_valloc(Ctx& x, uint32_t, uint32_t n) { return x.proc.heap().allocAligned(n, 4096); }
uint32_t m_zone_pressure_relief(Ctx&, uint32_t, uint32_t) { return 0; }
void m_zone_statistics(Ctx& x, uint32_t, GuestPtr stats) { if (stats) for (uint32_t i = 0; i < 20; i += 4) x.mem.write<uint32_t>(stats + i, 0); }
void m_zone_log(Ctx&, uint32_t, uint32_t) {}
uint32_t m_zone_batch_malloc(Ctx& x, uint32_t, uint32_t n, GuestPtr results, uint32_t count) {
    uint32_t i = 0;
    for (; i < count; ++i) { const uint32_t a = x.proc.heap().alloc(n); if (!a) break; x.mem.write<uint32_t>(results + i * 4, a); }
    return i;
}
void m_zone_batch_free(Ctx& x, uint32_t, GuestPtr ptrs, uint32_t count) {
    for (uint32_t i = 0; i < count; ++i) { const uint32_t p = x.mem.read<uint32_t>(ptrs + i * 4); if (p) x.proc.heap().free(p); }
}
uint32_t m_default_zone(Ctx& x) {
    static const char zoneKey = 0;
    uint32_t& zone = x.proc.processSlot(&zoneKey);
    if (!zone) {
        zone = x.proc.allocStatic(80);
        const uint32_t intro = x.proc.allocStatic(64);
        auto fn = [&](const char* sym) { return x.proc.lookupSymbol(sym).value_or(0); };
        x.mem.write<uint32_t>(zone + 8,  fn("__hvm_zone_size"));
        x.mem.write<uint32_t>(zone + 12, fn("_malloc_zone_malloc"));
        x.mem.write<uint32_t>(zone + 16, fn("_malloc_zone_calloc"));
        x.mem.write<uint32_t>(zone + 20, fn("_malloc_zone_valloc"));
        x.mem.write<uint32_t>(zone + 24, fn("_malloc_zone_free"));
        x.mem.write<uint32_t>(zone + 28, fn("_malloc_zone_realloc"));
        x.mem.write<uint32_t>(zone + 32, fn("_malloc_destroy_zone"));
        x.mem.write<uint32_t>(zone + 36, m_zone_name(x, zone));
        x.mem.write<uint32_t>(zone + 40, fn("_malloc_zone_batch_malloc"));
        x.mem.write<uint32_t>(zone + 44, fn("_malloc_zone_batch_free"));
        x.mem.write<uint32_t>(zone + 48, intro);
        x.mem.write<uint32_t>(zone + 52, 8);
        x.mem.write<uint32_t>(zone + 56, fn("_malloc_zone_memalign"));
        x.mem.write<uint32_t>(zone + 60, fn("__hvm_zone_free_definite"));
        x.mem.write<uint32_t>(zone + 64, fn("_malloc_zone_pressure_relief"));
    }
    return zone;
}

// ----------------------------------------------------------- conversions ---

template <typename T, typename F>
T convert(Ctx& x, GuestPtr s, GuestPtr endp, F fn) {
    const std::string str = x.proc.readCString(s, 4096);
    char* end = nullptr;
    errno = 0;
    T v = fn(str.c_str(), &end);
    if (errno) setErrno(x, errno);
    if (endp) x.mem.write<uint32_t>(endp, s + uint32_t(end - str.c_str()));
    return v;
}
int32_t c_atoi(Ctx& x, GuestPtr s) { return int32_t(std::strtol(x.proc.readCString(s, 4096).c_str(), nullptr, 10)); }
int64_t c_atoll(Ctx& x, GuestPtr s) { return std::strtoll(x.proc.readCString(s, 4096).c_str(), nullptr, 10); }
double  c_atof(Ctx& x, GuestPtr s) { return std::strtod(x.proc.readCString(s, 4096).c_str(), nullptr); }
int32_t c_strtol(Ctx& x, GuestPtr s, GuestPtr e, int32_t base) {
    return convert<int32_t>(x, s, e, [&](const char* a, char** b) {
        long long v = std::strtoll(a, b, base);
        if (v > INT32_MAX) { errno = ERANGE; return INT32_MAX; }
        if (v < INT32_MIN) { errno = ERANGE; return INT32_MIN; }
        return int32_t(v);
    });
}
uint32_t c_strtoul(Ctx& x, GuestPtr s, GuestPtr e, int32_t base) {
    return convert<uint32_t>(x, s, e, [&](const char* a, char** b) {
        const char* p = a; while (std::isspace(uint8_t(*p))) ++p;
        const bool neg = *p == '-';
        unsigned long long v = std::strtoull(a, b, base);
        if (!neg && v > UINT32_MAX) { errno = ERANGE; return UINT32_MAX; }
        return uint32_t(v);
    });
}
int64_t c_strtoll(Ctx& x, GuestPtr s, GuestPtr e, int32_t base) { return convert<int64_t>(x, s, e, [&](const char* a, char** b) { return int64_t(std::strtoll(a, b, base)); }); }
uint64_t c_strtoull(Ctx& x, GuestPtr s, GuestPtr e, int32_t base) { return convert<uint64_t>(x, s, e, [&](const char* a, char** b) { return uint64_t(std::strtoull(a, b, base)); }); }
double c_strtod(Ctx& x, GuestPtr s, GuestPtr e) { return convert<double>(x, s, e, [](const char* a, char** b) { return std::strtod(a, b); }); }
float  c_strtof(Ctx& x, GuestPtr s, GuestPtr e) { return convert<float>(x, s, e, [](const char* a, char** b) { return std::strtof(a, b); }); }

template <typename T, typename F>
T convertW(Ctx& x, GuestPtr s, GuestPtr endp, F fn) {
    std::string str;
    if (s) {
        for (size_t i = 0; i < 4096; ++i) {
            const uint32_t wc = x.mem.read<uint32_t>(s + uint32_t(i) * 4);
            if (!wc) break;
            str.push_back(wc < 0x80 ? char(wc) : '?');
        }
    }
    char* end = nullptr;
    errno = 0;
    T v = fn(str.c_str(), &end);
    if (errno) setErrno(x, errno);
    if (endp) x.mem.write<uint32_t>(endp, s + uint32_t(end - str.c_str()) * 4);
    return v;
}
int32_t  c_wcstol(Ctx& x, GuestPtr s, GuestPtr e, int32_t base) { return convertW<int32_t>(x, s, e, [&](const char* a, char** b) { return int32_t(std::strtoll(a, b, base)); }); }
uint32_t c_wcstoul(Ctx& x, GuestPtr s, GuestPtr e, int32_t base) { return convertW<uint32_t>(x, s, e, [&](const char* a, char** b) { return uint32_t(std::strtoull(a, b, base)); }); }
int64_t  c_wcstoll(Ctx& x, GuestPtr s, GuestPtr e, int32_t base) { return convertW<int64_t>(x, s, e, [&](const char* a, char** b) { return int64_t(std::strtoll(a, b, base)); }); }
uint64_t c_wcstoull(Ctx& x, GuestPtr s, GuestPtr e, int32_t base) { return convertW<uint64_t>(x, s, e, [&](const char* a, char** b) { return uint64_t(std::strtoull(a, b, base)); }); }
double   c_wcstod(Ctx& x, GuestPtr s, GuestPtr e) { return convertW<double>(x, s, e, [](const char* a, char** b) { return std::strtod(a, b); }); }
float    c_wcstof(Ctx& x, GuestPtr s, GuestPtr e) { return convertW<float>(x, s, e, [](const char* a, char** b) { return std::strtof(a, b); }); }

int32_t c_abs(Ctx&, int32_t v) { return v < 0 ? -v : v; }
int64_t c_llabs(Ctx&, int64_t v) { return v < 0 ? -v : v; }
// div_t / ldiv_t are 8 bytes, so they're returned through a hidden pointer.
void c_div(Ctx& x, GuestPtr out, int32_t n, int32_t d) {
    if (!d) { x.mem.write<uint32_t>(out, 0); x.mem.write<uint32_t>(out + 4, 0); return; }
    x.mem.write<int32_t>(out, n / d);
    x.mem.write<int32_t>(out + 4, n % d);
}

// --------------------------------------------------------------- sorting ---

void c_qsort(Ctx& x, GuestPtr base, uint32_t n, uint32_t size, GuestPtr cmp) {
    if (n < 2 || !size) return;
    std::vector<uint32_t> idx(n), tmp(n);
    for (uint32_t i = 0; i < n; ++i) idx[i] = i;
    auto less = [&](uint32_t a, uint32_t b) {
        return int32_t(x.proc.callGuest(x.cpu, cmp, {base + a * size, base + b * size})) > 0;
    };
    // Bottom-up merge sort: never assumes a consistent comparator, never
    // touches the array until the end.
    for (uint32_t w = 1; w < n; w *= 2) {
        for (uint32_t lo = 0; lo < n; lo += 2 * w) {
            const uint32_t mid = std::min(lo + w, n), hi = std::min(lo + 2 * w, n);
            uint32_t i = lo, j = mid, k = lo;
            while (i < mid && j < hi) tmp[k++] = less(idx[i], idx[j]) ? idx[j++] : idx[i++];
            while (i < mid) tmp[k++] = idx[i++];
            while (j < hi) tmp[k++] = idx[j++];
        }
        idx.swap(tmp);
    }
    std::vector<uint8_t> copy(size_t(n) * size);
    std::memcpy(copy.data(), x.mem.host(base), copy.size());
    for (uint32_t i = 0; i < n; ++i) std::memcpy(x.mem.host(base + i * size), copy.data() + size_t(idx[i]) * size, size);
}
void c_qsort_r(Ctx& x, GuestPtr base, uint32_t n, uint32_t size, GuestPtr thunk, GuestPtr cmp) {
    if (n < 2 || !size) return;
    std::vector<uint32_t> idx(n), tmp(n);
    for (uint32_t i = 0; i < n; ++i) idx[i] = i;
    auto less = [&](uint32_t a, uint32_t b) {
        return int32_t(x.proc.callGuest(x.cpu, cmp, {thunk, base + a * size, base + b * size})) > 0;
    };
    for (uint32_t w = 1; w < n; w *= 2) {
        for (uint32_t lo = 0; lo < n; lo += 2 * w) {
            const uint32_t mid = std::min(lo + w, n), hi = std::min(lo + 2 * w, n);
            uint32_t i = lo, j = mid, k = lo;
            while (i < mid && j < hi) tmp[k++] = less(idx[i], idx[j]) ? idx[j++] : idx[i++];
            while (i < mid) tmp[k++] = idx[i++];
            while (j < hi) tmp[k++] = idx[j++];
        }
        idx.swap(tmp);
    }
    std::vector<uint8_t> copy(size_t(n) * size);
    std::memcpy(copy.data(), x.mem.host(base), copy.size());
    for (uint32_t i = 0; i < n; ++i) std::memcpy(x.mem.host(base + i * size), copy.data() + size_t(idx[i]) * size, size);
}
uint32_t c_bsearch(Ctx& x, GuestPtr key, GuestPtr base, uint32_t n, uint32_t size, GuestPtr cmp) {
    uint32_t lo = 0, hi = n;
    while (lo < hi) {
        const uint32_t mid = lo + (hi - lo) / 2;
        const int32_t r = int32_t(x.proc.callGuest(x.cpu, cmp, {key, base + mid * size}));
        if (r == 0) return base + mid * size;
        if (r < 0) hi = mid; else lo = mid + 1;
    }
    return 0;
}

// ---------------------------------------------------------------- random ---

struct RandState {
    explicit RandState(Process&) {}
    std::mutex mu;
    uint32_t next = 1;            // rand()
    std::mt19937 random{1};       // random()
    std::mt19937 arc{std::random_device{}()};
    double drand = 0;
};
// Darwin/FreeBSD rand(): Park–Miller "minimal standard" generator.
int32_t r_rand(Ctx& x) {
    auto& s = x.proc.service<RandState>();
    std::lock_guard<std::mutex> lk(s.mu);
    long ctx = long(s.next);
    if (ctx == 0) ctx = 123459876;
    const long hi = ctx / 127773, lo = ctx % 127773;
    long v = 16807 * lo - 2836 * hi;
    if (v < 0) v += 0x7FFFFFFF;
    s.next = uint32_t(v);
    return int32_t(uint32_t(v) % (uint32_t(0x7FFFFFFF) + 1));
}
void r_srand(Ctx& x, uint32_t seed) { auto& s = x.proc.service<RandState>(); std::lock_guard<std::mutex> lk(s.mu); s.next = seed; }
int32_t r_random(Ctx& x) { auto& s = x.proc.service<RandState>(); std::lock_guard<std::mutex> lk(s.mu); return int32_t(s.random() & 0x7FFFFFFF); }
void r_srandom(Ctx& x, uint32_t seed) { auto& s = x.proc.service<RandState>(); std::lock_guard<std::mutex> lk(s.mu); s.random.seed(seed); }
uint32_t r_arc4random(Ctx& x) { auto& s = x.proc.service<RandState>(); std::lock_guard<std::mutex> lk(s.mu); return uint32_t(s.arc()); }
uint32_t r_arc4random_uniform(Ctx& x, uint32_t upper) {
    if (upper < 2) return 0;
    auto& s = x.proc.service<RandState>();
    std::lock_guard<std::mutex> lk(s.mu);
    return std::uniform_int_distribution<uint32_t>(0, upper - 1)(s.arc);
}
void r_arc4random_buf(Ctx& x, GuestPtr buf, uint32_t n) {
    auto& s = x.proc.service<RandState>();
    std::lock_guard<std::mutex> lk(s.mu);
    for (uint32_t i = 0; i < n; ++i) x.mem.write<uint8_t>(buf + i, uint8_t(s.arc()));
}
double r_drand48(Ctx& x) { auto& s = x.proc.service<RandState>(); std::lock_guard<std::mutex> lk(s.mu); return std::uniform_real_distribution<double>(0, 1)(s.random); }
void r_srand48(Ctx& x, int32_t seed) { r_srandom(x, uint32_t(seed)); }
int32_t r_lrand48(Ctx& x) { return r_random(x); }

// ----------------------------------------------------------- environment ---

uint32_t e_getenv(Ctx& x, GuestPtr name) { return x.proc.getenvAddr(x.proc.readCString(name)); }
int32_t e_setenv(Ctx& x, GuestPtr name, GuestPtr value, int32_t overwrite) {
    const std::string n = x.proc.readCString(name);
    if (!overwrite && x.proc.getenvAddr(n)) return 0;
    x.proc.setenv(n, x.proc.readCString(value));
    return 0;
}
int32_t e_unsetenv(Ctx& x, GuestPtr name) { x.proc.setenv(x.proc.readCString(name), ""); return 0; }
int32_t e_putenv(Ctx& x, GuestPtr s) {
    const std::string kv = x.proc.readCString(s);
    const size_t eq = kv.find('=');
    if (eq != std::string::npos) x.proc.setenv(kv.substr(0, eq), kv.substr(eq + 1));
    return 0;
}
uint32_t e_realpath(Ctx& x, GuestPtr path, GuestPtr resolved) {
    std::error_code ec;
    const auto p = std::filesystem::weakly_canonical(x.proc.readCString(path), ec);
    if (ec) { setErrno(x, ENOENT); return 0; }
    const std::string s = p.string();
    if (!resolved) return x.proc.allocCString(s);
    x.mem.copyIn(resolved, s.c_str(), std::min<size_t>(s.size() + 1, 1024));
    return resolved;
}
uint32_t e_mktemp(Ctx& x, GuestPtr tmpl) {
    std::string t = x.proc.readCString(tmpl);
    static const char chars[] = "abcdefghijklmnopqrstuvwxyz0123456789";
    for (char& c : t) if (c == 'X') c = chars[r_arc4random_uniform(x, 36)];
    x.mem.copyIn(tmpl, t.c_str(), t.size());
    return tmpl;
}
int32_t e_mkstemp(Ctx& x, GuestPtr tmpl) {
    for (int tries = 0; tries < 100; ++tries) {
        const std::string original = x.proc.readCString(tmpl);
        e_mktemp(x, tmpl);
        const std::string p = x.proc.readCString(tmpl);
        const int fd = ::open(p.c_str(), O_RDWR | O_CREAT | O_EXCL, 0600);
        if (fd >= 0) return fd;
        x.mem.copyIn(tmpl, original.c_str(), original.size());
    }
    setErrno(x, EEXIST);
    return -1;
}
int32_t e_system(Ctx&, GuestPtr) { return -1; }

// ------------------------------------------------------------------ exit ---

void p_exit(Ctx& x, int32_t code) {
    static thread_local bool inExit = false;
    if (!inExit) {
        inExit = true;
        try { x.proc.runAtExitHandlers(x.cpu); } catch (...) { inExit = false; throw; }
        inExit = false;
    }
    flushGuestStdio(x.proc);
    x.proc.exit(code);
}
void p__exit(Ctx& x, int32_t code) { flushGuestStdio(x.proc); x.proc.exit(code); }
void p_abort(Ctx& x) {
    flushGuestStdio(x.proc);
    throw std::runtime_error("abort() called by the app");
}
int32_t p_atexit(Ctx& x, GuestPtr fn) { x.proc.addAtExit(fn, 0, false); return 0; }
int32_t p_cxa_atexit(Ctx& x, GuestPtr fn, GuestPtr arg, GuestPtr) { x.proc.addAtExit(fn, arg, true); return 0; }
void p_assert_rtn(Ctx& x, GuestPtr func, GuestPtr file, int32_t line, GuestPtr expr) {
    flushGuestStdio(x.proc);
    throw std::runtime_error("Assertion failed: (" + x.proc.readCString(expr) + "), function " + x.proc.readCString(func) +
                             ", file " + x.proc.readCString(file) + ", line " + std::to_string(line));
}

} // namespace

void registerLibcStdlib(Process& p) {
    p.def<&m_malloc>("_malloc");
    p.def<&m_calloc>("_calloc");
    p.def<&m_realloc>("_realloc");
    p.def<&m_reallocf>("_reallocf");
    p.def<&m_free>("_free");
    p.def<&m_valloc>("_valloc");
    p.def<&m_posix_memalign>("_posix_memalign");
    p.def<&m_malloc_size>("_malloc_size");
    p.def<&m_malloc_good_size>("_malloc_good_size");
    p.def<&m_default_zone>("_malloc_default_zone");
    p.def<&m_zone_size>("__hvm_zone_size");
    p.def<&m_zone_free_definite>("__hvm_zone_free_definite");
    p.def<&m_zone_malloc>("_malloc_zone_malloc");
    p.def<&m_zone_calloc>("_malloc_zone_calloc");
    p.def<&m_zone_realloc>("_malloc_zone_realloc");
    p.def<&m_zone_free>("_malloc_zone_free");
    p.def<&m_zone_memalign>("_malloc_zone_memalign");
    p.def<&m_zone_from_ptr>("_malloc_zone_from_ptr");
    p.def<&m_create_zone>("_malloc_create_zone");
    p.def<&m_destroy_zone>("_malloc_destroy_zone");
    p.def<&m_set_zone_name>("_malloc_set_zone_name");
    p.def<&m_zone_name>("_malloc_get_zone_name");
    p.def<&m_zone_valloc>("_malloc_zone_valloc");
    p.def<&m_zone_pressure_relief>("_malloc_zone_pressure_relief");
    p.def<&m_zone_statistics>("_malloc_zone_statistics");
    p.def<&m_zone_log>("_malloc_zone_log");
    p.def<&m_zone_batch_malloc>("_malloc_zone_batch_malloc");
    p.def<&m_zone_batch_free>("_malloc_zone_batch_free");
    p.def<&m_default_zone>("_malloc_default_purgeable_zone");
    p.def<&c_atoi>("_atoi");
    p.def<&c_atoi>("_atol");
    p.def<&c_atoll>("_atoll");
    p.def<&c_atof>("_atof");
    p.def<&c_strtol>("_strtol");
    p.def<&c_strtoul>("_strtoul");
    p.def<&c_strtoll>("_strtoll");
    p.def<&c_strtoll>("_strtoq");
    p.def<&c_strtoull>("_strtoull");
    p.def<&c_strtoull>("_strtouq");
    p.def<&c_strtod>("_strtod");
    p.def<&c_strtof>("_strtof");
    p.def<&c_wcstol>("_wcstol");
    p.def<&c_wcstoul>("_wcstoul");
    p.def<&c_wcstoll>("_wcstoll");
    p.def<&c_wcstoull>("_wcstoull");
    p.def<&c_wcstod>("_wcstod");
    p.def<&c_wcstof>("_wcstof");
    p.def<&c_abs>("_abs");
    p.def<&c_abs>("_labs");
    p.def<&c_llabs>("_llabs");
    p.def<&c_div>("_div");
    p.def<&c_div>("_ldiv");
    p.def<&c_qsort>("_qsort");
    p.def<&c_qsort_r>("_qsort_r");
    p.def<&c_bsearch>("_bsearch");
    p.def<&r_rand>("_rand");
    p.def<&r_srand>("_srand");
    p.def<&r_random>("_random");
    p.def<&r_srandom>("_srandom");
    p.def<&r_arc4random>("_arc4random");
    p.def<&r_arc4random_uniform>("_arc4random_uniform");
    p.def<&r_arc4random_buf>("_arc4random_buf");
    p.def<&r_drand48>("_drand48");
    p.def<&r_srand48>("_srand48");
    p.def<&r_lrand48>("_lrand48");
    p.def<&e_getenv>("_getenv");
    p.def<&e_setenv>("_setenv");
    p.def<&e_unsetenv>("_unsetenv");
    p.def<&e_putenv>("_putenv");
    p.def<&e_realpath>("_realpath");
    p.def<&e_realpath>("_realpath$DARWIN_EXTSN");
    p.def<&e_mktemp>("_mktemp");
    p.def<&e_mkstemp>("_mkstemp");
    p.def<&e_system>("_system");
    p.def<&p_exit>("_exit");
    p.def<&p__exit>("__exit");
    p.def<&p_abort>("_abort");
    p.def<&p_atexit>("_atexit");
    p.def<&p_cxa_atexit>("___cxa_atexit");
    p.def<&p_assert_rtn>("___assert_rtn");
}

} // namespace Hydrogen::Runtime
