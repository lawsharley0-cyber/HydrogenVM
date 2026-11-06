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
// Buffers come from the guest heap, the same heap operator new/delete use, so
// the app's inline code may free or reuse them.
#include "runtime/Process.hpp"
#include <algorithm>
#include <cinttypes>
#include <cstdio>
#include <stdexcept>
#include <string>
#include <vector>

namespace Hydrogen::Runtime {
namespace {

constexpr uint32_t kNpos = 0xFFFFFFFFu;

[[noreturn]] void outOfRange(const char* where) { throw std::runtime_error(std::string("std::out_of_range: ") + where); }
[[noreturn]] void lengthError(const char* where) { throw std::runtime_error(std::string("std::length_error: ") + where); }

template <uint32_t W> struct Str {
    static constexpr uint32_t kMinCap = W == 1 ? 11 : 2;
    static constexpr uint32_t kShortData = W == 1 ? 1 : 4;
    static constexpr uint32_t kAlign = 16 / W;
    // allocator<T>::max_size() - __alignment (little-endian libc++).
    static constexpr uint32_t kMaxSize = (0xFFFFFFFFu / W) - 16;
    using Chars = std::vector<uint32_t>;

    Ctx& x;
    explicit Str(Ctx& c) : x(c) {}

    uint32_t load(uint32_t p, uint32_t i) const { return W == 1 ? x.mem.read<uint8_t>(p + i) : x.mem.read<uint32_t>(p + i * 4); }
    void store(uint32_t p, uint32_t i, uint32_t c) const {
        if constexpr (W == 1) x.mem.write<uint8_t>(p + i, uint8_t(c));
        else x.mem.write<uint32_t>(p + i * 4, c);
    }

    bool isLong(uint32_t s) const { return x.mem.read<uint8_t>(s) & 1; }
    uint32_t size(uint32_t s) const { return isLong(s) ? x.mem.read<uint32_t>(s + 4) : x.mem.read<uint8_t>(s) >> 1; }
    uint32_t data(uint32_t s) const { return isLong(s) ? x.mem.read<uint32_t>(s + 8) : s + kShortData; }
    uint32_t capacity(uint32_t s) const { return (isLong(s) ? (x.mem.read<uint32_t>(s) & ~1u) : kMinCap) - 1; }
    void setSize(uint32_t s, uint32_t n) const {
        if (isLong(s)) x.mem.write<uint32_t>(s + 4, n);
        else x.mem.write<uint8_t>(s, uint8_t(n << 1));
    }

    static uint32_t recommend(uint32_t n) {
        if (n < kMinCap) return kMinCap - 1;
        const uint64_t r = (uint64_t(n) + 1 + kAlign - 1) / kAlign * kAlign;
        return uint32_t(r - 1);
    }
    uint32_t alloc(uint32_t chars) const {   // chars includes the NUL
        if (uint64_t(chars) * W > 0xFFFFFFF0ull) lengthError("basic_string");
        const uint32_t p = x.proc.heap().alloc(chars * W);
        if (!p) throw std::runtime_error("std::__1::basic_string: out of guest memory");
        return p;
    }
    void dealloc(uint32_t p) const { if (p) x.proc.heap().free(p); }

    Chars read(uint32_t p, uint32_t n) const {
        Chars v(n);
        if (W == 1 && n) {
            const uint8_t* h = static_cast<const uint8_t*>(x.mem.host(p));
            for (uint32_t i = 0; i < n; ++i) v[i] = h[i];
        } else {
            for (uint32_t i = 0; i < n; ++i) v[i] = load(p, i);
        }
        return v;
    }
    Chars value(uint32_t s) const { return read(data(s), size(s)); }
    uint32_t cstrlen(uint32_t p) const {
        uint32_t n = 0;
        while (load(p, n)) ++n;
        return n;
    }

    // Makes `s` hold `v` with room for at least `cap` characters, reusing the
    // buffer when it is large enough (as libc++ does). Frees the old buffer.
    void put(uint32_t s, const Chars& v) {
        const uint32_t n = uint32_t(v.size());
        if (n > kMaxSize) lengthError("basic_string");
        if (n <= capacity(s)) {
            const uint32_t p = data(s);
            for (uint32_t i = 0; i < n; ++i) store(p, i, v[i]);
            store(p, n, 0);
            setSize(s, n);
            return;
        }
        // __grow_by_and_replace: at least twice the old capacity.
        const uint32_t oldCap = capacity(s);
        const uint32_t cap = oldCap < kMaxSize / 2 - 16 ? recommend(std::max(n, 2 * oldCap)) : kMaxSize - 1;
        const uint32_t p = alloc(cap + 1);
        for (uint32_t i = 0; i < n; ++i) store(p, i, v[i]);
        store(p, n, 0);
        if (isLong(s)) dealloc(data(s));
        setLong(s, p, cap, n);
    }
    void setLong(uint32_t s, uint32_t p, uint32_t cap, uint32_t n) const {
        x.mem.write<uint32_t>(s, (cap + 1) | 1);
        x.mem.write<uint32_t>(s + 4, n);
        x.mem.write<uint32_t>(s + 8, p);
    }
    // __init: construct `s` (uninitialised memory) from `v`, reserving `res`.
    void init(uint32_t s, const Chars& v, uint32_t res) {
        const uint32_t n = uint32_t(v.size());
        if (res > kMaxSize || n > kMaxSize) lengthError("basic_string");
        res = std::max(res, n);
        uint32_t p;
        if (res < kMinCap) {
            x.mem.write<uint8_t>(s, uint8_t(n << 1));
            p = s + kShortData;
        } else {
            const uint32_t cap = recommend(res);
            p = alloc(cap + 1);
            setLong(s, p, cap, n);
        }
        for (uint32_t i = 0; i < n; ++i) store(p, i, v[i]);
        store(p, n, 0);
    }

    // Character comparison as char_traits does it: unsigned bytes for char,
    // signed wchar_t (wmemcmp) for wchar_t.
    static int cmp(uint32_t a, uint32_t b) {
        if constexpr (W == 1) return a < b ? -1 : a > b ? 1 : 0;
        else return int32_t(a) < int32_t(b) ? -1 : int32_t(a) > int32_t(b) ? 1 : 0;
    }
    static int compare(const Chars& a, const Chars& b) {
        const size_t n = std::min(a.size(), b.size());
        for (size_t i = 0; i < n; ++i)
            if (const int r = cmp(a[i], b[i])) return r;
        return a.size() < b.size() ? -1 : a.size() > b.size() ? 1 : 0;
    }
};

template <uint32_t W> Str<W> S(Ctx& x) { return Str<W>(x); }
template <uint32_t W> typename Str<W>::Chars fill(uint32_t n, uint32_t c) { return typename Str<W>::Chars(n, c); }
template <uint32_t W> uint32_t clip(uint32_t sz, uint32_t pos, uint32_t n) { return std::min(n, sz - pos); }

// ---------------------------------------------------------- constructors ---
template <uint32_t W> void initPtr(Ctx& x, GuestPtr self, GuestPtr s, uint32_t n) { auto t = S<W>(x); t.init(self, t.read(s, n), 0); }
template <uint32_t W> void initPtrRes(Ctx& x, GuestPtr self, GuestPtr s, uint32_t n, uint32_t res) {
    auto t = S<W>(x);
    if (res > Str<W>::kMaxSize) lengthError("basic_string::__init");
    t.init(self, t.read(s, n), res);
}
template <uint32_t W> void initFill(Ctx& x, GuestPtr self, uint32_t n, uint32_t c) { S<W>(x).init(self, fill<W>(n, c), 0); }

// The iOS ARM C++ ABI returns `this` from constructors and destructors.
template <uint32_t W> uint32_t copyCtor(Ctx& x, GuestPtr self, GuestPtr other) {
    auto t = S<W>(x);
    if (!t.isLong(other)) {
        for (uint32_t i = 0; i < 12; i += 4) x.mem.write<uint32_t>(self + i, x.mem.read<uint32_t>(other + i));
    } else {
        t.init(self, t.value(other), 0);
    }
    return self;
}
template <uint32_t W> uint32_t copyCtorAlloc(Ctx& x, GuestPtr self, GuestPtr other, GuestPtr) { return copyCtor<W>(x, self, other); }
template <uint32_t W> uint32_t substrCtor(Ctx& x, GuestPtr self, GuestPtr other, uint32_t pos, uint32_t n, GuestPtr) {
    auto t = S<W>(x);
    const uint32_t sz = t.size(other);
    if (pos > sz) outOfRange("basic_string");
    t.init(self, t.read(t.data(other) + pos * W, clip<W>(sz, pos, n)), 0);
    return self;
}
template <uint32_t W> uint32_t dtor(Ctx& x, GuestPtr self) {
    auto t = S<W>(x);
    if (t.isLong(self)) t.dealloc(t.data(self));
    return self;
}

// -------------------------------------------------------------- assign ---
template <uint32_t W> uint32_t assignPtrN(Ctx& x, GuestPtr self, GuestPtr s, uint32_t n) { auto t = S<W>(x); t.put(self, t.read(s, n)); return self; }
template <uint32_t W> uint32_t assignPtr(Ctx& x, GuestPtr self, GuestPtr s) { auto t = S<W>(x); t.put(self, t.read(s, t.cstrlen(s))); return self; }
template <uint32_t W> uint32_t assignFill(Ctx& x, GuestPtr self, uint32_t n, uint32_t c) { S<W>(x).put(self, fill<W>(n, c)); return self; }
template <uint32_t W> uint32_t assignSub(Ctx& x, GuestPtr self, GuestPtr other, uint32_t pos, uint32_t n) {
    auto t = S<W>(x);
    const uint32_t sz = t.size(other);
    if (pos > sz) outOfRange("basic_string::assign");
    t.put(self, t.read(t.data(other) + pos * W, clip<W>(sz, pos, n)));
    return self;
}
template <uint32_t W> uint32_t opAssign(Ctx& x, GuestPtr self, GuestPtr other) {
    if (self != other) { auto t = S<W>(x); t.put(self, t.value(other)); }
    return self;
}
template <uint32_t W> uint32_t opAssignChar(Ctx& x, GuestPtr self, uint32_t c) { S<W>(x).put(self, fill<W>(1, c)); return self; }

// -------------------------------------------------------------- append ---
template <uint32_t W> void appendChars(Ctx& x, GuestPtr self, const typename Str<W>::Chars& add) {
    auto t = S<W>(x);
    auto v = t.value(self);
    if (uint64_t(v.size()) + add.size() > Str<W>::kMaxSize) lengthError("basic_string::append");
    v.insert(v.end(), add.begin(), add.end());
    t.put(self, v);
}
template <uint32_t W> uint32_t appendPtrN(Ctx& x, GuestPtr self, GuestPtr s, uint32_t n) { appendChars<W>(x, self, S<W>(x).read(s, n)); return self; }
template <uint32_t W> uint32_t appendPtr(Ctx& x, GuestPtr self, GuestPtr s) { auto t = S<W>(x); appendChars<W>(x, self, t.read(s, t.cstrlen(s))); return self; }
template <uint32_t W> uint32_t appendFill(Ctx& x, GuestPtr self, uint32_t n, uint32_t c) { appendChars<W>(x, self, fill<W>(n, c)); return self; }
template <uint32_t W> uint32_t appendSub(Ctx& x, GuestPtr self, GuestPtr other, uint32_t pos, uint32_t n) {
    auto t = S<W>(x);
    const uint32_t sz = t.size(other);
    if (pos > sz) outOfRange("basic_string::append");
    appendChars<W>(x, self, t.read(t.data(other) + pos * W, clip<W>(sz, pos, n)));
    return self;
}
template <uint32_t W> void pushBack(Ctx& x, GuestPtr self, uint32_t c) { appendChars<W>(x, self, fill<W>(1, c)); }

// ---------------------------------------------------- insert / replace ---
template <uint32_t W> void replaceChars(Ctx& x, GuestPtr self, uint32_t pos, uint32_t n1, const typename Str<W>::Chars& add, const char* where) {
    auto t = S<W>(x);
    auto v = t.value(self);
    if (pos > v.size()) outOfRange(where);
    n1 = clip<W>(uint32_t(v.size()), pos, n1);
    if (uint64_t(v.size()) - n1 + add.size() > Str<W>::kMaxSize) lengthError(where);
    v.erase(v.begin() + pos, v.begin() + pos + n1);
    v.insert(v.begin() + pos, add.begin(), add.end());
    t.put(self, v);
}
template <uint32_t W> uint32_t insertPtrN(Ctx& x, GuestPtr self, uint32_t pos, GuestPtr s, uint32_t n) {
    replaceChars<W>(x, self, pos, 0, S<W>(x).read(s, n), "basic_string::insert");
    return self;
}
template <uint32_t W> uint32_t insertPtr(Ctx& x, GuestPtr self, uint32_t pos, GuestPtr s) {
    auto t = S<W>(x);
    replaceChars<W>(x, self, pos, 0, t.read(s, t.cstrlen(s)), "basic_string::insert");
    return self;
}
template <uint32_t W> uint32_t insertFill(Ctx& x, GuestPtr self, uint32_t pos, uint32_t n, uint32_t c) {
    replaceChars<W>(x, self, pos, 0, fill<W>(n, c), "basic_string::insert");
    return self;
}
template <uint32_t W> uint32_t insertSub(Ctx& x, GuestPtr self, uint32_t pos1, GuestPtr other, uint32_t pos2, uint32_t n) {
    auto t = S<W>(x);
    const uint32_t sz = t.size(other);
    if (pos2 > sz) outOfRange("basic_string::insert");
    replaceChars<W>(x, self, pos1, 0, t.read(t.data(other) + pos2 * W, clip<W>(sz, pos2, n)), "basic_string::insert");
    return self;
}
// iterator insert(const_iterator p, T c): __wrap_iter is one pointer, passed and
// returned in a register. The buffer may move, so the result is recomputed.
template <uint32_t W> uint32_t insertIter(Ctx& x, GuestPtr self, GuestPtr it, uint32_t c) {
    auto t = S<W>(x);
    const uint32_t ip = (it - t.data(self)) / W;
    replaceChars<W>(x, self, ip, 0, fill<W>(1, c), "basic_string::insert");
    return t.data(self) + ip * W;
}
template <uint32_t W> uint32_t replacePtrN(Ctx& x, GuestPtr self, uint32_t pos, uint32_t n1, GuestPtr s, uint32_t n2) {
    replaceChars<W>(x, self, pos, n1, S<W>(x).read(s, n2), "basic_string::replace");
    return self;
}
template <uint32_t W> uint32_t replacePtr(Ctx& x, GuestPtr self, uint32_t pos, uint32_t n1, GuestPtr s) {
    auto t = S<W>(x);
    replaceChars<W>(x, self, pos, n1, t.read(s, t.cstrlen(s)), "basic_string::replace");
    return self;
}
template <uint32_t W> uint32_t replaceFill(Ctx& x, GuestPtr self, uint32_t pos, uint32_t n1, uint32_t n2, uint32_t c) {
    replaceChars<W>(x, self, pos, n1, fill<W>(n2, c), "basic_string::replace");
    return self;
}
template <uint32_t W> uint32_t replaceSub(Ctx& x, GuestPtr self, uint32_t pos1, uint32_t n1, GuestPtr other, uint32_t pos2, uint32_t n2) {
    auto t = S<W>(x);
    const uint32_t sz = t.size(other);
    if (pos2 > sz) outOfRange("basic_string::replace");
    replaceChars<W>(x, self, pos1, n1, t.read(t.data(other) + pos2 * W, clip<W>(sz, pos2, n2)), "basic_string::replace");
    return self;
}
template <uint32_t W> uint32_t erase(Ctx& x, GuestPtr self, uint32_t pos, uint32_t n) {
    replaceChars<W>(x, self, pos, n, {}, "basic_string::erase");
    return self;
}

// ------------------------------------------------------ reserve / resize ---
// libc++'s reserve also shrinks: reserve(0) on a long string that fits in the
// inline buffer moves it back inline.
template <uint32_t W> void reserve(Ctx& x, GuestPtr self, uint32_t res) {
    auto t = S<W>(x);
    if (res > Str<W>::kMaxSize) lengthError("basic_string::reserve");
    const uint32_t cap = t.capacity(self), sz = t.size(self);
    res = Str<W>::recommend(std::max(res, sz));
    if (res == cap) return;
    const auto v = t.value(self);
    const bool wasLong = t.isLong(self);
    const uint32_t old = t.data(self);
    if (res == Str<W>::kMinCap - 1) {
        x.mem.write<uint8_t>(self, uint8_t(sz << 1));
        for (uint32_t i = 0; i < sz; ++i) t.store(self + Str<W>::kShortData, i, v[i]);
        t.store(self + Str<W>::kShortData, sz, 0);
    } else {
        const uint32_t p = t.alloc(res + 1);
        for (uint32_t i = 0; i < sz; ++i) t.store(p, i, v[i]);
        t.store(p, sz, 0);
        t.setLong(self, p, res, sz);
    }
    if (wasLong) t.dealloc(old);
}
template <uint32_t W> void resize(Ctx& x, GuestPtr self, uint32_t n, uint32_t c) {
    auto t = S<W>(x);
    const uint32_t sz = t.size(self);
    if (n > sz) { appendChars<W>(x, self, fill<W>(n - sz, c)); return; }
    t.store(t.data(self), n, 0);
    t.setSize(self, n);
}

// ------------------------------------------------------------ queries ---
template <uint32_t W> uint32_t copyOut(Ctx& x, GuestPtr self, GuestPtr dst, uint32_t n, uint32_t pos) {
    auto t = S<W>(x);
    const uint32_t sz = t.size(self);
    if (pos > sz) outOfRange("basic_string::copy");
    const uint32_t r = clip<W>(sz, pos, n);
    const auto v = t.read(t.data(self) + pos * W, r);
    for (uint32_t i = 0; i < r; ++i) t.store(dst, i, v[i]);
    return r;
}
template <uint32_t W> uint32_t at(Ctx& x, GuestPtr self, uint32_t n) {
    auto t = S<W>(x);
    if (n >= t.size(self)) outOfRange("basic_string::at");
    return t.data(self) + n * W;
}

using Chars = std::vector<uint32_t>;
uint32_t findSeq(const Chars& h, const Chars& n, uint32_t pos) {
    if (pos > h.size()) return kNpos;
    if (n.empty()) return pos;
    auto it = std::search(h.begin() + pos, h.end(), n.begin(), n.end());
    return it == h.end() ? kNpos : uint32_t(it - h.begin());
}
uint32_t rfindSeq(const Chars& h, const Chars& n, uint32_t pos) {
    const uint32_t sz = uint32_t(h.size());
    pos = std::min(pos, sz);
    const uint32_t end = n.size() < sz - pos ? pos + uint32_t(n.size()) : sz;
    auto it = std::find_end(h.begin(), h.begin() + end, n.begin(), n.end());
    if (!n.empty() && it == h.begin() + end) return kNpos;
    return uint32_t(it - h.begin());
}
bool in(const Chars& set, uint32_t c) { return std::find(set.begin(), set.end(), c) != set.end(); }
uint32_t firstOf(const Chars& h, const Chars& set, uint32_t pos, bool want) {
    if (!want || !set.empty())
        for (uint32_t i = pos; i < h.size(); ++i) if (in(set, h[i]) == want) return i;
    return kNpos;
}
uint32_t lastOf(const Chars& h, const Chars& set, uint32_t pos, bool want) {
    if (h.empty() || (want && set.empty())) return kNpos;
    for (uint32_t i = std::min<uint32_t>(pos, uint32_t(h.size()) - 1) + 1; i-- > 0;) if (in(set, h[i]) == want) return i;
    return kNpos;
}

template <uint32_t W> uint32_t findPtr(Ctx& x, GuestPtr self, GuestPtr s, uint32_t pos, uint32_t n) { auto t = S<W>(x); return findSeq(t.value(self), t.read(s, n), pos); }
template <uint32_t W> uint32_t findChar(Ctx& x, GuestPtr self, uint32_t c, uint32_t pos) { auto t = S<W>(x); return findSeq(t.value(self), Chars{c}, pos); }
template <uint32_t W> uint32_t rfindPtr(Ctx& x, GuestPtr self, GuestPtr s, uint32_t pos, uint32_t n) { auto t = S<W>(x); return rfindSeq(t.value(self), t.read(s, n), pos); }
template <uint32_t W> uint32_t rfindChar(Ctx& x, GuestPtr self, uint32_t c, uint32_t pos) { auto t = S<W>(x); return rfindSeq(t.value(self), Chars{c}, pos); }
template <uint32_t W> uint32_t findFirstOf(Ctx& x, GuestPtr self, GuestPtr s, uint32_t pos, uint32_t n) { auto t = S<W>(x); return firstOf(t.value(self), t.read(s, n), pos, true); }
template <uint32_t W> uint32_t findFirstNotOf(Ctx& x, GuestPtr self, GuestPtr s, uint32_t pos, uint32_t n) { auto t = S<W>(x); return firstOf(t.value(self), t.read(s, n), pos, false); }
template <uint32_t W> uint32_t findLastOf(Ctx& x, GuestPtr self, GuestPtr s, uint32_t pos, uint32_t n) { auto t = S<W>(x); return lastOf(t.value(self), t.read(s, n), pos, true); }
template <uint32_t W> uint32_t findLastNotOf(Ctx& x, GuestPtr self, GuestPtr s, uint32_t pos, uint32_t n) { auto t = S<W>(x); return lastOf(t.value(self), t.read(s, n), pos, false); }

template <uint32_t W> int32_t compareAt(Ctx& x, GuestPtr self, uint32_t pos, uint32_t n1, const Chars& other) {
    auto t = S<W>(x);
    const uint32_t sz = t.size(self);
    if (pos > sz) outOfRange("basic_string::compare");
    return Str<W>::compare(t.read(t.data(self) + pos * W, clip<W>(sz, pos, n1)), other);
}
template <uint32_t W> int32_t comparePtr(Ctx& x, GuestPtr self, GuestPtr s) { auto t = S<W>(x); return compareAt<W>(x, self, 0, kNpos, t.read(s, t.cstrlen(s))); }
template <uint32_t W> int32_t comparePosPtr(Ctx& x, GuestPtr self, uint32_t pos, uint32_t n1, GuestPtr s) { auto t = S<W>(x); return compareAt<W>(x, self, pos, n1, t.read(s, t.cstrlen(s))); }
template <uint32_t W> int32_t comparePosPtrN(Ctx& x, GuestPtr self, uint32_t pos, uint32_t n1, GuestPtr s, uint32_t n2) { return compareAt<W>(x, self, pos, n1, S<W>(x).read(s, n2)); }
template <uint32_t W> int32_t comparePosSub(Ctx& x, GuestPtr self, uint32_t pos1, uint32_t n1, GuestPtr other, uint32_t pos2, uint32_t n2) {
    auto t = S<W>(x);
    const uint32_t sz = t.size(other);
    if (pos2 > sz) outOfRange("basic_string::compare");
    return compareAt<W>(x, self, pos1, n1, t.read(t.data(other) + pos2 * W, clip<W>(sz, pos2, n2)));
}

// ------------------------------------------------------------ growth ---
// __grow_by(old_cap, delta_cap, old_sz, n_copy, n_del, n_add): allocates a new
// buffer, copies [0, n_copy) and the tail after n_del + n_copy (leaving a gap
// of n_add), frees the old buffer and stores the new pointer and capacity. It
// does not set the size or the terminator; the inline caller does that.
template <uint32_t W> uint32_t growTo(Ctx& x, GuestPtr self, uint32_t oldCap, uint32_t delta, uint32_t oldSz, uint32_t nCopy,
                                      uint32_t nDel, uint32_t nAdd, const Chars* add) {
    auto t = S<W>(x);
    constexpr uint32_t ms = Str<W>::kMaxSize;
    if (delta > ms - oldCap) lengthError("basic_string");
    if (uint64_t(nCopy) + nDel > oldSz) outOfRange("basic_string::__grow_by");
    const uint32_t old = t.data(self);
    const uint32_t cap = oldCap < ms / 2 - 16 ? Str<W>::recommend(std::max(oldCap + delta, 2 * oldCap)) : ms - 1;
    const uint32_t tail = oldSz - nDel - nCopy;
    if (uint64_t(nCopy) + nAdd + tail > cap) lengthError("basic_string::__grow_by");
    const auto head = t.read(old, nCopy);
    const auto rest = t.read(old + (nCopy + nDel) * W, tail);
    const uint32_t p = t.alloc(cap + 1);
    for (uint32_t i = 0; i < nCopy; ++i) t.store(p, i, head[i]);
    if (add) for (uint32_t i = 0; i < nAdd; ++i) t.store(p, nCopy + i, (*add)[i]);
    for (uint32_t i = 0; i < tail; ++i) t.store(p, nCopy + nAdd + i, rest[i]);
    // libc++ frees unless the old buffer was the inline one (old_cap + 1 == min_cap).
    if (oldCap + 1 != Str<W>::kMinCap) t.dealloc(old);
    const uint32_t keepSize = t.isLong(self) ? x.mem.read<uint32_t>(self + 4) : 0;
    x.mem.write<uint32_t>(self, (cap + 1) | 1);
    x.mem.write<uint32_t>(self + 4, keepSize);
    x.mem.write<uint32_t>(self + 8, p);
    return p;
}
template <uint32_t W> void growBy(Ctx& x, GuestPtr self, uint32_t oldCap, uint32_t delta, uint32_t oldSz, uint32_t nCopy, uint32_t nDel, uint32_t nAdd) {
    growTo<W>(x, self, oldCap, delta, oldSz, nCopy, nDel, nAdd, nullptr);
}
template <uint32_t W> void growByAndReplace(Ctx& x, GuestPtr self, uint32_t oldCap, uint32_t delta, uint32_t oldSz, uint32_t nCopy, uint32_t nDel,
                                            uint32_t nAdd, GuestPtr s) {
    auto t = S<W>(x);
    const auto add = t.read(s, nAdd);
    const uint32_t p = growTo<W>(x, self, oldCap, delta, oldSz, nCopy, nDel, nAdd, &add);
    const uint32_t sz = nCopy + nAdd + (oldSz - nDel - nCopy);
    x.mem.write<uint32_t>(self + 4, sz);
    t.store(p, sz, 0);
}

// -------------------------------------------------------- to_string ---
void putResult(Ctx& x, GuestPtr ret, const std::string& s) {
    Str<1> t(x);
    t.init(ret, Chars(s.begin(), s.end()), 0);
}
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
