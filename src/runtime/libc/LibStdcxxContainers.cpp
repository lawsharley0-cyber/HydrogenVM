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

struct Strings { explicit Strings(Process&) {} uint32_t emptyRep = 0; };
uint32_t emptyData(Ctx& x) { return x.proc.service<Strings>().emptyRep + kHdr; }

[[noreturn]] void outOfRange(const char* where) { throw std::runtime_error(std::string("std::out_of_range: ") + where); }
[[noreturn]] void lengthError(const char* where) { throw std::runtime_error(std::string("std::length_error: ") + where); }

uint32_t len(Ctx& x, uint32_t data) { return x.mem.read<uint32_t>(data - kHdr); }
std::string bytes(Ctx& x, uint32_t p, uint32_t n) {
    std::string s(n, '\0');
    if (n) std::memcpy(s.data(), x.mem.host(p), n);
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
    x.mem.write<uint32_t>(rep + 8, uint32_t(rc - 1));
    if (rc <= 0) x.proc.heap().free(rep);
}
// A reference to `data` for a new owner: shares it, or copies a leaked buffer.
uint32_t grab(Ctx& x, uint32_t data) {
    if (data == emptyData(x)) return data;
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
    if (!s) throw std::runtime_error("std::logic_error: basic_string::_S_construct NULL not valid");
    x.mem.write<uint32_t>(self, make(x, x.proc.readCString(s)));
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
void r_destroy(Ctx& x, GuestPtr rep, GuestPtr) { if (rep && rep + kHdr != emptyData(x)) x.proc.heap().free(rep); }

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

// Shared with the stream code: build / read a guest std::string.
uint32_t guestStringData(Ctx& x, const std::string& s) { return make(x, s); }
std::string guestStringValue(Ctx& x, GuestPtr self) { return value(x, self); }
void guestStringDispose(Ctx& x, GuestPtr self) { dispose(x, x.mem.read<uint32_t>(self)); }

void registerLibStdcxxContainers(Process& p) {
    p.service<Strings>().emptyRep = p.defineDataBlock("__ZNSs4_Rep20_S_empty_rep_storageE", 16);
    p.mem().write<uint32_t>(p.defineDataBlock("__ZNSs4_Rep11_S_max_sizeE", 4), kMaxSize);
    p.defineDataBlock("__ZNSs4_Rep11_S_terminalE", 4);
    p.def<&c_cstr>("__ZNSsC1EPKcRKSaIcE");      p.def<&c_cstr>("__ZNSsC2EPKcRKSaIcE");
    p.def<&c_buf>("__ZNSsC1EPKcmRKSaIcE");      p.def<&c_buf>("__ZNSsC2EPKcmRKSaIcE");
    p.def<&c_copy>("__ZNSsC1ERKSs");            p.def<&c_copy>("__ZNSsC2ERKSs");
    p.def<&c_sub>("__ZNSsC1ERKSsmm");           p.def<&c_sub>("__ZNSsC2ERKSsmm");
    p.def<&d_str>("__ZNSsD1Ev");                p.def<&d_str>("__ZNSsD2Ev");
    p.def<&m_assignBuf>("__ZNSs6assignEPKcm");
    p.def<&m_assignStr>("__ZNSs6assignERKSs");
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
    p.def<&m_reserve>("__ZNSs7reserveEm");
    p.def<&m_swap>("__ZNSs4swapERSs");
    p.def<&m_leakHard>("__ZNSs12_M_leak_hardEv");
    p.def<&q_findBuf>("__ZNKSs4findEPKcmm");
    p.def<&q_findChar>("__ZNKSs4findEcm");
    p.def<&q_rfindBuf>("__ZNKSs5rfindEPKcmm");
    p.def<&q_rfindChar>("__ZNKSs5rfindEcm");
    p.def<&q_compareC>("__ZNKSs7compareEPKc");
    p.def<&q_compareStr>("__ZNKSs7compareERKSs");
    p.defineFunction("__ZNKSs6substrEmm", [](Ctx& x) { q_substr(x); });
    p.def<&r_create>("__ZNSs4_Rep9_S_createEmmRKSaIcE");
    p.def<&r_destroy>("__ZNSs4_Rep10_M_destroyERKSaIcE");

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
}

} // namespace Hydrogen::Runtime
