// HydrogenVM — libc++ std::__1::basic_string<char> and basic_string<wchar_t>:
// the members libc++.dylib exports (its extern-template list), the
// __basic_string_common error helpers, and std::__1::to_string.
//
// Most of basic_string is inline in the app, so the app reads and writes the
// object directly; the layout must be exactly the one 32-bit iOS uses. That is
// libc++'s classic layout (_LIBCPP_ABI_ALTERNATE_STRING_LAYOUT is only for
// arm64 and armv7k), little-endian, 12 bytes:
//
//   long:  { size_t cap | 1; size_t size; T* data; }      cap counts the NUL
//   short: { uint8_t size << 1; T data[min_cap]; }         bit 0 clear
//
// For char, min_cap is 11 and the inline data starts at +1 (10 chars + NUL).
// For wchar_t (4 bytes on iOS), the size byte shares a wchar_t-sized union, so
// the inline data starts at +4 and min_cap is 2 (1 char + NUL).
// Long capacities are rounded like libc++'s __recommend: (cap + 1) is a
// multiple of 16 bytes' worth of characters.
//
// Like libc++, mutations work in the string's own buffer when it is big enough
// and otherwise allocate the new buffer first and copy into it, so nothing on
// the host is sized by a guest-supplied length. Guest pointers and the string
// object are checked against guest page permissions before they are touched;
// a bad one raises a guest memory fault.
//
// Buffers come from the guest heap, the same heap operator new/delete use, so
// the app's inline code may free or reuse them.
#include "runtime/Process.hpp"
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <string>
#include <vector>

