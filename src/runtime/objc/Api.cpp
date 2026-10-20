// HydrogenVM — libobjc's C API: message dispatch, ARC entry points,
// introspection, exceptions, @synchronized, associated objects.
#include "ObjC.hpp"

namespace Hydrogen::Runtime::ObjC {
namespace {

// Tail-jump to an IMP. Host-implemented IMPs are called directly (cheaper
// than going through another block) and then return to the caller's LR.
inline void dispatch(Ctx& x, IMP imp) {
    auto& e = x.proc.engine();
    if (e.isHleAddress(imp)) e.callHle(&x.cpu, e.hleIndex(imp));
    else CPU::Engine::hleJump(x.cpu, imp);
}

void trace(Ctx& x, id self, SEL s) {
    Runtime& r = rt(x);
    const ClassInfo* ci = r.info(r.classOf(self));
    x.proc.log(LogLevel::Debug, std::string(ci && ci->isMeta ? "+[" : "-[") + r.objectClassName(self) + " " + r.selName(s) + "]");
}

IMP resolve(Ctx& x, id& self, SEL s) {
    Runtime& r = rt(x);
    if (x.proc.config().traceCalls) trace(x, self, s);
    IMP imp = r.lookupForSend(r.classOf(self), s);
    if (imp) return imp;
    // Dynamic method resolution.
    const ClassInfo* ci = r.info(r.classOf(self));
    const bool meta = ci && ci->isMeta;
    const SEL resolver = r.sel(meta ? "resolveClassMethod:" : "resolveInstanceMethod:");
    const Class target = meta ? ci->instanceClass : r.classOf(self);
    const IMP rimp = r.lookup(r.classOf(target), resolver);
    if (rimp && !x.proc.engine().isHleAddress(rimp)) {
        if (uint32_t(x.proc.callGuest(x.cpu, rimp, {target, resolver, s})) & 0xFF) {
            imp = r.lookup(r.classOf(self), s);
            if (imp) return imp;
        }
    }
    // Fast forwarding.
    const IMP ft = r.lookup(r.classOf(self), r.selForwardingTarget);
    if (ft && !x.proc.engine().isHleAddress(ft)) {
        const id t = id(x.proc.callGuest(x.cpu, ft, {self, r.selForwardingTarget, s}));
        if (t && t != self) {
            self = t;
            imp = r.lookupForSend(r.classOf(t), s);
            if (imp) return imp;
        }
    }
    return r.unrecognized(self, s);
}

void msgSend(Ctx& x) {
    id self = x.cpu.r[0];
    if (!self) { x.cpu.r[0] = x.cpu.r[1] = 0; return; }
    const IMP imp = resolve(x, self, x.cpu.r[1]);
    x.cpu.r[0] = self;
    dispatch(x, imp);
}
void msgSendStret(Ctx& x) {
    id self = x.cpu.r[1];
    if (!self) return;   // armv7: the struct is left untouched
    const IMP imp = resolve(x, self, x.cpu.r[2]);
    x.cpu.r[1] = self;
    dispatch(x, imp);
}
// struct objc_super { id receiver; Class class; }
void msgSendSuperImpl(Ctx& x, uint32_t superReg, uint32_t selReg, bool super2) {
    Runtime& r = rt(x);
    const uint32_t sup = x.cpu.r[superReg];
    const id self = x.mem.read<uint32_t>(sup);
    Class start = x.mem.read<uint32_t>(sup + 4);
    if (super2) start = r.superclass(start);
    const SEL s = x.cpu.r[selReg];
    x.cpu.r[superReg] = self;
    if (!self) { if (!superReg) x.cpu.r[0] = x.cpu.r[1] = 0; return; }
    if (x.proc.config().traceCalls) trace(x, self, s);
    IMP imp = r.lookup(start, s);
    if (!imp) imp = r.unrecognized(self, s);
    dispatch(x, imp);
}
void msgSendSuper(Ctx& x) { msgSendSuperImpl(x, 0, 1, false); }
void msgSendSuper2(Ctx& x) { msgSendSuperImpl(x, 0, 1, true); }
void msgSendSuperStret(Ctx& x) { msgSendSuperImpl(x, 1, 2, false); }
void msgSendSuper2Stret(Ctx& x) { msgSendSuperImpl(x, 1, 2, true); }

// ------------------------------------------------------------------ ARC ---

id a_retain(Ctx& x, id o) { return rt(x).retain(x.cpu, o); }
void a_release(Ctx& x, id o) { rt(x).release(x.cpu, o); }
id a_autorelease(Ctx& x, id o) { return rt(x).autorelease(x.cpu, o); }
id a_retainAutorelease(Ctx& x, id o) { return rt(x).autorelease(x.cpu, rt(x).retain(x.cpu, o)); }
// ARC's return-value handshake: when the caller marks its call site with
// `mov r7, r7` right after the call (meaning it will immediately call
// objc_retainAutoreleasedReturnValue), skip the autorelease/retain pair and
// hand the object over at +1, exactly like the real runtime. Without this,
// objects returned from methods would live until the next pool drain.
bool callerAcceptsOptimizedReturn(Ctx& x) {
    const uint32_t lr = x.cpu.r[CPU::kLR];
    if (lr & 1) return x.mem.isMapped(lr & ~1u) && x.mem.read<uint16_t>(lr & ~1u) == 0x463F;   // Thumb mov r7, r7
    return x.mem.isMapped(lr) && x.mem.read<uint32_t>(lr) == 0xE1A07007u;                        // ARM mov r7, r7
}
id a_autoreleaseReturn(Ctx& x, id o) {
    if (o && callerAcceptsOptimizedReturn(x)) { threadState(x.proc).optimizedReturn = o; return o; }
    return a_autorelease(x, o);
}
id a_retainAutoreleaseReturn(Ctx& x, id o) {
    o = a_retain(x, o);
    return a_autoreleaseReturn(x, o);
}
id a_retainAutoreleased(Ctx& x, id o) {
    ThreadState& ts = threadState(x.proc);
    if (o && ts.optimizedReturn == o) { ts.optimizedReturn = 0; return o; }
    ts.optimizedReturn = 0;
    return a_retain(x, o);
}
id a_claimAutoreleased(Ctx& x, id o) {
    ThreadState& ts = threadState(x.proc);
    if (o && ts.optimizedReturn == o) { ts.optimizedReturn = 0; a_release(x, o); return o; }
    ts.optimizedReturn = 0;
    return o;
}
void a_storeStrong(Ctx& x, GuestPtr slot, id o) {
    const id old = x.mem.read<uint32_t>(slot);
    if (old == o) return;
    rt(x).retain(x.cpu, o);
    x.mem.write<uint32_t>(slot, o);
    rt(x).release(x.cpu, old);
}
id a_loadWeakRetained(Ctx& x, GuestPtr slot) { const id o = rt(x).loadWeak(slot); return o ? rt(x).retain(x.cpu, o) : 0; }
id a_loadWeak(Ctx& x, GuestPtr slot) { const id o = a_loadWeakRetained(x, slot); return o ? rt(x).autorelease(x.cpu, o) : 0; }
id a_storeWeak(Ctx& x, GuestPtr slot, id o) { rt(x).storeWeak(slot, o); return o; }
id a_initWeak(Ctx& x, GuestPtr slot, id o) { x.mem.write<uint32_t>(slot, 0); rt(x).storeWeak(slot, o); return o; }
void a_destroyWeak(Ctx& x, GuestPtr slot) { rt(x).storeWeak(slot, 0); }
void a_copyWeak(Ctx& x, GuestPtr dst, GuestPtr src) { x.mem.write<uint32_t>(dst, 0); rt(x).storeWeak(dst, rt(x).loadWeak(src)); }
void a_moveWeak(Ctx& x, GuestPtr dst, GuestPtr src) { a_copyWeak(x, dst, src); rt(x).storeWeak(src, 0); }
uint32_t a_poolPush(Ctx& x) { return rt(x).poolPush(); }
void a_poolPop(Ctx& x, uint32_t token) { rt(x).poolPop(x.cpu, token); }

// --------------------------------------------------------- introspection ---

Class c_getClass(Ctx& x, GuestPtr name) {
    if (!name) return 0;
    const Class c = rt(x).getClass(x.proc.readCString(name));
    const ClassInfo* ci = c ? rt(x).info(c) : nullptr;
    return ci && ci->stub ? 0 : c;   // placeholders for missing classes look absent
}
Class c_getRequiredClass(Ctx& x, GuestPtr name) {
    const Class c = c_getClass(x, name);
    if (!c) throw ObjCError("objc_getRequiredClass: class " + x.proc.readCString(name) + " not found");
    return c;
}
Class c_getMetaClass(Ctx& x, GuestPtr name) { const Class c = c_getClass(x, name); return c ? rt(x).classOf(c) : 0; }
uint32_t c_className(Ctx& x, Class c) { return c ? x.proc.internCString(rt(x).className(c)) : x.proc.internCString("nil"); }
Class c_superclass(Ctx& x, Class c) { return rt(x).superclass(c); }
bool c_isMeta(Ctx& x, Class c) { return rt(x).isMeta(c); }
uint32_t c_instanceSize(Ctx& x, Class c) { const ClassInfo* ci = rt(x).info(c); return ci ? ci->instanceSize : 0; }
Class o_getClass(Ctx& x, id o) { return rt(x).classOf(o); }
Class o_setClass(Ctx& x, id o, Class c) { const Class old = rt(x).classOf(o); if (o) x.mem.write<uint32_t>(o, c); return old; }
uint32_t o_getClassName(Ctx& x, id o) { return x.proc.internCString(o ? rt(x).objectClassName(o) : "nil"); }
id c_createInstance(Ctx& x, Class c, uint32_t extra) { return rt(x).createInstance(c, extra); }
id o_dispose(Ctx& x, id o) { rt(x).disposeInstance(x.cpu, o); return 0; }
id o_copy(Ctx& x, id o, uint32_t size) {
    const Runtime& r = rt(x);
    const ClassInfo* ci = r.info(r.classOf(o));
    const uint32_t n = size ? size : (ci ? ci->instanceSize : 4);
    const id c = x.proc.heap().alloc(n);
    std::memcpy(x.mem.host(c), x.mem.host(o), n);
    return c;
}
SEL s_register(Ctx& x, GuestPtr name) { return rt(x).sel(x.proc.readCString(name)); }
uint32_t s_getName(Ctx& x, SEL s) { return s ? s : x.proc.internCString("<null selector>"); }
bool s_isEqual(Ctx&, SEL a, SEL b) { return a == b; }
bool c_respondsTo(Ctx& x, Class c, SEL s) { return rt(x).lookup(c, s) != 0; }
IMP c_getMethodImplementation(Ctx& x, Class c, SEL s) {
    const IMP imp = rt(x).lookup(c, s);
    return imp ? imp : *x.proc.lookupSymbol("__objc_msgForward");
}
bool c_conformsTo(Ctx& x, Class c, uint32_t p) { return rt(x).conformsTo(c, p); }
uint32_t p_get(Ctx& x, GuestPtr name) { return rt(x).getProtocol(x.proc.readCString(name)); }
uint32_t p_getName(Ctx& x, uint32_t p) { return x.proc.internCString(rt(x).protocolName(p)); }

// Method objects: guest method_t {SEL name; char *types; IMP imp} per (class, selector).
struct Methods {
    explicit Methods(Process&) {}
    std::mutex mu;
    std::map<std::pair<Class, SEL>, uint32_t> objects;
    std::unordered_map<uint32_t, std::pair<Class, SEL>> owners;
};
uint32_t methodObject(Ctx& x, Class cls, SEL s, bool inherited) {
    Runtime& r = rt(x);
    Class owner = 0;
    for (Class c = cls; c; c = r.superclass(c)) {
        const ClassInfo* ci = r.info(c);
        if (!ci) break;
        if (ci->methods.count(s)) { owner = c; break; }
        if (!inherited) break;
    }
    if (!owner) return 0;
    auto& ms = x.proc.service<Methods>();
    std::lock_guard<std::mutex> lk(ms.mu);
    auto key = std::make_pair(owner, s);
    auto it = ms.objects.find(key);
    const ClassInfo* oi = r.info(owner);
    const IMP imp = oi->methods.at(s);
    if (it != ms.objects.end()) { x.mem.write<uint32_t>(it->second + 8, imp); return it->second; }
    const uint32_t m = x.proc.allocStatic(12);
    x.mem.write<uint32_t>(m, s);
    auto t = oi->types.find(s);
    x.mem.write<uint32_t>(m + 4, x.proc.internCString(t != oi->types.end() ? t->second : "@@:"));
    x.mem.write<uint32_t>(m + 8, imp);
    ms.objects[key] = m;
    ms.owners[m] = key;
    return m;
}
uint32_t c_getInstanceMethod(Ctx& x, Class c, SEL s) { return methodObject(x, c, s, true); }
uint32_t c_getClassMethod(Ctx& x, Class c, SEL s) { return methodObject(x, rt(x).classOf(c), s, true); }
SEL m_getName(Ctx& x, uint32_t m) { return m ? x.mem.read<uint32_t>(m) : 0; }
IMP m_getImplementation(Ctx& x, uint32_t m) { return m ? x.mem.read<uint32_t>(m + 8) : 0; }
uint32_t m_getTypeEncoding(Ctx& x, uint32_t m) { return m ? x.mem.read<uint32_t>(m + 4) : 0; }
IMP m_setImplementation(Ctx& x, uint32_t m, IMP imp) {
    if (!m) return 0;
    auto& ms = x.proc.service<Methods>();
    std::pair<Class, SEL> owner;
    {
        std::lock_guard<std::mutex> lk(ms.mu);
        auto it = ms.owners.find(m);
        if (it == ms.owners.end()) return 0;
        owner = it->second;
    }
    const IMP old = x.mem.read<uint32_t>(m + 8);
    x.mem.write<uint32_t>(m + 8, imp);
    rt(x).addMethod(owner.first, owner.second, imp, x.proc.readCString(x.mem.read<uint32_t>(m + 4)));
    return old;
}
void m_exchange(Ctx& x, uint32_t a, uint32_t b) {
    if (!a || !b) return;
    const IMP ia = x.mem.read<uint32_t>(a + 8), ib = x.mem.read<uint32_t>(b + 8);
    m_setImplementation(x, a, ib);
    m_setImplementation(x, b, ia);
}
bool c_addMethod(Ctx& x, Class c, SEL s, IMP imp, GuestPtr types) {
    const ClassInfo* ci = rt(x).info(c);
    if (!ci || ci->methods.count(s)) return false;
    rt(x).addMethod(c, s, imp, types ? x.proc.readCString(types) : "v@:");
    return true;
}
IMP c_replaceMethod(Ctx& x, Class c, SEL s, IMP imp, GuestPtr types) {
    const ClassInfo* ci = rt(x).info(c);
    if (!ci) return 0;
    auto it = ci->methods.find(s);
    const IMP old = it == ci->methods.end() ? 0 : it->second;
    rt(x).addMethod(c, s, imp, types ? x.proc.readCString(types) : "v@:");
    return old;
}
uint32_t c_copyMethodList(Ctx& x, Class c, GuestPtr outCount) {
    const ClassInfo* ci = rt(x).info(c);
    std::vector<SEL> sels;
    if (ci) for (auto& [s, imp] : ci->methods) sels.push_back(s);
    if (outCount) x.mem.write<uint32_t>(outCount, uint32_t(sels.size()));
    if (sels.empty()) return 0;
    const uint32_t list = x.proc.heap().alloc(uint32_t(sels.size() + 1) * 4);
    for (size_t i = 0; i < sels.size(); ++i) x.mem.write<uint32_t>(list + uint32_t(i) * 4, methodObject(x, c, sels[i], false));
    x.mem.write<uint32_t>(list + uint32_t(sels.size()) * 4, 0);
    return list;
}

// Ivar objects: guest {name, type, offset} per ivar.
uint32_t ivarObject(Ctx& x, const Ivar& iv) {
    const uint32_t o = x.proc.allocStatic(12);
    x.mem.write<uint32_t>(o, x.proc.internCString(iv.name));
    x.mem.write<uint32_t>(o + 4, x.proc.internCString(iv.type));
    x.mem.write<uint32_t>(o + 8, iv.offsetPtr ? x.mem.read<uint32_t>(iv.offsetPtr) : iv.offset);
    return o;
}
uint32_t c_getInstanceVariable(Ctx& x, Class c, GuestPtr name) {
    const Ivar* iv = rt(x).findIvar(c, x.proc.readCString(name));
    return iv ? ivarObject(x, *iv) : 0;
}
uint32_t c_copyIvarList(Ctx& x, Class c, GuestPtr outCount) {
    const auto ivars = rt(x).allIvars(c);
    if (outCount) x.mem.write<uint32_t>(outCount, uint32_t(ivars.size()));
    if (ivars.empty()) return 0;
    const uint32_t list = x.proc.heap().alloc(uint32_t(ivars.size() + 1) * 4);
    for (size_t i = 0; i < ivars.size(); ++i) x.mem.write<uint32_t>(list + uint32_t(i) * 4, ivarObject(x, ivars[i]));
    x.mem.write<uint32_t>(list + uint32_t(ivars.size()) * 4, 0);
    return list;
}
uint32_t i_getName(Ctx& x, uint32_t iv) { return iv ? x.mem.read<uint32_t>(iv) : 0; }
uint32_t i_getType(Ctx& x, uint32_t iv) { return iv ? x.mem.read<uint32_t>(iv + 4) : 0; }
int32_t i_getOffset(Ctx& x, uint32_t iv) { return iv ? int32_t(x.mem.read<uint32_t>(iv + 8)) : 0; }
id o_getIvar(Ctx& x, id o, uint32_t iv) { return o && iv ? x.mem.read<uint32_t>(o + x.mem.read<uint32_t>(iv + 8)) : 0; }
void o_setIvar(Ctx& x, id o, uint32_t iv, id v) { if (o && iv) x.mem.write<uint32_t>(o + x.mem.read<uint32_t>(iv + 8), v); }
uint32_t o_getInstanceVariable(Ctx& x, id o, GuestPtr name, GuestPtr out) {
    if (!o) return 0;
    const Ivar* iv = rt(x).findIvar(rt(x).classOf(o), x.proc.readCString(name));
    if (!iv) return 0;
    const uint32_t off = iv->offsetPtr ? x.mem.read<uint32_t>(iv->offsetPtr) : iv->offset;
    if (out) x.mem.write<uint32_t>(out, x.mem.read<uint32_t>(o + off));
    return ivarObject(x, *iv);
}
uint32_t o_setInstanceVariable(Ctx& x, id o, GuestPtr name, uint32_t value) {
    if (!o) return 0;
    const Ivar* iv = rt(x).findIvar(rt(x).classOf(o), x.proc.readCString(name));
    if (!iv) return 0;
    const uint32_t off = iv->offsetPtr ? x.mem.read<uint32_t>(iv->offsetPtr) : iv->offset;
    x.mem.write<uint32_t>(o + off, value);
    return ivarObject(x, *iv);
}

uint32_t c_copyPropertyList(Ctx& x, Class c, GuestPtr outCount) {
    const ClassInfo* ci = rt(x).info(c);
    const size_t n = ci ? ci->properties.size() : 0;
    if (outCount) x.mem.write<uint32_t>(outCount, uint32_t(n));
    if (!n) return 0;
    const uint32_t list = x.proc.heap().alloc(uint32_t(n + 1) * 4);
    for (size_t i = 0; i < n; ++i) {
        const uint32_t prop = x.proc.allocStatic(8);
        x.mem.write<uint32_t>(prop, x.proc.internCString(ci->properties[i].name));
        x.mem.write<uint32_t>(prop + 4, x.proc.internCString(ci->properties[i].attributes));
        x.mem.write<uint32_t>(list + uint32_t(i) * 4, prop);
    }
    x.mem.write<uint32_t>(list + uint32_t(n) * 4, 0);
    return list;
}
uint32_t c_getProperty(Ctx& x, Class c, GuestPtr name) {
    const std::string n = x.proc.readCString(name);
    for (Class k = c; k; k = rt(x).superclass(k)) {
        const ClassInfo* ci = rt(x).info(k);
        if (!ci) break;
        for (const auto& p : ci->properties) if (p.name == n) {
            const uint32_t prop = x.proc.allocStatic(8);
            x.mem.write<uint32_t>(prop, x.proc.internCString(p.name));
            x.mem.write<uint32_t>(prop + 4, x.proc.internCString(p.attributes));
            return prop;
        }
    }
    return 0;
}
uint32_t pr_getName(Ctx& x, uint32_t p) { return p ? x.mem.read<uint32_t>(p) : 0; }
uint32_t pr_getAttributes(Ctx& x, uint32_t p) { return p ? x.mem.read<uint32_t>(p + 4) : 0; }

// Dynamic class creation.
Class c_allocateClassPair(Ctx& x, Class super, GuestPtr name, uint32_t extra) {
    const std::string n = x.proc.readCString(name);
    if (rt(x).getClass(n)) return 0;
    // ClassBuilder path keeps things uniform; mark as guest-owned.
    ClassBuilder b = rt(x).defineClass(n, super ? rt(x).className(super) : "");
    if (ClassInfo* ci = rt(x).info(b.cls())) ci->host = false;
    if (ClassInfo* mi = rt(x).info(b.meta())) mi->host = false;
    (void)extra;
    return b.cls();
}
void c_registerClassPair(Ctx&, Class) {}
bool c_addIvar(Ctx& x, Class c, GuestPtr name, uint32_t size, uint8_t alignLog2, GuestPtr types) {
    ClassInfo* ci = rt(x).info(c);
    if (!ci) return false;
    const uint32_t align = 1u << std::min<uint32_t>(alignLog2, 4);
    Ivar iv;
    iv.name = x.proc.readCString(name);
    iv.type = types ? x.proc.readCString(types) : "";
    iv.offset = (ci->instanceSize + align - 1) & ~(align - 1);
    iv.size = size; iv.alignment = align;
    ci->instanceSize = iv.offset + size;
    x.mem.write<uint32_t>(ci->ro + 8, ci->instanceSize);
    ci->ivars.push_back(iv);
    return true;
}
bool c_addProtocol(Ctx& x, Class c, uint32_t p) { if (ClassInfo* ci = rt(x).info(c)) { ci->protocols.push_back(p); return true; } return false; }

// ------------------------------------------------------- properties ---

id prop_get(Ctx& x, id self, SEL, int32_t offset, bool atomic) {
    const id v = x.mem.read<uint32_t>(self + uint32_t(offset));
    if (!atomic) return v;
    return rt(x).autorelease(x.cpu, rt(x).retain(x.cpu, v));
}
void prop_set(Ctx& x, id self, SEL, int32_t offset, id v, bool atomic, int32_t copy) {
    (void)atomic;
    Runtime& r = rt(x);
    id nv;
    if (copy == 2) nv = id(r.send(x.cpu, v, "mutableCopyWithZone:", {0}));
    else if (copy) nv = id(r.send(x.cpu, v, "copyWithZone:", {0}));
    else nv = r.retain(x.cpu, v);
    const uint32_t slot = self + uint32_t(offset);
    const id old = x.mem.read<uint32_t>(slot);
    x.mem.write<uint32_t>(slot, nv);
    r.release(x.cpu, old);
}
// objc_setProperty_{non,}atomic{,_copy}(self, _cmd, newValue, offset)
void prop_set_atomic(Ctx& x, id self, SEL s, id v, int32_t off) { prop_set(x, self, s, off, v, true, 0); }
void prop_set_nonatomic(Ctx& x, id self, SEL s, id v, int32_t off) { prop_set(x, self, s, off, v, false, 0); }
void prop_set_atomic_copy(Ctx& x, id self, SEL s, id v, int32_t off) { prop_set(x, self, s, off, v, true, 1); }
void prop_set_nonatomic_copy(Ctx& x, id self, SEL s, id v, int32_t off) { prop_set(x, self, s, off, v, false, 1); }
void p_copyStruct(Ctx& x, GuestPtr dst, GuestPtr src, uint32_t size, bool, bool) { std::memmove(x.mem.host(dst), x.mem.host(src), size); }

// ------------------------------------------------------- exceptions etc ---

void e_void(Ctx&) {}
void e_mutation(Ctx& x, id o) { throw ObjCError("Collection " + rt(x).objectClassName(o) + " was mutated while being enumerated"); }
int32_t s_enter(Ctx& x, id o) { if (o) rt(x).syncEnter(o); return 0; }
int32_t s_exit(Ctx& x, id o) { if (o) rt(x).syncExit(o); return 0; }
void as_set(Ctx& x, id o, uint32_t key, id v, uint32_t policy) { rt(x).setAssociated(x.cpu, o, key, v, policy); }
id as_get(Ctx& x, id o, uint32_t key) { return rt(x).getAssociated(o, key); }
void as_remove(Ctx& x, id o) {
    Runtime& r = rt(x);
    (void)r;
    (void)o;
}
// int objc_getClassList(Class* buffer, int count): fills up to `count`, returns the total.
int32_t c_getClassList(Ctx& x, GuestPtr buf, int32_t count) {
    const std::vector<Class> all = rt(x).classList();
    if (buf) for (int32_t i = 0; i < count && size_t(i) < all.size(); ++i) x.mem.write<uint32_t>(buf + uint32_t(i) * 4, all[size_t(i)]);
    return int32_t(all.size());
}
// Class* objc_copyClassList(unsigned* outCount): malloc'ed, NULL-terminated.
uint32_t c_copyClassList(Ctx& x, GuestPtr outCount) {
    const std::vector<Class> all = rt(x).classList();
    const uint32_t arr = x.proc.heap().alloc(uint32_t(all.size() + 1) * 4);
    for (size_t i = 0; i < all.size(); ++i) x.mem.write<uint32_t>(arr + uint32_t(i) * 4, all[i]);
    x.mem.write<uint32_t>(arr + uint32_t(all.size()) * 4, 0);
    if (outCount) x.mem.write<uint32_t>(outCount, uint32_t(all.size()));
    return arr;
}
void msgForward(Ctx& x) { rt(x).unrecognized(x.cpu.r[0], x.cpu.r[1]); x.cpu.r[0] = x.cpu.r[1] = 0; }
int32_t z_zero(Ctx&) { return 0; }
void z_void1(Ctx&, uint32_t) {}

} // namespace

void registerObjCApi(Process& p, Runtime& r) {
    (void)r;
    p.defineFunction("_objc_msgSend", &msgSend);
    p.defineFunction("_objc_msgSend_stret", &msgSendStret);
    p.defineFunction("_objc_msgSendSuper", &msgSendSuper);
    p.defineFunction("_objc_msgSendSuper2", &msgSendSuper2);
    p.defineFunction("_objc_msgSendSuper_stret", &msgSendSuperStret);
    p.defineFunction("_objc_msgSendSuper2_stret", &msgSendSuper2Stret);
    p.def<&c_getClassList>("_objc_getClassList");
    p.def<&c_copyClassList>("_objc_copyClassList");
    p.defineFunction("__objc_msgForward", &msgForward);
    p.defineFunction("__objc_msgForward_stret", &msgForward);
    p.def<&a_retain>("_objc_retain");
    p.def<&a_release>("_objc_release");
    p.def<&a_autorelease>("_objc_autorelease");
    p.def<&a_retainAutorelease>("_objc_retainAutorelease");
    p.def<&a_retainAutoreleaseReturn>("_objc_retainAutoreleaseReturnValue");
    p.def<&a_autoreleaseReturn>("_objc_autoreleaseReturnValue");
    p.def<&a_retainAutoreleased>("_objc_retainAutoreleasedReturnValue");
    p.def<&a_claimAutoreleased>("_objc_unsafeClaimAutoreleasedReturnValue");
    p.def<&a_storeStrong>("_objc_storeStrong");
    p.def<&a_loadWeak>("_objc_loadWeak");
    p.def<&a_loadWeakRetained>("_objc_loadWeakRetained");
    p.def<&a_storeWeak>("_objc_storeWeak");
    p.def<&a_initWeak>("_objc_initWeak");
    p.def<&a_destroyWeak>("_objc_destroyWeak");
    p.def<&a_copyWeak>("_objc_copyWeak");
    p.def<&a_moveWeak>("_objc_moveWeak");
    p.def<&a_poolPush>("_objc_autoreleasePoolPush");
    p.def<&a_poolPop>("_objc_autoreleasePoolPop");
    p.def<&c_getClass>("_objc_getClass");
    p.def<&c_getClass>("_objc_lookUpClass");
    p.def<&c_getClass>("_NSClassFromString_cstr");
    p.def<&c_getRequiredClass>("_objc_getRequiredClass");
    p.def<&c_getMetaClass>("_objc_getMetaClass");
    p.def<&c_className>("_class_getName");
    p.def<&c_superclass>("_class_getSuperclass");
    p.def<&c_isMeta>("_class_isMetaClass");
    p.def<&c_instanceSize>("_class_getInstanceSize");
    p.def<&o_getClass>("_object_getClass");
    p.def<&o_setClass>("_object_setClass");
    p.def<&o_getClassName>("_object_getClassName");
    p.def<&c_createInstance>("_class_createInstance");
    p.def<&o_dispose>("_object_dispose");
    p.def<&o_copy>("_object_copy");
    p.def<&s_register>("_sel_registerName");
    p.def<&s_register>("_sel_getUid");
    p.def<&s_getName>("_sel_getName");
    p.def<&s_isEqual>("_sel_isEqual");
    p.def<&c_respondsTo>("_class_respondsToSelector");
    p.def<&c_getMethodImplementation>("_class_getMethodImplementation");
    p.def<&c_getMethodImplementation>("_class_getMethodImplementation_stret");
    p.def<&c_conformsTo>("_class_conformsToProtocol");
    p.def<&p_get>("_objc_getProtocol");
    p.def<&p_getName>("_protocol_getName");
    p.def<&c_getInstanceMethod>("_class_getInstanceMethod");
    p.def<&c_getClassMethod>("_class_getClassMethod");
    p.def<&m_getName>("_method_getName");
    p.def<&m_getImplementation>("_method_getImplementation");
    p.def<&m_getTypeEncoding>("_method_getTypeEncoding");
    p.def<&m_setImplementation>("_method_setImplementation");
    p.def<&m_exchange>("_method_exchangeImplementations");
    p.def<&c_addMethod>("_class_addMethod");
    p.def<&c_replaceMethod>("_class_replaceMethod");
    p.def<&c_copyMethodList>("_class_copyMethodList");
    p.def<&c_getInstanceVariable>("_class_getInstanceVariable");
    p.def<&c_copyIvarList>("_class_copyIvarList");
    p.def<&i_getName>("_ivar_getName");
    p.def<&i_getType>("_ivar_getTypeEncoding");
    p.def<&i_getOffset>("_ivar_getOffset");
    p.def<&o_getIvar>("_object_getIvar");
    p.def<&o_setIvar>("_object_setIvar");
    p.def<&o_getInstanceVariable>("_object_getInstanceVariable");
    p.def<&o_setInstanceVariable>("_object_setInstanceVariable");
    p.def<&c_copyPropertyList>("_class_copyPropertyList");
    p.def<&c_getProperty>("_class_getProperty");
    p.def<&pr_getName>("_property_getName");
    p.def<&pr_getAttributes>("_property_getAttributes");
    p.def<&c_allocateClassPair>("_objc_allocateClassPair");
    p.def<&c_registerClassPair>("_objc_registerClassPair");
    p.def<&c_addIvar>("_class_addIvar");
    p.def<&c_addProtocol>("_class_addProtocol");
    p.def<&prop_get>("_objc_getProperty");
    p.def<&prop_set>("_objc_setProperty");
    p.def<&prop_set_atomic>("_objc_setProperty_atomic");
    p.def<&prop_set_nonatomic>("_objc_setProperty_nonatomic");
    p.def<&prop_set_atomic_copy>("_objc_setProperty_atomic_copy");
    p.def<&prop_set_nonatomic_copy>("_objc_setProperty_nonatomic_copy");
    p.def<&p_copyStruct>("_objc_copyStruct");
    p.def<&p_copyStruct>("_objc_copyCppObjectAtomic");
    p.def<&e_void>("_objc_terminate");
    p.def<&e_mutation>("_objc_enumerationMutation");
    p.def<&z_void1>("_objc_setEnumerationMutationHandler");
    p.def<&s_enter>("_objc_sync_enter");
    p.def<&s_exit>("_objc_sync_exit");
    p.def<&as_set>("_objc_setAssociatedObject");
    p.def<&as_get>("_objc_getAssociatedObject");
    p.def<&as_remove>("_objc_removeAssociatedObjects");
    p.def<&z_zero>("_objc_collectingEnabled");
    p.def<&e_void>("_objc_registerThreadWithCollector");
    p.defineDataBlock("_objc_ehtype_vtable", 16);
}

} // namespace Hydrogen::Runtime::ObjC
