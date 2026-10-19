// HydrogenVM — the root class, NSObject (the parts libobjc provides; the
// Foundation module adds -description, KVC, delayed performs, etc.).
#include "ObjC.hpp"

namespace Hydrogen::Runtime::ObjC {
namespace {

id o_alloc(Ctx& x, id self, SEL) {
    Runtime& r = rt(x);
    const SEL awz = r.sel("allocWithZone:");
    const IMP imp = r.lookup(r.classOf(self), awz);
    if (imp && imp != *x.proc.lookupSymbol("__hvm_objc_rootAllocWithZone"))
        return id(r.send(x.cpu, self, awz, {0}));
    return r.createInstance(self);
}
id o_allocWithZone(Ctx& x, id self, SEL, uint32_t) { return rt(x).createInstance(self); }
id o_new(Ctx& x, id self, SEL) {
    Runtime& r = rt(x);
    const id obj = id(r.send(x.cpu, self, r.selAlloc));
    return id(r.send(x.cpu, obj, r.selInit));
}
id o_self(Ctx&, id self, SEL) { return self; }
void o_dealloc(Ctx& x, id self, SEL) { rt(x).disposeInstance(x.cpu, self); }
id o_retain(Ctx& x, id self, SEL) { return rt(x).rootRetain(self); }
void o_release(Ctx& x, id self, SEL) { Runtime& r = rt(x); if (r.rootRelease(self)) r.send(x.cpu, self, r.selDealloc); }
id o_autorelease(Ctx& x, id self, SEL) { rt(x).addToPool(self); return self; }
uint32_t o_retainCount(Ctx& x, id self, SEL) { return rt(x).retainCount(self); }
uint32_t o_classRetainCount(Ctx&, id, SEL) { return 0xFFFFFFFFu; }
void o_classRelease(Ctx&, id, SEL) {}
id o_class(Ctx& x, id self, SEL) { return rt(x).classOf(self); }
id o_superclass(Ctx& x, id self, SEL) { return rt(x).superclass(rt(x).classOf(self)); }
id o_classSuperclass(Ctx& x, id self, SEL) { return rt(x).superclass(self); }
bool o_isKindOfClass(Ctx& x, id self, SEL, Class cls) { return rt(x).isKindOf(self, cls); }
bool o_isMemberOfClass(Ctx& x, id self, SEL, Class cls) { return rt(x).classOf(self) == cls; }
bool o_isSubclassOfClass(Ctx& x, id self, SEL, Class cls) { return rt(x).isSubclass(self, cls); }
bool o_respondsToSelector(Ctx& x, id self, SEL, SEL s) { return s && rt(x).lookup(rt(x).classOf(self), s) != 0; }
bool o_instancesRespond(Ctx& x, id self, SEL, SEL s) { return s && rt(x).lookup(self, s) != 0; }
bool o_conformsToProtocol(Ctx& x, id self, SEL, uint32_t proto) { return rt(x).conformsTo(rt(x).classOf(self), proto); }
bool o_classConformsToProtocol(Ctx& x, id self, SEL, uint32_t proto) { return rt(x).conformsTo(self, proto); }
uint32_t o_hash(Ctx&, id self, SEL) { return self; }
bool o_isEqual(Ctx&, id self, SEL, id other) { return self == other; }
uint32_t o_zone(Ctx&, id, SEL) { return 0; }
bool o_no(Ctx&, id, SEL) { return false; }
bool o_no1(Ctx&, id, SEL, uint32_t) { return false; }
bool o_yes(Ctx&, id, SEL) { return true; }
void o_noop(Ctx&, id, SEL) {}
id o_nil1(Ctx&, id, SEL, uint32_t) { return 0; }
id o_copy(Ctx& x, id self, SEL) { return id(rt(x).send(x.cpu, self, "copyWithZone:", {0})); }
id o_mutableCopy(Ctx& x, id self, SEL) { return id(rt(x).send(x.cpu, self, "mutableCopyWithZone:", {0})); }
IMP o_methodForSelector(Ctx& x, id self, SEL, SEL s) { return rt(x).lookup(rt(x).classOf(self), s); }
IMP o_instanceMethodForSelector(Ctx& x, id self, SEL, SEL s) { return rt(x).lookup(self, s); }
void o_doesNotRecognize(Ctx& x, id self, SEL, SEL s) { rt(x).unrecognizedFatal(self, s); }
bool o_isDeallocating(Ctx& x, id self, SEL) { return rt(x).retainCount(self) == 0; }

// -performSelector:withObject:withObject: — re-dispatch with the arguments
// shifted into place and tail-jump, so any return type comes back intact.
void o_performSelector(Ctx& x) {
    Runtime& r = rt(x);
    const id self = x.cpu.r[0];
    const SEL s = x.cpu.r[2];
    if (!s) throw ObjCError("-[NSObject performSelector:]: null selector");
    IMP imp = r.lookupForSend(r.classOf(self), s);
    if (!imp) imp = r.unrecognized(self, s);
    x.cpu.r[1] = s;
    x.cpu.r[2] = x.cpu.r[3];
    x.cpu.r[3] = x.mem.read<uint32_t>(x.cpu.r[CPU::kSP]);   // 2nd object (only for withObject:withObject:)
    if (x.proc.engine().isHleAddress(imp)) x.proc.engine().callHle(&x.cpu, x.proc.engine().hleIndex(imp));
    else CPU::Engine::hleJump(x.cpu, imp);
}

} // namespace

void registerNSObject(Runtime& r) {
    Process& p = r.proc();
    ClassBuilder o = r.defineClass("NSObject", "");
    r.nsObject = o.cls();
    const uint32_t awz = p.def<&o_allocWithZone>("__hvm_objc_rootAllocWithZone");
    r.addMethod(o.meta(), r.sel("allocWithZone:"), awz, "@@:^v");
    o.cm<&o_alloc>("alloc")
     .cm<&o_new>("new")
     .cm<&o_self>("class")
     .cm<&o_classSuperclass>("superclass")
     .cm<&o_isSubclassOfClass>("isSubclassOfClass:", "c@:#")
     .cm<&o_instancesRespond>("instancesRespondToSelector:", "c@::")
     .cm<&o_classConformsToProtocol>("conformsToProtocol:", "c@:@")
     .cm<&o_instanceMethodForSelector>("instanceMethodForSelector:", "^?@::")
     .cm<&o_noop>("load", "v@:")
     .cm<&o_noop>("initialize", "v@:")
     .cm<&o_no1>("resolveInstanceMethod:", "c@::")
     .cm<&o_no1>("resolveClassMethod:", "c@::")
     .cm<&o_self>("retain")
     .cm<&o_classRelease>("release", "v@:")
     .cm<&o_self>("autorelease")
     .cm<&o_classRetainCount>("retainCount", "I@:")
     .cm<&o_self>("copy")
     .cm<&o_self>("self");
    o.m<&o_self>("init")
     .m<&o_dealloc>("dealloc", "v@:")
     .m<&o_retain>("retain")
     .m<&o_release>("release", "v@:")
     .m<&o_autorelease>("autorelease")
     .m<&o_retainCount>("retainCount", "I@:")
     .m<&o_class>("class", "#@:")
     .m<&o_superclass>("superclass", "#@:")
     .m<&o_isKindOfClass>("isKindOfClass:", "c@:#")
     .m<&o_isMemberOfClass>("isMemberOfClass:", "c@:#")
     .m<&o_respondsToSelector>("respondsToSelector:", "c@::")
     .m<&o_conformsToProtocol>("conformsToProtocol:", "c@:@")
     .m<&o_hash>("hash", "I@:")
     .m<&o_isEqual>("isEqual:", "c@:@")
     .m<&o_self>("self")
     .m<&o_zone>("zone", "^v@:")
     .m<&o_copy>("copy")
     .m<&o_mutableCopy>("mutableCopy")
     .m<&o_methodForSelector>("methodForSelector:", "^?@::")
     .m<&o_doesNotRecognize>("doesNotRecognizeSelector:", "v@::")
     .m<&o_no>("isProxy", "c@:")
     .m<&o_yes>("allowsWeakReference", "c@:")
     .m<&o_yes>("retainWeakReference", "c@:")
     .m<&o_isDeallocating>("_isDeallocating", "c@:")
     .m<&o_yes>("_tryRetain", "c@:")
     .m<&o_nil1>("forwardingTargetForSelector:", "@@::");
    for (const char* s : {"performSelector:", "performSelector:withObject:", "performSelector:withObject:withObject:"}) {
        o.method(s, &o_performSelector, "@@::@@");
        o.classMethod(s, &o_performSelector, "@@::@@");
    }
    r.rootRetainImp = r.lookup(o.cls(), r.selRetain);
    r.rootReleaseImp = r.lookup(o.cls(), r.selRelease);
    r.rootAutoreleaseImp = r.lookup(o.cls(), r.selAutorelease);
    r.makeImmortal(o.cls());

    ClassBuilder proto = r.defineClass("Protocol", "NSObject");
    r.protocolClass = proto.cls();
}

} // namespace Hydrogen::Runtime::ObjC
