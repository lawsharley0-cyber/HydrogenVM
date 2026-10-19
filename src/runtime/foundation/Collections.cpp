// HydrogenVM — NSArray, NSDictionary, NSSet (+ mutable variants),
// NSEnumerator, fast enumeration, property-list conversion, -description.
#include "Foundation.hpp"
#include <algorithm>
#include <fstream>

namespace Hydrogen::Runtime::Foundation {


namespace {

struct ArrayData : ObjC::HostObject { std::vector<id> items; bool isMutable = false; uint32_t mutationCell = 0; };
struct DictData : ObjC::HostObject {
    std::vector<std::pair<id, id>> entries;
    std::unordered_multimap<uint32_t, size_t> index;   // hash -> entry
    bool isMutable = false;
    uint32_t mutationCell = 0;
};
struct SetData : ObjC::HostObject { std::vector<id> items; bool isMutable = false; uint32_t mutationCell = 0; };
struct EnumData : ObjC::HostObject { std::vector<id> items; size_t pos = 0; id owner = 0; };

struct Colls {
    explicit Colls(Process&) {}
    Class nsArray = 0, nsMutableArray = 0, arrayI = 0, arrayM = 0;
    Class nsDict = 0, nsMutableDict = 0, dictI = 0, dictM = 0;
    Class nsSet = 0, nsMutableSet = 0, setI = 0, setM = 0;
    Class enumerator = 0;
    uint32_t constMutationCell = 0;
};
Colls& C(Ctx& x) { return x.proc.service<Colls>(); }

ArrayData* ad(Ctx& x, id a) { return rt(x).hostIf<ArrayData>(a); }
DictData* dd(Ctx& x, id d) { return rt(x).hostIf<DictData>(d); }
SetData* sd(Ctx& x, id s) { return rt(x).hostIf<SetData>(s); }

[[noreturn]] void rangeError(const char* method, uint32_t index, size_t count) {
    throw ObjC::ObjCError(std::string("*** -[NSArray ") + method + "]: index " + std::to_string(index) +
                          " beyond bounds " + (count ? "[0 .. " + std::to_string(count - 1) + "]" : "for empty array"));
}

id retainObj(Ctx& x, id o) { return rt(x).retain(x.cpu, o); }
void releaseObj(Ctx& x, id o) { rt(x).release(x.cpu, o); }
id copyKey(Ctx& x, id k) {
    if (isString(x, k) || isNumber(x, k)) return id(rt(x).send(x.cpu, k, "copyWithZone:", {0}));
    if (rt(x).respondsTo(k, rt(x).sel("copyWithZone:"))) return id(rt(x).send(x.cpu, k, "copyWithZone:", {0}));
    return retainObj(x, k);
}

void bump(Ctx& x, uint32_t cell) { if (cell) x.mem.write<uint32_t>(cell, x.mem.read<uint32_t>(cell) + 1); }

// ------------------------------------------------------------- arrays ---

id newArray(Ctx& x, bool isMutable) {
    const id a = rt(x).createInstance(isMutable ? C(x).arrayM : C(x).arrayI);
    auto d = std::make_unique<ArrayData>();
    d->isMutable = isMutable;
    rt(x).setHost(a, std::move(d));
    return a;
}

std::vector<id> itemsOf(Ctx& x, id a) {
    if (!a) return {};
    if (ArrayData* d = ad(x, a)) return d->items;
    if (SetData* s = sd(x, a)) return s->items;
    // Guest NSArray subclass: primitive methods.
    const uint32_t n = uint32_t(rt(x).send(x.cpu, a, "count"));
    std::vector<id> v;
    for (uint32_t i = 0; i < n; ++i) v.push_back(id(rt(x).send(x.cpu, a, "objectAtIndex:", {i})));
    return v;
}

ArrayData& mut(Ctx& x, id a) {
    ArrayData* d = ad(x, a);
    if (!d) throw ObjC::ObjCError("mutating method sent to a non-HydrogenVM array " + rt(x).objectClassName(a));
    if (!d->isMutable) throw ObjC::ObjCError("*** -[__NSArrayI addObject:]: mutating method sent to immutable object");
    return *d;
}

id a_allocWithZone(Ctx& x, id self, SEL, uint32_t) {
    if (self == C(x).nsArray || self == C(x).arrayI) return newArray(x, false);
    if (self == C(x).nsMutableArray || self == C(x).arrayM) return newArray(x, true);
    return rt(x).createInstance(self);
}
id a_alloc(Ctx& x, id self, SEL s) { return a_allocWithZone(x, self, s, 0); }

void setItems(Ctx& x, id a, std::vector<id> items) {
    for (id o : items) if (!o) throw ObjC::ObjCError("*** -[NSArray initWithObjects:count:]: attempt to insert nil object");
    for (id o : items) retainObj(x, o);
    if (ArrayData* d = ad(x, a)) {
        for (id o : d->items) releaseObj(x, o);
        d->items = std::move(items);
    }
}
id ai_init(Ctx& x, id self, SEL) { setItems(x, self, {}); return self; }
id ai_initWithCapacity(Ctx& x, id self, SEL, uint32_t) { setItems(x, self, {}); return self; }
id ai_initWithObjectsCount(Ctx& x, id self, SEL, GuestPtr objs, uint32_t n) {
    std::vector<id> v(n);
    for (uint32_t i = 0; i < n; ++i) v[i] = x.mem.read<uint32_t>(objs + i * 4);
    setItems(x, self, std::move(v));
    return self;
}
std::vector<id> nilTerminated(Ctx& x, id first, ArgReader& rest) {
    std::vector<id> v;
    for (id o = first; o; o = rest.word()) v.push_back(o);
    return v;
}
id ai_initWithObjects(Ctx& x, id self, SEL, id first, VarArgs va) { setItems(x, self, nilTerminated(x, first, va.args)); return self; }
id ai_initWithArray(Ctx& x, id self, SEL, id other) { setItems(x, self, itemsOf(x, other)); return self; }
id ai_initWithArrayCopy(Ctx& x, id self, SEL, id other, bool copyItems) {
    auto v = itemsOf(x, other);
    if (copyItems) {
        for (auto& o : v) o = id(rt(x).send(x.cpu, o, "copyWithZone:", {0}));
        setItems(x, self, v);
        for (id o : v) releaseObj(x, o);
    } else setItems(x, self, v);
    return self;
}
id ai_initWithContentsOfFile(Ctx& x, id self, SEL, id path) {
    try {
        auto v = Plist::parseFile(utf8(x, path));
        if (v->type != Plist::Value::Type::Array) { releaseObj(x, self); return 0; }
        const bool m = ad(x, self) && ad(x, self)->isMutable;
        const id parsed = plistToObject(x, *v, m);
        setItems(x, self, itemsOf(x, parsed));
        return self;
    } catch (...) { releaseObj(x, self); return 0; }
}
template <typename F, typename... A> id aconstruct(Ctx& x, id cls, F init, A... a) {
    id o = a_allocWithZone(x, cls, 0, 0);
    o = init(x, o, 0, a...);
    return o ? autoreleased(x, o) : 0;
}
id ac_array(Ctx& x, id self, SEL) { return aconstruct(x, self, ai_init); }
id ac_arrayWithCapacity(Ctx& x, id self, SEL, uint32_t n) { return aconstruct(x, self, ai_initWithCapacity, n); }
id ac_arrayWithObject(Ctx& x, id self, SEL, id o) {
    const id a = a_allocWithZone(x, self, 0, 0);
    setItems(x, a, {o});
    return autoreleased(x, a);
}
id ac_arrayWithObjectsCount(Ctx& x, id self, SEL, GuestPtr objs, uint32_t n) { return aconstruct(x, self, ai_initWithObjectsCount, objs, n); }
id ac_arrayWithObjects(Ctx& x, id self, SEL, id first, VarArgs va) {
    const id a = a_allocWithZone(x, self, 0, 0);
    setItems(x, a, nilTerminated(x, first, va.args));
    return autoreleased(x, a);
}
id ac_arrayWithArray(Ctx& x, id self, SEL, id o) { return aconstruct(x, self, ai_initWithArray, o); }
id ac_arrayWithContentsOfFile(Ctx& x, id self, SEL, id path) { return aconstruct(x, self, ai_initWithContentsOfFile, path); }

uint32_t a_count(Ctx& x, id self, SEL) { ArrayData* d = ad(x, self); return d ? uint32_t(d->items.size()) : 0; }
id a_objectAtIndex(Ctx& x, id self, SEL, uint32_t i) {
    ArrayData* d = ad(x, self);
    if (!d || i >= d->items.size()) rangeError("objectAtIndex:", i, d ? d->items.size() : 0);
    return d->items[i];
}
id a_firstObject(Ctx& x, id self, SEL) { auto v = itemsOf(x, self); return v.empty() ? 0 : v.front(); }
id a_lastObject(Ctx& x, id self, SEL) { auto v = itemsOf(x, self); return v.empty() ? 0 : v.back(); }
uint32_t a_indexOfObject(Ctx& x, id self, SEL, id o) {
    auto v = itemsOf(x, self);
    for (size_t i = 0; i < v.size(); ++i) if (objectsEqual(x, v[i], o)) return uint32_t(i);
    return kNotFound;
}
uint32_t a_indexOfObjectIdenticalTo(Ctx& x, id self, SEL, id o) {
    auto v = itemsOf(x, self);
    for (size_t i = 0; i < v.size(); ++i) if (v[i] == o) return uint32_t(i);
    return kNotFound;
}
bool a_containsObject(Ctx& x, id self, SEL s, id o) { return a_indexOfObject(x, self, s, o) != kNotFound; }
bool a_isEqualToArray(Ctx& x, id self, SEL, id o) {
    if (o == self) return true;
    if (!o || !isArray(x, o)) return false;
    auto a = itemsOf(x, self), b = itemsOf(x, o);
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i) if (!objectsEqual(x, a[i], b[i])) return false;
    return true;
}
uint32_t a_hash(Ctx& x, id self, SEL) { return uint32_t(itemsOf(x, self).size()); }
id a_subarrayWithRange(Ctx& x, id self, SEL, uint32_t loc, uint32_t len) {
    auto v = itemsOf(x, self);
    if (uint64_t(loc) + len > v.size()) rangeError("subarrayWithRange:", loc + len, v.size());
    return makeArray(x, std::vector<id>(v.begin() + loc, v.begin() + loc + len));
}
id a_arrayByAddingObject(Ctx& x, id self, SEL, id o) { auto v = itemsOf(x, self); v.push_back(o); return makeArray(x, v); }
id a_arrayByAddingObjects(Ctx& x, id self, SEL, id other) { auto v = itemsOf(x, self); auto w = itemsOf(x, other); v.insert(v.end(), w.begin(), w.end()); return makeArray(x, v); }
id a_componentsJoined(Ctx& x, id self, SEL, id sep) {
    std::u16string out;
    const std::u16string s = stringValue(x, sep);
    bool first = true;
    for (id o : itemsOf(x, self)) { if (!first) out += s; first = false; out += isString(x, o) ? stringValue(x, o) : Plist::utf8ToUtf16(describe(x, o)); }
    return makeString(x, out);
}
void a_makeObjectsPerform(Ctx& x, id self, SEL, SEL s) { for (id o : itemsOf(x, self)) rt(x).send(x.cpu, o, s); }
void a_makeObjectsPerformWith(Ctx& x, id self, SEL, SEL s, id arg) { for (id o : itemsOf(x, self)) rt(x).send(x.cpu, o, s, {arg}); }
id a_copyWithZone(Ctx& x, id self, SEL, uint32_t) {
    ArrayData* d = ad(x, self);
    if (d && !d->isMutable) return retainObj(x, self);
    return makeArray(x, itemsOf(x, self), true, false);
}
id a_mutableCopyWithZone(Ctx& x, id self, SEL, uint32_t) { return makeArray(x, itemsOf(x, self), true, true); }
id a_copy(Ctx& x, id self, SEL s) { return a_copyWithZone(x, self, s, 0); }
id a_mutableCopy(Ctx& x, id self, SEL s) { return a_mutableCopyWithZone(x, self, s, 0); }

uint32_t fastEnumerate(Ctx& x, const std::vector<id>& items, uint32_t mutationCell, GuestPtr state, GuestPtr buf, uint32_t len) {
    uint32_t pos = x.mem.read<uint32_t>(state);
    if (pos == 0) x.mem.write<uint32_t>(state + 8, mutationCell ? mutationCell : C(x).constMutationCell);
    if (pos >= items.size()) return 0;
    const uint32_t n = std::min<uint32_t>(len, uint32_t(items.size()) - pos);
    for (uint32_t i = 0; i < n; ++i) x.mem.write<uint32_t>(buf + i * 4, items[pos + i]);
    x.mem.write<uint32_t>(state + 4, buf);
    x.mem.write<uint32_t>(state, pos + n);
    return n;
}
uint32_t mutationCellFor(Ctx& x, uint32_t& cell) { if (!cell) cell = x.proc.heap().allocZeroed(4); return cell; }
uint32_t a_countByEnumerating(Ctx& x, id self, SEL, GuestPtr state, GuestPtr buf, uint32_t len) {
    ArrayData* d = ad(x, self);
    if (!d) return fastEnumerate(x, itemsOf(x, self), 0, state, buf, len);
    return fastEnumerate(x, d->items, d->isMutable ? mutationCellFor(x, d->mutationCell) : 0, state, buf, len);
}

id newEnumerator(Ctx& x, std::vector<id> items, id owner) {
    const id e = rt(x).createInstance(C(x).enumerator);
    auto d = std::make_unique<EnumData>();
    d->items = std::move(items);
    d->owner = retainObj(x, owner);
    rt(x).setHost(e, std::move(d));
    return autoreleased(x, e);
}
id a_objectEnumerator(Ctx& x, id self, SEL) { return newEnumerator(x, itemsOf(x, self), self); }
id a_reverseObjectEnumerator(Ctx& x, id self, SEL) { auto v = itemsOf(x, self); std::reverse(v.begin(), v.end()); return newEnumerator(x, v, self); }

void callBlock(Ctx& x, id block, std::initializer_list<uint32_t> args) {
    const uint32_t invoke = x.mem.read<uint32_t>(block + 12);
    std::vector<uint32_t> a{block};
    a.insert(a.end(), args.begin(), args.end());
    x.proc.callGuest(x.cpu, invoke, a);
}
void a_enumerateUsingBlock(Ctx& x, id self, SEL, id block) {
    auto v = itemsOf(x, self);
    const uint32_t stop = x.proc.heap().allocZeroed(4);
    for (size_t i = 0; i < v.size(); ++i) {
        callBlock(x, block, {v[i], uint32_t(i), stop});
        if (x.mem.read<uint8_t>(stop)) break;
    }
    x.proc.heap().free(stop);
}
void a_enumerateWithOptions(Ctx& x, id self, SEL, uint32_t opts, id block) {
    auto v = itemsOf(x, self);
    const uint32_t stop = x.proc.heap().allocZeroed(4);
    const bool rev = opts & 2;   // NSEnumerationReverse
    for (size_t k = 0; k < v.size(); ++k) {
        const size_t i = rev ? v.size() - 1 - k : k;
        callBlock(x, block, {v[i], uint32_t(i), stop});
        if (x.mem.read<uint8_t>(stop)) break;
    }
    x.proc.heap().free(stop);
}
uint32_t a_indexOfObjectPassingTest(Ctx& x, id self, SEL, id block) {
    auto v = itemsOf(x, self);
    const uint32_t stop = x.proc.heap().allocZeroed(4);
    const uint32_t invoke = x.mem.read<uint32_t>(block + 12);
    uint32_t found = kNotFound;
    for (size_t i = 0; i < v.size(); ++i) {
        if (uint32_t(x.proc.callGuest(x.cpu, invoke, {block, v[i], uint32_t(i), stop})) & 0xFF) { found = uint32_t(i); break; }
        if (x.mem.read<uint8_t>(stop)) break;
    }
    x.proc.heap().free(stop);
    return found;
}

// sorting (stable merge sort with a guest comparator)
template <typename Cmp>
void stableSort(std::vector<id>& v, Cmp cmp) {
    std::vector<id> tmp(v.size());
    for (size_t w = 1; w < v.size(); w *= 2) {
        for (size_t lo = 0; lo < v.size(); lo += 2 * w) {
            const size_t mid = std::min(lo + w, v.size()), hi = std::min(lo + 2 * w, v.size());
            size_t i = lo, j = mid, k = lo;
            while (i < mid && j < hi) tmp[k++] = cmp(v[i], v[j]) > 0 ? v[j++] : v[i++];
            while (i < mid) tmp[k++] = v[i++];
            while (j < hi) tmp[k++] = v[j++];
        }
        v.swap(tmp);
    }
}
std::vector<id> sortedBySelector(Ctx& x, std::vector<id> v, SEL s) {
    stableSort(v, [&](id a, id b) { return int32_t(rt(x).send(x.cpu, a, s, {b})); });
    return v;
}
std::vector<id> sortedByFunction(Ctx& x, std::vector<id> v, uint32_t fn, uint32_t ctx) {
    stableSort(v, [&](id a, id b) { return int32_t(x.proc.callGuest(x.cpu, fn, {a, b, ctx})); });
    return v;
}
std::vector<id> sortedByBlock(Ctx& x, std::vector<id> v, id block) {
    const uint32_t invoke = x.mem.read<uint32_t>(block + 12);
    stableSort(v, [&](id a, id b) { return int32_t(x.proc.callGuest(x.cpu, invoke, {block, a, b})); });
    return v;
}
id a_sortedUsingSelector(Ctx& x, id self, SEL, SEL s) { return makeArray(x, sortedBySelector(x, itemsOf(x, self), s)); }
id a_sortedUsingFunction(Ctx& x, id self, SEL, uint32_t fn, uint32_t ctx) { return makeArray(x, sortedByFunction(x, itemsOf(x, self), fn, ctx)); }
id a_sortedUsingComparator(Ctx& x, id self, SEL, id block) { return makeArray(x, sortedByBlock(x, itemsOf(x, self), block)); }
void m_sortUsingSelector(Ctx& x, id self, SEL, SEL s) { auto& d = mut(x, self); d.items = sortedBySelector(x, d.items, s); bump(x, d.mutationCell); }
void m_sortUsingFunction(Ctx& x, id self, SEL, uint32_t fn, uint32_t ctx) { auto& d = mut(x, self); d.items = sortedByFunction(x, d.items, fn, ctx); }
void m_sortUsingComparator(Ctx& x, id self, SEL, id block) { auto& d = mut(x, self); d.items = sortedByBlock(x, d.items, block); }

id a_valueForKey(Ctx& x, id self, SEL, id key) {
    std::vector<id> out;
    for (id o : itemsOf(x, self)) { id v = id(rt(x).send(x.cpu, o, "valueForKey:", {key})); out.push_back(v ? v : 0); }
    out.erase(std::remove(out.begin(), out.end(), 0u), out.end());
    return makeArray(x, out);
}
bool a_writeToFile(Ctx& x, id self, SEL, id path, bool) {
    auto v = objectToPlist(x, self);
    if (!v) return false;
    try { Plist::writeXmlFile(*v, utf8(x, path)); return true; } catch (...) { return false; }
}

// mutation
void m_addObject(Ctx& x, id self, SEL, id o) {
    if (!o) throw ObjC::ObjCError("*** -[__NSArrayM insertObject:atIndex:]: object cannot be nil");
    auto& d = mut(x, self);
    d.items.push_back(retainObj(x, o));
    bump(x, d.mutationCell);
}
void m_insertObject(Ctx& x, id self, SEL, id o, uint32_t i) {
    if (!o) throw ObjC::ObjCError("*** -[__NSArrayM insertObject:atIndex:]: object cannot be nil");
    auto& d = mut(x, self);
    if (i > d.items.size()) rangeError("insertObject:atIndex:", i, d.items.size());
    d.items.insert(d.items.begin() + i, retainObj(x, o));
    bump(x, d.mutationCell);
}
void m_removeObjectAtIndex(Ctx& x, id self, SEL, uint32_t i) {
    auto& d = mut(x, self);
    if (i >= d.items.size()) rangeError("removeObjectAtIndex:", i, d.items.size());
    const id o = d.items[i];
    d.items.erase(d.items.begin() + i);
    bump(x, d.mutationCell);
    releaseObj(x, o);
}
void m_removeLastObject(Ctx& x, id self, SEL s) { auto& d = mut(x, self); if (!d.items.empty()) m_removeObjectAtIndex(x, self, s, uint32_t(d.items.size() - 1)); }
void m_removeAllObjects(Ctx& x, id self, SEL) {
    auto& d = mut(x, self);
    auto old = std::move(d.items);
    d.items.clear();
    bump(x, d.mutationCell);
    for (id o : old) releaseObj(x, o);
}
void m_removeObject(Ctx& x, id self, SEL, id o) {
    auto& d = mut(x, self);
    std::vector<id> removed;
    for (size_t i = 0; i < d.items.size();) {
        if (objectsEqual(x, d.items[i], o)) { removed.push_back(d.items[i]); d.items.erase(d.items.begin() + long(i)); }
        else ++i;
    }
    bump(x, d.mutationCell);
    for (id r : removed) releaseObj(x, r);
}
void m_removeObjectIdenticalTo(Ctx& x, id self, SEL, id o) {
    auto& d = mut(x, self);
    size_t n = 0;
    d.items.erase(std::remove_if(d.items.begin(), d.items.end(), [&](id e) { if (e == o) { ++n; return true; } return false; }), d.items.end());
    bump(x, d.mutationCell);
    for (size_t i = 0; i < n; ++i) releaseObj(x, o);
}
void m_removeObjectsInRange(Ctx& x, id self, SEL, uint32_t loc, uint32_t len) {
    auto& d = mut(x, self);
    if (uint64_t(loc) + len > d.items.size()) rangeError("removeObjectsInRange:", loc + len, d.items.size());
    std::vector<id> removed(d.items.begin() + loc, d.items.begin() + loc + len);
    d.items.erase(d.items.begin() + loc, d.items.begin() + loc + len);
    bump(x, d.mutationCell);
    for (id r : removed) releaseObj(x, r);
}
void m_removeObjectsInArray(Ctx& x, id self, SEL s, id other) { for (id o : itemsOf(x, other)) m_removeObject(x, self, s, o); }
void m_replaceObject(Ctx& x, id self, SEL, uint32_t i, id o) {
    if (!o) throw ObjC::ObjCError("*** -[__NSArrayM replaceObjectAtIndex:withObject:]: object cannot be nil");
    auto& d = mut(x, self);
    if (i >= d.items.size()) rangeError("replaceObjectAtIndex:withObject:", i, d.items.size());
    const id old = d.items[i];
    d.items[i] = retainObj(x, o);
    bump(x, d.mutationCell);
    releaseObj(x, old);
}
void m_setObjectAtIndexedSubscript(Ctx& x, id self, SEL s, id o, uint32_t i) {
    auto& d = mut(x, self);
    if (i == d.items.size()) m_addObject(x, self, s, o);
    else m_replaceObject(x, self, s, i, o);
}
void m_exchange(Ctx& x, id self, SEL, uint32_t a, uint32_t b) {
    auto& d = mut(x, self);
    if (a >= d.items.size() || b >= d.items.size()) rangeError("exchangeObjectAtIndex:withObjectAtIndex:", std::max(a, b), d.items.size());
    std::swap(d.items[a], d.items[b]);
}
void m_addObjectsFromArray(Ctx& x, id self, SEL s, id other) { for (id o : itemsOf(x, other)) m_addObject(x, self, s, o); }
void m_setArray(Ctx& x, id self, SEL s, id other) {
    auto items = itemsOf(x, other);
    for (id o : items) retainObj(x, o);
    m_removeAllObjects(x, self, s);
    auto& d = mut(x, self);
    d.items = std::move(items);
}

void array_dealloc(Ctx& x, id self, SEL) {
    if (ArrayData* d = ad(x, self)) {
        auto items = std::move(d->items);
        d->items.clear();
        if (d->mutationCell) x.proc.heap().free(d->mutationCell);
        for (id o : items) releaseObj(x, o);
    }
    rt(x).disposeInstance(x.cpu, self);
}

// ------------------------------------------------------- dictionaries ---

id newDict(Ctx& x, bool isMutable) {
    const id d = rt(x).createInstance(isMutable ? C(x).dictM : C(x).dictI);
    auto data = std::make_unique<DictData>();
    data->isMutable = isMutable;
    rt(x).setHost(d, std::move(data));
    return d;
}
long findKey(Ctx& x, DictData& d, id key, uint32_t h) {
    auto range = d.index.equal_range(h);
    for (auto it = range.first; it != range.second; ++it)
        if (objectsEqual(x, d.entries[it->second].first, key)) return long(it->second);
    return -1;
}
void reindex(Ctx& x, DictData& d) {
    d.index.clear();
    for (size_t i = 0; i < d.entries.size(); ++i) d.index.emplace(objectHash(x, d.entries[i].first), i);
}
void dictSet(Ctx& x, DictData& d, id key, id value) {
    if (!key) throw ObjC::ObjCError("*** -[__NSDictionaryM setObject:forKey:]: key cannot be nil");
    if (!value) throw ObjC::ObjCError("*** -[__NSDictionaryM setObject:forKey:]: object cannot be nil (key: " + describe(x, key) + ")");
    const uint32_t h = objectHash(x, key);
    const long i = findKey(x, d, key, h);
    retainObj(x, value);
    if (i >= 0) {
        const id old = d.entries[size_t(i)].second;
        d.entries[size_t(i)].second = value;
        releaseObj(x, old);
        return;
    }
    d.entries.emplace_back(copyKey(x, key), value);
    d.index.emplace(h, d.entries.size() - 1);
    bump(x, d.mutationCell);
}
bool dictRemove(Ctx& x, DictData& d, id key) {
    const long i = findKey(x, d, key, objectHash(x, key));
    if (i < 0) return false;
    const auto e = d.entries[size_t(i)];
    d.entries.erase(d.entries.begin() + i);
    reindex(x, d);
    bump(x, d.mutationCell);
    releaseObj(x, e.first);
    releaseObj(x, e.second);
    return true;
}
std::vector<std::pair<id, id>> entriesOf(Ctx& x, id dict) {
    if (!dict) return {};
    if (DictData* d = dd(x, dict)) return d->entries;
    std::vector<std::pair<id, id>> out;
    const id keys = id(rt(x).send(x.cpu, dict, "allKeys"));
    for (id k : itemsOf(x, keys)) out.emplace_back(k, id(rt(x).send(x.cpu, dict, "objectForKey:", {k})));
    return out;
}
void dictAssign(Ctx& x, id self, const std::vector<std::pair<id, id>>& entries) {
    DictData* d = dd(x, self);
    if (!d) return;
    for (auto& [k, v] : entries) dictSet(x, *d, k, v);
}
DictData& dmut(Ctx& x, id self) {
    DictData* d = dd(x, self);
    if (!d) throw ObjC::ObjCError("mutating method sent to a non-HydrogenVM dictionary");
    if (!d->isMutable) throw ObjC::ObjCError("*** -[__NSDictionaryI setObject:forKey:]: mutating method sent to immutable object");
    return *d;
}

id d_allocWithZone(Ctx& x, id self, SEL, uint32_t) {
    if (self == C(x).nsDict || self == C(x).dictI) return newDict(x, false);
    if (self == C(x).nsMutableDict || self == C(x).dictM) return newDict(x, true);
    return rt(x).createInstance(self);
}
id d_alloc(Ctx& x, id self, SEL s) { return d_allocWithZone(x, self, s, 0); }
id di_init(Ctx&, id self, SEL) { return self; }
id di_initWithCapacity(Ctx&, id self, SEL, uint32_t) { return self; }
id di_initWithObjectsForKeysCount(Ctx& x, id self, SEL, GuestPtr objs, GuestPtr keys, uint32_t n) {
    std::vector<std::pair<id, id>> e;
    for (uint32_t i = 0; i < n; ++i) e.emplace_back(x.mem.read<uint32_t>(keys + i * 4), x.mem.read<uint32_t>(objs + i * 4));
    dictAssign(x, self, e);
    return self;
}
id di_initWithObjectsAndKeys(Ctx& x, id self, SEL, id first, VarArgs va) {
    std::vector<std::pair<id, id>> e;
    for (id v = first; v; v = va.args.word()) { const id k = va.args.word(); e.emplace_back(k, v); }
    dictAssign(x, self, e);
    return self;
}
id di_initWithObjectsForKeys(Ctx& x, id self, SEL, id objs, id keys) {
    auto o = itemsOf(x, objs), k = itemsOf(x, keys);
    std::vector<std::pair<id, id>> e;
    for (size_t i = 0; i < std::min(o.size(), k.size()); ++i) e.emplace_back(k[i], o[i]);
    dictAssign(x, self, e);
    return self;
}
id di_initWithDictionary(Ctx& x, id self, SEL, id other) { dictAssign(x, self, entriesOf(x, other)); return self; }
id di_initWithDictionaryCopy(Ctx& x, id self, SEL s, id other, bool) { return di_initWithDictionary(x, self, s, other); }
id di_initWithContentsOfFile(Ctx& x, id self, SEL, id path) {
    try {
        auto v = Plist::parseFile(utf8(x, path));
        if (v->type != Plist::Value::Type::Dict) { releaseObj(x, self); return 0; }
        const bool m = dd(x, self) && dd(x, self)->isMutable;
        dictAssign(x, self, entriesOf(x, plistToObject(x, *v, m)));
        return self;
    } catch (...) { releaseObj(x, self); return 0; }
}
template <typename F, typename... A> id dconstruct(Ctx& x, id cls, F init, A... a) {
    id o = d_allocWithZone(x, cls, 0, 0);
    o = init(x, o, 0, a...);
    return o ? autoreleased(x, o) : 0;
}
id dc_dictionary(Ctx& x, id self, SEL) { return dconstruct(x, self, di_init); }
id dc_dictionaryWithCapacity(Ctx& x, id self, SEL, uint32_t n) { return dconstruct(x, self, di_initWithCapacity, n); }
id dc_dictionaryWithObjectForKey(Ctx& x, id self, SEL, id o, id k) {
    const id d = d_allocWithZone(x, self, 0, 0);
    dictAssign(x, d, {{k, o}});
    return autoreleased(x, d);
}
id dc_dictionaryWithObjectsForKeysCount(Ctx& x, id self, SEL, GuestPtr o, GuestPtr k, uint32_t n) { return dconstruct(x, self, di_initWithObjectsForKeysCount, o, k, n); }
id dc_dictionaryWithObjectsAndKeys(Ctx& x, id self, SEL, id first, VarArgs va) {
    const id d = d_allocWithZone(x, self, 0, 0);
    std::vector<std::pair<id, id>> e;
    for (id v = first; v; v = va.args.word()) { const id k = va.args.word(); e.emplace_back(k, v); }
    dictAssign(x, d, e);
    return autoreleased(x, d);
}
id dc_dictionaryWithObjectsForKeys(Ctx& x, id self, SEL, id o, id k) { return dconstruct(x, self, di_initWithObjectsForKeys, o, k); }
id dc_dictionaryWithDictionary(Ctx& x, id self, SEL, id o) { return dconstruct(x, self, di_initWithDictionary, o); }
id dc_dictionaryWithContentsOfFile(Ctx& x, id self, SEL, id path) { return dconstruct(x, self, di_initWithContentsOfFile, path); }

uint32_t d_count(Ctx& x, id self, SEL) { DictData* d = dd(x, self); return d ? uint32_t(d->entries.size()) : 0; }
id d_objectForKey(Ctx& x, id self, SEL, id key) {
    DictData* d = dd(x, self);
    if (!d || !key) return 0;
    const long i = findKey(x, *d, key, objectHash(x, key));
    return i < 0 ? 0 : d->entries[size_t(i)].second;
}
id d_valueForKey(Ctx& x, id self, SEL s, id key) {
    const std::string k = utf8(x, key);
    if (!k.empty() && k[0] == '@') return id(rt(x).sendSuper(x.cpu, self, rt(x).nsObject, rt(x).sel("valueForKey:"), {makeString(x, k.substr(1))}));
    return d_objectForKey(x, self, s, key);
}
id d_allKeys(Ctx& x, id self, SEL) { std::vector<id> k; for (auto& e : entriesOf(x, self)) k.push_back(e.first); return makeArray(x, k); }
id d_allValues(Ctx& x, id self, SEL) { std::vector<id> v; for (auto& e : entriesOf(x, self)) v.push_back(e.second); return makeArray(x, v); }
id d_allKeysForObject(Ctx& x, id self, SEL, id o) { std::vector<id> k; for (auto& e : entriesOf(x, self)) if (objectsEqual(x, e.second, o)) k.push_back(e.first); return makeArray(x, k); }
id d_keyEnumerator(Ctx& x, id self, SEL) { std::vector<id> k; for (auto& e : entriesOf(x, self)) k.push_back(e.first); return newEnumerator(x, k, self); }
id d_objectEnumerator(Ctx& x, id self, SEL) { std::vector<id> v; for (auto& e : entriesOf(x, self)) v.push_back(e.second); return newEnumerator(x, v, self); }
uint32_t d_countByEnumerating(Ctx& x, id self, SEL, GuestPtr state, GuestPtr buf, uint32_t len) {
    std::vector<id> k;
    DictData* d = dd(x, self);
    for (auto& e : entriesOf(x, self)) k.push_back(e.first);
    return fastEnumerate(x, k, d && d->isMutable ? mutationCellFor(x, d->mutationCell) : 0, state, buf, len);
}
void d_enumerateKeysAndObjects(Ctx& x, id self, SEL, id block) {
    const uint32_t stop = x.proc.heap().allocZeroed(4);
    for (auto& [k, v] : entriesOf(x, self)) {
        callBlock(x, block, {k, v, stop});
        if (x.mem.read<uint8_t>(stop)) break;
    }
    x.proc.heap().free(stop);
}
bool d_isEqualToDictionary(Ctx& x, id self, SEL, id o) {
    if (o == self) return true;
    if (!o || !isDict(x, o)) return false;
    auto a = entriesOf(x, self);
    if (a.size() != entriesOf(x, o).size()) return false;
    for (auto& [k, v] : a) if (!objectsEqual(x, v, dictGet(x, o, k))) return false;
    return true;
}
uint32_t d_hash(Ctx& x, id self, SEL) { return uint32_t(entriesOf(x, self).size()); }
id d_copyWithZone(Ctx& x, id self, SEL, uint32_t) {
    DictData* d = dd(x, self);
    if (d && !d->isMutable) return retainObj(x, self);
    return makeDict(x, entriesOf(x, self), true, false);
}
id d_mutableCopyWithZone(Ctx& x, id self, SEL, uint32_t) { return makeDict(x, entriesOf(x, self), true, true); }
id d_copy(Ctx& x, id self, SEL s) { return d_copyWithZone(x, self, s, 0); }
id d_mutableCopy(Ctx& x, id self, SEL s) { return d_mutableCopyWithZone(x, self, s, 0); }
bool d_writeToFile(Ctx& x, id self, SEL, id path, bool) {
    auto v = objectToPlist(x, self);
    if (!v) return false;
    try { Plist::writeXmlFile(*v, utf8(x, path)); return true; } catch (...) { return false; }
}
id d_keysSortedByValueUsingSelector(Ctx& x, id self, SEL, SEL s) {
    auto e = entriesOf(x, self);
    std::vector<id> keys;
    for (auto& p : e) keys.push_back(p.first);
    stableSort(keys, [&](id a, id b) { return int32_t(rt(x).send(x.cpu, dictGet(x, self, a), s, {dictGet(x, self, b)})); });
    return makeArray(x, keys);
}
void dm_setObjectForKey(Ctx& x, id self, SEL, id o, id k) { dictSet(x, dmut(x, self), k, o); }
void dm_setValueForKey(Ctx& x, id self, SEL, id o, id k) { if (o) dictSet(x, dmut(x, self), k, o); else dictRemove(x, dmut(x, self), k); }
void dm_removeObjectForKey(Ctx& x, id self, SEL, id k) { if (k) dictRemove(x, dmut(x, self), k); }
void dm_removeAllObjects(Ctx& x, id self, SEL) {
    auto& d = dmut(x, self);
    auto e = std::move(d.entries);
    d.entries.clear();
    d.index.clear();
    bump(x, d.mutationCell);
    for (auto& [k, v] : e) { releaseObj(x, k); releaseObj(x, v); }
}
void dm_addEntries(Ctx& x, id self, SEL, id other) { auto& d = dmut(x, self); for (auto& [k, v] : entriesOf(x, other)) dictSet(x, d, k, v); }
void dm_removeObjectsForKeys(Ctx& x, id self, SEL, id keys) { auto& d = dmut(x, self); for (id k : itemsOf(x, keys)) dictRemove(x, d, k); }
void dm_setDictionary(Ctx& x, id self, SEL s, id other) {
    auto e = entriesOf(x, other);
    for (auto& [k, v] : e) { retainObj(x, k); retainObj(x, v); }
    dm_removeAllObjects(x, self, s);
    auto& d = dmut(x, self);
    for (auto& [k, v] : e) { dictSet(x, d, k, v); releaseObj(x, k); releaseObj(x, v); }
}
void dict_dealloc(Ctx& x, id self, SEL) {
    if (DictData* d = dd(x, self)) {
        auto e = std::move(d->entries);
        d->entries.clear();
        if (d->mutationCell) x.proc.heap().free(d->mutationCell);
        for (auto& [k, v] : e) { releaseObj(x, k); releaseObj(x, v); }
    }
    rt(x).disposeInstance(x.cpu, self);
}

// --------------------------------------------------------------- sets ---

id newSet(Ctx& x, bool isMutable) {
    const id s = rt(x).createInstance(isMutable ? C(x).setM : C(x).setI);
    auto d = std::make_unique<SetData>();
    d->isMutable = isMutable;
    rt(x).setHost(s, std::move(d));
    return s;
}
bool setContains(Ctx& x, SetData& d, id o) { for (id e : d.items) if (objectsEqual(x, e, o)) return true; return false; }
void setAdd(Ctx& x, SetData& d, id o) {
    if (!o) throw ObjC::ObjCError("*** -[NSSet addObject:]: object cannot be nil");
    if (setContains(x, d, o)) return;
    d.items.push_back(retainObj(x, o));
    bump(x, d.mutationCell);
}
id s_allocWithZone(Ctx& x, id self, SEL, uint32_t) {
    if (self == C(x).nsSet || self == C(x).setI) return newSet(x, false);
    if (self == C(x).nsMutableSet || self == C(x).setM) return newSet(x, true);
    return rt(x).createInstance(self);
}
id s_alloc(Ctx& x, id self, SEL s) { return s_allocWithZone(x, self, s, 0); }
void setAssign(Ctx& x, id self, const std::vector<id>& items) { if (SetData* d = sd(x, self)) for (id o : items) setAdd(x, *d, o); }
id si_init(Ctx&, id self, SEL) { return self; }
id si_initWithCapacity(Ctx&, id self, SEL, uint32_t) { return self; }
id si_initWithArray(Ctx& x, id self, SEL, id a) { setAssign(x, self, itemsOf(x, a)); return self; }
id si_initWithSet(Ctx& x, id self, SEL, id a) { setAssign(x, self, itemsOf(x, a)); return self; }
id si_initWithObjects(Ctx& x, id self, SEL, id first, VarArgs va) { setAssign(x, self, nilTerminated(x, first, va.args)); return self; }
id si_initWithObjectsCount(Ctx& x, id self, SEL, GuestPtr objs, uint32_t n) {
    std::vector<id> v;
    for (uint32_t i = 0; i < n; ++i) v.push_back(x.mem.read<uint32_t>(objs + i * 4));
    setAssign(x, self, v);
    return self;
}
id sc_set(Ctx& x, id self, SEL) { return autoreleased(x, s_allocWithZone(x, self, 0, 0)); }
id sc_setWithCapacity(Ctx& x, id self, SEL, uint32_t) { return sc_set(x, self, 0); }
id sc_setWithObject(Ctx& x, id self, SEL, id o) { const id s = s_allocWithZone(x, self, 0, 0); setAssign(x, s, {o}); return autoreleased(x, s); }
id sc_setWithArray(Ctx& x, id self, SEL, id a) { const id s = s_allocWithZone(x, self, 0, 0); setAssign(x, s, itemsOf(x, a)); return autoreleased(x, s); }
id sc_setWithObjects(Ctx& x, id self, SEL, id first, VarArgs va) { const id s = s_allocWithZone(x, self, 0, 0); setAssign(x, s, nilTerminated(x, first, va.args)); return autoreleased(x, s); }
id sc_setWithObjectsCount(Ctx& x, id self, SEL, GuestPtr o, uint32_t n) { const id s = s_allocWithZone(x, self, 0, 0); si_initWithObjectsCount(x, s, 0, o, n); return autoreleased(x, s); }
uint32_t st_count(Ctx& x, id self, SEL) { SetData* d = sd(x, self); return d ? uint32_t(d->items.size()) : 0; }
id st_anyObject(Ctx& x, id self, SEL) { SetData* d = sd(x, self); return d && !d->items.empty() ? d->items.front() : 0; }
id st_allObjects(Ctx& x, id self, SEL) { return makeArray(x, itemsOf(x, self)); }
bool st_containsObject(Ctx& x, id self, SEL, id o) { SetData* d = sd(x, self); return d && o && setContains(x, *d, o); }
id st_member(Ctx& x, id self, SEL, id o) { SetData* d = sd(x, self); if (d) for (id e : d->items) if (objectsEqual(x, e, o)) return e; return 0; }
id st_objectEnumerator(Ctx& x, id self, SEL) { return newEnumerator(x, itemsOf(x, self), self); }
uint32_t st_countByEnumerating(Ctx& x, id self, SEL, GuestPtr state, GuestPtr buf, uint32_t len) {
    SetData* d = sd(x, self);
    return fastEnumerate(x, itemsOf(x, self), d && d->isMutable ? mutationCellFor(x, d->mutationCell) : 0, state, buf, len);
}
void st_enumerateUsingBlock(Ctx& x, id self, SEL, id block) {
    const uint32_t stop = x.proc.heap().allocZeroed(4);
    for (id o : itemsOf(x, self)) { callBlock(x, block, {o, stop}); if (x.mem.read<uint8_t>(stop)) break; }
    x.proc.heap().free(stop);
}
void st_makeObjectsPerform(Ctx& x, id self, SEL, SEL s) { for (id o : itemsOf(x, self)) rt(x).send(x.cpu, o, s); }
bool st_isEqualToSet(Ctx& x, id self, SEL, id o) {
    SetData *a = sd(x, self), *b = o ? sd(x, o) : nullptr;
    if (!a || !b || a->items.size() != b->items.size()) return false;
    for (id e : a->items) if (!setContains(x, *b, e)) return false;
    return true;
}
bool st_intersectsSet(Ctx& x, id self, SEL, id o) { SetData* a = sd(x, self); if (!a) return false; for (id e : itemsOf(x, o)) if (setContains(x, *a, e)) return true; return false; }
id st_setByAddingObject(Ctx& x, id self, SEL, id o) { auto v = itemsOf(x, self); v.push_back(o); return makeSet(x, v); }
id st_copyWithZone(Ctx& x, id self, SEL, uint32_t) { SetData* d = sd(x, self); if (d && !d->isMutable) return retainObj(x, self); return makeSet(x, itemsOf(x, self), true, false); }
id st_mutableCopyWithZone(Ctx& x, id self, SEL, uint32_t) { return makeSet(x, itemsOf(x, self), true, true); }
id st_copy(Ctx& x, id self, SEL s) { return st_copyWithZone(x, self, s, 0); }
id st_mutableCopy(Ctx& x, id self, SEL s) { return st_mutableCopyWithZone(x, self, s, 0); }
SetData& smut(Ctx& x, id self) {
    SetData* d = sd(x, self);
    if (!d || !d->isMutable) throw ObjC::ObjCError("*** -[NSSet addObject:]: mutating method sent to immutable object");
    return *d;
}
void sm_addObject(Ctx& x, id self, SEL, id o) { setAdd(x, smut(x, self), o); }
void sm_addObjectsFromArray(Ctx& x, id self, SEL, id a) { auto& d = smut(x, self); for (id o : itemsOf(x, a)) setAdd(x, d, o); }
void sm_unionSet(Ctx& x, id self, SEL s, id other) { sm_addObjectsFromArray(x, self, s, other); }
void sm_removeObject(Ctx& x, id self, SEL, id o) {
    auto& d = smut(x, self);
    for (size_t i = 0; i < d.items.size(); ++i)
        if (objectsEqual(x, d.items[i], o)) { const id e = d.items[i]; d.items.erase(d.items.begin() + long(i)); bump(x, d.mutationCell); releaseObj(x, e); return; }
}
void sm_removeAllObjects(Ctx& x, id self, SEL) { auto& d = smut(x, self); auto v = std::move(d.items); d.items.clear(); bump(x, d.mutationCell); for (id o : v) releaseObj(x, o); }
void sm_minusSet(Ctx& x, id self, SEL s, id other) { for (id o : itemsOf(x, other)) sm_removeObject(x, self, s, o); }
void sm_intersectSet(Ctx& x, id self, SEL s, id other) {
    auto& d = smut(x, self);
    SetData* o = sd(x, other);
    auto items = d.items;
    for (id e : items) if (!o || !setContains(x, *o, e)) sm_removeObject(x, self, s, e);
}
void set_dealloc(Ctx& x, id self, SEL) {
    if (SetData* d = sd(x, self)) {
        auto v = std::move(d->items);
        d->items.clear();
        if (d->mutationCell) x.proc.heap().free(d->mutationCell);
        for (id o : v) releaseObj(x, o);
    }
    rt(x).disposeInstance(x.cpu, self);
}

// -------------------------------------------------------- enumerators ---

id e_nextObject(Ctx& x, id self, SEL) {
    EnumData* d = rt(x).hostIf<EnumData>(self);
    if (!d || d->pos >= d->items.size()) return 0;
    return d->items[d->pos++];
}
id e_allObjects(Ctx& x, id self, SEL) {
    EnumData* d = rt(x).hostIf<EnumData>(self);
    if (!d) return makeArray(x, {});
    std::vector<id> rest(d->items.begin() + long(d->pos), d->items.end());
    d->pos = d->items.size();
    return makeArray(x, rest);
}
uint32_t e_countByEnumerating(Ctx& x, id self, SEL, GuestPtr state, GuestPtr buf, uint32_t len) {
    EnumData* d = rt(x).hostIf<EnumData>(self);
    if (!d) return 0;
    std::vector<id> rest(d->items.begin() + long(d->pos), d->items.end());
    const uint32_t n = fastEnumerate(x, rest, 0, state, buf, len);
    return n;
}
void enum_dealloc(Ctx& x, id self, SEL) {
    id owner = 0;
    if (EnumData* d = rt(x).hostIf<EnumData>(self)) owner = d->owner;
    rt(x).disposeInstance(x.cpu, self);
    releaseObj(x, owner);
}

// -------------------------------------------------------- descriptions ---

std::string quoteIfNeeded(const std::string& s) {
    bool plain = !s.empty();
    for (char c : s) if (!(std::isalnum(uint8_t(c)) || c == '_' || c == '.' || c == '$' || c == '/')) plain = false;
    if (plain) return s;
    std::string out = "\"";
    for (char c : s) { if (c == '"' || c == '\\') out += '\\'; if (c == '\n') { out += "\\n"; continue; } out += c; }
    return out + "\"";
}
std::string describeIndented(Ctx& x, id obj, int level) {
    const std::string pad(size_t(level) * 4, ' '), inner(size_t(level + 1) * 4, ' ');
    if (isArray(x, obj)) {
        auto items = itemsOf(x, obj);
        std::string s = "(\n";
        for (size_t i = 0; i < items.size(); ++i) s += inner + describeIndented(x, items[i], level + 1) + (i + 1 < items.size() ? ",\n" : "\n");
        return s + pad + ")";
    }
    if (isDict(x, obj)) {
        std::string s = "{\n";
        for (auto& [k, v] : entriesOf(x, obj)) s += inner + quoteIfNeeded(isString(x, k) ? utf8(x, k) : describe(x, k)) + " = " + describeIndented(x, v, level + 1) + ";\n";
        return s + pad + "}";
    }
    if (isString(x, obj)) return quoteIfNeeded(utf8(x, obj));
    return describe(x, obj);
}
id a_description(Ctx& x, id self, SEL) { return makeString(x, describeIndented(x, self, 0)); }
id st_description(Ctx& x, id self, SEL) {
    std::string s = "{(\n";
    auto items = itemsOf(x, self);
    for (size_t i = 0; i < items.size(); ++i) s += "    " + describe(x, items[i]) + (i + 1 < items.size() ? ",\n" : "\n");
    return makeString(x, s + ")}");
}

} // namespace

