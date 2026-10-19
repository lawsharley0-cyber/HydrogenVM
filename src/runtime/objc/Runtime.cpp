#include "ObjC.hpp"
#include <algorithm>
#include <thread>

namespace Hydrogen::Runtime::ObjC {

namespace {
// class_ro_t flags
constexpr uint32_t RO_META = 1, RO_ROOT = 2, RO_HAS_CXX_STRUCTORS = 4;

struct DispatchCache {
    uint64_t gen = 0;
    const Runtime* owner = nullptr;
    std::unordered_map<uint64_t, IMP> map;
};
thread_local DispatchCache t_cache;
} // namespace

Runtime& rt(Process& p) { return p.service<Runtime>(); }

ThreadState& threadState(Process& p) {
    GuestThread& t = p.currentThread();
    if (!t.objcState) {
        t.objcState = new ThreadState;
        t.objcStateFree = [](void* s) { delete static_cast<ThreadState*>(s); };
    }
    return *static_cast<ThreadState*>(t.objcState);
}

// ------------------------------------------------------------ builder ---

Class ClassBuilder::meta() const { return rt_.classOf(cls_); }

ClassBuilder& ClassBuilder::method(const std::string& sel, Process::HostFn fn, const std::string& types) {
    const std::string debug = "-[" + rt_.className(cls_) + " " + sel + "]";
    const IMP imp = rt_.proc().registerHostFunction(debug, std::move(fn));
    rt_.addMethod(cls_, rt_.sel(sel), imp, types);
    return *this;
}

ClassBuilder& ClassBuilder::classMethod(const std::string& sel, Process::HostFn fn, const std::string& types) {
    const std::string debug = "+[" + rt_.className(cls_) + " " + sel + "]";
    const IMP imp = rt_.proc().registerHostFunction(debug, std::move(fn));
    rt_.addMethod(rt_.classOf(cls_), rt_.sel(sel), imp, types);
    return *this;
}

ClassBuilder& ClassBuilder::alias(const std::string& sel, const std::string& existing) {
    ClassInfo* ci = rt_.info(cls_);
    auto it = ci->methods.find(rt_.sel(existing));
    if (it != ci->methods.end()) rt_.addMethod(cls_, rt_.sel(sel), it->second, ci->types[it->first]);
    return *this;
}

// ------------------------------------------------------------ runtime ---

Runtime::Runtime(Process& p) : proc_(p) {
    emptyCache = p.defineDataBlock("__objc_empty_cache", 16);
    p.defineDataBlock("__objc_empty_vtable", 16);
    selAlloc = sel("alloc"); selInit = sel("init"); selDealloc = sel("dealloc"); selRetain = sel("retain");
    selRelease = sel("release"); selAutorelease = sel("autorelease"); selInitialize = sel("initialize");
    selLoad = sel("load"); selCxxDestruct = sel(".cxx_destruct"); selCxxConstruct = sel(".cxx_construct");
    selCopy = sel("copy"); selDescription = sel("description"); selRetainCount = sel("retainCount");
    selForwardingTarget = sel("forwardingTargetForSelector:"); selDoesNotRecognize = sel("doesNotRecognizeSelector:");
    selRespondsTo = sel("respondsToSelector:"); selIsEqual = sel("isEqual:"); selHash = sel("hash");
}

SEL Runtime::sel(const std::string& name) {
    {
        std::lock_guard<std::mutex> lk(selMu_);
        auto it = selByName_.find(name);
        if (it != selByName_.end()) return it->second;
    }
    const uint32_t s = proc_.internCString(name);
    std::lock_guard<std::mutex> lk(selMu_);
    auto [it, inserted] = selByName_.emplace(name, s);
    if (inserted) selNames_[s] = name;
    return it->second;
}

SEL Runtime::selFromString(uint32_t guestCString) {
    const std::string name = proc_.readCString(guestCString, 4096);
    std::lock_guard<std::mutex> lk(selMu_);
    auto it = selByName_.find(name);
    if (it != selByName_.end()) return it->second;
    selByName_.emplace(name, guestCString);
    selNames_[guestCString] = name;
    return guestCString;
}

std::string Runtime::selName(SEL s) const {
    if (!s) return "(null)";
    {
        std::lock_guard<std::mutex> lk(selMu_);
        auto it = selNames_.find(s);
        if (it != selNames_.end()) return it->second;
    }
    return proc_.readCString(s, 1024);
}

ClassInfo* Runtime::info(Class cls) const {
    std::shared_lock<std::shared_mutex> lk(classMu_);
    auto it = classes_.find(cls);
    return it == classes_.end() ? nullptr : it->second.get();
}

bool Runtime::isMeta(Class cls) const { const ClassInfo* ci = info(cls); return ci && ci->isMeta; }

std::string Runtime::className(Class cls) const {
    if (!cls) return "nil";
    if (const ClassInfo* ci = info(cls)) return ci->name;
    char buf[32];
    std::snprintf(buf, sizeof buf, "<unknown class 0x%x>", cls);
    return buf;
}

Class Runtime::getClass(const std::string& name) const {
    std::shared_lock<std::shared_mutex> lk(classMu_);
    auto it = classByName_.find(name);
    return it == classByName_.end() ? 0 : it->second;
}

bool Runtime::isSubclass(Class sub, Class cls) const {
    for (Class c = sub; c; c = superclass(c)) if (c == cls) return true;
    return false;
}
bool Runtime::isKindOf(id obj, Class cls) const { return obj && isSubclass(classOf(obj), cls); }

void Runtime::registerClassInfo(std::unique_ptr<ClassInfo> ci) {
    std::unique_lock<std::shared_mutex> lk(classMu_);
    if (!ci->isMeta) classByName_[ci->name] = ci->cls;
    const Class c = ci->cls;
    classes_[c] = std::move(ci);
}

Class Runtime::allocClassPair(const std::string& name, Class super, uint32_t instanceSize, bool host) {
    auto& m = proc_.mem();
    const uint32_t block = proc_.allocStatic(20 + 20 + 40 + 40, 16);
    const Class cls = block, meta = block + 20;
    const uint32_t ro = block + 40, metaRo = block + 80;
    const uint32_t nameStr = proc_.internCString(name);
    // Every metaclass's isa is the root metaclass, whose own isa is itself.
    const Class rootMeta = super ? classOf(classOf(super)) : meta;
    const ClassInfo* superInfo = super ? info(super) : nullptr;
    const uint32_t start = superInfo ? superInfo->instanceSize : 4;
    const uint32_t size = std::max(instanceSize ? instanceSize : start, start);
    // class
    m.write<uint32_t>(cls, meta);
    m.write<uint32_t>(cls + 4, super);
    m.write<uint32_t>(cls + 8, emptyCache);
    m.write<uint32_t>(cls + 12, 0);
    m.write<uint32_t>(cls + 16, ro);
    // metaclass
    m.write<uint32_t>(meta, super ? rootMeta : meta);
    m.write<uint32_t>(meta + 4, super ? classOf(super) : cls);
    m.write<uint32_t>(meta + 8, emptyCache);
    m.write<uint32_t>(meta + 12, 0);
    m.write<uint32_t>(meta + 16, metaRo);
    // ro
    m.write<uint32_t>(ro, super ? 0 : RO_ROOT);
    m.write<uint32_t>(ro + 4, start);
    m.write<uint32_t>(ro + 8, size);
    m.write<uint32_t>(ro + 16, nameStr);
    m.write<uint32_t>(metaRo, RO_META | (super ? 0 : RO_ROOT));
    m.write<uint32_t>(metaRo + 4, 20);
    m.write<uint32_t>(metaRo + 8, 20);
    m.write<uint32_t>(metaRo + 16, nameStr);

    auto ci = std::make_unique<ClassInfo>();
    ci->cls = cls; ci->meta = meta; ci->name = name; ci->host = host;
    ci->instanceStart = start; ci->instanceSize = size; ci->ro = ro;
    auto mi = std::make_unique<ClassInfo>();
    mi->cls = meta; mi->meta = super ? rootMeta : meta; mi->instanceClass = cls; mi->isMeta = true; mi->name = name;
    mi->host = host; mi->instanceSize = 20; mi->ro = metaRo;
    registerClassInfo(std::move(mi));
    registerClassInfo(std::move(ci));
    return cls;
}

ClassBuilder Runtime::defineClass(const std::string& name, const std::string& superName, uint32_t instanceSize) {
    Class super = 0;
    if (!superName.empty()) {
        super = getClass(superName);
        if (!super) throw std::runtime_error("defineClass " + name + ": superclass " + superName + " not defined yet");
    }
    if (Class existing = getClass(name)) return ClassBuilder(*this, existing);
    const Class cls = allocClassPair(name, super, instanceSize, true);
    proc_.defineData("_OBJC_CLASS_$_" + name, cls);
    proc_.defineData("_OBJC_METACLASS_$_" + name, classOf(cls));
    if (name == "NSObject") nsObject = cls;
    return ClassBuilder(*this, cls);
}

Class Runtime::stubClass(const std::string& name) {
    if (Class c = getClass(name)) return c;
    // Guess a sensible superclass so inherited behaviour mostly works.
    auto ends = [&](const char* s) { const size_t n = std::strlen(s); return name.size() > n && name.compare(name.size() - n, n, s) == 0; };
    std::string super = "NSObject";
    if (name.compare(0, 2, "UI") == 0) {
        if (ends("ViewController") && getClass("UIViewController")) super = "UIViewController";
        else if ((ends("View") || ends("Bar") || ends("Button") || ends("Label") || ends("Field")) && getClass("UIView")) super = "UIView";
    }
    ClassBuilder b = defineClass(name, super);
    info(b.cls())->stub = true;
    info(classOf(b.cls()))->stub = true;
    proc_.log(LogLevel::Warning, "Missing class " + name + " (created a placeholder subclass of " + super + ")");
    return b.cls();
}

void Runtime::addMethod(Class cls, SEL s, IMP imp, const std::string& types, bool replace) {
    {
        std::unique_lock<std::shared_mutex> lk(classMu_);
        auto it = classes_.find(cls);
        if (it == classes_.end()) return;
        ClassInfo& ci = *it->second;
        if (!replace && ci.methods.count(s)) return;
        ci.methods[s] = imp;
        ci.types[s] = types;
        if (s == selCxxDestruct) ci.hasCxxDestruct = true;
        if (s == selCxxConstruct) ci.hasCxxConstruct = true;
    }
    invalidateCaches();
}

void Runtime::invalidateCaches() { cacheGen_.fetch_add(1, std::memory_order_acq_rel); }

IMP Runtime::lookup(Class cls, SEL s) {
    if (!cls) return 0;
    const uint64_t gen = cacheGen_.load(std::memory_order_acquire);
    if (t_cache.gen != gen || t_cache.owner != this) { t_cache.map.clear(); t_cache.gen = gen; t_cache.owner = this; }
    const uint64_t key = (uint64_t(cls) << 32) | s;
    auto hit = t_cache.map.find(key);
    if (hit != t_cache.map.end()) return hit->second;
    IMP imp = 0;
    {
        std::shared_lock<std::shared_mutex> lk(classMu_);
        Class c = cls;
        for (int depth = 0; c && depth < 64; ++depth) {
            auto it = classes_.find(c);
            if (it == classes_.end()) break;
            auto m = it->second->methods.find(s);
            if (m != it->second->methods.end()) { imp = m->second; break; }
            c = proc_.mem().read<uint32_t>(c + 4);
        }
    }
    if (t_cache.map.size() > 8192) t_cache.map.clear();
    t_cache.map.emplace(key, imp);
    return imp;
}

void Runtime::initializeClass(CPU::Cpu& c, Class cls) {
    ClassInfo* ci = info(cls);
    if (!ci || ci->isMeta) return;
    if (ci->initState.load(std::memory_order_acquire) == 2) return;
    const uint32_t me = proc_.currentThread().id;
    if (ci->initState.load() == 1) {
        if (ci->initThread == me) return;     // messages sent during our own +initialize
        while (ci->initState.load() == 1) {
            if (proc_.stopping()) throw ProcessExit(0, "stopping");
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        return;
    }
    int expected = 0;
    if (!ci->initState.compare_exchange_strong(expected, 1)) { initializeClass(c, cls); return; }
    ci->initThread = me;
    if (Class s = superclass(cls)) initializeClass(c, s);
    try {
        const IMP imp = lookup(classOf(cls), selInitialize);
        if (imp && !proc_.engine().isHleAddress(imp)) proc_.callGuest(c, imp, {cls, selInitialize});
    } catch (...) {
        ci->initState = 2;
        throw;
    }
    ci->initState.store(2, std::memory_order_release);
}

IMP Runtime::lookupForSend(Class cls, SEL s) {
    ClassInfo* ci = info(cls);
    if (ci) {
        const Class instCls = ci->isMeta ? ci->instanceClass : cls;
        if (ClassInfo* ii = instCls ? info(instCls) : nullptr; ii && ii->initState.load(std::memory_order_acquire) != 2)
            initializeClass(proc_.currentThread().cpu, instCls);
    }
    return lookup(cls, s);
}

bool Runtime::respondsTo(id obj, SEL s) { return obj && lookup(classOf(obj), s) != 0; }

void Runtime::unrecognized(id self, SEL s) {
    const Class cls = classOf(self);
    const ClassInfo* ci = info(cls);
    const bool meta = ci && ci->isMeta;
    const std::string sig = std::string(meta ? "+[" : "-[") + className(cls) + " " + selName(s) + "]";
    // Is the selector expected to come from a framework class we implement?
    bool frameworkClass = false;
    for (Class c = cls; c; c = superclass(c)) {
        const ClassInfo* k = info(c);
        if (!k) break;
        if (k->host && k->name != "NSObject") { frameworkClass = true; break; }
        if (k->stub) { frameworkClass = true; break; }
    }
    if (frameworkClass) {
        proc_.reportMissing(sig);
        throw ObjCError("Missing method " + sig + " (HydrogenVM doesn't implement it yet)");
    }
    char buf[64];
    std::snprintf(buf, sizeof buf, "%s 0x%x", meta ? "class" : "instance", self);
    throw ObjCError(sig + ": unrecognized selector sent to " + buf);
}

uint64_t Runtime::send(CPU::Cpu& c, id self, SEL s, std::initializer_list<uint32_t> args) {
    if (!self) return 0;
    IMP imp = lookupForSend(classOf(self), s);
    if (!imp) {
        // forwardingTargetForSelector:
        const IMP ft = lookup(classOf(self), selForwardingTarget);
        if (ft) {
            const id target = id(proc_.callGuest(c, ft, {self, selForwardingTarget, s}));
            if (target && target != self) return send(c, target, s, args);
        }
        unrecognized(self, s);
    }
    std::vector<uint32_t> a;
    a.reserve(args.size() + 2);
    a.push_back(self);
    a.push_back(s);
    a.insert(a.end(), args.begin(), args.end());
    return proc_.callGuest(c, imp, a);
}

uint64_t Runtime::sendSuper(CPU::Cpu& c, id self, Class startClass, SEL s, std::initializer_list<uint32_t> args) {
    if (!self) return 0;
    const IMP imp = lookup(startClass, s);
    if (!imp) unrecognized(self, s);
    std::vector<uint32_t> a{self, s};
    a.insert(a.end(), args.begin(), args.end());
    return proc_.callGuest(c, imp, a);
}

// ------------------------------------------------------------ objects ---

id Runtime::createInstance(Class cls, uint32_t extraBytes) {
    const ClassInfo* ci = info(cls);
    const uint32_t size = (ci ? ci->instanceSize : 16) + extraBytes;
    const id obj = proc_.heap().allocZeroed(std::max<uint32_t>(size, 4));
    if (!obj) throw ObjCError("out of guest memory allocating " + className(cls));
    proc_.mem().write<uint32_t>(obj, cls);
    // C++ ivar constructors, superclass first.
    std::vector<Class> chain;
    for (Class k = cls; k; k = superclass(k)) { const ClassInfo* ki = info(k); if (!ki) break; if (ki->hasCxxConstruct) chain.push_back(k); }
    for (auto it = chain.rbegin(); it != chain.rend(); ++it) cxxConstruct(proc_.currentThread().cpu, *it, obj);
    return obj;
}

void Runtime::cxxConstruct(CPU::Cpu& c, Class cls, id obj) {
    const ClassInfo* ci = info(cls);
    auto it = ci->methods.find(selCxxConstruct);
    if (it != ci->methods.end()) proc_.callGuest(c, it->second, {obj, selCxxConstruct});
}
void Runtime::cxxDestruct(CPU::Cpu& c, Class cls, id obj) {
    const ClassInfo* ci = info(cls);
    auto it = ci->methods.find(selCxxDestruct);
    if (it != ci->methods.end()) proc_.callGuest(c, it->second, {obj, selCxxDestruct});
}

void Runtime::disposeInstance(CPU::Cpu& c, id obj) {
    if (!obj) return;
    // .cxx_destruct, most-derived first (ARC uses it to release strong ivars).
    for (Class k = classOf(obj); k; k = superclass(k)) {
        const ClassInfo* ki = info(k);
        if (!ki) break;
        if (ki->hasCxxDestruct) cxxDestruct(c, k, obj);
    }
    // Associated objects.
    std::unordered_map<uint32_t, std::pair<id, uint32_t>> assoc;
    {
        std::lock_guard<std::mutex> lk(assocMu_);
        auto it = assoc_.find(obj);
        if (it != assoc_.end()) { assoc = std::move(it->second); assoc_.erase(it); }
    }
    for (auto& [k, v] : assoc) if (v.second != 0) release(c, v.first);
    {
        std::lock_guard<std::mutex> lk(rcMu_);
        auto w = weakRefs_.find(obj);
        if (w != weakRefs_.end()) {
            for (uint32_t slot : w->second) { proc_.mem().write<uint32_t>(slot, 0); weakSlots_.erase(slot); }
            weakRefs_.erase(w);
        }
        extraRefs_.erase(obj);
        deallocating_.erase(obj);
    }
    std::vector<std::unique_ptr<HostObject>> hostData;
    {
        std::lock_guard<std::mutex> lk(hostMu_);
        auto it = hostData_.find(obj);
        if (it != hostData_.end()) { hostData = std::move(it->second); hostData_.erase(it); }
    }
    hostData.clear();   // destructors may free guest memory: run them outside the lock
    {
        std::lock_guard<std::mutex> lk(syncMu_);
        syncLocks_.erase(obj);
    }
    proc_.mem().write<uint32_t>(obj, 0);
    proc_.heap().free(obj);
}

// ------------------------------------------------------------ retain/release ---

void Runtime::makeImmortal(id obj) { std::lock_guard<std::mutex> lk(rcMu_); immortal_.insert(obj); }

id Runtime::rootRetain(id obj) {
    if (!obj) return 0;
    std::lock_guard<std::mutex> lk(rcMu_);
    if (immortal_.count(obj) || deallocating_.count(obj)) return obj;
    ++extraRefs_[obj];
    return obj;
}

bool Runtime::rootRelease(id obj) {
    if (!obj) return false;
    std::lock_guard<std::mutex> lk(rcMu_);
    if (immortal_.count(obj) || deallocating_.count(obj)) return false;
    auto it = extraRefs_.find(obj);
    if (it == extraRefs_.end() || it->second == 0) {
        if (it != extraRefs_.end()) extraRefs_.erase(it);
        deallocating_.insert(obj);
        return true;
    }
    --it->second;
    return false;
}

uint32_t Runtime::retainCount(id obj) {
    std::lock_guard<std::mutex> lk(rcMu_);
    if (immortal_.count(obj)) return 0xFFFFFFFFu;
    auto it = extraRefs_.find(obj);
    return 1 + (it == extraRefs_.end() ? 0 : it->second);
}

id Runtime::retain(CPU::Cpu& c, id obj) {
    if (!obj) return 0;
    const IMP imp = lookup(classOf(obj), selRetain);
    if (imp == rootRetainImp || !imp) return rootRetain(obj);
    return id(send(c, obj, selRetain));
}

void Runtime::release(CPU::Cpu& c, id obj) {
    if (!obj) return;
    const IMP imp = lookup(classOf(obj), selRelease);
    if (imp == rootReleaseImp || !imp) {
        if (rootRelease(obj)) send(c, obj, selDealloc);
        return;
    }
    send(c, obj, selRelease);
}

id Runtime::autorelease(CPU::Cpu& c, id obj) {
    if (!obj) return 0;
    const IMP imp = lookup(classOf(obj), selAutorelease);
    if (imp == rootAutoreleaseImp || !imp) { addToPool(obj); return obj; }
    return id(send(c, obj, selAutorelease));
}

uint32_t Runtime::poolPush() {
    ThreadState& ts = threadState(proc_);
    ts.marks.push_back(uint32_t(ts.pool.size()));
    return uint32_t(ts.marks.size());
}

void Runtime::poolPop(CPU::Cpu& c, uint32_t token) {
    ThreadState& ts = threadState(proc_);
    if (token == 0 || token > ts.marks.size()) return;
    while (ts.marks.size() >= token) {
        const uint32_t mark = ts.marks.back();
        while (ts.pool.size() > mark) {
            const id obj = ts.pool.back();
            ts.pool.pop_back();
            release(c, obj);
        }
        ts.marks.pop_back();
    }
}

void Runtime::addToPool(id obj) {
    ThreadState& ts = threadState(proc_);
    if (ts.marks.empty()) {
        static std::atomic<int> warned{0};
        if (warned++ < 3)
            proc_.log(LogLevel::Warning, "Object of class " + objectClassName(obj) + " autoreleased with no pool in place - just leaking");
        return;
    }
    ts.pool.push_back(obj);
}

// ------------------------------------------------------------ weak ---

id Runtime::loadWeak(uint32_t slot) {
    if (!slot) return 0;
    std::lock_guard<std::mutex> lk(rcMu_);
    const id v = proc_.mem().read<uint32_t>(slot);
    if (!v || deallocating_.count(v)) return 0;
    return v;
}

void Runtime::storeWeak(uint32_t slot, id obj) {
    std::lock_guard<std::mutex> lk(rcMu_);
    auto old = weakSlots_.find(slot);
    if (old != weakSlots_.end()) {
        auto w = weakRefs_.find(old->second);
        if (w != weakRefs_.end()) { w->second.erase(slot); if (w->second.empty()) weakRefs_.erase(w); }
        weakSlots_.erase(old);
    }
    if (obj && !deallocating_.count(obj)) {
        weakRefs_[obj].insert(slot);
        weakSlots_[slot] = obj;
        proc_.mem().write<uint32_t>(slot, obj);
    } else {
        proc_.mem().write<uint32_t>(slot, 0);
    }
}

void Runtime::clearWeakSlot(uint32_t slot) { storeWeak(slot, 0); }

// ------------------------------------------------------------ ivars ---

const Ivar* Runtime::findIvar(Class cls, const std::string& name, Class* owner) {
    for (Class c = cls; c; c = superclass(c)) {
        const ClassInfo* ci = info(c);
        if (!ci) break;
        for (const Ivar& iv : ci->ivars) if (iv.name == name) { if (owner) *owner = c; return &iv; }
    }
    return nullptr;
}

std::vector<Ivar> Runtime::allIvars(Class cls) {
    std::vector<Ivar> out;
    if (const ClassInfo* ci = info(cls)) out = ci->ivars;
    return out;
}

// ------------------------------------------------------------ associated / sync ---

void Runtime::setAssociated(CPU::Cpu& c, id obj, uint32_t key, id value, uint32_t policy) {
    if (!obj) return;
    const uint32_t kind = policy & 0xFF;   // 0 assign, 1 retain, 3 copy
    id stored = value;
    if (value && kind == 1) stored = retain(c, value);
    else if (value && kind == 3) stored = id(send(c, value, selCopy));
    std::pair<id, uint32_t> old{0, 0};
    {
        std::lock_guard<std::mutex> lk(assocMu_);
        auto& m = assoc_[obj];
        auto it = m.find(key);
        if (it != m.end()) { old = it->second; m.erase(it); }
        if (value) m[key] = {stored, kind};
    }
    if (old.first && old.second) release(c, old.first);
}

id Runtime::getAssociated(id obj, uint32_t key) {
    std::lock_guard<std::mutex> lk(assocMu_);
    auto it = assoc_.find(obj);
    if (it == assoc_.end()) return 0;
    auto k = it->second.find(key);
    return k == it->second.end() ? 0 : k->second.first;
}

void Runtime::syncEnter(id obj) {
    std::recursive_mutex* m;
    {
        std::lock_guard<std::mutex> lk(syncMu_);
        auto& p = syncLocks_[obj];
        if (!p) p = std::make_unique<std::recursive_mutex>();
        m = p.get();
    }
    while (!m->try_lock()) {
        if (proc_.stopping()) throw ProcessExit(0, "stopping");
        std::this_thread::sleep_for(std::chrono::microseconds(100));
    }
}

void Runtime::syncExit(id obj) {
    std::lock_guard<std::mutex> lk(syncMu_);
    auto it = syncLocks_.find(obj);
    if (it != syncLocks_.end()) it->second->unlock();
}

// ------------------------------------------------------------ protocols ---

uint32_t Runtime::getProtocol(const std::string& name) const {
    std::lock_guard<std::mutex> lk(selMu_);
    auto it = protocols_.find(name);
    return it == protocols_.end() ? 0 : it->second;
}

std::string Runtime::protocolName(uint32_t proto) const {
    {
        std::lock_guard<std::mutex> lk(selMu_);
        auto it = protocolNames_.find(proto);
        if (it != protocolNames_.end()) return it->second;
    }
    return proto ? proc_.readCString(proc_.mem().read<uint32_t>(proto + 4)) : "";
}

bool Runtime::conformsTo(Class cls, uint32_t proto) {
    const std::string want = protocolName(proto);
    std::function<bool(uint32_t, int)> protoMatches = [&](uint32_t p, int depth) -> bool {
        if (!p || depth > 16) return false;
        if (protocolName(p) == want) return true;
        const uint32_t list = proc_.mem().read<uint32_t>(p + 8);
        if (!list) return false;
        const uint32_t n = proc_.mem().read<uint32_t>(list);
        for (uint32_t i = 0; i < n && i < 256; ++i)
            if (protoMatches(proc_.mem().read<uint32_t>(list + 4 + i * 4), depth + 1)) return true;
        return false;
    };
    for (Class c = cls; c; c = superclass(c)) {
        const ClassInfo* ci = info(c);
        if (!ci) break;
        for (uint32_t p : ci->protocols) if (protoMatches(p, 0)) return true;
    }
    return false;
}

// ------------------------------------------------------------ image registration ---

void Runtime::readMethodList(ClassInfo& ci, uint32_t list) {
    if (!list) return;
    auto& m = proc_.mem();
    const uint32_t entsize = m.read<uint32_t>(list) & ~3u;
    const uint32_t count = m.read<uint32_t>(list + 4);
    if (entsize < 12 || count > 100000) return;
    for (uint32_t i = 0; i < count; ++i) {
        const uint32_t e = list + 8 + i * entsize;
        const SEL s = selFromString(m.read<uint32_t>(e));
        m.write<uint32_t>(e, s);   // uniqued, as the real runtime does
        ci.methods[s] = m.read<uint32_t>(e + 8);
        ci.types[s] = proc_.readCString(m.read<uint32_t>(e + 4), 512);
        if (s == selCxxDestruct) ci.hasCxxDestruct = true;
        if (s == selCxxConstruct) ci.hasCxxConstruct = true;
    }
}

void Runtime::realizeClass(Class cls, std::unordered_set<Class>& visiting) {
    if (!cls || info(cls) || visiting.count(cls)) return;
    visiting.insert(cls);
    auto& m = proc_.mem();
    const Class meta = m.read<uint32_t>(cls);
    const Class super = m.read<uint32_t>(cls + 4);
    if (super && !info(super)) realizeClass(super, visiting);
    const uint32_t ro = m.read<uint32_t>(cls + 16) & ~3u;
    const uint32_t roFlags = m.read<uint32_t>(ro);
    auto ci = std::make_unique<ClassInfo>();
    ci->cls = cls; ci->meta = meta; ci->ro = ro;
    ci->name = proc_.readCString(m.read<uint32_t>(ro + 16), 1024);
    ci->instanceStart = m.read<uint32_t>(ro + 4);
    ci->instanceSize = m.read<uint32_t>(ro + 8);
    readMethodList(*ci, m.read<uint32_t>(ro + 20));
    // ivars, sliding them past a superclass that is bigger than the compiler thought
    const uint32_t superSize = super && info(super) ? info(super)->instanceSize : 0;
    uint32_t slide = 0;
    const uint32_t ivars = m.read<uint32_t>(ro + 28);
    uint32_t maxAlign = 4;
    if (ivars) {
        const uint32_t n = m.read<uint32_t>(ivars + 4), entsize = m.read<uint32_t>(ivars) ? m.read<uint32_t>(ivars) : 20;
        for (uint32_t i = 0; i < n && i < 4096; ++i) {
            const uint32_t e = ivars + 8 + i * entsize;
            const uint32_t alignRaw = m.read<uint32_t>(e + 12);
            const uint32_t align = alignRaw == 0xFFFFFFFFu ? 4 : (1u << std::min<uint32_t>(alignRaw, 4));
            maxAlign = std::max(maxAlign, align);
        }
    }
    if (superSize > ci->instanceStart && !(roFlags & RO_ROOT)) {
        slide = (superSize - ci->instanceStart + maxAlign - 1) & ~(maxAlign - 1);
        ci->instanceStart += slide;
        ci->instanceSize += slide;
        m.write<uint32_t>(ro + 4, ci->instanceStart);
        m.write<uint32_t>(ro + 8, ci->instanceSize);
    }
    if (ivars) {
        const uint32_t entsize = m.read<uint32_t>(ivars) ? m.read<uint32_t>(ivars) : 20;
        const uint32_t n = m.read<uint32_t>(ivars + 4);
        for (uint32_t i = 0; i < n && i < 4096; ++i) {
            const uint32_t e = ivars + 8 + i * entsize;
            Ivar iv;
            iv.offsetPtr = m.read<uint32_t>(e);
            iv.name = proc_.readCString(m.read<uint32_t>(e + 4), 256);
            iv.type = proc_.readCString(m.read<uint32_t>(e + 8), 256);
            const uint32_t alignRaw = m.read<uint32_t>(e + 12);
            iv.alignment = alignRaw == 0xFFFFFFFFu ? 4 : (1u << std::min<uint32_t>(alignRaw, 4));
            iv.size = m.read<uint32_t>(e + 16);
            if (iv.offsetPtr) {
                iv.offset = m.read<uint32_t>(iv.offsetPtr) + slide;
                if (slide) m.write<uint32_t>(iv.offsetPtr, iv.offset);
            }
            ci->ivars.push_back(iv);
        }
    }
    // properties
    if (const uint32_t props = m.read<uint32_t>(ro + 36)) {
        const uint32_t n = m.read<uint32_t>(props + 4);
        for (uint32_t i = 0; i < n && i < 4096; ++i)
            ci->properties.push_back({proc_.readCString(m.read<uint32_t>(props + 8 + i * 8)), proc_.readCString(m.read<uint32_t>(props + 12 + i * 8))});
    }
    if (const uint32_t pl = m.read<uint32_t>(ro + 24)) {
        const uint32_t n = m.read<uint32_t>(pl);
        for (uint32_t i = 0; i < n && i < 1024; ++i) ci->protocols.push_back(m.read<uint32_t>(pl + 4 + i * 4));
    }
    m.write<uint32_t>(cls + 8, emptyCache);
    // metaclass
    auto mi = std::make_unique<ClassInfo>();
    const uint32_t mro = m.read<uint32_t>(meta + 16) & ~3u;
    mi->cls = meta; mi->meta = m.read<uint32_t>(meta); mi->isMeta = true; mi->instanceClass = cls; mi->name = ci->name;
    mi->ro = mro; mi->instanceSize = 20;
    readMethodList(*mi, m.read<uint32_t>(mro + 20));
    m.write<uint32_t>(meta + 8, emptyCache);
    auto load = mi->methods.find(selLoad);
    if (load != mi->methods.end()) pendingLoads_.emplace_back(cls, false);
    registerClassInfo(std::move(mi));
    registerClassInfo(std::move(ci));
    visiting.erase(cls);
}

void Runtime::attachCategory(uint32_t cat) {
    auto& m = proc_.mem();
    const Class cls = m.read<uint32_t>(cat + 4);
    if (!cls || !info(cls)) return;
    const Class meta = classOf(cls);
    ClassInfo tmp, tmpMeta;
    readMethodList(tmp, m.read<uint32_t>(cat + 8));
    readMethodList(tmpMeta, m.read<uint32_t>(cat + 12));
    for (auto& [s, imp] : tmp.methods) addMethod(cls, s, imp, tmp.types[s]);
    for (auto& [s, imp] : tmpMeta.methods) {
        if (s == selLoad) { pendingCategoryLoads_.push_back(cat); continue; }   // category +load is called, not attached
        addMethod(meta, s, imp, tmpMeta.types[s]);
    }
    if (const uint32_t pl = m.read<uint32_t>(cat + 16)) {
        const uint32_t n = m.read<uint32_t>(pl);
        std::unique_lock<std::shared_mutex> lk(classMu_);
        auto& ci = *classes_[cls];
        for (uint32_t i = 0; i < n && i < 1024; ++i) ci.protocols.push_back(m.read<uint32_t>(pl + 4 + i * 4));
    }
}

void Runtime::registerImage(const Loader::Image& img) {
    auto& m = proc_.mem();
    auto words = [&](const char* sect, auto fn) {
        if (const Loader::Section* s = img.findSection("__DATA", sect))
            for (uint32_t a = s->addr; a + 4 <= s->addr + s->size; a += 4) fn(a);
    };
    // Selector references.
    size_t nsel = 0;
    words("__objc_selrefs", [&](uint32_t a) { m.write<uint32_t>(a, selFromString(m.read<uint32_t>(a))); ++nsel; });
    // Protocols: one canonical object per name.
    words("__objc_protolist", [&](uint32_t a) {
        const uint32_t p = m.read<uint32_t>(a);
        const std::string name = proc_.readCString(m.read<uint32_t>(p + 4));
        std::lock_guard<std::mutex> lk(selMu_);
        if (!protocols_.count(name)) { protocols_[name] = p; protocolNames_[p] = name; }
        if (protocolClass) m.write<uint32_t>(p, protocolClass);
    });
    words("__objc_protorefs", [&](uint32_t a) {
        const uint32_t p = m.read<uint32_t>(a);
        if (const uint32_t canon = getProtocol(proc_.readCString(m.read<uint32_t>(p + 4)))) m.write<uint32_t>(a, canon);
    });
    // Classes.
    std::unordered_set<Class> visiting;
    size_t ncls = 0;
    words("__objc_classlist", [&](uint32_t a) { realizeClass(m.read<uint32_t>(a), visiting); ++ncls; });
    size_t ncat = 0;
    words("__objc_catlist", [&](uint32_t a) { attachCategory(m.read<uint32_t>(a)); ++ncat; });
    if (ncls || ncat)
        proc_.logf(LogLevel::Info, "Objective-C: %zu classes, %zu categories, %zu selector refs", ncls, ncat, nsel);
    invalidateCaches();
}

void Runtime::callLoadMethods(CPU::Cpu& c) {
    // Superclasses before subclasses, then categories.
    std::unordered_set<Class> done;
    std::function<void(Class)> loadClass = [&](Class cls) {
        if (!cls || done.count(cls)) return;
        done.insert(cls);
        if (Class s = superclass(cls)) {
            for (auto& [k, cat] : pendingLoads_) if (k == s) loadClass(s);
        }
        ClassInfo* mi = info(classOf(cls));
        if (!mi) return;
        auto it = mi->methods.find(selLoad);
        if (it != mi->methods.end()) proc_.callGuest(c, it->second, {cls, selLoad});
    };
    auto loads = std::move(pendingLoads_);
    pendingLoads_.clear();
    for (auto& [cls, cat] : loads) loadClass(cls);
    auto cats = std::move(pendingCategoryLoads_);
    pendingCategoryLoads_.clear();
    auto& m = proc_.mem();
    for (uint32_t cat : cats) {
        const Class cls = m.read<uint32_t>(cat + 4);
        const uint32_t list = m.read<uint32_t>(cat + 12);
        const uint32_t n = m.read<uint32_t>(list + 4), entsize = m.read<uint32_t>(list) & ~3u;
        for (uint32_t i = 0; i < n; ++i)
            if (m.read<uint32_t>(list + 8 + i * entsize) == selLoad)
                proc_.callGuest(c, m.read<uint32_t>(list + 8 + i * entsize + 8), {cls, selLoad});
    }
}

} // namespace Hydrogen::Runtime::ObjC
