// HydrogenVM — GCC 4.2 libstdc++ containers that apps import: the
// copy-on-write std::string, the red-black tree under std::map / std::set,
// and std::list node splicing.
//
// std::string is one pointer to the characters, preceded by a _Rep header
// {size_t length; size_t capacity; int refcount}. refcount 0 = one owner,
// n > 0 = n + 1 owners (copies share the buffer), -1 = "leaked" (someone holds
// a mutable reference/iterator, so it can't be shared). The empty string
// shares _Rep::_S_empty_rep_storage and is never freed. Apps' inline code
// reads and writes these fields directly, so the layout and rules match GCC.
//
// Every mutation here builds a fresh buffer (simple, always correct for COW);
// the app's own inline fast paths may still modify unshared buffers in place.
#include "runtime/Process.hpp"
#include <algorithm>
#include <cstring>

namespace Hydrogen::Runtime {
namespace {

constexpr uint32_t kHdr = 12;
constexpr uint32_t kNpos = 0xFFFFFFFFu;
constexpr uint32_t kMaxSize = 0x3FFFFFFCu;   // ((npos - sizeof(_Rep)) / 1 - 1) / 4

struct Strings { explicit Strings(Process&) {} uint32_t emptyRep = 0; uint32_t emptyWRep = 0; };
uint32_t emptyData(Ctx& x) { return x.proc.service<Strings>().emptyRep + kHdr; }
uint32_t emptyWData(Ctx& x) { return x.proc.service<Strings>().emptyWRep + kHdr; }

[[noreturn]] void outOfRange(const char* where) { throw std::runtime_error(std::string("std::out_of_range: ") + where); }
[[noreturn]] void lengthError(const char* where) { throw std::runtime_error(std::string("std::length_error: ") + where); }

uint32_t len(Ctx& x, uint32_t data) { return data ? x.mem.read<uint32_t>(data - kHdr) : 0; }
std::string bytes(Ctx& x, uint32_t p, uint32_t n) {
    std::string s(n, '\0');
    if (n && p) std::memcpy(s.data(), x.mem.host(p), n);
    return s;
}
std::string value(Ctx& x, GuestPtr self) { const uint32_t d = x.mem.read<uint32_t>(self); return bytes(x, d, len(x, d)); }

uint32_t newRep(Ctx& x, uint32_t capacity) {
    if (capacity > kMaxSize) lengthError("basic_string::_S_create");
    const uint32_t rep = x.proc.heap().alloc(kHdr + capacity + 1);
    if (!rep) throw std::runtime_error("std::string: out of guest memory");
    x.mem.write<uint32_t>(rep, 0);
    x.mem.write<uint32_t>(rep + 4, capacity);
    x.mem.write<uint32_t>(rep + 8, 0);
    x.mem.write<uint8_t>(rep + kHdr, 0);
    return rep;
}
// A new buffer holding `s` (capacity at least `minCap`); returns the data pointer.
uint32_t make(Ctx& x, const std::string& s, uint32_t minCap = 0) {
    if (s.empty() && !minCap) return emptyData(x);
    const uint32_t rep = newRep(x, std::max<uint32_t>(uint32_t(s.size()), minCap));
    x.mem.write<uint32_t>(rep, uint32_t(s.size()));
    if (!s.empty()) std::memcpy(x.mem.host(rep + kHdr), s.data(), s.size());
    x.mem.write<uint8_t>(rep + kHdr + uint32_t(s.size()), 0);
    return rep + kHdr;
}
void dispose(Ctx& x, uint32_t data) {
    if (!data || data == emptyData(x)) return;
    const uint32_t rep = data - kHdr;
    const int32_t rc = int32_t(x.mem.read<uint32_t>(rep + 8));
    if (rc < 0) return;
    if (rc == 0) {
        x.proc.heap().free(rep);
    } else {
        x.mem.write<uint32_t>(rep + 8, uint32_t(rc - 1));
    }
}
// A reference to `data` for a new owner: shares it, or copies a leaked buffer.
uint32_t grab(Ctx& x, uint32_t data) {
    if (!data || data == emptyData(x)) return emptyData(x);
    const int32_t rc = int32_t(x.mem.read<uint32_t>(data - kHdr + 8));
    if (rc < 0) return make(x, bytes(x, data, len(x, data)));
    x.mem.write<uint32_t>(data - kHdr + 8, uint32_t(rc + 1));
    return data;
}
// Replace *self with `s` (the old buffer is released).
void set(Ctx& x, GuestPtr self, const std::string& s, uint32_t minCap = 0) {
    const uint32_t old = x.mem.read<uint32_t>(self);
    x.mem.write<uint32_t>(self, make(x, s, minCap));
    dispose(x, old);
}
uint32_t clip(uint32_t size, uint32_t pos, uint32_t n) { return std::min(n, size - pos); }
void checkPos(uint32_t pos, uint32_t size, const char* where) { if (pos > size) outOfRange(where); }

// ---- constructors / destructor
void c_cstr(Ctx& x, GuestPtr self, GuestPtr s, GuestPtr) {
    x.mem.write<uint32_t>(self, make(x, s ? x.proc.readCString(s) : ""));
}
void c_buf(Ctx& x, GuestPtr self, GuestPtr s, uint32_t n, GuestPtr) { x.mem.write<uint32_t>(self, make(x, bytes(x, s, n))); }
void c_copy(Ctx& x, GuestPtr self, GuestPtr other) { x.mem.write<uint32_t>(self, grab(x, x.mem.read<uint32_t>(other))); }
void c_sub(Ctx& x, GuestPtr self, GuestPtr other, uint32_t pos, uint32_t n) {
    const std::string v = value(x, other);
    checkPos(pos, uint32_t(v.size()), "basic_string::basic_string");
    x.mem.write<uint32_t>(self, make(x, v.substr(pos, clip(uint32_t(v.size()), pos, n))));
}
void d_str(Ctx& x, GuestPtr self) { dispose(x, x.mem.read<uint32_t>(self)); }

// ---- modifiers (all return *this)
uint32_t m_assignBuf(Ctx& x, GuestPtr self, GuestPtr s, uint32_t n) { set(x, self, bytes(x, s, n)); return self; }
uint32_t m_assignStr(Ctx& x, GuestPtr self, GuestPtr other) {
    const uint32_t mine = x.mem.read<uint32_t>(self), theirs = x.mem.read<uint32_t>(other);
    if (mine != theirs) { x.mem.write<uint32_t>(self, grab(x, theirs)); dispose(x, mine); }
    return self;
}
uint32_t m_appendBuf(Ctx& x, GuestPtr self, GuestPtr s, uint32_t n) { std::string v = value(x, self); v += bytes(x, s, n); set(x, self, v); return self; }
uint32_t m_appendStr(Ctx& x, GuestPtr self, GuestPtr other) { std::string v = value(x, self); v += value(x, other); set(x, self, v); return self; }
uint32_t m_appendChars(Ctx& x, GuestPtr self, uint32_t n, uint32_t c) { std::string v = value(x, self); v.append(n, char(c)); set(x, self, v); return self; }
void m_pushBack(Ctx& x, GuestPtr self, uint32_t c) {
    const uint32_t d = x.mem.read<uint32_t>(self);
    const bool empty = d == emptyData(x);
    const uint32_t n = len(x, d);
    const uint32_t cap = empty ? 0 : x.mem.read<uint32_t>(d - kHdr + 4);
    const int32_t ref = empty ? 0 : int32_t(x.mem.read<uint32_t>(d - kHdr + 8));
    if (!empty && ref <= 0 && n + 1 <= cap) {
        // Unshared with room: append in place (and the string becomes sharable again).
        x.mem.write<uint8_t>(d + n, uint8_t(c));
        x.mem.write<uint8_t>(d + n + 1, 0);
        x.mem.write<uint32_t>(d - kHdr, n + 1);
        x.mem.write<uint32_t>(d - kHdr + 8, 0);
        return;
    }
    std::string v = value(x, self);
    v.push_back(char(c));
    // Grow geometrically so repeated push_back stays linear.
    set(x, self, v, std::max<uint32_t>(uint32_t(v.size()), std::min<uint32_t>(cap * 2, kMaxSize)));
}
uint32_t m_insertBuf(Ctx& x, GuestPtr self, uint32_t pos, GuestPtr s, uint32_t n) {
    std::string v = value(x, self);
    checkPos(pos, uint32_t(v.size()), "basic_string::insert");
    v.insert(pos, bytes(x, s, n));
    set(x, self, v);
    return self;
}
uint32_t m_insertSub(Ctx& x, GuestPtr self, uint32_t pos, GuestPtr other, uint32_t pos2, uint32_t n) {
    std::string v = value(x, self);
    const std::string o = value(x, other);
    checkPos(pos, uint32_t(v.size()), "basic_string::insert");
    checkPos(pos2, uint32_t(o.size()), "basic_string::insert");
    v.insert(pos, o.substr(pos2, clip(uint32_t(o.size()), pos2, n)));
    set(x, self, v);
    return self;
}
uint32_t m_erase(Ctx& x, GuestPtr self, uint32_t pos, uint32_t n) {
    std::string v = value(x, self);
    checkPos(pos, uint32_t(v.size()), "basic_string::erase");
    v.erase(pos, clip(uint32_t(v.size()), pos, n));
    set(x, self, v);
    return self;
}
uint32_t m_replace(Ctx& x, GuestPtr self, uint32_t pos, uint32_t n1, GuestPtr s, uint32_t n2) {
    std::string v = value(x, self);
    checkPos(pos, uint32_t(v.size()), "basic_string::replace");
    v.replace(pos, clip(uint32_t(v.size()), pos, n1), bytes(x, s, n2));
    set(x, self, v);
    return self;
}
// _M_replace_aux(pos, n1, n2, c): n1 characters at pos become n2 copies of c.
uint32_t m_replaceAux(Ctx& x, GuestPtr self, uint32_t pos, uint32_t n1, uint32_t n2, uint32_t c) {
    std::string v = value(x, self);
    checkPos(pos, uint32_t(v.size()), "basic_string::_M_replace_aux");
    v.replace(pos, clip(uint32_t(v.size()), pos, n1), std::string(n2, char(c)));
    set(x, self, v);
    return self;
}
// _M_mutate(pos, len1, len2): make room — len1 characters at pos become len2 uninitialised ones.
void m_mutate(Ctx& x, GuestPtr self, uint32_t pos, uint32_t len1, uint32_t len2) {
    std::string v = value(x, self);
    pos = std::min<uint32_t>(pos, uint32_t(v.size()));
    v.replace(pos, clip(uint32_t(v.size()), pos, len1), std::string(len2, '\0'));
    set(x, self, v);
}
void m_resize(Ctx& x, GuestPtr self, uint32_t n, uint32_t c) { std::string v = value(x, self); v.resize(n, char(c)); set(x, self, v); }
void m_resizeN(Ctx& x, GuestPtr self, uint32_t n) { m_resize(x, self, n, 0); }
void m_reserve(Ctx& x, GuestPtr self, uint32_t n) {
    const uint32_t d = x.mem.read<uint32_t>(self);
    const uint32_t cap = d == emptyData(x) ? 0 : x.mem.read<uint32_t>(d - kHdr + 4);
    const int32_t rc = d == emptyData(x) ? 0 : int32_t(x.mem.read<uint32_t>(d - kHdr + 8));
    if (n <= cap && rc <= 0) return;
    const std::string v = value(x, self);
    set(x, self, v, std::max<uint32_t>(n, uint32_t(v.size())));
}
void m_swap(Ctx& x, GuestPtr self, GuestPtr other) {
    for (GuestPtr s : {self, other}) {   // leaked buffers become shareable again
        const uint32_t d = x.mem.read<uint32_t>(s);
        if (d != emptyData(x) && int32_t(x.mem.read<uint32_t>(d - kHdr + 8)) < 0) x.mem.write<uint32_t>(d - kHdr + 8, 0);
    }
    const uint32_t a = x.mem.read<uint32_t>(self), b = x.mem.read<uint32_t>(other);
    x.mem.write<uint32_t>(self, b);
    x.mem.write<uint32_t>(other, a);
}
// _M_leak_hard(): give *this its own buffer and mark it unshareable.
void m_leakHard(Ctx& x, GuestPtr self) {
    uint32_t d = x.mem.read<uint32_t>(self);
    if (d == emptyData(x)) return;
    if (int32_t(x.mem.read<uint32_t>(d - kHdr + 8)) > 0) { set(x, self, value(x, self)); d = x.mem.read<uint32_t>(self); }
    if (d != emptyData(x)) x.mem.write<uint32_t>(d - kHdr + 8, uint32_t(-1));
}

// ---- constructors / assignment / accessors added for libstdc++-heavy apps
void c_default(Ctx& x, GuestPtr self) { x.mem.write<uint32_t>(self, emptyData(x)); }
void c_fill(Ctx& x, GuestPtr self, uint32_t n, uint32_t c, GuestPtr) { x.mem.write<uint32_t>(self, make(x, std::string(n, char(c)))); }
uint32_t m_assignC(Ctx& x, GuestPtr self, GuestPtr s) { set(x, self, x.proc.readCString(s)); return self; }
uint32_t m_assignChar(Ctx& x, GuestPtr self, uint32_t c) { set(x, self, std::string(1, char(c))); return self; }
uint32_t m_appendC(Ctx& x, GuestPtr self, GuestPtr s) { std::string v = value(x, self); v += x.proc.readCString(s); set(x, self, v); return self; }
uint32_t m_plusC(Ctx& x, GuestPtr self, GuestPtr s) { return m_appendC(x, self, s); }
uint32_t m_plusStr(Ctx& x, GuestPtr self, GuestPtr o) { std::string v = value(x, self); v += value(x, o); set(x, self, v); return self; }
uint32_t m_plusChar(Ctx& x, GuestPtr self, uint32_t c) { std::string v = value(x, self); v.push_back(char(c)); set(x, self, v); return self; }
uint32_t m_replaceStr(Ctx& x, GuestPtr self, uint32_t pos, uint32_t n, GuestPtr o) {
    std::string v = value(x, self);
    const uint32_t size = uint32_t(v.size());
    if (pos > size) outOfRange("basic_string::replace");
    v.replace(pos, clip(size, pos, n), value(x, o));
    set(x, self, v);
    return self;
}
uint32_t q_size(Ctx& x, GuestPtr self) { return len(x, x.mem.read<uint32_t>(self)); }
uint32_t q_empty(Ctx& x, GuestPtr self) { return len(x, x.mem.read<uint32_t>(self)) == 0; }
uint32_t q_cstr(Ctx& x, GuestPtr self) { return x.mem.read<uint32_t>(self); }   // data pointer (NUL-terminated)
// begin()/end(): iterators are char*. const versions return the shared buffer; the mutable
// versions leak the string first (the caller may write through the iterator).
uint32_t q_beginConst(Ctx& x, GuestPtr self) { return x.mem.read<uint32_t>(self); }
uint32_t q_endConst(Ctx& x, GuestPtr self) { const uint32_t d = x.mem.read<uint32_t>(self); return d + len(x, d); }
uint32_t q_begin(Ctx& x, GuestPtr self) { m_leakHard(x, self); return x.mem.read<uint32_t>(self); }
uint32_t q_end(Ctx& x, GuestPtr self) { m_leakHard(x, self); const uint32_t d = x.mem.read<uint32_t>(self); return d + len(x, d); }
uint32_t q_at(Ctx& x, GuestPtr self, uint32_t i) {
    const uint32_t d = x.mem.read<uint32_t>(self);
    if (i >= len(x, d)) outOfRange("basic_string::at");
    return d + i;
}
uint32_t q_atConst(Ctx& x, GuestPtr self, uint32_t i) { return x.mem.read<uint32_t>(self) + i; }   // operator[] has no bounds check
uint32_t q_findCStr(Ctx& x, GuestPtr self, GuestPtr s, uint32_t pos) {
    const size_t r = value(x, self).find(x.proc.readCString(s), pos);
    return r == std::string::npos ? kNpos : uint32_t(r);
}
uint32_t q_findStr(Ctx& x, GuestPtr self, GuestPtr o, uint32_t pos) {
    const size_t r = value(x, self).find(value(x, o), pos);
    return r == std::string::npos ? kNpos : uint32_t(r);
}
uint32_t q_rfindCStr(Ctx& x, GuestPtr self, GuestPtr s, uint32_t pos) {
    const size_t r = value(x, self).rfind(x.proc.readCString(s), pos == kNpos ? std::string::npos : pos);
    return r == std::string::npos ? kNpos : uint32_t(r);
}
uint32_t q_findStrDefault(Ctx& x, GuestPtr self, GuestPtr o) { return q_findStr(x, self, o, 0); }

// ---- queries
uint32_t q_findBuf(Ctx& x, GuestPtr self, GuestPtr s, uint32_t pos, uint32_t n) {
    const size_t r = value(x, self).find(bytes(x, s, n), pos);
    return r == std::string::npos ? kNpos : uint32_t(r);
}
uint32_t q_findChar(Ctx& x, GuestPtr self, uint32_t c, uint32_t pos) {
    const size_t r = value(x, self).find(char(c), pos);
    return r == std::string::npos ? kNpos : uint32_t(r);
}
uint32_t q_rfindBuf(Ctx& x, GuestPtr self, GuestPtr s, uint32_t pos, uint32_t n) {
    const size_t r = value(x, self).rfind(bytes(x, s, n), pos == kNpos ? std::string::npos : pos);
    return r == std::string::npos ? kNpos : uint32_t(r);
}
uint32_t q_rfindChar(Ctx& x, GuestPtr self, uint32_t c, uint32_t pos) {
    const size_t r = value(x, self).rfind(char(c), pos == kNpos ? std::string::npos : pos);
    return r == std::string::npos ? kNpos : uint32_t(r);
}
int32_t q_compareC(Ctx& x, GuestPtr self, GuestPtr s) { const int r = value(x, self).compare(x.proc.readCString(s)); return r < 0 ? -1 : r > 0; }
int32_t q_compareStr(Ctx& x, GuestPtr self, GuestPtr other) { const int r = value(x, self).compare(value(x, other)); return r < 0 ? -1 : r > 0; }
int32_t q_compareSubStr(Ctx& x, GuestPtr self, uint32_t pos, uint32_t n, GuestPtr other) {
    const std::string v = value(x, self);
    checkPos(pos, uint32_t(v.size()), "basic_string::compare");
    const int r = v.compare(pos, clip(uint32_t(v.size()), pos, n), value(x, other));
    return r < 0 ? -1 : r > 0;
}
int32_t q_compareSubC(Ctx& x, GuestPtr self, uint32_t pos, uint32_t n, GuestPtr s) {
    const std::string v = value(x, self);
    checkPos(pos, uint32_t(v.size()), "basic_string::compare");
    const int r = v.compare(pos, clip(uint32_t(v.size()), pos, n), x.proc.readCString(s));
    return r < 0 ? -1 : r > 0;
}
uint32_t m_assignSub(Ctx& x, GuestPtr self, GuestPtr other, uint32_t pos, uint32_t n) {
    const std::string o = value(x, other);
    checkPos(pos, uint32_t(o.size()), "basic_string::assign");
    set(x, self, o.substr(pos, clip(uint32_t(o.size()), pos, n)));
    return self;
}
uint32_t q_findFirstOfBuf(Ctx& x, GuestPtr self, GuestPtr s, uint32_t pos, uint32_t n) {
    const size_t r = value(x, self).find_first_of(bytes(x, s, n), pos);
    return r == std::string::npos ? kNpos : uint32_t(r);
}
uint32_t q_findLastOfBuf(Ctx& x, GuestPtr self, GuestPtr s, uint32_t pos, uint32_t n) {
    const size_t r = value(x, self).find_last_of(bytes(x, s, n), pos == kNpos ? std::string::npos : pos);
    return r == std::string::npos ? kNpos : uint32_t(r);
}
uint32_t q_findFirstNotOfBuf(Ctx& x, GuestPtr self, GuestPtr s, uint32_t pos, uint32_t n) {
    const size_t r = value(x, self).find_first_not_of(bytes(x, s, n), pos);
    return r == std::string::npos ? kNpos : uint32_t(r);
}
uint32_t q_findLastNotOfBuf(Ctx& x, GuestPtr self, GuestPtr s, uint32_t pos, uint32_t n) {
    const size_t r = value(x, self).find_last_not_of(bytes(x, s, n), pos == kNpos ? std::string::npos : pos);
    return r == std::string::npos ? kNpos : uint32_t(r);
}
// std::string substr(pos, n) const — returned through r0; this in r1.
void q_substr(Ctx& x) {
    const uint32_t ret = x.cpu.r[0], self = x.cpu.r[1], pos = x.cpu.r[2], n = x.cpu.r[3];
    const std::string v = value(x, self);
    checkPos(pos, uint32_t(v.size()), "basic_string::substr");
    x.mem.write<uint32_t>(ret, make(x, v.substr(pos, clip(uint32_t(v.size()), pos, n))));
    x.cpu.r[0] = ret;
}

// ---- _Rep
// _Rep* _Rep::_S_create(size_type capacity, size_type old_capacity, const allocator&)
uint32_t r_create(Ctx& x, uint32_t cap, uint32_t oldCap, GuestPtr) {
    if (cap > oldCap && cap < 2 * oldCap) cap = 2 * oldCap;
    return newRep(x, cap);
}
void r_destroy(Ctx& x, GuestPtr rep, GuestPtr) { if (rep && rep + kHdr != emptyData(x) && rep + kHdr != emptyWData(x)) x.proc.heap().free(rep); }
void r_dispose(Ctx& x, GuestPtr rep, GuestPtr) {
    if (!rep) return;
    dispose(x, rep + kHdr);
}
uint32_t r_refcopy(Ctx& x, GuestPtr rep) {
    if (rep) {
        const int32_t rc = int32_t(x.mem.read<uint32_t>(rep + 8));
        if (rc >= 0) x.mem.write<uint32_t>(rep + 8, uint32_t(rc + 1));
    }
    return rep + kHdr;
}
uint32_t r_refdata(Ctx& x, GuestPtr rep) { return rep ? rep + kHdr : 0; }
// _Rep::_M_set_length_and_sharable(n): inline in libstdc++, out of line in some
// builds (Army of Darkness Defense). The shared empty rep is never written.
template <uint32_t CharSize> void r_setLengthAndSharable(Ctx& x, GuestPtr rep, uint32_t n) {
    if (!rep) return;
    const uint32_t empty = CharSize == 1 ? emptyData(x) - kHdr : emptyWData(x) - kHdr;
    if (rep == empty) return;
    x.mem.write<uint32_t>(rep + 8, 0);   // sharable
    x.mem.write<uint32_t>(rep, n);       // length
    if (CharSize == 1) x.mem.write<uint8_t>(rep + kHdr + n, 0);
    else x.mem.write<uint32_t>(rep + kHdr + n * 4, 0);
}
template <uint32_t CharSize> void r_setSharable(Ctx& x, GuestPtr rep) {
    const uint32_t empty = CharSize == 1 ? emptyData(x) - kHdr : emptyWData(x) - kHdr;
    if (rep && rep != empty) x.mem.write<uint32_t>(rep + 8, 0);
}
template <uint32_t CharSize> void r_setLeaked(Ctx& x, GuestPtr rep) {
    const uint32_t empty = CharSize == 1 ? emptyData(x) - kHdr : emptyWData(x) - kHdr;
    if (rep && rep != empty) x.mem.write<uint32_t>(rep + 8, 0xFFFFFFFFu);
}
uint32_t r_emptyRep(Ctx& x) { return x.proc.service<Strings>().emptyRep; }

// --------------------------------------------------------------- std::wstring
// std::basic_string<wchar_t, std::char_traits<wchar_t>, std::allocator<wchar_t>>
// On 32-bit Darwin ARM, sizeof(wchar_t) == 4. _Rep header is 12 bytes
// {length_in_wchars, capacity_in_wchars, refcount} followed by (capacity + 1) * 4 bytes.
constexpr uint32_t kWMaxSize = 0x0FFFFFFCu;

std::u32string wchars(Ctx& x, uint32_t p, uint32_t n) {
    std::u32string s(n, U'\0');
    for (uint32_t i = 0; i < n; ++i) s[i] = char32_t(x.mem.read<uint32_t>(p + i * 4));
    return s;
}
std::u32string readWCString(Ctx& x, uint32_t p) {
    std::u32string s;
    if (!p) return s;
    for (uint32_t i = 0; i < (1u << 20); ++i) {
        const uint32_t c = x.mem.read<uint32_t>(p + i * 4);
        if (!c) break;
        s.push_back(char32_t(c));
    }
    return s;
}
std::u32string wvalue(Ctx& x, GuestPtr self) {
    const uint32_t d = x.mem.read<uint32_t>(self);
    return (!d || d == emptyWData(x)) ? std::u32string{} : wchars(x, d, len(x, d));
}
uint32_t newWRep(Ctx& x, uint32_t capacity) {
    if (capacity > kWMaxSize) lengthError("basic_string<wchar_t>::_S_create");
    const uint32_t rep = x.proc.heap().alloc(kHdr + (capacity + 1) * 4);
    if (!rep) throw std::runtime_error("std::wstring: out of guest memory");
    x.mem.write<uint32_t>(rep, 0);
    x.mem.write<uint32_t>(rep + 4, capacity);
    x.mem.write<uint32_t>(rep + 8, 0);
    x.mem.write<uint32_t>(rep + kHdr, 0);
    return rep;
}
uint32_t wmake(Ctx& x, const std::u32string& s, uint32_t minCap = 0) {
    if (s.empty() && !minCap) return emptyWData(x);
    const uint32_t rep = newWRep(x, std::max<uint32_t>(uint32_t(s.size()), minCap));
    x.mem.write<uint32_t>(rep, uint32_t(s.size()));
    for (size_t i = 0; i < s.size(); ++i) x.mem.write<uint32_t>(rep + kHdr + uint32_t(i * 4), uint32_t(s[i]));
    x.mem.write<uint32_t>(rep + kHdr + uint32_t(s.size() * 4), 0);
    return rep + kHdr;
}
void wdispose(Ctx& x, uint32_t data) {
    if (!data || data == emptyWData(x)) return;
    const uint32_t rep = data - kHdr;
    const int32_t rc = int32_t(x.mem.read<uint32_t>(rep + 8));
    if (rc < 0) return;
    if (rc == 0) {
        x.proc.heap().free(rep);
    } else {
        x.mem.write<uint32_t>(rep + 8, uint32_t(rc - 1));
    }
}
uint32_t wgrab(Ctx& x, uint32_t data) {
    if (!data || data == emptyWData(x)) return emptyWData(x);
    const int32_t rc = int32_t(x.mem.read<uint32_t>(data - kHdr + 8));
    if (rc < 0) return wmake(x, wchars(x, data, len(x, data)));
    x.mem.write<uint32_t>(data - kHdr + 8, uint32_t(rc + 1));
    return data;
}
void wset(Ctx& x, GuestPtr self, const std::u32string& s, uint32_t minCap = 0) {
    const uint32_t old = x.mem.read<uint32_t>(self);
    x.mem.write<uint32_t>(self, wmake(x, s, minCap));
    wdispose(x, old);
}

void wc_default(Ctx& x, GuestPtr self) { x.mem.write<uint32_t>(self, emptyWData(x)); }
void wc_fill(Ctx& x, GuestPtr self, uint32_t n, uint32_t c, GuestPtr) { x.mem.write<uint32_t>(self, wmake(x, std::u32string(n, char32_t(c)))); }
void wc_cstr(Ctx& x, GuestPtr self, GuestPtr s, GuestPtr) { x.mem.write<uint32_t>(self, wmake(x, readWCString(x, s))); }
void wc_buf(Ctx& x, GuestPtr self, GuestPtr s, uint32_t n, GuestPtr) { x.mem.write<uint32_t>(self, wmake(x, wchars(x, s, n))); }
void wc_copy(Ctx& x, GuestPtr self, GuestPtr other) { x.mem.write<uint32_t>(self, wgrab(x, x.mem.read<uint32_t>(other))); }
void wc_sub(Ctx& x, GuestPtr self, GuestPtr other, uint32_t pos, uint32_t n) {
    const std::u32string v = wvalue(x, other);
    checkPos(pos, uint32_t(v.size()), "basic_string<wchar_t>::basic_string");
    x.mem.write<uint32_t>(self, wmake(x, v.substr(pos, clip(uint32_t(v.size()), pos, n))));
}
void wd_str(Ctx& x, GuestPtr self) { wdispose(x, x.mem.read<uint32_t>(self)); }

uint32_t wm_assignBuf(Ctx& x, GuestPtr self, GuestPtr s, uint32_t n) { wset(x, self, wchars(x, s, n)); return self; }
uint32_t wm_assignC(Ctx& x, GuestPtr self, GuestPtr s) { wset(x, self, readWCString(x, s)); return self; }
uint32_t wm_assignStr(Ctx& x, GuestPtr self, GuestPtr other) {
    const uint32_t mine = x.mem.read<uint32_t>(self), theirs = x.mem.read<uint32_t>(other);
    if (mine != theirs) { x.mem.write<uint32_t>(self, wgrab(x, theirs)); wdispose(x, mine); }
    return self;
}
uint32_t wm_appendBuf(Ctx& x, GuestPtr self, GuestPtr s, uint32_t n) { std::u32string v = wvalue(x, self); v += wchars(x, s, n); wset(x, self, v); return self; }
uint32_t wm_appendC(Ctx& x, GuestPtr self, GuestPtr s) { std::u32string v = wvalue(x, self); v += readWCString(x, s); wset(x, self, v); return self; }
uint32_t wm_appendStr(Ctx& x, GuestPtr self, GuestPtr other) { std::u32string v = wvalue(x, self); v += wvalue(x, other); wset(x, self, v); return self; }
uint32_t wm_appendChars(Ctx& x, GuestPtr self, uint32_t n, uint32_t c) { std::u32string v = wvalue(x, self); v.append(n, char32_t(c)); wset(x, self, v); return self; }
void wm_pushBack(Ctx& x, GuestPtr self, uint32_t c) {
    std::u32string v = wvalue(x, self);
    v.push_back(char32_t(c));
    wset(x, self, v);
}
uint32_t wm_insertBuf(Ctx& x, GuestPtr self, uint32_t pos, GuestPtr s, uint32_t n) {
    std::u32string v = wvalue(x, self);
    checkPos(pos, uint32_t(v.size()), "basic_string<wchar_t>::insert");
    v.insert(pos, wchars(x, s, n));
    wset(x, self, v);
    return self;
}
uint32_t wm_insertSub(Ctx& x, GuestPtr self, uint32_t pos, GuestPtr other, uint32_t pos2, uint32_t n) {
    std::u32string v = wvalue(x, self);
    const std::u32string o = wvalue(x, other);
    checkPos(pos, uint32_t(v.size()), "basic_string<wchar_t>::insert");
    checkPos(pos2, uint32_t(o.size()), "basic_string<wchar_t>::insert");
    v.insert(pos, o.substr(pos2, clip(uint32_t(o.size()), pos2, n)));
    wset(x, self, v);
    return self;
}
uint32_t wm_erase(Ctx& x, GuestPtr self, uint32_t pos, uint32_t n) {
    std::u32string v = wvalue(x, self);
    checkPos(pos, uint32_t(v.size()), "basic_string<wchar_t>::erase");
    v.erase(pos, clip(uint32_t(v.size()), pos, n));
    wset(x, self, v);
    return self;
}
uint32_t wm_replace(Ctx& x, GuestPtr self, uint32_t pos, uint32_t n1, GuestPtr s, uint32_t n2) {
    std::u32string v = wvalue(x, self);
    checkPos(pos, uint32_t(v.size()), "basic_string<wchar_t>::replace");
    v.replace(pos, clip(uint32_t(v.size()), pos, n1), wchars(x, s, n2));
    wset(x, self, v);
    return self;
}
void wm_mutate(Ctx& x, GuestPtr self, uint32_t pos, uint32_t len1, uint32_t len2) {
    std::u32string v = wvalue(x, self);
    pos = std::min<uint32_t>(pos, uint32_t(v.size()));
    v.replace(pos, clip(uint32_t(v.size()), pos, len1), std::u32string(len2, U'\0'));
    wset(x, self, v);
}
void wm_resize(Ctx& x, GuestPtr self, uint32_t n, uint32_t c) { std::u32string v = wvalue(x, self); v.resize(n, char32_t(c)); wset(x, self, v); }
void wm_resizeN(Ctx& x, GuestPtr self, uint32_t n) { wm_resize(x, self, n, 0); }
void wm_reserve(Ctx& x, GuestPtr self, uint32_t n) {
    const uint32_t d = x.mem.read<uint32_t>(self);
    const uint32_t cap = d == emptyWData(x) ? 0 : x.mem.read<uint32_t>(d - kHdr + 4);
    const int32_t rc = d == emptyWData(x) ? 0 : int32_t(x.mem.read<uint32_t>(d - kHdr + 8));
    if (n <= cap && rc <= 0) return;
    const std::u32string v = wvalue(x, self);
    wset(x, self, v, std::max<uint32_t>(n, uint32_t(v.size())));
}
void wm_leakHard(Ctx& x, GuestPtr self) {
    uint32_t d = x.mem.read<uint32_t>(self);
    if (d == emptyWData(x)) return;
    if (int32_t(x.mem.read<uint32_t>(d - kHdr + 8)) > 0) { wset(x, self, wvalue(x, self)); d = x.mem.read<uint32_t>(self); }
    if (d != emptyWData(x)) x.mem.write<uint32_t>(d - kHdr + 8, uint32_t(-1));
}
uint32_t wq_beginConst(Ctx& x, GuestPtr self) { return x.mem.read<uint32_t>(self); }
uint32_t wq_endConst(Ctx& x, GuestPtr self) { const uint32_t d = x.mem.read<uint32_t>(self); return d + len(x, d) * 4; }
uint32_t wq_begin(Ctx& x, GuestPtr self) { wm_leakHard(x, self); return x.mem.read<uint32_t>(self); }
uint32_t wq_end(Ctx& x, GuestPtr self) { wm_leakHard(x, self); const uint32_t d = x.mem.read<uint32_t>(self); return d + len(x, d) * 4; }
uint32_t wq_findBuf(Ctx& x, GuestPtr self, GuestPtr s, uint32_t pos, uint32_t n) {
    const size_t r = wvalue(x, self).find(wchars(x, s, n), pos);
    return r == std::u32string::npos ? kNpos : uint32_t(r);
}
uint32_t wq_rfindBuf(Ctx& x, GuestPtr self, GuestPtr s, uint32_t pos, uint32_t n) {
    const size_t r = wvalue(x, self).rfind(wchars(x, s, n), pos == kNpos ? std::u32string::npos : pos);
    return r == std::u32string::npos ? kNpos : uint32_t(r);
}
uint32_t wq_findChar(Ctx& x, GuestPtr self, uint32_t c, uint32_t pos) {
    const size_t r = wvalue(x, self).find(char32_t(c), pos);
    return r == std::u32string::npos ? kNpos : uint32_t(r);
}
uint32_t wq_rfindChar(Ctx& x, GuestPtr self, uint32_t c, uint32_t pos) {
    const size_t r = wvalue(x, self).rfind(char32_t(c), pos == kNpos ? std::u32string::npos : pos);
    return r == std::u32string::npos ? kNpos : uint32_t(r);
}
uint32_t wq_findFirstOfBuf(Ctx& x, GuestPtr self, GuestPtr s, uint32_t pos, uint32_t n) {
    const size_t r = wvalue(x, self).find_first_of(wchars(x, s, n), pos);
    return r == std::u32string::npos ? kNpos : uint32_t(r);
}
uint32_t wq_findLastOfBuf(Ctx& x, GuestPtr self, GuestPtr s, uint32_t pos, uint32_t n) {
    const size_t r = wvalue(x, self).find_last_of(wchars(x, s, n), pos == kNpos ? std::u32string::npos : pos);
    return r == std::u32string::npos ? kNpos : uint32_t(r);
}
uint32_t wq_findFirstNotOfBuf(Ctx& x, GuestPtr self, GuestPtr s, uint32_t pos, uint32_t n) {
    const size_t r = wvalue(x, self).find_first_not_of(wchars(x, s, n), pos);
    return r == std::u32string::npos ? kNpos : uint32_t(r);
}
uint32_t wq_findLastNotOfBuf(Ctx& x, GuestPtr self, GuestPtr s, uint32_t pos, uint32_t n) {
    const size_t r = wvalue(x, self).find_last_not_of(wchars(x, s, n), pos == kNpos ? std::u32string::npos : pos);
    return r == std::u32string::npos ? kNpos : uint32_t(r);
}
int32_t wq_compareC(Ctx& x, GuestPtr self, GuestPtr s) { const int r = wvalue(x, self).compare(readWCString(x, s)); return r < 0 ? -1 : r > 0; }
int32_t wq_compareStr(Ctx& x, GuestPtr self, GuestPtr other) { const int r = wvalue(x, self).compare(wvalue(x, other)); return r < 0 ? -1 : r > 0; }
void wq_substr(Ctx& x) {
    const uint32_t ret = x.cpu.r[0], self = x.cpu.r[1], pos = x.cpu.r[2], n = x.cpu.r[3];
    const std::u32string v = wvalue(x, self);
    checkPos(pos, uint32_t(v.size()), "basic_string<wchar_t>::substr");
    x.mem.write<uint32_t>(ret, wmake(x, v.substr(pos, clip(uint32_t(v.size()), pos, n))));
    x.cpu.r[0] = ret;
}
uint32_t wr_create(Ctx& x, uint32_t cap, uint32_t oldCap, GuestPtr) {
    if (cap > oldCap && cap < 2 * oldCap) cap = 2 * oldCap;
    return newWRep(x, cap);
}
void wr_destroy(Ctx& x, GuestPtr rep, GuestPtr) { if (rep && rep + kHdr != emptyWData(x)) x.proc.heap().free(rep); }
void wr_dispose(Ctx& x, GuestPtr rep, GuestPtr) {
    if (!rep) return;
    wdispose(x, rep + kHdr);
}
uint32_t wr_emptyRep(Ctx& x) { return x.proc.service<Strings>().emptyWRep; }

// ---------------------------------------------------------------- std::list
// _List_node_base { next; prev; }
void l_hook(Ctx& x, GuestPtr self, GuestPtr pos) {
    const uint32_t prev = x.mem.read<uint32_t>(pos + 4);
    x.mem.write<uint32_t>(self, pos);
    x.mem.write<uint32_t>(self + 4, prev);
    x.mem.write<uint32_t>(prev, self);
    x.mem.write<uint32_t>(pos + 4, self);
}
void l_unhook(Ctx& x, GuestPtr self) {
    const uint32_t next = x.mem.read<uint32_t>(self), prev = x.mem.read<uint32_t>(self + 4);
    x.mem.write<uint32_t>(prev, next);
    x.mem.write<uint32_t>(next + 4, prev);
}
// Moves [first, last) in front of this.
void l_transfer(Ctx& x, GuestPtr self, GuestPtr first, GuestPtr last) {
    if (self == last) return;
    auto next = [&](uint32_t n) { return x.mem.read<uint32_t>(n); };
    auto prev = [&](uint32_t n) { return x.mem.read<uint32_t>(n + 4); };
    auto setNext = [&](uint32_t n, uint32_t v) { x.mem.write<uint32_t>(n, v); };
    auto setPrev = [&](uint32_t n, uint32_t v) { x.mem.write<uint32_t>(n + 4, v); };
    setNext(prev(last), self);
    setNext(prev(first), last);
    setNext(prev(self), first);
    const uint32_t tmp = prev(self);
    setPrev(self, prev(last));
    setPrev(last, prev(first));
    setPrev(first, tmp);
    (void)next;
}
// static void swap(_List_node_base& a, _List_node_base& b): exchanges two lists' contents.
void l_swap(Ctx& x, GuestPtr a, GuestPtr b) {
    auto next = [&](uint32_t n) { return x.mem.read<uint32_t>(n); };
    auto prev = [&](uint32_t n) { return x.mem.read<uint32_t>(n + 4); };
    auto link = [&](uint32_t head) {   // make the first and last nodes point back at head
        x.mem.write<uint32_t>(next(head) + 4, head);
        x.mem.write<uint32_t>(prev(head), head);
    };
    auto clear = [&](uint32_t head) { x.mem.write<uint32_t>(head, head); x.mem.write<uint32_t>(head + 4, head); };
    const bool aHas = next(a) != a, bHas = next(b) != b;
    if (aHas && bHas) {
        const uint32_t an = next(a), ap = prev(a);
        x.mem.write<uint32_t>(a, next(b)); x.mem.write<uint32_t>(a + 4, prev(b));
        x.mem.write<uint32_t>(b, an); x.mem.write<uint32_t>(b + 4, ap);
        link(a); link(b);
    } else if (aHas) {
        x.mem.write<uint32_t>(b, next(a)); x.mem.write<uint32_t>(b + 4, prev(a));
        link(b); clear(a);
    } else if (bHas) {
        x.mem.write<uint32_t>(a, next(b)); x.mem.write<uint32_t>(a + 4, prev(b));
        link(a); clear(b);
    }
}

// ----------------------------------------------------------- red-black tree
// _Rb_tree_node_base { int color (red 0, black 1); parent; left; right; }
// The header node: parent = root, left = leftmost, right = rightmost.
struct Tree {
    Ctx& x;
    static constexpr uint32_t kRed = 0, kBlack = 1;
    uint32_t color(uint32_t n) const { return x.mem.read<uint32_t>(n); }
    uint32_t parent(uint32_t n) const { return x.mem.read<uint32_t>(n + 4); }
    uint32_t left(uint32_t n) const { return x.mem.read<uint32_t>(n + 8); }
    uint32_t right(uint32_t n) const { return x.mem.read<uint32_t>(n + 12); }
    void setColor(uint32_t n, uint32_t v) { x.mem.write<uint32_t>(n, v); }
    void setParent(uint32_t n, uint32_t v) { x.mem.write<uint32_t>(n + 4, v); }
    void setLeft(uint32_t n, uint32_t v) { x.mem.write<uint32_t>(n + 8, v); }
    void setRight(uint32_t n, uint32_t v) { x.mem.write<uint32_t>(n + 12, v); }
    uint32_t minimum(uint32_t n) const { while (left(n)) n = left(n); return n; }
    uint32_t maximum(uint32_t n) const { while (right(n)) n = right(n); return n; }