// ------------------------------------------------------------ public API ---

id makeArray(Ctx& x, const std::vector<id>& items, bool owned, bool isMutable) {
    const id a = newArray(x, isMutable);
    setItems(x, a, items);
    return owned ? a : autoreleased(x, a);
}
std::vector<id> arrayItems(Ctx& x, id array) { return itemsOf(x, array); }
bool isArray(Ctx& x, id obj) { return obj && rt(x).isKindOf(obj, C(x).nsArray); }
id makeDict(Ctx& x, const std::vector<std::pair<id, id>>& entries, bool owned, bool isMutable) {
    const id d = newDict(x, isMutable);
    dictAssign(x, d, entries);
    return owned ? d : autoreleased(x, d);
}
std::vector<std::pair<id, id>> dictEntries(Ctx& x, id dict) { return entriesOf(x, dict); }
id dictGet(Ctx& x, id dict, id key) {
    if (!dict || !key) return 0;
    if (DictData* d = dd(x, dict)) { const long i = findKey(x, *d, key, objectHash(x, key)); return i < 0 ? 0 : d->entries[size_t(i)].second; }
    return id(rt(x).send(x.cpu, dict, "objectForKey:", {key}));
}
id dictGet(Ctx& x, id dict, const std::string& key) { return dictGet(x, dict, constString(x.proc, key)); }
bool isDict(Ctx& x, id obj) { return obj && rt(x).isKindOf(obj, C(x).nsDict); }
id makeSet(Ctx& x, const std::vector<id>& items, bool owned, bool isMutable) {
    const id s = newSet(x, isMutable);
    setAssign(x, s, items);
    return owned ? s : autoreleased(x, s);
}
std::vector<id> setItems(Ctx& x, id set) { return itemsOf(x, set); }

