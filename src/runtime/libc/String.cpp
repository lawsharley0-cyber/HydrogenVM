// HydrogenVM — <string.h>, <strings.h>, <ctype.h>, <wchar.h> basics.
// Operates directly on guest memory through the arena's host mapping.
#include "runtime/Process.hpp"
#include <cctype>
#include <cstring>
#include <strings.h>

namespace Hydrogen::Runtime {
namespace {

inline char* H(Ctx& x, GuestPtr p) { return reinterpret_cast<char*>(x.mem.host(p)); }
inline GuestPtr G(Ctx& x, const void* p) { return p ? x.mem.guestAddr(p) : 0; }


uint32_t s_memcpy(Ctx& x, GuestPtr d, GuestPtr s, uint32_t n) { if (n) std::memmove(H(x, d), H(x, s), n); return d; }
uint32_t s_memset(Ctx& x, GuestPtr d, int32_t c, uint32_t n) { if (n) std::memset(H(x, d), c, n); return d; }
int32_t  s_memcmp(Ctx& x, GuestPtr a, GuestPtr b, uint32_t n) { return n ? std::memcmp(H(x, a), H(x, b), n) : 0; }
uint32_t s_memchr(Ctx& x, GuestPtr s, int32_t c, uint32_t n) { return n ? G(x, std::memchr(H(x, s), c, n)) : 0; }
void     s_bzero(Ctx& x, GuestPtr d, uint32_t n) { if (n) std::memset(H(x, d), 0, n); }
void     s_bcopy(Ctx& x, GuestPtr s, GuestPtr d, uint32_t n) { if (n) std::memmove(H(x, d), H(x, s), n); }
uint32_t s_memset_pattern4(Ctx& x, GuestPtr d, GuestPtr pat, uint32_t n) {
    uint8_t p[4]; std::memcpy(p, H(x, pat), 4);
    for (uint32_t i = 0; i < n; ++i) H(x, d)[i] = char(p[i & 3]);
    return d;
}
uint32_t s_memset_pattern16(Ctx& x, GuestPtr d, GuestPtr pat, uint32_t n) {
    uint8_t p[16]; std::memcpy(p, H(x, pat), 16);
    for (uint32_t i = 0; i < n; ++i) H(x, d)[i] = char(p[i & 15]);
    return d;
}

uint32_t s_strlen(Ctx& x, GuestPtr s) { return uint32_t(std::strlen(H(x, s))); }
uint32_t s_strnlen(Ctx& x, GuestPtr s, uint32_t n) { return uint32_t(strnlen(H(x, s), n)); }
int32_t  s_strcmp(Ctx& x, GuestPtr a, GuestPtr b) { return std::strcmp(H(x, a), H(x, b)); }
int32_t  s_strncmp(Ctx& x, GuestPtr a, GuestPtr b, uint32_t n) { return std::strncmp(H(x, a), H(x, b), n); }
int32_t  s_strcasecmp(Ctx& x, GuestPtr a, GuestPtr b) { return ::strcasecmp(H(x, a), H(x, b)); }
int32_t  s_strncasecmp(Ctx& x, GuestPtr a, GuestPtr b, uint32_t n) { return ::strncasecmp(H(x, a), H(x, b), n); }
int32_t  s_strcoll(Ctx& x, GuestPtr a, GuestPtr b) { return std::strcmp(H(x, a), H(x, b)); }
uint32_t s_strcpy(Ctx& x, GuestPtr d, GuestPtr s) { const size_t n = std::strlen(H(x, s)); std::memmove(H(x, d), H(x, s), n + 1); return d; }
uint32_t s_stpcpy(Ctx& x, GuestPtr d, GuestPtr s) { const size_t n = std::strlen(H(x, s)); std::memmove(H(x, d), H(x, s), n + 1); return d + uint32_t(n); }
uint32_t s_strncpy(Ctx& x, GuestPtr d, GuestPtr s, uint32_t n) {
    const size_t len = strnlen(H(x, s), n);
    std::memmove(H(x, d), H(x, s), len);
    if (len < n) std::memset(H(x, d) + len, 0, n - len);
    return d;
}
uint32_t s_strcat(Ctx& x, GuestPtr d, GuestPtr s) { const size_t dl = std::strlen(H(x, d)), sl = std::strlen(H(x, s)); std::memmove(H(x, d) + dl, H(x, s), sl + 1); return d; }
uint32_t s_strncat(Ctx& x, GuestPtr d, GuestPtr s, uint32_t n) {
    const size_t dl = std::strlen(H(x, d)), sl = strnlen(H(x, s), n);
    std::memmove(H(x, d) + dl, H(x, s), sl);
    H(x, d)[dl + sl] = 0;
    return d;
}
uint32_t s_strlcpy(Ctx& x, GuestPtr d, GuestPtr s, uint32_t n) {
    const size_t sl = std::strlen(H(x, s));
    if (n) { const size_t c = std::min<size_t>(sl, n - 1); std::memmove(H(x, d), H(x, s), c); H(x, d)[c] = 0; }
    return uint32_t(sl);
}
uint32_t s_strlcat(Ctx& x, GuestPtr d, GuestPtr s, uint32_t n) {
    const size_t dl = strnlen(H(x, d), n), sl = std::strlen(H(x, s));
    if (dl == n) return uint32_t(n + sl);
    const size_t c = std::min<size_t>(sl, n - dl - 1);
    std::memmove(H(x, d) + dl, H(x, s), c);
    H(x, d)[dl + c] = 0;
    return uint32_t(dl + sl);
}
uint32_t s_strchr(Ctx& x, GuestPtr s, int32_t c) { return G(x, std::strchr(H(x, s), c)); }
uint32_t s_strrchr(Ctx& x, GuestPtr s, int32_t c) { return G(x, std::strrchr(H(x, s), c)); }
uint32_t s_strstr(Ctx& x, GuestPtr a, GuestPtr b) { return G(x, std::strstr(H(x, a), H(x, b))); }
uint32_t s_strcasestr(Ctx& x, GuestPtr a, GuestPtr b) { return G(x, ::strcasestr(H(x, a), H(x, b))); }
uint32_t s_strnstr(Ctx& x, GuestPtr a, GuestPtr b, uint32_t n) {
    const char* hay = H(x, a); const char* nd = H(x, b);
    const size_t nl = std::strlen(nd);
    if (!nl) return a;
    for (size_t i = 0; i + nl <= n && hay[i]; ++i) if (std::strncmp(hay + i, nd, nl) == 0) return a + uint32_t(i);
    return 0;
}
uint32_t s_strpbrk(Ctx& x, GuestPtr a, GuestPtr b) { return G(x, std::strpbrk(H(x, a), H(x, b))); }
uint32_t s_strspn(Ctx& x, GuestPtr a, GuestPtr b) { return uint32_t(std::strspn(H(x, a), H(x, b))); }
uint32_t s_strcspn(Ctx& x, GuestPtr a, GuestPtr b) { return uint32_t(std::strcspn(H(x, a), H(x, b))); }
uint32_t s_strdup(Ctx& x, GuestPtr s) {
    const size_t n = std::strlen(H(x, s));
    const uint32_t d = x.proc.heap().alloc(uint32_t(n + 1));
    if (d) std::memcpy(H(x, d), H(x, s), n + 1);
    return d;
}
uint32_t s_strndup(Ctx& x, GuestPtr s, uint32_t max) {
    const size_t n = strnlen(H(x, s), max);
    const uint32_t d = x.proc.heap().alloc(uint32_t(n + 1));
    if (d) { std::memcpy(H(x, d), H(x, s), n); H(x, d)[n] = 0; }
    return d;
}

// strtok / strsep keep state in guest memory, exactly like libc.
uint32_t s_strtok_r(Ctx& x, GuestPtr s, GuestPtr delim, GuestPtr save) {
    uint32_t p = s ? s : x.mem.read<uint32_t>(save);
    if (!p) return 0;
    const std::string d = x.proc.readCString(delim);
    while (char c = char(x.mem.read<uint8_t>(p))) { if (d.find(c) == std::string::npos) break; ++p; }
    if (!x.mem.read<uint8_t>(p)) { x.mem.write<uint32_t>(save, 0); return 0; }
    const uint32_t tok = p;
    while (char c = char(x.mem.read<uint8_t>(p))) {
        if (d.find(c) != std::string::npos) { x.mem.write<uint8_t>(p, 0); x.mem.write<uint32_t>(save, p + 1); return tok; }
        ++p;
    }
    x.mem.write<uint32_t>(save, 0);
    return tok;
}
uint32_t s_strtok(Ctx& x, GuestPtr s, GuestPtr delim) {
    static const char saveKey = 0; uint32_t& save = x.proc.threadSlot(&saveKey);
    if (!save) save = x.proc.allocStatic(4);
    return s_strtok_r(x, s, delim, save);
}
uint32_t s_strsep(Ctx& x, GuestPtr sp, GuestPtr delim) {
    const uint32_t s = x.mem.read<uint32_t>(sp);
    if (!s) return 0;
    const std::string d = x.proc.readCString(delim);
    for (uint32_t p = s;; ++p) {
        const char c = char(x.mem.read<uint8_t>(p));
        if (d.find(c) != std::string::npos || !c) {
            if (!c) x.mem.write<uint32_t>(sp, 0);
            else { x.mem.write<uint8_t>(p, 0); x.mem.write<uint32_t>(sp, p + 1); }
            return s;
        }
    }
}

uint32_t s_strerror(Ctx& x, int32_t e) { return x.proc.internCString(std::strerror(e)); }
int32_t s_strerror_r(Ctx& x, int32_t e, GuestPtr buf, uint32_t n) { s_strlcpy(x, buf, x.proc.internCString(std::strerror(e)), n); return 0; }

// ctype (C locale)
int32_t c_isalpha(Ctx&, int32_t c) { return c >= 0 && c < 256 && std::isalpha(c); }
int32_t c_isdigit(Ctx&, int32_t c) { return c >= '0' && c <= '9'; }
int32_t c_isalnum(Ctx&, int32_t c) { return c >= 0 && c < 256 && std::isalnum(c); }
int32_t c_isspace(Ctx&, int32_t c) { return c >= 0 && c < 256 && std::isspace(c); }
int32_t c_isupper(Ctx&, int32_t c) { return c >= 'A' && c <= 'Z'; }
int32_t c_islower(Ctx&, int32_t c) { return c >= 'a' && c <= 'z'; }
int32_t c_ispunct(Ctx&, int32_t c) { return c >= 0 && c < 256 && std::ispunct(c); }
int32_t c_isxdigit(Ctx&, int32_t c) { return c >= 0 && c < 256 && std::isxdigit(c); }
int32_t c_isprint(Ctx&, int32_t c) { return c >= 0x20 && c < 0x7F; }
int32_t c_iscntrl(Ctx&, int32_t c) { return (c >= 0 && c < 0x20) || c == 0x7F; }
int32_t c_isgraph(Ctx&, int32_t c) { return c > 0x20 && c < 0x7F; }
int32_t c_toupper(Ctx&, int32_t c) { return c >= 'a' && c <= 'z' ? c - 32 : c; }
int32_t c_tolower(Ctx&, int32_t c) { return c >= 'A' && c <= 'Z' ? c + 32 : c; }
int32_t c_isascii(Ctx&, int32_t c) { return (c & ~0x7F) == 0; }
int32_t c_maskrune(Ctx&, int32_t c, uint32_t mask) {
    // Darwin <ctype.h> inlines call __maskrune for non-ASCII values only.
    (void)mask;
    return c >= 0 && c < 256 && std::isalpha(c) ? int32_t(mask) : 0;
}
int32_t c_toupper_l(Ctx& x, int32_t c, uint32_t) { return c_toupper(x, c); }
int32_t c_tolower_l(Ctx& x, int32_t c, uint32_t) { return c_tolower(x, c); }

uint32_t w_wcslen(Ctx& x, GuestPtr s) { uint32_t n = 0; while (x.mem.read<uint32_t>(s + n * 4)) ++n; return n; }
uint32_t w_wcscpy(Ctx& x, GuestPtr d, GuestPtr s) {
    const uint32_t n = w_wcslen(x, s);
    std::memmove(H(x, d), H(x, s), (n + 1) * 4);
    return d;
}
uint32_t w_wcsncpy(Ctx& x, GuestPtr d, GuestPtr s, uint32_t n) {
    uint32_t len = 0;
    while (len < n && x.mem.read<uint32_t>(s + len * 4)) ++len;
    if (len) std::memmove(H(x, d), H(x, s), len * 4);
    if (len < n) std::memset(H(x, d + len * 4), 0, (n - len) * 4);
    return d;
}
uint32_t w_wcscat(Ctx& x, GuestPtr d, GuestPtr s) {
    const uint32_t dl = w_wcslen(x, d), sl = w_wcslen(x, s);
    std::memmove(H(x, d + dl * 4), H(x, s), (sl + 1) * 4);
    return d;
}
uint32_t w_wcsncat(Ctx& x, GuestPtr d, GuestPtr s, uint32_t n) {
    const uint32_t dl = w_wcslen(x, d);
    uint32_t sl = 0;
    while (sl < n && x.mem.read<uint32_t>(s + sl * 4)) ++sl;
    if (sl) std::memmove(H(x, d + dl * 4), H(x, s), sl * 4);
    x.mem.write<uint32_t>(d + (dl + sl) * 4, 0);
    return d;
}
int32_t w_wcscmp(Ctx& x, GuestPtr a, GuestPtr b) {
    for (uint32_t i = 0;; ++i) {
        const int32_t ca = x.mem.read<int32_t>(a + i * 4), cb = x.mem.read<int32_t>(b + i * 4);
        if (ca != cb || !ca) return ca < cb ? -1 : ca > cb ? 1 : 0;
    }
}
int32_t w_wcsncmp(Ctx& x, GuestPtr a, GuestPtr b, uint32_t n) {
    for (uint32_t i = 0; i < n; ++i) {
        const int32_t ca = x.mem.read<int32_t>(a + i * 4), cb = x.mem.read<int32_t>(b + i * 4);
        if (ca != cb || !ca) return ca < cb ? -1 : ca > cb ? 1 : 0;
    }
    return 0;
}
uint32_t w_wcschr(Ctx& x, GuestPtr s, int32_t c) {
    for (uint32_t i = 0;; ++i) {
        const int32_t ch = x.mem.read<int32_t>(s + i * 4);
        if (ch == c) return s + i * 4;
        if (!ch) return 0;
    }
}
uint32_t w_wcsrchr(Ctx& x, GuestPtr s, int32_t c) {
    uint32_t last = 0;
    for (uint32_t i = 0;; ++i) {
        const int32_t ch = x.mem.read<int32_t>(s + i * 4);
        if (ch == c) last = s + i * 4;
        if (!ch) return last;
    }
}
uint32_t w_wcsstr(Ctx& x, GuestPtr hay, GuestPtr nd) {
    const uint32_t nl = w_wcslen(x, nd);
    if (!nl) return hay;
    const uint32_t hl = w_wcslen(x, hay);
    if (nl > hl) return 0;
    for (uint32_t i = 0; i + nl <= hl; ++i)
        if (w_wcsncmp(x, hay + i * 4, nd, nl) == 0) return hay + i * 4;
    return 0;
}
uint32_t w_wmemcpy(Ctx& x, GuestPtr d, GuestPtr s, uint32_t n) { if (n) std::memmove(H(x, d), H(x, s), n * 4); return d; }
uint32_t w_wmemset(Ctx& x, GuestPtr d, int32_t c, uint32_t n) { for (uint32_t i = 0; i < n; ++i) x.mem.write<int32_t>(d + i * 4, c); return d; }
int32_t  w_wmemcmp(Ctx& x, GuestPtr a, GuestPtr b, uint32_t n) { return w_wcsncmp(x, a, b, n); }
uint32_t w_wmemchr(Ctx& x, GuestPtr s, int32_t c, uint32_t n) {
    for (uint32_t i = 0; i < n; ++i) if (x.mem.read<int32_t>(s + i * 4) == c) return s + i * 4;
    return 0;
}
int32_t w_towlower(Ctx& x, int32_t c) { return c_tolower(x, c); }
int32_t w_towupper(Ctx& x, int32_t c) { return c_toupper(x, c); }
int32_t w_iswspace(Ctx& x, int32_t c) { return c_isspace(x, c); }
int32_t w_iswdigit(Ctx& x, int32_t c) { return c_isdigit(x, c); }
int32_t w_iswalpha(Ctx& x, int32_t c) { return c_isalpha(x, c); }
int32_t w_iswalnum(Ctx& x, int32_t c) { return c_isalnum(x, c); }


// Helper function to safely read strings up to a maximum length.
// Validates page-by-page readable permissions. Throws GuestFault on invalid access.
// Helper function to safely read strings up to a maximum length.
// Validates page-by-page readable permissions. Throws GuestFault on invalid access.
size_t safe_strnlen(Ctx& x, GuestPtr p, size_t max_len) {
    if (max_len == 0) return 0;

    size_t len = 0;
    while (len < max_len) {
        uint64_t addr = uint64_t(p) + len;
        if (addr >= Memory::GuestArena::kArenaSize) {
             throw CPU::GuestFault(CPU::Exit::MemFault, uint32_t(addr), x.cpu.r[CPU::kPC], uint32_t(x.cpu.thumb), "invalid guest memory access in safe_strnlen (overflow)");
        }
        if (!x.mem.validateRange(uint32_t(addr), 1, Memory::kProtRead)) {
            throw CPU::GuestFault(CPU::Exit::MemFault, uint32_t(addr), x.cpu.r[CPU::kPC], uint32_t(x.cpu.thumb), "invalid guest memory access in safe_strnlen");
        }

        uint32_t page_offset = uint32_t(addr) & (Memory::GuestArena::kGuestPageSize - 1);
        uint32_t scan_limit = Memory::GuestArena::kGuestPageSize - page_offset;
        if (scan_limit > max_len - len) scan_limit = max_len - len;

        const void* null_ptr = std::memchr(H(x, uint32_t(addr)), 0, scan_limit);
        if (null_ptr) return len + (static_cast<const char*>(null_ptr) - H(x, uint32_t(addr)));

        len += scan_limit;
    }
    return max_len;
}

size_t safe_strlen(Ctx& x, GuestPtr p) {
    size_t len = 0;
    while (true) {
        uint64_t addr = uint64_t(p) + len;
        if (addr >= Memory::GuestArena::kArenaSize) {
             throw CPU::GuestFault(CPU::Exit::MemFault, uint32_t(addr), x.cpu.r[CPU::kPC], uint32_t(x.cpu.thumb), "invalid guest memory access in safe_strlen (overflow)");
        }
        if (!x.mem.validateRange(uint32_t(addr), 1, Memory::kProtRead)) {
            throw CPU::GuestFault(CPU::Exit::MemFault, uint32_t(addr), x.cpu.r[CPU::kPC], uint32_t(x.cpu.thumb), "invalid guest memory access in safe_strlen");
        }

        uint32_t page_offset = uint32_t(addr) & (Memory::GuestArena::kGuestPageSize - 1);
        uint32_t scan_limit = Memory::GuestArena::kGuestPageSize - page_offset;

        const void* null_ptr = std::memchr(H(x, uint32_t(addr)), 0, scan_limit);
        if (null_ptr) return len + (static_cast<const char*>(null_ptr) - H(x, uint32_t(addr)));

        len += scan_limit;
    }
}

[[noreturn]] void s_chk_fail(Ctx& x) {
    throw CPU::GuestFault(CPU::Exit::MemFault, 0, x.cpu.r[CPU::kPC], uint32_t(x.cpu.thumb), "buffer overflow detected (__chk_fail)");
}

uint32_t s_memcpy_chk(Ctx& x, GuestPtr d, GuestPtr s, uint32_t n, uint32_t destlen) {
    if (n > destlen) s_chk_fail(x);
    if (!x.mem.validateRange(s, n, Memory::kProtRead) || !x.mem.validateRange(d, n, Memory::kProtWrite)) {
        throw CPU::GuestFault(CPU::Exit::MemFault, d, x.cpu.r[CPU::kPC], uint32_t(x.cpu.thumb), "invalid guest memory access in memcpy_chk");
    }
    return s_memcpy(x, d, s, n);
}

uint32_t s_memmove_chk(Ctx& x, GuestPtr d, GuestPtr s, uint32_t n, uint32_t destlen) {
    if (n > destlen) s_chk_fail(x);
    if (!x.mem.validateRange(s, n, Memory::kProtRead) || !x.mem.validateRange(d, n, Memory::kProtWrite)) {
        throw CPU::GuestFault(CPU::Exit::MemFault, d, x.cpu.r[CPU::kPC], uint32_t(x.cpu.thumb), "invalid guest memory access in memmove_chk");
    }
    return s_memcpy(x, d, s, n); // s_memcpy uses memmove internally.
}

uint32_t s_memset_chk(Ctx& x, GuestPtr d, int32_t c, uint32_t n, uint32_t destlen) {
    if (n > destlen) s_chk_fail(x);
    if (!x.mem.validateRange(d, n, Memory::kProtWrite)) {
        throw CPU::GuestFault(CPU::Exit::MemFault, d, x.cpu.r[CPU::kPC], uint32_t(x.cpu.thumb), "invalid guest memory access in memset_chk");
    }
    return s_memset(x, d, c, n);
}

uint32_t s_strcpy_chk(Ctx& x, GuestPtr d, GuestPtr s, uint32_t destlen) {
    const size_t slen = safe_strlen(x, s);
    if (slen + 1 > destlen) s_chk_fail(x);
    if (!x.mem.validateRange(d, slen + 1, Memory::kProtWrite)) {
        throw CPU::GuestFault(CPU::Exit::MemFault, d, x.cpu.r[CPU::kPC], uint32_t(x.cpu.thumb), "invalid guest memory access in strcpy_chk");
    }
    std::memmove(H(x, d), H(x, s), slen + 1);
    return d;
}

uint32_t s_strncpy_chk(Ctx& x, GuestPtr d, GuestPtr s, uint32_t n, uint32_t destlen) {
    if (n > destlen) s_chk_fail(x);
    if (!x.mem.validateRange(d, n, Memory::kProtWrite)) {
        throw CPU::GuestFault(CPU::Exit::MemFault, d, x.cpu.r[CPU::kPC], uint32_t(x.cpu.thumb), "invalid guest memory access in strncpy_chk");
    }
    const size_t sl = safe_strnlen(x, s, n);
    std::memcpy(H(x, d), H(x, s), sl);
    if (sl < n) std::memset(H(x, d) + sl, 0, n - sl);
    return d;
}

uint32_t s_strcat_chk(Ctx& x, GuestPtr d, GuestPtr s, uint32_t destlen) {
    const size_t dl = safe_strlen(x, d);
    if (dl >= destlen) s_chk_fail(x);
    const size_t sl = safe_strlen(x, s);
    if (dl + sl + 1 > destlen) s_chk_fail(x);
    if (!x.mem.validateRange(d + dl, sl + 1, Memory::kProtWrite)) {
        throw CPU::GuestFault(CPU::Exit::MemFault, d + dl, x.cpu.r[CPU::kPC], uint32_t(x.cpu.thumb), "invalid guest memory access in strcat_chk");
    }
    std::memmove(H(x, d) + dl, H(x, s), sl + 1);
    return d;
}

uint32_t s_strncat_chk(Ctx& x, GuestPtr d, GuestPtr s, uint32_t n, uint32_t destlen) {
    const size_t dl = safe_strlen(x, d);
    if (dl >= destlen) s_chk_fail(x);
    const size_t sl = safe_strnlen(x, s, n);
    if (dl + sl + 1 > destlen) s_chk_fail(x);
    if (!x.mem.validateRange(d + dl, sl + 1, Memory::kProtWrite)) {
         throw CPU::GuestFault(CPU::Exit::MemFault, d + dl, x.cpu.r[CPU::kPC], uint32_t(x.cpu.thumb), "invalid guest memory access in strncat_chk");
    }
    std::memcpy(H(x, d) + dl, H(x, s), sl);
    H(x, d)[dl + sl] = ' ';
    return d;
}

uint32_t s_strlcpy_chk(Ctx& x, GuestPtr d, GuestPtr s, uint32_t n, uint32_t destlen) {
    if (n > destlen) s_chk_fail(x);
    // strlcpy always reads up to n-1 chars, or the NUL byte, from s.
    // It writes MIN(strlen(s), n-1) chars to d, plus a NUL, assuming n > 0.
    const size_t sl = safe_strlen(x, s);
    if (n > 0) {
        size_t copy_len = std::min<size_t>(sl, n - 1);
        if (!x.mem.validateRange(d, copy_len + 1, Memory::kProtWrite)) {
            throw CPU::GuestFault(CPU::Exit::MemFault, d, x.cpu.r[CPU::kPC], uint32_t(x.cpu.thumb), "invalid guest memory access in strlcpy_chk");
        }
    }
    return s_strlcpy(x, d, s, n);
}

uint32_t s_strlcat_chk(Ctx& x, GuestPtr d, GuestPtr s, uint32_t n, uint32_t destlen) {
    if (n > destlen) s_chk_fail(x);
    if (n == 0) return s_strlcat(x, d, s, 0); // No reads/writes on d when n == 0.
    // strlcat needs to read up to n bytes of d to find the NUL, and then read from s.
    const size_t dl = safe_strnlen(x, d, n);
    const size_t sl = safe_strlen(x, s);
    if (dl < n) { // We only append if we found NUL before n.
        size_t append_len = std::min<size_t>(sl, n - dl - 1);
        if (!x.mem.validateRange(d + dl, append_len + 1, Memory::kProtWrite)) {
             throw CPU::GuestFault(CPU::Exit::MemFault, d + dl, x.cpu.r[CPU::kPC], uint32_t(x.cpu.thumb), "invalid guest memory access in strlcat_chk");
        }
    }
    return s_strlcat(x, d, s, n);
}

} // namespace

// Darwin's <ctype.h> inlines isalpha() & co. as table lookups into
// _DefaultRuneLocale.__runetype[c] for ASCII, so the table must be real.
static void buildRuneLocale(Process& p) {
    // struct _RuneLocale: magic[8], encoding[32], sgetrune, sputrune, invalid_rune,
    // __runetype[256] (uint32), __maplower[256], __mapupper[256], ...
    constexpr uint32_t kRuneTypeOff = 8 + 32 + 4 + 4 + 4;
    const uint32_t size = kRuneTypeOff + 256 * 4 * 3 + 64;
    const uint32_t a = p.defineDataBlock("__DefaultRuneLocale", size);
    auto& m = p.mem();
    m.copyIn(a, "RuneMagA", 8);
    m.copyIn(a + 8, "NONE", 5);
    enum : uint32_t { A = 0x100, C = 0x200, D = 0x400, G = 0x800, L = 0x1000, P = 0x2000, S = 0x4000, U = 0x8000,
                      X = 0x10000, B = 0x20000, R = 0x40000 };
    for (uint32_t c = 0; c < 256; ++c) {
        uint32_t t = 0;
        if (c < 128) {
            if (std::isalpha(int(c))) t |= A;
            if (std::iscntrl(int(c))) t |= C;
            if (std::isdigit(int(c))) t |= D | uint32_t(c - '0');
            if (std::isgraph(int(c))) t |= G;
            if (std::islower(int(c))) t |= L;
            if (std::ispunct(int(c))) t |= P;
            if (std::isspace(int(c))) t |= S;
            if (std::isupper(int(c))) t |= U;
            if (std::isxdigit(int(c))) { t |= X; if (!std::isdigit(int(c))) t |= uint32_t(std::tolower(int(c)) - 'a' + 10); }
            if (c == ' ' || c == '\t') t |= B;
            if (std::isprint(int(c))) t |= R;
        }
        m.write<uint32_t>(a + kRuneTypeOff + c * 4, t);
        m.write<uint32_t>(a + kRuneTypeOff + 1024 + c * 4, c < 128 ? uint32_t(std::tolower(int(c))) : c);
        m.write<uint32_t>(a + kRuneTypeOff + 2048 + c * 4, c < 128 ? uint32_t(std::toupper(int(c))) : c);
    }
    const uint32_t cur = p.defineDataBlock("__CurrentRuneLocale", 4);
    m.write<uint32_t>(cur, a);
    p.defineData("___mb_cur_max", p.defineDataBlock("___mb_cur_max_storage", 4));
    m.write<uint32_t>(*p.lookupSymbol("___mb_cur_max"), 1);
}

void registerLibcString(Process& p) {
    p.def<&s_memcpy>("_memcpy");
    p.def<&s_memcpy>("_memmove");
    p.def<&s_memcpy_chk>("___memcpy_chk");
    p.def<&s_memmove_chk>("___memmove_chk");
    p.def<&s_memset>("_memset");
    p.def<&s_memset_chk>("___memset_chk");
    p.def<&s_memcmp>("_memcmp");
    p.def<&s_memcmp>("_bcmp");
    p.def<&s_memchr>("_memchr");
    p.def<&s_bzero>("_bzero");
    p.def<&s_bzero>("___bzero");
    p.def<&s_bcopy>("_bcopy");
    p.def<&s_memset_pattern4>("_memset_pattern4");
    p.def<&s_memset_pattern16>("_memset_pattern16");
    p.def<&s_strlen>("_strlen");
    p.def<&s_strnlen>("_strnlen");
    p.def<&s_strcmp>("_strcmp");
    p.def<&s_strncmp>("_strncmp");
    p.def<&s_strcasecmp>("_strcasecmp");
    p.def<&s_strncasecmp>("_strncasecmp");
    p.def<&s_strcoll>("_strcoll");
    p.def<&s_strcpy>("_strcpy");
    p.def<&s_strcpy_chk>("___strcpy_chk");
    p.def<&s_stpcpy>("_stpcpy");
    p.def<&s_strncpy>("_strncpy");
    p.def<&s_strncpy_chk>("___strncpy_chk");
    p.def<&s_strcat>("_strcat");
    p.def<&s_strcat_chk>("___strcat_chk");
    p.def<&s_strncat>("_strncat");
    p.def<&s_strncat_chk>("___strncat_chk");
    p.def<&s_strlcpy>("_strlcpy");
    p.def<&s_strlcpy_chk>("___strlcpy_chk");
    p.def<&s_strlcat>("_strlcat");
    p.def<&s_strlcat_chk>("___strlcat_chk");
    p.def<&s_strchr>("_strchr");
    p.def<&s_strchr>("_index");
    p.def<&s_strrchr>("_strrchr");
    p.def<&s_strrchr>("_rindex");
    p.def<&s_strstr>("_strstr");
    p.def<&s_strcasestr>("_strcasestr");
    p.def<&s_strnstr>("_strnstr");
    p.def<&s_strpbrk>("_strpbrk");
    p.def<&s_strspn>("_strspn");
    p.def<&s_strcspn>("_strcspn");
    p.def<&s_strdup>("_strdup");
    p.def<&s_strndup>("_strndup");
    p.def<&s_strtok>("_strtok");
    p.def<&s_strtok_r>("_strtok_r");
    p.def<&s_strsep>("_strsep");
    p.def<&s_strerror>("_strerror");
    p.def<&s_strerror_r>("_strerror_r");
    p.def<&c_isalpha>("_isalpha");
    p.def<&c_isdigit>("_isdigit");
    p.def<&c_isalnum>("_isalnum");
    p.def<&c_isspace>("_isspace");
    p.def<&c_isupper>("_isupper");
    p.def<&c_islower>("_islower");
    p.def<&c_ispunct>("_ispunct");
    p.def<&c_isxdigit>("_isxdigit");
    p.def<&c_isprint>("_isprint");
    p.def<&c_iscntrl>("_iscntrl");
    p.def<&c_isgraph>("_isgraph");
    p.def<&c_isascii>("_isascii");
    p.def<&c_toupper>("_toupper");
    p.def<&c_tolower>("_tolower");
    p.def<&c_toupper>("___toupper");
    p.def<&c_tolower>("___tolower");
    p.def<&c_toupper_l>("_toupper_l");
    p.def<&c_tolower_l>("_tolower_l");
    p.def<&c_maskrune>("___maskrune");
    p.def<&w_wcslen>("_wcslen");
    p.def<&w_wcscpy>("_wcscpy");
    p.def<&w_wcsncpy>("_wcsncpy");
    p.def<&w_wcscat>("_wcscat");
    p.def<&w_wcsncat>("_wcsncat");
    p.def<&w_wcscmp>("_wcscmp");
    p.def<&w_wcsncmp>("_wcsncmp");
    p.def<&w_wcschr>("_wcschr");
    p.def<&w_wcsrchr>("_wcsrchr");
    p.def<&w_wcsstr>("_wcsstr");
    p.def<&w_wmemcpy>("_wmemcpy");
    p.def<&w_wmemcpy>("_wmemmove");
    p.def<&w_wmemset>("_wmemset");
    p.def<&w_wmemcmp>("_wmemcmp");
    p.def<&w_wmemchr>("_wmemchr");
    p.def<&w_towlower>("_towlower");
    p.def<&w_towupper>("_towupper");
    p.def<&w_iswspace>("_iswspace");
    p.def<&w_iswdigit>("_iswdigit");
    p.def<&w_iswalpha>("_iswalpha");
    p.def<&w_iswalnum>("_iswalnum");
    buildRuneLocale(p);
}

} // namespace Hydrogen::Runtime