    // `root` is the header's parent field (address header + 4).
    void rotateLeft(uint32_t n, uint32_t rootRef) {
        const uint32_t y = right(n);
        setRight(n, left(y));
        if (left(y)) setParent(left(y), n);
        setParent(y, parent(n));
        if (n == x.mem.read<uint32_t>(rootRef)) x.mem.write<uint32_t>(rootRef, y);
        else if (n == left(parent(n))) setLeft(parent(n), y);
        else setRight(parent(n), y);
        setLeft(y, n);
        setParent(n, y);
    }
    void rotateRight(uint32_t n, uint32_t rootRef) {
        const uint32_t y = left(n);
        setLeft(n, right(y));
        if (right(y)) setParent(right(y), n);
        setParent(y, parent(n));
        if (n == x.mem.read<uint32_t>(rootRef)) x.mem.write<uint32_t>(rootRef, y);
        else if (n == right(parent(n))) setRight(parent(n), y);
        else setLeft(parent(n), y);
        setRight(y, n);
        setParent(n, y);
    }
};

uint32_t t_increment(Ctx& x, GuestPtr n) {
    Tree t{x};
    if (t.right(n)) return t.minimum(t.right(n));
    uint32_t y = t.parent(n);
    while (n == t.right(y)) { n = y; y = t.parent(y); }
    return t.right(n) != y ? y : n;
}
uint32_t t_decrement(Ctx& x, GuestPtr n) {
    Tree t{x};
    if (t.color(n) == Tree::kRed && t.parent(t.parent(n)) == n) return t.right(n);   // header -> rightmost
    if (t.left(n)) return t.maximum(t.left(n));
    uint32_t y = t.parent(n);
    while (n == t.left(y)) { n = y; y = t.parent(y); }
    return y;
}

void t_insertRebalance(Ctx& x, int32_t insertLeft, GuestPtr n, GuestPtr p, GuestPtr header) {
    Tree t{x};
    const uint32_t rootRef = header + 4;
    t.setParent(n, p);
    t.setLeft(n, 0);
    t.setRight(n, 0);
    t.setColor(n, Tree::kRed);
    if (insertLeft & 0xFF) {
        t.setLeft(p, n);   // also makes leftmost = n when p == header
        if (p == header) { x.mem.write<uint32_t>(rootRef, n); t.setRight(header, n); }
        else if (p == t.left(header)) t.setLeft(header, n);
    } else {
        t.setRight(p, n);
        if (p == t.right(header)) t.setRight(header, n);
    }
    while (n != x.mem.read<uint32_t>(rootRef) && t.color(t.parent(n)) == Tree::kRed) {
        const uint32_t xpp = t.parent(t.parent(n));
        if (t.parent(n) == t.left(xpp)) {
            const uint32_t y = t.right(xpp);
            if (y && t.color(y) == Tree::kRed) {
                t.setColor(t.parent(n), Tree::kBlack); t.setColor(y, Tree::kBlack); t.setColor(xpp, Tree::kRed);
                n = xpp;
            } else {
                if (n == t.right(t.parent(n))) { n = t.parent(n); t.rotateLeft(n, rootRef); }
                t.setColor(t.parent(n), Tree::kBlack); t.setColor(xpp, Tree::kRed);
                t.rotateRight(xpp, rootRef);
            }
        } else {
            const uint32_t y = t.left(xpp);
            if (y && t.color(y) == Tree::kRed) {
                t.setColor(t.parent(n), Tree::kBlack); t.setColor(y, Tree::kBlack); t.setColor(xpp, Tree::kRed);
                n = xpp;
            } else {
                if (n == t.left(t.parent(n))) { n = t.parent(n); t.rotateRight(n, rootRef); }
                t.setColor(t.parent(n), Tree::kBlack); t.setColor(xpp, Tree::kRed);
                t.rotateLeft(xpp, rootRef);
            }
        }
    }
    t.setColor(x.mem.read<uint32_t>(rootRef), Tree::kBlack);
}

// Unlinks z, rebalances, and returns the node to free (always z).
uint32_t t_rebalanceForErase(Ctx& x, GuestPtr z, GuestPtr header) {
    Tree t{x};
    const uint32_t rootRef = header + 4, leftRef = header + 8, rightRef = header + 12;
    auto rd = [&](uint32_t a) { return x.mem.read<uint32_t>(a); };
    auto wr = [&](uint32_t a, uint32_t v) { x.mem.write<uint32_t>(a, v); };
    uint32_t y = z, xn = 0, xParent = 0;
    if (!t.left(y)) xn = t.right(y);
    else if (!t.right(y)) xn = t.left(y);
    else { y = t.minimum(t.right(y)); xn = t.right(y); }
    if (y != z) {
        // z has two children: y is z's successor; relink y in z's place.
        t.setParent(t.left(z), y);
        t.setLeft(y, t.left(z));
        if (y != t.right(z)) {
            xParent = t.parent(y);
            if (xn) t.setParent(xn, t.parent(y));
            t.setLeft(t.parent(y), xn);
            t.setRight(y, t.right(z));
            t.setParent(t.right(z), y);
        } else {
            xParent = y;
        }
        if (rd(rootRef) == z) wr(rootRef, y);
        else if (t.left(t.parent(z)) == z) t.setLeft(t.parent(z), y);
        else t.setRight(t.parent(z), y);
        t.setParent(y, t.parent(z));
        const uint32_t c = t.color(y); t.setColor(y, t.color(z)); t.setColor(z, c);
        y = z;
    } else {
        xParent = t.parent(y);
        if (xn) t.setParent(xn, t.parent(y));
        if (rd(rootRef) == z) wr(rootRef, xn);
        else if (t.left(t.parent(z)) == z) t.setLeft(t.parent(z), xn);
        else t.setRight(t.parent(z), xn);
        if (rd(leftRef) == z) wr(leftRef, !t.right(z) ? t.parent(z) : t.minimum(xn));
        if (rd(rightRef) == z) wr(rightRef, !t.left(z) ? t.parent(z) : t.maximum(xn));
    }
    if (t.color(y) != Tree::kRed) {
        while (xn != rd(rootRef) && (!xn || t.color(xn) == Tree::kBlack)) {
            if (xn == t.left(xParent)) {
                uint32_t w = t.right(xParent);
                if (t.color(w) == Tree::kRed) {
                    t.setColor(w, Tree::kBlack); t.setColor(xParent, Tree::kRed);
                    t.rotateLeft(xParent, rootRef);
                    w = t.right(xParent);
                }
                if ((!t.left(w) || t.color(t.left(w)) == Tree::kBlack) && (!t.right(w) || t.color(t.right(w)) == Tree::kBlack)) {
                    t.setColor(w, Tree::kRed);
                    xn = xParent;
                    xParent = t.parent(xParent);
                } else {
                    if (!t.right(w) || t.color(t.right(w)) == Tree::kBlack) {
                        t.setColor(t.left(w), Tree::kBlack); t.setColor(w, Tree::kRed);
                        t.rotateRight(w, rootRef);
                        w = t.right(xParent);
                    }
                    t.setColor(w, t.color(xParent)); t.setColor(xParent, Tree::kBlack);
                    if (t.right(w)) t.setColor(t.right(w), Tree::kBlack);
                    t.rotateLeft(xParent, rootRef);
                    break;
                }
            } else {
                uint32_t w = t.left(xParent);
                if (t.color(w) == Tree::kRed) {
                    t.setColor(w, Tree::kBlack); t.setColor(xParent, Tree::kRed);
                    t.rotateRight(xParent, rootRef);
                    w = t.left(xParent);
                }
                if ((!t.right(w) || t.color(t.right(w)) == Tree::kBlack) && (!t.left(w) || t.color(t.left(w)) == Tree::kBlack)) {
                    t.setColor(w, Tree::kRed);
                    xn = xParent;
                    xParent = t.parent(xParent);
                } else {
                    if (!t.left(w) || t.color(t.left(w)) == Tree::kBlack) {
                        t.setColor(t.right(w), Tree::kBlack); t.setColor(w, Tree::kRed);
                        t.rotateLeft(w, rootRef);
                        w = t.left(xParent);
                    }
                    t.setColor(w, t.color(xParent)); t.setColor(xParent, Tree::kBlack);
                    if (t.left(w)) t.setColor(t.left(w), Tree::kBlack);
                    t.rotateRight(xParent, rootRef);
                    break;
                }
            }
        }
        if (xn) t.setColor(xn, Tree::kBlack);
    }
    return y;
}

} // namespace

// Shared with the stream code: build / read a guest std::string or std::wstring.
uint32_t guestStringData(Ctx& x, const std::string& s) { return make(x, s); }
std::string guestStringValue(Ctx& x, GuestPtr self) { return value(x, self); }
void guestStringDispose(Ctx& x, GuestPtr self) { dispose(x, x.mem.read<uint32_t>(self)); }
void guestStringSet(Ctx& x, GuestPtr self, const std::string& s) { set(x, self, s); }
uint32_t guestWStringData(Ctx& x, const std::u32string& s) { return wmake(x, s); }
std::u32string guestWStringValue(Ctx& x, GuestPtr self) { return wvalue(x, self); }

void registerLibStdcxxContainers(Process& p) {
    p.service<Strings>().emptyRep = p.defineDataBlock("__ZNSs4_Rep20_S_empty_rep_storageE", 16);
    p.mem().write<uint32_t>(p.service<Strings>().emptyRep + 8, 0xFFFFFFFFu);
    p.mem().write<uint32_t>(p.defineDataBlock("__ZNSs4_Rep11_S_max_sizeE", 4), kMaxSize);
    p.defineDataBlock("__ZNSs4_Rep11_S_terminalE", 4);
    p.service<Strings>().emptyWRep = p.defineDataBlock("__ZNSbIwSt11char_traitsIwESaIwEE4_Rep20_S_empty_rep_storageE", 32);
    p.mem().write<uint32_t>(p.service<Strings>().emptyWRep + 8, 0xFFFFFFFFu);
    p.mem().write<uint32_t>(p.defineDataBlock("__ZNSbIwSt11char_traitsIwESaIwEE4_Rep11_S_max_sizeE", 4), kWMaxSize);
    p.defineDataBlock("__ZNSbIwSt11char_traitsIwESaIwEE4_Rep11_S_terminalE", 4);
    p.def<&c_default>("__ZNSsC1Ev");            p.def<&c_default>("__ZNSsC2Ev");
    p.def<&c_fill>("__ZNSsC1EmcRKSaIcE");       p.def<&c_fill>("__ZNSsC2EmcRKSaIcE");
    p.def<&c_cstr>("__ZNSsC1EPKcRKSaIcE");      p.def<&c_cstr>("__ZNSsC2EPKcRKSaIcE");
    p.def<&c_buf>("__ZNSsC1EPKcmRKSaIcE");      p.def<&c_buf>("__ZNSsC2EPKcmRKSaIcE");
    p.def<&c_copy>("__ZNSsC1ERKSs");            p.def<&c_copy>("__ZNSsC2ERKSs");
    p.def<&c_sub>("__ZNSsC1ERKSsmm");           p.def<&c_sub>("__ZNSsC2ERKSsmm");
    p.def<&d_str>("__ZNSsD1Ev");                p.def<&d_str>("__ZNSsD2Ev");
    p.def<&m_assignBuf>("__ZNSs6assignEPKcm");
    p.def<&m_assignStr>("__ZNSs6assignERKSs");
    p.def<&m_assignSub>("__ZNSs6assignERKSsmm");
    p.def<&m_appendBuf>("__ZNSs6appendEPKcm");
    p.def<&m_appendStr>("__ZNSs6appendERKSs");
    p.def<&m_appendChars>("__ZNSs6appendEmc");
    p.def<&m_pushBack>("__ZNSs9push_backEc");
    p.def<&m_insertBuf>("__ZNSs6insertEmPKcm");
    p.def<&m_insertSub>("__ZNSs6insertEmRKSsmm");
    p.def<&m_erase>("__ZNSs5eraseEmm");
    p.def<&m_replace>("__ZNSs7replaceEmmPKcm");
    p.def<&m_replaceAux>("__ZNSs14_M_replace_auxEmmmc");
    p.def<&m_replace>("__ZNSs15_M_replace_safeEmmPKcm");
    p.def<&m_mutate>("__ZNSs9_M_mutateEmmm");
    p.def<&m_resize>("__ZNSs6resizeEmc");
    p.def<&m_resizeN>("__ZNSs6resizeEm");
    p.def<&m_reserve>("__ZNSs7reserveEm");
    p.def<&m_assignC>("__ZNSsaSEPKc");
    p.def<&m_assignStr>("__ZNSsaSERKSs");
    p.def<&m_assignChar>("__ZNSsaSEc");
    p.def<&m_appendC>("__ZNSs6appendEPKc");
    p.def<&m_plusC>("__ZNSspLEPKc");
    p.def<&m_plusStr>("__ZNSspLERKSs");
    p.def<&m_plusChar>("__ZNSspLEc");
    p.def<&m_replaceStr>("__ZNSs7replaceEmmRKSs");
    p.def<&q_size>("__ZNKSs4sizeEv");
    p.def<&q_size>("__ZNKSs6lengthEv");
    p.def<&q_empty>("__ZNKSs5emptyEv");
    p.def<&q_cstr>("__ZNKSs5c_strEv");
    p.def<&q_cstr>("__ZNKSs4dataEv");
    p.def<&q_beginConst>("__ZNKSs5beginEv");
    p.def<&q_endConst>("__ZNKSs3endEv");
    p.def<&q_begin>("__ZNSs5beginEv");
    p.def<&q_end>("__ZNSs3endEv");
    // Out-of-line copies of members libstdc++ normally inlines (Army of Darkness Defense).
    p.def<+[](Ctx& x, GuestPtr sret, GuestPtr self) {
        m_leakHard(x, self);
        const uint32_t d = x.mem.read<uint32_t>(self);
        x.mem.write<uint32_t>(sret, d + len(x, d));
        return sret;
    }>("__ZNSs6rbeginEv");
    p.def<+[](Ctx& x, GuestPtr self) { set(x, self, std::string()); }>("__ZNSs5clearEv");
    p.def<+[](Ctx& x, GuestPtr self, uint32_t pos, uint32_t n, GuestPtr s) {
        return m_replace(x, self, pos, n, s, s ? uint32_t(x.proc.readCString(s).size()) : 0);
    }>("__ZNSs7replaceEmmPKc");
    for (const char* n : {"__ZNSs12_Alloc_hiderC1EPcRKSaIcE", "__ZNSs12_Alloc_hiderC2EPcRKSaIcE"})
        p.def<+[](Ctx& x, GuestPtr self, GuestPtr data, GuestPtr) { x.mem.write<uint32_t>(self, data); return self; }>(n);
    p.def<&q_atConst>("__ZNKSsixEm");
    p.def<&q_at>("__ZNSsixEm");
    p.def<&q_atConst>("__ZNKSs2atEm");
    p.def<&q_at>("__ZNSs2atEm");
    p.def<&q_findCStr>("__ZNKSs4findEPKcm");
    p.def<&q_findStr>("__ZNKSs4findERKSsm");
    p.def<&q_findStrDefault>("__ZNKSs4findERKSs");
    p.def<&q_rfindCStr>("__ZNKSs5rfindEPKcm");
    p.def<&m_swap>("__ZNSs4swapERSs");
    p.def<&m_leakHard>("__ZNSs12_M_leak_hardEv");
    p.def<&q_findBuf>("__ZNKSs4findEPKcmm");
    p.def<&q_findChar>("__ZNKSs4findEcm");
    p.def<&q_rfindBuf>("__ZNKSs5rfindEPKcmm");
    p.def<&q_rfindChar>("__ZNKSs5rfindEcm");
    p.def<&q_findFirstOfBuf>("__ZNKSs13find_first_ofEPKcmm");
    p.def<&q_findLastOfBuf>("__ZNKSs12find_last_ofEPKcmm");
    p.def<&q_findFirstNotOfBuf>("__ZNKSs17find_first_not_ofEPKcmm");
    p.def<&q_findLastNotOfBuf>("__ZNKSs16find_last_not_ofEPKcmm");
    p.def<&q_compareC>("__ZNKSs7compareEPKc");
    p.def<&q_compareStr>("__ZNKSs7compareERKSs");
    p.def<&q_compareSubStr>("__ZNKSs7compareEmmRKSs");
    p.def<&q_compareSubC>("__ZNKSs7compareEmmPKc");
    p.defineFunction("__ZNKSs6substrEmm", [](Ctx& x) { q_substr(x); });
    p.def<&r_create>("__ZNSs4_Rep9_S_createEmmRKSaIcE");
    p.def<&r_dispose>("__ZNSs4_Rep10_M_disposeERKSaIcE");
    p.def<&r_destroy>("__ZNSs4_Rep10_M_destroyERKSaIcE");
    p.def<&r_refcopy>("__ZNSs4_Rep10_M_refcopyEv");
    p.def<&r_refdata>("__ZNSs4_Rep10_M_refdataEv");
    p.def<&r_setLengthAndSharable<1>>("__ZNSs4_Rep26_M_set_length_and_sharableEm");
    p.def<&r_setSharable<1>>("__ZNSs4_Rep13_M_set_sharableEv");
    p.def<&r_setLeaked<1>>("__ZNSs4_Rep13_M_set_leakedEv");
    p.def<&r_emptyRep>("__ZNSs4_Rep12_S_empty_repEv");

    // std::wstring
    p.def<&wc_default>("__ZNSbIwSt11char_traitsIwESaIwEEC1Ev");
    p.def<&wc_default>("__ZNSbIwSt11char_traitsIwESaIwEEC2Ev");
    p.def<&wc_fill>("__ZNSbIwSt11char_traitsIwESaIwEEC1EmwRKS1_");
    p.def<&wc_fill>("__ZNSbIwSt11char_traitsIwESaIwEEC2EmwRKS1_");
    p.def<&wc_cstr>("__ZNSbIwSt11char_traitsIwESaIwEEC1EPKwRKS1_");
    p.def<&wc_cstr>("__ZNSbIwSt11char_traitsIwESaIwEEC2EPKwRKS1_");
    p.def<&wc_buf>("__ZNSbIwSt11char_traitsIwESaIwEEC1EPKwmRKS1_");
    p.def<&wc_buf>("__ZNSbIwSt11char_traitsIwESaIwEEC2EPKwmRKS1_");
    p.def<&wc_copy>("__ZNSbIwSt11char_traitsIwESaIwEEC1ERKS2_");
    p.def<&wc_copy>("__ZNSbIwSt11char_traitsIwESaIwEEC2ERKS2_");
    p.def<&wc_sub>("__ZNSbIwSt11char_traitsIwESaIwEEC1ERKS2_mm");
    p.def<&wc_sub>("__ZNSbIwSt11char_traitsIwESaIwEEC2ERKS2_mm");
    p.def<&wd_str>("__ZNSbIwSt11char_traitsIwESaIwEED1Ev");
    p.def<&wd_str>("__ZNSbIwSt11char_traitsIwESaIwEED2Ev");
    p.def<&wm_assignBuf>("__ZNSbIwSt11char_traitsIwESaIwEE6assignEPKwm");
    p.def<&wm_assignC>("__ZNSbIwSt11char_traitsIwESaIwEE6assignEPKw");
    p.def<&wm_assignStr>("__ZNSbIwSt11char_traitsIwESaIwEE6assignERKS2_");
    p.def<&wm_assignC>("__ZNSbIwSt11char_traitsIwESaIwEEaSEPKw");
    p.def<&wm_assignStr>("__ZNSbIwSt11char_traitsIwESaIwEEaSERKS2_");
    p.def<&wm_appendBuf>("__ZNSbIwSt11char_traitsIwESaIwEE6appendEPKwm");
    p.def<&wm_appendC>("__ZNSbIwSt11char_traitsIwESaIwEE6appendEPKw");
    p.def<&wm_appendStr>("__ZNSbIwSt11char_traitsIwESaIwEE6appendERKS2_");
    p.def<&wm_appendChars>("__ZNSbIwSt11char_traitsIwESaIwEE6appendEmw");
    p.def<&wm_pushBack>("__ZNSbIwSt11char_traitsIwESaIwEE9push_backEw");
    p.def<&wm_insertBuf>("__ZNSbIwSt11char_traitsIwESaIwEE6insertEmPKwm");
    p.def<&wm_insertSub>("__ZNSbIwSt11char_traitsIwESaIwEE6insertEmRKS2_mm");
    p.def<&wm_erase>("__ZNSbIwSt11char_traitsIwESaIwEE5eraseEmm");
    p.def<&wm_replace>("__ZNSbIwSt11char_traitsIwESaIwEE7replaceEmmPKwm");
    p.def<&wm_replace>("__ZNSbIwSt11char_traitsIwESaIwEE15_M_replace_safeEmmPKwm");
    p.def<&wm_mutate>("__ZNSbIwSt11char_traitsIwESaIwEE9_M_mutateEmmm");
    p.def<&wm_resize>("__ZNSbIwSt11char_traitsIwESaIwEE6resizeEmw");
    p.def<&wm_resizeN>("__ZNSbIwSt11char_traitsIwESaIwEE6resizeEm");
    p.def<&wm_reserve>("__ZNSbIwSt11char_traitsIwESaIwEE7reserveEm");
    p.def<&wm_leakHard>("__ZNSbIwSt11char_traitsIwESaIwEE12_M_leak_hardEv");
    p.def<&q_size>("__ZNKSbIwSt11char_traitsIwESaIwEE4sizeEv");
    p.def<&q_size>("__ZNKSbIwSt11char_traitsIwESaIwEE6lengthEv");
    p.def<&q_empty>("__ZNKSbIwSt11char_traitsIwESaIwEE5emptyEv");
    p.def<&q_cstr>("__ZNKSbIwSt11char_traitsIwESaIwEE5c_strEv");
    p.def<&q_cstr>("__ZNKSbIwSt11char_traitsIwESaIwEE4dataEv");
    p.def<&wq_beginConst>("__ZNKSbIwSt11char_traitsIwESaIwEE5beginEv");
    p.def<&wq_endConst>("__ZNKSbIwSt11char_traitsIwESaIwEE3endEv");
    p.def<&wq_begin>("__ZNSbIwSt11char_traitsIwESaIwEE5beginEv");
    p.def<&wq_end>("__ZNSbIwSt11char_traitsIwESaIwEE3endEv");
    p.def<&wq_findBuf>("__ZNKSbIwSt11char_traitsIwESaIwEE4findEPKwmm");
    p.def<&wq_rfindBuf>("__ZNKSbIwSt11char_traitsIwESaIwEE5rfindEPKwmm");
    p.def<&wq_findChar>("__ZNKSbIwSt11char_traitsIwESaIwEE4findEwm");
    p.def<&wq_rfindChar>("__ZNKSbIwSt11char_traitsIwESaIwEE5rfindEwm");
    p.def<&wq_findFirstOfBuf>("__ZNKSbIwSt11char_traitsIwESaIwEE13find_first_ofEPKwmm");
    p.def<&wq_findLastOfBuf>("__ZNKSbIwSt11char_traitsIwESaIwEE12find_last_ofEPKwmm");
    p.def<&wq_findFirstNotOfBuf>("__ZNKSbIwSt11char_traitsIwESaIwEE17find_first_not_ofEPKwmm");
    p.def<&wq_findLastNotOfBuf>("__ZNKSbIwSt11char_traitsIwESaIwEE16find_last_not_ofEPKwmm");
    p.def<&wq_compareC>("__ZNKSbIwSt11char_traitsIwESaIwEE7compareEPKw");
    p.def<&wq_compareStr>("__ZNKSbIwSt11char_traitsIwESaIwEE7compareERKS2_");
    p.defineFunction("__ZNKSbIwSt11char_traitsIwESaIwEE6substrEmm", [](Ctx& x) { wq_substr(x); });
    p.def<&wr_create>("__ZNSbIwSt11char_traitsIwESaIwEE4_Rep9_S_createEmmRKS1_");
    p.def<&wr_dispose>("__ZNSbIwSt11char_traitsIwESaIwEE4_Rep10_M_disposeERKS1_");
    p.def<&wr_destroy>("__ZNSbIwSt11char_traitsIwESaIwEE4_Rep10_M_destroyERKS1_");
    p.def<&r_refcopy>("__ZNSbIwSt11char_traitsIwESaIwEE4_Rep10_M_refcopyEv");
    p.def<&r_refdata>("__ZNSbIwSt11char_traitsIwESaIwEE4_Rep10_M_refdataEv");
    p.def<&r_setLengthAndSharable<4>>("__ZNSbIwSt11char_traitsIwESaIwEE4_Rep26_M_set_length_and_sharableEm");
    p.def<&r_setSharable<4>>("__ZNSbIwSt11char_traitsIwESaIwEE4_Rep13_M_set_sharableEv");
    p.def<&r_setLeaked<4>>("__ZNSbIwSt11char_traitsIwESaIwEE4_Rep13_M_set_leakedEv");
    p.def<&wr_emptyRep>("__ZNSbIwSt11char_traitsIwESaIwEE4_Rep12_S_empty_repEv");

    p.def<&l_hook>("__ZNSt15_List_node_base4hookEPS_");
    p.def<&l_unhook>("__ZNSt15_List_node_base6unhookEv");
    p.def<&l_transfer>("__ZNSt15_List_node_base8transferEPS_S0_");
    p.def<&l_swap>("__ZNSt15_List_node_base4swapERS_S0_");

    p.def<&t_increment>("__ZSt18_Rb_tree_incrementPSt18_Rb_tree_node_base");
    p.def<&t_increment>("__ZSt18_Rb_tree_incrementPKSt18_Rb_tree_node_base");
    p.def<&t_decrement>("__ZSt18_Rb_tree_decrementPSt18_Rb_tree_node_base");
    p.def<&t_decrement>("__ZSt18_Rb_tree_decrementPKSt18_Rb_tree_node_base");
    p.def<&t_insertRebalance>("__ZSt29_Rb_tree_insert_and_rebalancebPSt18_Rb_tree_node_baseS0_RS_");
    p.def<&t_rebalanceForErase>("__ZSt28_Rb_tree_rebalance_for_erasePSt18_Rb_tree_node_baseRS_");

    // std::exception, std::runtime_error, std::logic_error
    const uint32_t vtExc = p.defineDataBlock("__ZTVSt9exception", 32);
    const uint32_t vtRt  = p.defineDataBlock("__ZTVSt13runtime_error", 32);
    const uint32_t vtLg  = p.defineDataBlock("__ZTVSt11logic_error", 32);
    const uint32_t fnExcD1 = p.def<+[](Ctx&, uint32_t self) -> uint32_t { return self; }>("__ZNSt9exceptionD1Ev");
    p.def<+[](Ctx&, uint32_t self) -> uint32_t { return self; }>("__ZNSt9exceptionD2Ev");
    const uint32_t fnExcD0 = p.def<+[](Ctx& x, uint32_t self) -> uint32_t { x.proc.heap().free(self); return self; }>("__ZNSt9exceptionD0Ev");
    const uint32_t fnExcWhat = p.def<+[](Ctx& x, uint32_t) -> uint32_t { return x.proc.internCString("std::exception"); }>("__ZNKSt9exception4whatEv");

    auto strExcCtor = [](uint32_t vptr) {
        return [vptr](Ctx& x, uint32_t self, uint32_t str) -> uint32_t {
            x.mem.write<uint32_t>(self, vptr);
            x.mem.write<uint32_t>(self + 4, str ? grab(x, x.mem.read<uint32_t>(str)) : make(x, ""));
            return self;
        };
    };
    const uint32_t fnRtD1 = p.def<+[](Ctx& x, uint32_t self) -> uint32_t { dispose(x, x.mem.read<uint32_t>(self + 4)); return self; }>("__ZNSt13runtime_errorD1Ev");
    p.def<+[](Ctx& x, uint32_t self) -> uint32_t { dispose(x, x.mem.read<uint32_t>(self + 4)); return self; }>("__ZNSt13runtime_errorD2Ev");
    const uint32_t fnRtD0 = p.def<+[](Ctx& x, uint32_t self) -> uint32_t { dispose(x, x.mem.read<uint32_t>(self + 4)); x.proc.heap().free(self); return self; }>("__ZNSt13runtime_errorD0Ev");
    const uint32_t fnRtWhat = p.def<+[](Ctx& x, uint32_t self) -> uint32_t {
        const uint32_t s = x.mem.read<uint32_t>(self + 4);
        return s ? s : x.proc.internCString("std::runtime_error");
    }>("__ZNKSt13runtime_error4whatEv");

    const uint32_t fnLgD1 = p.def<+[](Ctx& x, uint32_t self) -> uint32_t { dispose(x, x.mem.read<uint32_t>(self + 4)); return self; }>("__ZNSt11logic_errorD1Ev");
    p.def<+[](Ctx& x, uint32_t self) -> uint32_t { dispose(x, x.mem.read<uint32_t>(self + 4)); return self; }>("__ZNSt11logic_errorD2Ev");
    const uint32_t fnLgD0 = p.def<+[](Ctx& x, uint32_t self) -> uint32_t { dispose(x, x.mem.read<uint32_t>(self + 4)); x.proc.heap().free(self); return self; }>("__ZNSt11logic_errorD0Ev");
    const uint32_t fnLgWhat = p.def<+[](Ctx& x, uint32_t self) -> uint32_t {
        const uint32_t s = x.mem.read<uint32_t>(self + 4);
        return s ? s : x.proc.internCString("std::logic_error");
    }>("__ZNKSt11logic_error4whatEv");

    for (const char* s : {"__ZNSt13runtime_errorC1ERKSs", "__ZNSt13runtime_errorC2ERKSs"}) {
        const uint32_t vp = vtRt + 8;
        p.defineFunction(s, [strExcCtor, vp](Ctx& x) { x.cpu.r[0] = strExcCtor(vp)(x, x.cpu.r[0], x.cpu.r[1]); });
    }
    for (const char* s : {"__ZNSt11logic_errorC1ERKSs", "__ZNSt11logic_errorC2ERKSs",
                          // length_error etc. share logic_error's layout; their own type info isn't modelled.
                          "__ZNSt12length_errorC1ERKSs", "__ZNSt12length_errorC2ERKSs",
                          "__ZNSt12out_of_rangeC1ERKSs", "__ZNSt12out_of_rangeC2ERKSs",
                          "__ZNSt16invalid_argumentC1ERKSs", "__ZNSt16invalid_argumentC2ERKSs"}) {
        const uint32_t vp = vtLg + 8;
        p.defineFunction(s, [strExcCtor, vp](Ctx& x) { x.cpu.r[0] = strExcCtor(vp)(x, x.cpu.r[0], x.cpu.r[1]); });
    }
    p.mem().write<uint32_t>(vtExc + 8, fnExcD1);
    p.mem().write<uint32_t>(vtExc + 12, fnExcD0);
    p.mem().write<uint32_t>(vtExc + 16, fnExcWhat);
    p.mem().write<uint32_t>(vtRt + 8, fnRtD1);
    p.mem().write<uint32_t>(vtRt + 12, fnRtD0);
    p.mem().write<uint32_t>(vtRt + 16, fnRtWhat);
    p.mem().write<uint32_t>(vtLg + 8, fnLgD1);
    p.mem().write<uint32_t>(vtLg + 12, fnLgD0);
    p.mem().write<uint32_t>(vtLg + 16, fnLgWhat);
}

} // namespace Hydrogen::Runtime