bool objectsEqual(Ctx& x, id a, id b) {
    if (a == b) return true;
    if (!a || !b) return false;
    const bool sa = isString(x, a), sb = isString(x, b);
    if (sa && sb) return stringValue(x, a) == stringValue(x, b);
    if (sa != sb && (sa ? isNumber(x, b) : isNumber(x, a))) return false;
    return (uint32_t(rt(x).send(x.cpu, a, rt(x).selIsEqual, {b})) & 0xFF) != 0;
}
uint32_t objectHash(Ctx& x, id a) {
    if (!a) return 0;
    return uint32_t(rt(x).send(x.cpu, a, rt(x).selHash));
}
std::string describe(Ctx& x, id obj) {
    if (!obj) return "(null)";
    if (isString(x, obj)) return utf8(x, obj);
    const id d = id(rt(x).send(x.cpu, obj, rt(x).selDescription));
    if (!d) return "(null)";
    return isString(x, d) ? utf8(x, d) : "<?>";
}

id plistToObject(Ctx& x, const Plist::Value& v, bool mc) {
    using T = Plist::Value::Type;
    switch (v.type) {
        case T::String: return makeString(x, v.s, false, mc);
        case T::Integer: case T::Uid: return makeNumberInt(x, v.i);
        case T::Real: return makeNumberDouble(x, v.r);
        case T::Bool: return makeBool(x, v.b);
        case T::Data: return makeData(x, v.s.data(), uint32_t(v.s.size()), false, mc);
        case T::Date: {
            const id cls = rt(x).getClass("NSDate");
            uint64_t bits; std::memcpy(&bits, &v.r, 8);
            return id(rt(x).send(x.cpu, cls, "dateWithTimeIntervalSinceReferenceDate:", {uint32_t(bits), uint32_t(bits >> 32)}));
        }
        case T::Array: { std::vector<id> items; for (auto& e : v.array) items.push_back(plistToObject(x, *e, mc)); return makeArray(x, items, false, mc); }
        case T::Dict: {
            std::vector<std::pair<id, id>> e;
            for (auto& [k, val] : v.dict) e.emplace_back(makeString(x, k), plistToObject(x, *val, mc));
            return makeDict(x, e, false, mc);
        }
        default: return 0;
    }
}