namespace Hydrogen::Runtime {
namespace {

constexpr uint32_t kNpos = 0xFFFFFFFFu;
constexpr uint64_t kGuestHeapBytes = 0x70000000u;   // the guest heap's whole range; no buffer can be larger

[[noreturn]] void outOfRange(const char* where) { throw std::runtime_error(std::string("std::out_of_range: ") + where); }
[[noreturn]] void lengthError(const char* where) { throw std::runtime_error(std::string("std::length_error: ") + where); }

// Where the characters to write come from: a guest range, or `n` copies of `c`.
struct Src {
    uint32_t ptr = 0, n = 0;
    bool fill = false;
    uint32_t c = 0;
    static Src range(uint32_t p, uint32_t n) { return {p, n, false, 0}; }
    static Src chars(uint32_t n, uint32_t c) { return {0, n, true, c}; }
};

template <uint32_t W> struct Str {
    static constexpr uint32_t kMinCap = W == 1 ? 11 : 2;
    static constexpr uint32_t kShortData = W == 1 ? 1 : 4;
    static constexpr uint32_t kAlign = 16 / W;
    // allocator<T>::max_size() - __alignment (little-endian libc++).
    static constexpr uint32_t kMaxSize = (0xFFFFFFFFu / W) - 16;

    Ctx& x;
    explicit Str(Ctx& c) : x(c) {}

    // ---- guest memory, checked
    [[noreturn]] void fault(uint32_t at, const char* what) const {
        throw CPU::GuestFault(CPU::Exit::MemFault, at, x.cpu.r[CPU::kPC], uint32_t(x.cpu.thumb), what);
    }
    void need(uint32_t addr, uint64_t bytes, uint8_t prot, const char* what) const {
        if (!bytes) return;
        if (uint64_t(addr) + bytes > 0x100000000ull) fault(addr, what);
        constexpr uint32_t sh = Memory::GuestArena::kGuestPageShift;
        const uint64_t last = (uint64_t(addr) + bytes - 1) >> sh;
        for (uint64_t pg = addr >> sh; pg <= last; ++pg)
            if ((x.mem.protAt(uint32_t(pg << sh)) & prot) != prot) fault(std::max<uint32_t>(addr, uint32_t(pg << sh)), what);
    }
    void readable(uint32_t p, uint64_t chars, const char* what) const { need(p, chars * W, Memory::kProtRead, what); }
    void writable(uint32_t p, uint64_t chars, const char* what) const { need(p, chars * W, Memory::kProtRW, what); }
    void object(uint32_t s, bool write) const { need(s, 12, write ? Memory::kProtRW : Memory::kProtRead, "std::__1::basic_string object"); }

    uint32_t load(uint32_t p, uint32_t i) const {
        if constexpr (W == 1) return x.mem.read<uint8_t>(p + i);
        else return x.mem.read<uint32_t>(p + i * 4);
    }
    void store(uint32_t p, uint32_t i, uint32_t c) const {
        if constexpr (W == 1) x.mem.write<uint8_t>(p + i, uint8_t(c));
        else x.mem.write<uint32_t>(p + i * 4, c);
    }
    // Callers have checked both ranges.
    void move(uint32_t dst, uint32_t src, uint32_t n) const { if (n && dst != src) std::memmove(x.mem.host(dst), x.mem.host(src), size_t(n) * W); }
    // Length of a NUL-terminated guest string; each page it touches must be readable.
    uint32_t cstrlen(uint32_t p) const {
        constexpr uint32_t sh = Memory::GuestArena::kGuestPageShift;
        uint64_t checkedPage = ~uint64_t(0);
        for (uint32_t n = 0;;) {
            const uint64_t at = uint64_t(p) + uint64_t(n) * W;
            if (at + W > 0x100000000ull) fault(uint32_t(at), "std::__1::basic_string: C string");
            if ((at >> sh) != checkedPage || ((at + W - 1) >> sh) != checkedPage) {
                readable(uint32_t(at), 1, "std::__1::basic_string: C string");
                checkedPage = (at + W - 1) >> sh;
            }
            if (!load(p, n)) return n;
            if (++n > kMaxSize) lengthError("basic_string: C string");
        }
    }

    // ---- the object
    bool isLong(uint32_t s) const { object(s, false); return x.mem.read<uint8_t>(s) & 1; }
    uint32_t size(uint32_t s) const { return isLong(s) ? x.mem.read<uint32_t>(s + 4) : x.mem.read<uint8_t>(s) >> 1; }
    uint32_t data(uint32_t s) const { return isLong(s) ? x.mem.read<uint32_t>(s + 8) : s + kShortData; }
    uint32_t capacity(uint32_t s) const { return (isLong(s) ? (x.mem.read<uint32_t>(s) & ~1u) : kMinCap) - 1; }
    void setSize(uint32_t s, uint32_t n) const {
        object(s, true);
        if (isLong(s)) x.mem.write<uint32_t>(s + 4, n);
        else x.mem.write<uint8_t>(s, uint8_t(n << 1));
    }
    void setLong(uint32_t s, uint32_t p, uint32_t cap, uint32_t n) const {
        object(s, true);
        x.mem.write<uint32_t>(s, (cap + 1) | 1);
        x.mem.write<uint32_t>(s + 4, n);
        x.mem.write<uint32_t>(s + 8, p);
    }
    // A long string read from guest memory may be garbage: its size must not exceed its
    // capacity and its whole buffer must fit below 4 GB, so no offset computed from them
    // can wrap. Checked before any offset arithmetic (cheap: no page walk).
    void consistent(uint32_t s) const {
        if (!isLong(s)) {
            // The inline buffer holds kMinCap - 1 characters; a larger size byte would read past it.
            if ((x.mem.read<uint8_t>(s) >> 1) > kMinCap - 1) fault(s, "std::__1::basic_string object: short size exceeds the inline buffer");
            return;
        }
        const uint32_t cap = capacity(s), sz = x.mem.read<uint32_t>(s + 4), d = x.mem.read<uint32_t>(s + 8);
        if (sz > cap || cap > kMaxSize || uint64_t(d) + (uint64_t(cap) + 1) * W > 0x100000000ull)
            fault(s, "std::__1::basic_string object: size, capacity and data pointer are inconsistent");
    }
    // The string's characters [from, from + n), checked readable: each operation checks
    // only the range it reads or writes, so an append does not rescan the whole buffer.
    uint32_t readChars(uint32_t s, uint32_t from, uint32_t n) const {
        consistent(s);
        const uint32_t d = data(s);
        readable(d + from * W, n, "std::__1::basic_string buffer");
        return d;
    }

    static uint32_t recommend(uint32_t n) {
        if (n < kMinCap) return kMinCap - 1;
        const uint64_t r = (uint64_t(n) + 1 + kAlign - 1) / kAlign * kAlign;
        return uint32_t(r - 1);
    }
    static uint32_t grown(uint32_t oldCap, uint32_t need) {
        return oldCap < kMaxSize / 2 - 16 ? recommend(std::max(need, 2 * oldCap)) : kMaxSize - 1;
    }
    uint32_t alloc(uint32_t chars) const {   // chars includes the NUL
        if (uint64_t(chars) * W > kGuestHeapBytes) throw std::runtime_error("std::__1::basic_string: out of guest memory (std::bad_alloc)");
        const uint32_t p = x.proc.heap().alloc(chars * W);
        if (!p) throw std::runtime_error("std::__1::basic_string: out of guest memory (std::bad_alloc)");
        return p;
    }
    void dealloc(uint32_t p) const { if (p) x.proc.heap().free(p); }

    // Writes `src` at dst. `tmp` holds the source when it overlapped the buffer being changed.
    void emit(uint32_t dst, const Src& src, const std::vector<uint32_t>* tmp) const {
        if (src.fill) { for (uint32_t i = 0; i < src.n; ++i) store(dst, i, src.c); return; }
        if (tmp) { for (uint32_t i = 0; i < src.n; ++i) store(dst, i, (*tmp)[i]); return; }
        move(dst, src.ptr, src.n);
    }
    // A copy of a guest source that overlaps [lo, hi): it is bounded by that buffer's size.
    bool overlaps(const Src& src, uint32_t lo, uint64_t hi) const {
        return !src.fill && src.n && uint64_t(src.ptr) < hi && uint64_t(src.ptr) + uint64_t(src.n) * W > lo;
    }
    std::vector<uint32_t> copyOf(const Src& src) const {
        std::vector<uint32_t> v(src.n);
        for (uint32_t i = 0; i < src.n; ++i) v[i] = load(src.ptr, i);
        return v;
    }

    // __init: constructs `s` (uninitialised memory) from `src`, reserving `res`.
    void init(uint32_t s, const Src& src, uint32_t res, const char* where) {
        object(s, true);
        const uint32_t n = src.n;
        if (n > kMaxSize || res > kMaxSize) lengthError(where);
        if (!src.fill) readable(src.ptr, n, where);
        res = std::max(res, n);
        std::vector<uint32_t> tmp;
        const bool alias = overlaps(src, s, uint64_t(s) + 12);
        if (alias) tmp = copyOf(src);
        uint32_t p;
        if (res < kMinCap) {
            p = s + kShortData;
            emit(p, src, alias ? &tmp : nullptr);
            x.mem.write<uint8_t>(s, uint8_t(n << 1));
        } else {
            const uint32_t cap = recommend(res);
            p = alloc(cap + 1);
            emit(p, src, alias ? &tmp : nullptr);
            setLong(s, p, cap, n);
        }
        store(p, n, 0);
    }
    void initHost(uint32_t s, const std::string& v) {
        object(s, true);
        const uint32_t n = uint32_t(v.size());
        uint32_t p;
        if (n < kMinCap) { p = s + kShortData; x.mem.write<uint8_t>(s, uint8_t(n << 1)); }
        else { const uint32_t cap = recommend(n); p = alloc(cap + 1); setLong(s, p, cap, n); }
        for (uint32_t i = 0; i < n; ++i) store(p, i, uint8_t(v[i]));
        store(p, n, 0);
    }

    // Replaces [pos, pos + n1) with `src`: every mutating member is one of these.
    // In place when the result fits the capacity; otherwise a new buffer of at
    // least twice the old capacity (__grow_by_and_replace) is allocated first.
    void replace(uint32_t s, uint32_t pos, uint32_t n1, const Src& src, const char* where) {
        object(s, true);
        consistent(s);
        const uint32_t sz = size(s), cap = capacity(s), p = data(s);
        if (pos > sz) outOfRange(where);
        n1 = std::min(n1, sz - pos);
        const uint32_t n2 = src.n;
        if (uint64_t(sz) - n1 + n2 > kMaxSize) lengthError(where);
        if (!src.fill) readable(src.ptr, n2, where);
        const uint32_t newSz = sz - n1 + n2, tail = sz - pos - n1;
        std::vector<uint32_t> tmp;
        const bool alias = overlaps(src, p, uint64_t(p) + (uint64_t(cap) + 1) * W) || overlaps(src, s, uint64_t(s) + 12);
        if (alias) tmp = copyOf(src);
        if (newSz <= cap) {
            // Reads the tail after the edit; writes [pos, newSz] (moved tail, new chars, NUL).
            readable(p + (pos + n1) * W, tail, "std::__1::basic_string buffer");
            writable(p + pos * W, uint64_t(newSz - pos) + 1, "std::__1::basic_string buffer");
            if (n1 != n2) move(p + (pos + n2) * W, p + (pos + n1) * W, tail);
            emit(p + pos * W, src, alias ? &tmp : nullptr);
            store(p, newSz, 0);
            setSize(s, newSz);
            return;
        }
        readable(p, sz, "std::__1::basic_string buffer");   // head and tail are copied
        const uint32_t newCap = grown(cap, newSz);
        const uint32_t q = alloc(newCap + 1);
        move(q, p, pos);
        emit(q + pos * W, src, alias ? &tmp : nullptr);
        move(q + (pos + n2) * W, p + (pos + n1) * W, tail);
        store(q, newSz, 0);
        if (isLong(s)) dealloc(p);
        setLong(s, q, newCap, newSz);
    }

    // A substring of another string as a source; checks pos against its size.
    Src sub(uint32_t other, uint32_t pos, uint32_t n, const char* where) const {
        const uint32_t sz = size(other);
        if (pos > sz) outOfRange(where);
        const uint32_t n2 = std::min(n, sz - pos);
        return Src::range(readChars(other, pos, n2) + pos * W, n2);
    }
    Src whole(uint32_t other) const { const uint32_t n = size(other); return Src::range(readChars(other, 0, n), n); }

    // Character comparison as char_traits does it: unsigned bytes for char,
    // signed wchar_t (wmemcmp) for wchar_t.
    static int cmp(uint32_t a, uint32_t b) {
        if constexpr (W == 1) return a < b ? -1 : a > b ? 1 : 0;
        else return int32_t(a) < int32_t(b) ? -1 : int32_t(a) > int32_t(b) ? 1 : 0;
    }
    int compare(uint32_t a, uint32_t na, uint32_t b, uint32_t nb) const {
        const uint32_t n = std::min(na, nb);
        for (uint32_t i = 0; i < n; ++i)
            if (const int r = cmp(load(a, i), load(b, i))) return r;
        return na < nb ? -1 : na > nb ? 1 : 0;
    }
};

template <uint32_t W> Str<W> S(Ctx& x) { return Str<W>(x); }

// ---------------------------------------------------------- constructors ---
template <uint32_t W> void initPtr(Ctx& x, GuestPtr self, GuestPtr s, uint32_t n) { S<W>(x).init(self, Src::range(s, n), 0, "basic_string::__init"); }
template <uint32_t W> void initPtrRes(Ctx& x, GuestPtr self, GuestPtr s, uint32_t n, uint32_t res) {
    S<W>(x).init(self, Src::range(s, n), res, "basic_string::__init");
}
template <uint32_t W> void initFill(Ctx& x, GuestPtr self, uint32_t n, uint32_t c) { S<W>(x).init(self, Src::chars(n, c), 0, "basic_string::__init"); }

// The iOS ARM C++ ABI returns `this` from constructors and destructors.
template <uint32_t W> uint32_t copyCtor(Ctx& x, GuestPtr self, GuestPtr other) {
    auto t = S<W>(x);
    if (!t.isLong(other)) {
        t.object(self, true);
        for (uint32_t i = 0; i < 12; i += 4) x.mem.write<uint32_t>(self + i, x.mem.read<uint32_t>(other + i));
    } else {
        t.init(self, t.whole(other), 0, "basic_string");
    }
    return self;
}
template <uint32_t W> uint32_t copyCtorAlloc(Ctx& x, GuestPtr self, GuestPtr other, GuestPtr) { return copyCtor<W>(x, self, other); }
template <uint32_t W> uint32_t substrCtor(Ctx& x, GuestPtr self, GuestPtr other, uint32_t pos, uint32_t n, GuestPtr) {
    auto t = S<W>(x);
    t.init(self, t.sub(other, pos, n, "basic_string"), 0, "basic_string");
    return self;
}
template <uint32_t W> uint32_t dtor(Ctx& x, GuestPtr self) {
    auto t = S<W>(x);
    if (t.isLong(self)) t.dealloc(t.data(self));
    return self;
}

// -------------------------------------------- assign / append / insert ---
template <uint32_t W> uint32_t assignSrc(Ctx& x, GuestPtr self, const Src& src, const char* where) {
    auto t = S<W>(x);
    t.replace(self, 0, kNpos, src, where);
    return self;
}
template <uint32_t W> uint32_t assignPtrN(Ctx& x, GuestPtr self, GuestPtr s, uint32_t n) { return assignSrc<W>(x, self, Src::range(s, n), "basic_string::assign"); }
template <uint32_t W> uint32_t assignPtr(Ctx& x, GuestPtr self, GuestPtr s) { return assignSrc<W>(x, self, Src::range(s, S<W>(x).cstrlen(s)), "basic_string::assign"); }
template <uint32_t W> uint32_t assignFill(Ctx& x, GuestPtr self, uint32_t n, uint32_t c) { return assignSrc<W>(x, self, Src::chars(n, c), "basic_string::assign"); }
template <uint32_t W> uint32_t assignSub(Ctx& x, GuestPtr self, GuestPtr other, uint32_t pos, uint32_t n) {
    return assignSrc<W>(x, self, S<W>(x).sub(other, pos, n, "basic_string::assign"), "basic_string::assign");
}
template <uint32_t W> uint32_t opAssign(Ctx& x, GuestPtr self, GuestPtr other) {
    if (self != other) assignSrc<W>(x, self, S<W>(x).whole(other), "basic_string::operator=");
    return self;
}
template <uint32_t W> uint32_t opAssignChar(Ctx& x, GuestPtr self, uint32_t c) { return assignSrc<W>(x, self, Src::chars(1, c), "basic_string::operator="); }

template <uint32_t W> uint32_t appendSrc(Ctx& x, GuestPtr self, const Src& src) {
    auto t = S<W>(x);
    t.replace(self, t.size(self), 0, src, "basic_string::append");
    return self;
}
template <uint32_t W> uint32_t appendPtrN(Ctx& x, GuestPtr self, GuestPtr s, uint32_t n) { return appendSrc<W>(x, self, Src::range(s, n)); }
template <uint32_t W> uint32_t appendPtr(Ctx& x, GuestPtr self, GuestPtr s) { return appendSrc<W>(x, self, Src::range(s, S<W>(x).cstrlen(s))); }
template <uint32_t W> uint32_t appendFill(Ctx& x, GuestPtr self, uint32_t n, uint32_t c) { return appendSrc<W>(x, self, Src::chars(n, c)); }
template <uint32_t W> uint32_t appendSub(Ctx& x, GuestPtr self, GuestPtr other, uint32_t pos, uint32_t n) {
    return appendSrc<W>(x, self, S<W>(x).sub(other, pos, n, "basic_string::append"));
}
template <uint32_t W> void pushBack(Ctx& x, GuestPtr self, uint32_t c) { appendSrc<W>(x, self, Src::chars(1, c)); }

template <uint32_t W> uint32_t insertSrc(Ctx& x, GuestPtr self, uint32_t pos, const Src& src) {
    S<W>(x).replace(self, pos, 0, src, "basic_string::insert");
    return self;
}
template <uint32_t W> uint32_t insertPtrN(Ctx& x, GuestPtr self, uint32_t pos, GuestPtr s, uint32_t n) { return insertSrc<W>(x, self, pos, Src::range(s, n)); }
template <uint32_t W> uint32_t insertPtr(Ctx& x, GuestPtr self, uint32_t pos, GuestPtr s) { return insertSrc<W>(x, self, pos, Src::range(s, S<W>(x).cstrlen(s))); }
template <uint32_t W> uint32_t insertFill(Ctx& x, GuestPtr self, uint32_t pos, uint32_t n, uint32_t c) { return insertSrc<W>(x, self, pos, Src::chars(n, c)); }
template <uint32_t W> uint32_t insertSub(Ctx& x, GuestPtr self, uint32_t pos1, GuestPtr other, uint32_t pos2, uint32_t n) {
    return insertSrc<W>(x, self, pos1, S<W>(x).sub(other, pos2, n, "basic_string::insert"));
}
// iterator insert(const_iterator p, T c): __wrap_iter is one pointer, passed and
// returned in a register. The buffer may move, so the result is recomputed.
template <uint32_t W> uint32_t insertIter(Ctx& x, GuestPtr self, GuestPtr it, uint32_t c) {
    auto t = S<W>(x);
    const uint32_t d = t.data(self);
    if (it < d || (it - d) % W) outOfRange("basic_string::insert(iterator)");
    const uint32_t ip = (it - d) / W;
    t.replace(self, ip, 0, Src::chars(1, c), "basic_string::insert");
    return t.data(self) + ip * W;
}

// ------------------------------------------------- replace / erase ---
template <uint32_t W> uint32_t replacePtrN(Ctx& x, GuestPtr self, uint32_t pos, uint32_t n1, GuestPtr s, uint32_t n2) {
    S<W>(x).replace(self, pos, n1, Src::range(s, n2), "basic_string::replace");
    return self;
}
template <uint32_t W> uint32_t replacePtr(Ctx& x, GuestPtr self, uint32_t pos, uint32_t n1, GuestPtr s) {
    auto t = S<W>(x);
    t.replace(self, pos, n1, Src::range(s, t.cstrlen(s)), "basic_string::replace");
    return self;
}
template <uint32_t W> uint32_t replaceFill(Ctx& x, GuestPtr self, uint32_t pos, uint32_t n1, uint32_t n2, uint32_t c) {
    S<W>(x).replace(self, pos, n1, Src::chars(n2, c), "basic_string::replace");
    return self;
}
template <uint32_t W> uint32_t replaceSub(Ctx& x, GuestPtr self, uint32_t pos1, uint32_t n1, GuestPtr other, uint32_t pos2, uint32_t n2) {
    auto t = S<W>(x);
    t.replace(self, pos1, n1, t.sub(other, pos2, n2, "basic_string::replace"), "basic_string::replace");
    return self;
}
template <uint32_t W> uint32_t erase(Ctx& x, GuestPtr self, uint32_t pos, uint32_t n) {
    S<W>(x).replace(self, pos, n, Src::chars(0, 0), "basic_string::erase");
    return self;
}

// ------------------------------------------------------ reserve / resize ---
// libc++'s reserve also shrinks: reserve(0) on a long string that fits in the
// inline buffer moves it back inline.
template <uint32_t W> void reserve(Ctx& x, GuestPtr self, uint32_t res) {
    auto t = S<W>(x);
    t.object(self, true);
    if (res > Str<W>::kMaxSize) lengthError("basic_string::reserve");
    const uint32_t cap = t.capacity(self), sz = t.size(self);
    res = Str<W>::recommend(std::max(res, sz));
    if (res == cap) return;
    const bool wasLong = t.isLong(self);
    const uint32_t old = t.readChars(self, 0, sz);
    if (res == Str<W>::kMinCap - 1) {
        // Long to inline: the inline buffer overlaps the object's own fields, so copy via the host.
        std::vector<uint32_t> v(sz);
        for (uint32_t i = 0; i < sz; ++i) v[i] = t.load(old, i);
        x.mem.write<uint8_t>(self, uint8_t(sz << 1));
        for (uint32_t i = 0; i < sz; ++i) t.store(self + Str<W>::kShortData, i, v[i]);
        t.store(self + Str<W>::kShortData, sz, 0);
    } else {
        const uint32_t p = t.alloc(res + 1);
        t.move(p, old, sz);
        t.store(p, sz, 0);
        t.setLong(self, p, res, sz);
    }
    if (wasLong) t.dealloc(old);
}
template <uint32_t W> void resize(Ctx& x, GuestPtr self, uint32_t n, uint32_t c) {
    auto t = S<W>(x);
    t.consistent(self);
    const uint32_t sz = t.size(self);
    if (n > sz) { appendSrc<W>(x, self, Src::chars(n - sz, c)); return; }
    t.object(self, true);
    const uint32_t d = t.data(self);
    t.writable(d + n * W, 1, "std::__1::basic_string buffer");
    t.store(d, n, 0);
    t.setSize(self, n);
}

// ------------------------------------------------------------ queries ---
template <uint32_t W> uint32_t copyOut(Ctx& x, GuestPtr self, GuestPtr dst, uint32_t n, uint32_t pos) {
    auto t = S<W>(x);
    const Src s = t.sub(self, pos, n, "basic_string::copy");
    t.writable(dst, s.n, "basic_string::copy");
    t.move(dst, s.ptr, s.n);
    return s.n;
}
template <uint32_t W> uint32_t at(Ctx& x, GuestPtr self, uint32_t n) {
    auto t = S<W>(x);
    t.consistent(self);
    if (n >= t.size(self)) outOfRange("basic_string::at");
    return t.data(self) + n * W;
}

// The searched string [h, h + hn) and the needle or set [s, s + sn), both checked readable.
template <uint32_t W> struct Hay {
    Str<W> t; uint32_t h, hn, s, sn;
    Hay(Ctx& x, GuestPtr self, GuestPtr needle, uint32_t n) : t(x) {
        hn = t.size(self);
        h = t.readChars(self, 0, hn);
        s = needle;
        sn = n;
        t.readable(s, sn, "basic_string: search argument");
    }
    bool matchAt(uint32_t i) const {
        for (uint32_t j = 0; j < sn; ++j) if (t.load(h, i + j) != t.load(s, j)) return false;
        return true;
    }
    bool inSet(uint32_t c) const {
        for (uint32_t j = 0; j < sn; ++j) if (t.load(s, j) == c) return true;
        return false;
    }
    uint32_t find(uint32_t pos) const {
        if (pos > hn) return kNpos;
        if (sn == 0) return pos;
        if (sn > hn) return kNpos;
        for (uint32_t i = pos; i + sn <= hn; ++i) if (matchAt(i)) return i;
        return kNpos;
    }
    uint32_t rfind(uint32_t pos) const {
        pos = std::min(pos, hn);
        if (sn > hn) return kNpos;
        uint32_t i = std::min(pos, hn - sn);
        for (;; --i) {
            if (matchAt(i)) return i;
            if (i == 0) return kNpos;
        }
    }
    uint32_t firstOf(uint32_t pos, bool want) const {
        if (want && sn == 0) return kNpos;
        for (uint32_t i = pos; i < hn; ++i) if (inSet(t.load(h, i)) == want) return i;
        return kNpos;
    }
    uint32_t lastOf(uint32_t pos, bool want) const {
        if (hn == 0 || (want && sn == 0)) return kNpos;
        for (uint32_t i = std::min(pos, hn - 1) + 1; i-- > 0;) if (inSet(t.load(h, i)) == want) return i;
        return kNpos;
    }
};
// A one-character needle lives in a register, not guest memory: search for it directly.
template <uint32_t W> uint32_t findChar(Ctx& x, GuestPtr self, uint32_t c, uint32_t pos) {
    auto t = S<W>(x);
    const uint32_t n = t.size(self), h = t.readChars(self, 0, n);
    for (uint32_t i = pos; i < n; ++i) if (t.load(h, i) == c) return i;
    return kNpos;
}
template <uint32_t W> uint32_t rfindChar(Ctx& x, GuestPtr self, uint32_t c, uint32_t pos) {
    auto t = S<W>(x);
    const uint32_t n = t.size(self), h = t.readChars(self, 0, n);
    if (!n) return kNpos;
    for (uint32_t i = std::min(pos, n - 1) + 1; i-- > 0;) if (t.load(h, i) == c) return i;
    return kNpos;
}
template <uint32_t W> uint32_t findPtr(Ctx& x, GuestPtr self, GuestPtr s, uint32_t pos, uint32_t n) { return Hay<W>(x, self, s, n).find(pos); }
template <uint32_t W> uint32_t rfindPtr(Ctx& x, GuestPtr self, GuestPtr s, uint32_t pos, uint32_t n) { return Hay<W>(x, self, s, n).rfind(pos); }
template <uint32_t W> uint32_t findFirstOf(Ctx& x, GuestPtr self, GuestPtr s, uint32_t pos, uint32_t n) { return Hay<W>(x, self, s, n).firstOf(pos, true); }
template <uint32_t W> uint32_t findFirstNotOf(Ctx& x, GuestPtr self, GuestPtr s, uint32_t pos, uint32_t n) { return Hay<W>(x, self, s, n).firstOf(pos, false); }
template <uint32_t W> uint32_t findLastOf(Ctx& x, GuestPtr self, GuestPtr s, uint32_t pos, uint32_t n) { return Hay<W>(x, self, s, n).lastOf(pos, true); }
template <uint32_t W> uint32_t findLastNotOf(Ctx& x, GuestPtr self, GuestPtr s, uint32_t pos, uint32_t n) { return Hay<W>(x, self, s, n).lastOf(pos, false); }

template <uint32_t W> int32_t compareAt(Ctx& x, GuestPtr self, uint32_t pos, uint32_t n1, const Src& other) {
    auto t = S<W>(x);
    const Src mine = t.sub(self, pos, n1, "basic_string::compare");
    t.readable(other.ptr, other.n, "basic_string::compare");
    return t.compare(mine.ptr, mine.n, other.ptr, other.n);
}
template <uint32_t W> int32_t comparePtr(Ctx& x, GuestPtr self, GuestPtr s) { return compareAt<W>(x, self, 0, kNpos, Src::range(s, S<W>(x).cstrlen(s))); }
template <uint32_t W> int32_t comparePosPtr(Ctx& x, GuestPtr self, uint32_t pos, uint32_t n1, GuestPtr s) {
    return compareAt<W>(x, self, pos, n1, Src::range(s, S<W>(x).cstrlen(s)));
}
template <uint32_t W> int32_t comparePosPtrN(Ctx& x, GuestPtr self, uint32_t pos, uint32_t n1, GuestPtr s, uint32_t n2) {
    return compareAt<W>(x, self, pos, n1, Src::range(s, n2));
}
template <uint32_t W> int32_t comparePosSub(Ctx& x, GuestPtr self, uint32_t pos1, uint32_t n1, GuestPtr other, uint32_t pos2, uint32_t n2) {
    return compareAt<W>(x, self, pos1, n1, S<W>(x).sub(other, pos2, n2, "basic_string::compare"));
}

// ------------------------------------------------------------ growth ---
// __grow_by(old_cap, delta_cap, old_sz, n_copy, n_del, n_add): allocates a new
// buffer, copies [0, n_copy) and the tail after n_del + n_copy (leaving a gap
// of n_add), frees the old buffer and stores the new pointer and capacity. It
// does not set the size or the terminator; the inline caller does that.
// libc++'s inline callers pass the string's current capacity and size, so other
// values are rejected instead of reading past the buffer or leaking it.
template <uint32_t W> uint32_t growTo(Ctx& x, GuestPtr self, uint32_t oldCap, uint32_t delta, uint32_t oldSz, uint32_t nCopy,
                                      uint32_t nDel, uint32_t nAdd, GuestPtr add) {
    auto t = S<W>(x);
    t.object(self, true);
    constexpr uint32_t ms = Str<W>::kMaxSize;
    if (oldCap != t.capacity(self) || oldSz > oldCap || uint64_t(nCopy) + nDel > oldSz)
        throw std::runtime_error("std::__1::basic_string::__grow_by: arguments do not match the string");
    if (delta > ms - oldCap) lengthError("basic_string");
    const uint32_t old = t.readChars(self, 0, oldSz);   // n_copy and the tail lie within old_sz
    const uint32_t cap = Str<W>::grown(oldCap, oldCap + delta);
    const uint32_t tail = oldSz - nDel - nCopy;
    if (uint64_t(nCopy) + nAdd + tail > cap) lengthError("basic_string::__grow_by");
    if (add) t.readable(add, nAdd, "basic_string::__grow_by_and_replace");
    const uint32_t p = t.alloc(cap + 1);
    t.move(p, old, nCopy);
    if (add) t.move(p + nCopy * W, add, nAdd);   // may point into the old buffer, which is still allocated
    t.move(p + (nCopy + nAdd) * W, old + (nCopy + nDel) * W, tail);
    const bool wasLong = t.isLong(self);
    const uint32_t keepSize = wasLong ? x.mem.read<uint32_t>(self + 4) : 0;
    if (wasLong) t.dealloc(old);
    t.setLong(self, p, cap, keepSize);
    return p;
}
template <uint32_t W> void growBy(Ctx& x, GuestPtr self, uint32_t oldCap, uint32_t delta, uint32_t oldSz, uint32_t nCopy, uint32_t nDel, uint32_t nAdd) {
    growTo<W>(x, self, oldCap, delta, oldSz, nCopy, nDel, nAdd, 0);
}
template <uint32_t W> void growByAndReplace(Ctx& x, GuestPtr self, uint32_t oldCap, uint32_t delta, uint32_t oldSz, uint32_t nCopy, uint32_t nDel,
                                            uint32_t nAdd, GuestPtr s) {
    auto t = S<W>(x);
    if (!s && nAdd) t.fault(0, "basic_string::__grow_by_and_replace");
    const uint32_t p = growTo<W>(x, self, oldCap, delta, oldSz, nCopy, nDel, nAdd, s);
    const uint32_t sz = nCopy + nAdd + (oldSz - nDel - nCopy);
    x.mem.write<uint32_t>(self + 4, sz);
    t.store(p, sz, 0);
}

// -------------------------------------------------------- to_string ---
void putResult(Ctx& x, GuestPtr ret, const std::string& s) { Str<1>(x).initHost(ret, s); }
void toStringI(Ctx& x, GuestPtr ret, int32_t v) { putResult(x, ret, std::to_string(v)); }
void toStringU(Ctx& x, GuestPtr ret, uint32_t v) { putResult(x, ret, std::to_string(v)); }
void toStringLL(Ctx& x, GuestPtr ret, int64_t v) { putResult(x, ret, std::to_string(v)); }
void toStringULL(Ctx& x, GuestPtr ret, uint64_t v) { putResult(x, ret, std::to_string(v)); }
void toStringD(Ctx& x, GuestPtr ret, double v) {
    char b[512];
    std::snprintf(b, sizeof b, "%f", v);
    putResult(x, ret, b);
}
void toStringF(Ctx& x, GuestPtr ret, float v) { toStringD(x, ret, double(v)); }

void throwLength(Ctx&, GuestPtr) { lengthError("basic_string"); }
void throwRange(Ctx&, GuestPtr) { outOfRange("basic_string"); }

template <uint32_t W> void registerFor(Process& p, const std::string& T /* "c" or "w" */) {
    const std::string cls = "St3__112basic_stringI" + T + "NS_11char_traitsI" + T + "EENS_9allocatorI" + T + "EEE";
    const std::string m = "__ZN" + cls, k = "__ZNK" + cls;
    const std::string P = "PK" + T;   // const T*
    p.def<&initPtr<W>>(m + "6__initE" + P + "m");
    p.def<&initPtrRes<W>>(m + "6__initE" + P + "mm");
    p.def<&initFill<W>>(m + "6__initEm" + T);
    p.def<&copyCtor<W>>(m + "C1ERKS5_");
    p.def<&copyCtor<W>>(m + "C2ERKS5_");
    p.def<&copyCtorAlloc<W>>(m + "C1ERKS5_RKS4_");
    p.def<&copyCtorAlloc<W>>(m + "C2ERKS5_RKS4_");
    p.def<&substrCtor<W>>(m + "C1ERKS5_mmRKS4_");
    p.def<&substrCtor<W>>(m + "C2ERKS5_mmRKS4_");
    p.def<&dtor<W>>(m + "D1Ev");
    p.def<&dtor<W>>(m + "D2Ev");
    p.def<&opAssign<W>>(m + "aSERKS5_");
    p.def<&opAssignChar<W>>(m + "aSE" + T);
    p.def<&assignPtr<W>>(m + "6assignE" + P);
    p.def<&assignPtrN<W>>(m + "6assignE" + P + "m");
    p.def<&assignFill<W>>(m + "6assignEm" + T);
    p.def<&assignSub<W>>(m + "6assignERKS5_mm");
    p.def<&appendPtr<W>>(m + "6appendE" + P);
    p.def<&appendPtrN<W>>(m + "6appendE" + P + "m");
    p.def<&appendFill<W>>(m + "6appendEm" + T);
    p.def<&appendSub<W>>(m + "6appendERKS5_mm");
    p.def<&pushBack<W>>(m + "9push_backE" + T);
    p.def<&insertPtr<W>>(m + "6insertEm" + P);
    p.def<&insertPtrN<W>>(m + "6insertEm" + P + "m");
    p.def<&insertFill<W>>(m + "6insertEmm" + T);
    p.def<&insertSub<W>>(m + "6insertEmRKS5_mm");
    p.def<&insertIter<W>>(m + "6insertENS_11__wrap_iterI" + P + "EE" + T);
    p.def<&replacePtr<W>>(m + "7replaceEmm" + P);
    p.def<&replacePtrN<W>>(m + "7replaceEmm" + P + "m");
    p.def<&replaceFill<W>>(m + "7replaceEmmm" + T);
    p.def<&replaceSub<W>>(m + "7replaceEmmRKS5_mm");
    p.def<&erase<W>>(m + "5eraseEmm");
    p.def<&reserve<W>>(m + "7reserveEm");
    p.def<&resize<W>>(m + "6resizeEm" + T);
    p.def<&at<W>>(m + "2atEm");
    p.def<&at<W>>(k + "2atEm");
    p.def<&copyOut<W>>(k + "4copyEP" + T + "mm");
    p.def<&findPtr<W>>(k + "4findE" + P + "mm");
    p.def<&findChar<W>>(k + "4findE" + T + "m");
    p.def<&rfindPtr<W>>(k + "5rfindE" + P + "mm");
    p.def<&rfindChar<W>>(k + "5rfindE" + T + "m");
    p.def<&findFirstOf<W>>(k + "13find_first_ofE" + P + "mm");
    p.def<&findLastOf<W>>(k + "12find_last_ofE" + P + "mm");
    p.def<&findFirstNotOf<W>>(k + "17find_first_not_ofE" + P + "mm");
    p.def<&findLastNotOf<W>>(k + "16find_last_not_ofE" + P + "mm");
    p.def<&comparePtr<W>>(k + "7compareE" + P);
    p.def<&comparePosPtr<W>>(k + "7compareEmm" + P);
    p.def<&comparePosPtrN<W>>(k + "7compareEmm" + P + "m");
    p.def<&comparePosSub<W>>(k + "7compareEmmRKS5_mm");
    p.def<&growBy<W>>(m + "9__grow_byEmmmmmm");
    p.def<&growByAndReplace<W>>(m + "21__grow_by_and_replaceEmmmmmm" + P);
    p.mem().write<uint32_t>(p.defineDataBlock(m + "4nposE", 4), kNpos);
}

} // namespace

void registerLibcxxString(Process& p) {
    registerFor<1>(p, "c");
    registerFor<4>(p, "w");
    p.def<&throwLength>("__ZNKSt3__121__basic_string_commonILb1EE20__throw_length_errorEv");
    p.def<&throwRange>("__ZNKSt3__121__basic_string_commonILb1EE20__throw_out_of_rangeEv");
    p.def<&toStringI>("__ZNSt3__19to_stringEi");
    p.def<&toStringI>("__ZNSt3__19to_stringEl");
    p.def<&toStringU>("__ZNSt3__19to_stringEj");
    p.def<&toStringU>("__ZNSt3__19to_stringEm");
    p.def<&toStringLL>("__ZNSt3__19to_stringEx");
    p.def<&toStringULL>("__ZNSt3__19to_stringEy");
    p.def<&toStringF>("__ZNSt3__19to_stringEf");
    p.def<&toStringD>("__ZNSt3__19to_stringEd");
    p.def<&toStringD>("__ZNSt3__19to_stringEe");
}

} // namespace Hydrogen::Runtime