Plist::ValuePtr objectToPlist(Ctx& x, id obj) {
    if (!obj) return nullptr;
    if (isString(x, obj)) return Plist::Value::makeString(utf8(x, obj));
    if (isBoolean(x, obj)) return Plist::Value::makeBool(numberInt(x, obj) != 0);
    if (isNumber(x, obj)) {
        const char t = numberType(x, obj);
        if (t == 'd' || t == 'f') return Plist::Value::makeReal(numberDouble(x, obj));
        return Plist::Value::makeInt(numberInt(x, obj));
    }
    if (isData(x, obj)) { auto b = dataBytes(x, obj); return Plist::Value::makeData(std::string(b.begin(), b.end())); }
    if (isArray(x, obj)) {
        auto a = Plist::Value::makeArray();
        for (id e : itemsOf(x, obj)) { auto v = objectToPlist(x, e); if (!v) return nullptr; a->array.push_back(v); }
        return a;
    }
    if (isDict(x, obj)) {
        auto d = Plist::Value::makeDict();
        for (auto& [k, v] : entriesOf(x, obj)) { auto pv = objectToPlist(x, v); if (!pv || !isString(x, k)) return nullptr; d->dict.emplace_back(utf8(x, k), pv); }
        return d;
    }
    if (const Class dateCls = rt(x).getClass("NSDate"); dateCls && rt(x).isKindOf(obj, dateCls)) {
        const uint64_t bits = rt(x).send(x.cpu, obj, "timeIntervalSinceReferenceDate");
        double t; std::memcpy(&t, &bits, 8);
        return Plist::Value::makeDate(t);
    }
    return nullptr;
}

void registerCollections(Process& p, ObjC::Runtime& r) {
    auto& c = p.service<Colls>();
    c.constMutationCell = p.allocStatic(4);
    ObjC::ClassBuilder na = r.defineClass("NSArray", "NSObject");
    ObjC::ClassBuilder nma = r.defineClass("NSMutableArray", "NSArray");
    ObjC::ClassBuilder ai = r.defineClass("__NSArrayI", "NSArray");
    ObjC::ClassBuilder am = r.defineClass("__NSArrayM", "NSMutableArray");
    c.nsArray = na.cls(); c.nsMutableArray = nma.cls(); c.arrayI = ai.cls(); c.arrayM = am.cls();
    na.cm<&a_alloc>("alloc").cm<&a_allocWithZone>("allocWithZone:")
      .cm<&ac_array>("array").cm<&ac_arrayWithObject>("arrayWithObject:").cm<&ac_arrayWithObjects>("arrayWithObjects:")
      .cm<&ac_arrayWithObjectsCount>("arrayWithObjects:count:").cm<&ac_arrayWithArray>("arrayWithArray:")
      .cm<&ac_arrayWithContentsOfFile>("arrayWithContentsOfFile:");
    na.m<&ai_init>("init").m<&ai_initWithObjects>("initWithObjects:").m<&ai_initWithObjectsCount>("initWithObjects:count:")
      .m<&ai_initWithArray>("initWithArray:").m<&ai_initWithArrayCopy>("initWithArray:copyItems:")
      .m<&ai_initWithContentsOfFile>("initWithContentsOfFile:")
      .m<&a_count>("count", "I@:").m<&a_objectAtIndex>("objectAtIndex:").m<&a_objectAtIndex>("objectAtIndexedSubscript:")
      .m<&a_firstObject>("firstObject").m<&a_lastObject>("lastObject")
      .m<&a_indexOfObject>("indexOfObject:", "I@:@").m<&a_indexOfObjectIdenticalTo>("indexOfObjectIdenticalTo:", "I@:@")
      .m<&a_containsObject>("containsObject:", "c@:@").m<&a_isEqualToArray>("isEqualToArray:", "c@:@")
      .m<&a_isEqualToArray>("isEqual:", "c@:@").m<&a_hash>("hash", "I@:")
      .m<&a_subarrayWithRange>("subarrayWithRange:").m<&a_arrayByAddingObject>("arrayByAddingObject:")
      .m<&a_arrayByAddingObjects>("arrayByAddingObjectsFromArray:").m<&a_componentsJoined>("componentsJoinedByString:")
      .m<&a_makeObjectsPerform>("makeObjectsPerformSelector:", "v@::").m<&a_makeObjectsPerformWith>("makeObjectsPerformSelector:withObject:", "v@::@")
      .m<&a_copyWithZone>("copyWithZone:").m<&a_mutableCopyWithZone>("mutableCopyWithZone:").m<&a_copy>("copy").m<&a_mutableCopy>("mutableCopy")
      .m<&a_countByEnumerating>("countByEnumeratingWithState:objects:count:", "I@:^v^@I")
      .m<&a_objectEnumerator>("objectEnumerator").m<&a_reverseObjectEnumerator>("reverseObjectEnumerator")
      .m<&a_enumerateUsingBlock>("enumerateObjectsUsingBlock:", "v@:@?").m<&a_enumerateWithOptions>("enumerateObjectsWithOptions:usingBlock:")
      .m<&a_indexOfObjectPassingTest>("indexOfObjectPassingTest:")
      .m<&a_sortedUsingSelector>("sortedArrayUsingSelector:").m<&a_sortedUsingFunction>("sortedArrayUsingFunction:context:")
      .m<&a_sortedUsingComparator>("sortedArrayUsingComparator:").m<&a_valueForKey>("valueForKey:")
      .m<&a_writeToFile>("writeToFile:atomically:").m<&a_description>("description");
    nma.cm<&ac_arrayWithCapacity>("arrayWithCapacity:")
       .m<&ai_initWithCapacity>("initWithCapacity:")
       .m<&m_addObject>("addObject:", "v@:@").m<&m_insertObject>("insertObject:atIndex:", "v@:@I")
       .m<&m_removeObjectAtIndex>("removeObjectAtIndex:", "v@:I").m<&m_removeLastObject>("removeLastObject", "v@:")
       .m<&m_removeAllObjects>("removeAllObjects", "v@:").m<&m_removeObject>("removeObject:", "v@:@")
       .m<&m_removeObjectIdenticalTo>("removeObjectIdenticalTo:", "v@:@").m<&m_removeObjectsInRange>("removeObjectsInRange:")
       .m<&m_removeObjectsInArray>("removeObjectsInArray:").m<&m_replaceObject>("replaceObjectAtIndex:withObject:")
       .m<&m_setObjectAtIndexedSubscript>("setObject:atIndexedSubscript:").m<&m_exchange>("exchangeObjectAtIndex:withObjectAtIndex:")
       .m<&m_addObjectsFromArray>("addObjectsFromArray:").m<&m_setArray>("setArray:")
       .m<&m_sortUsingSelector>("sortUsingSelector:").m<&m_sortUsingFunction>("sortUsingFunction:context:")
       .m<&m_sortUsingComparator>("sortUsingComparator:");
    ai.m<&array_dealloc>("dealloc", "v@:");
    am.m<&array_dealloc>("dealloc", "v@:");

    ObjC::ClassBuilder nd = r.defineClass("NSDictionary", "NSObject");
    ObjC::ClassBuilder nmd = r.defineClass("NSMutableDictionary", "NSDictionary");
    ObjC::ClassBuilder di = r.defineClass("__NSDictionaryI", "NSDictionary");
    ObjC::ClassBuilder dm = r.defineClass("__NSDictionaryM", "NSMutableDictionary");
    c.nsDict = nd.cls(); c.nsMutableDict = nmd.cls(); c.dictI = di.cls(); c.dictM = dm.cls();
    nd.cm<&d_alloc>("alloc").cm<&d_allocWithZone>("allocWithZone:")
      .cm<&dc_dictionary>("dictionary").cm<&dc_dictionaryWithObjectForKey>("dictionaryWithObject:forKey:")
      .cm<&dc_dictionaryWithObjectsForKeysCount>("dictionaryWithObjects:forKeys:count:")
      .cm<&dc_dictionaryWithObjectsAndKeys>("dictionaryWithObjectsAndKeys:")
      .cm<&dc_dictionaryWithObjectsForKeys>("dictionaryWithObjects:forKeys:")
      .cm<&dc_dictionaryWithDictionary>("dictionaryWithDictionary:").cm<&dc_dictionaryWithContentsOfFile>("dictionaryWithContentsOfFile:");
    nd.m<&di_init>("init").m<&di_initWithObjectsForKeysCount>("initWithObjects:forKeys:count:")
      .m<&di_initWithObjectsAndKeys>("initWithObjectsAndKeys:").m<&di_initWithObjectsForKeys>("initWithObjects:forKeys:")
      .m<&di_initWithDictionary>("initWithDictionary:").m<&di_initWithDictionaryCopy>("initWithDictionary:copyItems:")
      .m<&di_initWithContentsOfFile>("initWithContentsOfFile:")
      .m<&d_count>("count", "I@:").m<&d_objectForKey>("objectForKey:").m<&d_objectForKey>("objectForKeyedSubscript:")
      .m<&d_valueForKey>("valueForKey:").m<&d_allKeys>("allKeys").m<&d_allValues>("allValues")
      .m<&d_allKeysForObject>("allKeysForObject:").m<&d_keyEnumerator>("keyEnumerator").m<&d_objectEnumerator>("objectEnumerator")
      .m<&d_countByEnumerating>("countByEnumeratingWithState:objects:count:", "I@:^v^@I")
      .m<&d_enumerateKeysAndObjects>("enumerateKeysAndObjectsUsingBlock:")
      .m<&d_isEqualToDictionary>("isEqualToDictionary:", "c@:@").m<&d_isEqualToDictionary>("isEqual:", "c@:@").m<&d_hash>("hash", "I@:")
      .m<&d_copyWithZone>("copyWithZone:").m<&d_mutableCopyWithZone>("mutableCopyWithZone:").m<&d_copy>("copy").m<&d_mutableCopy>("mutableCopy")
      .m<&d_writeToFile>("writeToFile:atomically:").m<&d_keysSortedByValueUsingSelector>("keysSortedByValueUsingSelector:")
      .m<&a_description>("description");
    nmd.cm<&dc_dictionaryWithCapacity>("dictionaryWithCapacity:")
       .m<&di_initWithCapacity>("initWithCapacity:")
       .m<&dm_setObjectForKey>("setObject:forKey:", "v@:@@").m<&dm_setObjectForKey>("setObject:forKeyedSubscript:", "v@:@@")
       .m<&dm_setValueForKey>("setValue:forKey:", "v@:@@").m<&dm_removeObjectForKey>("removeObjectForKey:", "v@:@")
       .m<&dm_removeAllObjects>("removeAllObjects", "v@:").m<&dm_addEntries>("addEntriesFromDictionary:")
       .m<&dm_removeObjectsForKeys>("removeObjectsForKeys:").m<&dm_setDictionary>("setDictionary:");
    di.m<&dict_dealloc>("dealloc", "v@:");
    dm.m<&dict_dealloc>("dealloc", "v@:");

    ObjC::ClassBuilder ns = r.defineClass("NSSet", "NSObject");
    ObjC::ClassBuilder nms = r.defineClass("NSMutableSet", "NSSet");
    ObjC::ClassBuilder si = r.defineClass("__NSSetI", "NSSet");
    ObjC::ClassBuilder sm = r.defineClass("__NSSetM", "NSMutableSet");
    c.nsSet = ns.cls(); c.nsMutableSet = nms.cls(); c.setI = si.cls(); c.setM = sm.cls();
    ns.cm<&s_alloc>("alloc").cm<&s_allocWithZone>("allocWithZone:").cm<&sc_set>("set").cm<&sc_setWithObject>("setWithObject:")
      .cm<&sc_setWithArray>("setWithArray:").cm<&sc_setWithObjects>("setWithObjects:").cm<&sc_setWithObjectsCount>("setWithObjects:count:")
      .cm<&sc_setWithArray>("setWithSet:");
    ns.m<&si_init>("init").m<&si_initWithArray>("initWithArray:").m<&si_initWithSet>("initWithSet:").m<&si_initWithObjects>("initWithObjects:")
      .m<&si_initWithObjectsCount>("initWithObjects:count:")
      .m<&st_count>("count", "I@:").m<&st_anyObject>("anyObject").m<&st_allObjects>("allObjects")
      .m<&st_containsObject>("containsObject:", "c@:@").m<&st_member>("member:").m<&st_objectEnumerator>("objectEnumerator")
      .m<&st_countByEnumerating>("countByEnumeratingWithState:objects:count:", "I@:^v^@I")
      .m<&st_enumerateUsingBlock>("enumerateObjectsUsingBlock:").m<&st_makeObjectsPerform>("makeObjectsPerformSelector:", "v@::")
      .m<&st_isEqualToSet>("isEqualToSet:", "c@:@").m<&st_isEqualToSet>("isEqual:", "c@:@").m<&st_intersectsSet>("intersectsSet:", "c@:@")
      .m<&st_setByAddingObject>("setByAddingObject:")
      .m<&st_copyWithZone>("copyWithZone:").m<&st_mutableCopyWithZone>("mutableCopyWithZone:").m<&st_copy>("copy").m<&st_mutableCopy>("mutableCopy")
      .m<&st_description>("description");
    nms.cm<&sc_setWithCapacity>("setWithCapacity:").m<&si_initWithCapacity>("initWithCapacity:")
       .m<&sm_addObject>("addObject:", "v@:@").m<&sm_addObjectsFromArray>("addObjectsFromArray:").m<&sm_unionSet>("unionSet:")
       .m<&sm_removeObject>("removeObject:", "v@:@").m<&sm_removeAllObjects>("removeAllObjects", "v@:")
       .m<&sm_minusSet>("minusSet:").m<&sm_intersectSet>("intersectSet:");
    si.m<&set_dealloc>("dealloc", "v@:");
    sm.m<&set_dealloc>("dealloc", "v@:");

    ObjC::ClassBuilder ne = r.defineClass("NSEnumerator", "NSObject");
    ObjC::ClassBuilder ae = r.defineClass("__NSFastEnumerationEnumerator", "NSEnumerator");
    c.enumerator = ae.cls();
    ne.m<&e_nextObject>("nextObject").m<&e_allObjects>("allObjects")
      .m<&e_countByEnumerating>("countByEnumeratingWithState:objects:count:", "I@:^v^@I");
    ae.m<&enum_dealloc>("dealloc", "v@:");
}

} // namespace Hydrogen::Runtime::Foundation
