// HydrogenVM — NSMethodSignature, NSInvocation and message forwarding.
//
// Argument marshalling follows 32-bit iOS (APCS): every argument occupies
// whole 32-bit words in order — r0..r3, then the stack — with no alignment
// padding for 64-bit values; floats and doubles travel in core registers
// (softfp). Structs larger than 4 bytes are returned through a hidden pointer
// (objc_msgSend_stret).
//
// Forwarding: when a message isn't understood and the receiver's class
// overrides -forwardInvocation:, the runtime asks -methodSignatureForSelector:
// and, given a signature, builds an NSInvocation from the caller's registers
// and stack, calls -forwardInvocation:, and returns its return value.
#include "Foundation.hpp"
#include <cstring>

namespace Hydrogen::Runtime::Foundation {
namespace {

using ObjC::Class;
using ObjC::SEL;
ObjC::Runtime& R(Ctx& x) { return ObjC::rt(x); }

// ---- type encodings
struct TypeInfo { std::string enc; uint32_t size = 0, align = 1; };

const char* skipQualifiers(const char* p) { while (*p && std::strchr("rnNoORV", *p)) ++p; return p; }
const char* skipDigits(const char* p) { if (*p == '-') ++p; while (*p >= '0' && *p <= '9') ++p; return p; }

// Parses one type at p; returns the position after it (offset digits not included).
const char* parseType(const char* p, TypeInfo& t);
const char* parseAggregate(const char* p, char open, char close, TypeInfo& t) {
    const char* start = p;
    ++p;
    while (*p && *p != '=' && *p != close) ++p;   // name
    uint32_t size = 0, align = 1, maxSize = 0;
    if (*p == '=') {
        ++p;
        while (*p && *p != close) {
            if (*p == '"') { ++p; while (*p && *p != '"') ++p; if (*p) ++p; continue; }   // field name
            TypeInfo f;
            p = parseType(p, f);
            align = std::max(align, f.align);
            if (open == '(') maxSize = std::max(maxSize, f.size);
            else { size = (size + f.align - 1) / f.align * f.align; size += f.size; }
        }
    }
    if (*p == close) ++p;
    if (open == '(') size = maxSize;
    size = (size + align - 1) / align * align;
    t.enc.assign(start, p);
    t.size = size;
    t.align = align;
    return p;
}
const char* parseType(const char* p, TypeInfo& t) {
    p = skipQualifiers(p);
    const char* start = p;
    switch (*p) {
        case 'c': case 'C': case 'B': t.size = 1; t.align = 1; ++p; break;
        case 's': case 'S': t.size = 2; t.align = 2; ++p; break;
        case 'i': case 'I': case 'l': case 'L': case 'f': case '*': case '#': case ':': t.size = 4; t.align = 4; ++p; break;
        case 'q': case 'Q': case 'd': t.size = 8; t.align = 4; ++p; break;   // 64-bit types are 4-byte aligned on iOS armv7
        case 'v': t.size = 0; t.align = 1; ++p; break;
        case '@':
            ++p;
            if (*p == '?') ++p;                                   // block
            else if (*p == '"') { ++p; while (*p && *p != '"') ++p; if (*p) ++p; }   // @"ClassName"
            t.size = 4; t.align = 4;
            break;
        case '^': { ++p; TypeInfo inner; p = parseType(p, inner); t.size = 4; t.align = 4; break; }
        case '{': return parseAggregate(p, '{', '}', t);
        case '(': return parseAggregate(p, '(', ')', t);
        case '[': {
            ++p;
            const uint32_t n = uint32_t(std::strtoul(p, nullptr, 10));
            p = skipDigits(p);
            TypeInfo e;
            p = parseType(p, e);
            if (*p == ']') ++p;
            t.size = n * e.size; t.align = e.align;
            t.enc.assign(start, p);
            return p;
        }
        case 'b': ++p; p = skipDigits(p); t.size = 4; t.align = 4; break;   // bitfield (approximation)
        case '?': t.size = 4; t.align = 4; ++p; break;
        default: t.size = 4; t.align = 4; if (*p) ++p; break;
    }
    t.enc.assign(start, p);
    return p;
}
bool parseSignature(const std::string& types, TypeInfo& ret, std::vector<TypeInfo>& args) {
    const char* p = types.c_str();
    if (!*p) return false;
    p = skipDigits(parseType(p, ret));
    while (*p) {
        TypeInfo a;
        const char* q = parseType(p, a);
        if (q == p) break;
        p = skipDigits(q);
        args.push_back(a);
    }
    return args.size() >= 2;
}
uint32_t words(const TypeInfo& t) { return std::max<uint32_t>(1, (t.size + 3) / 4); }
bool isObject(const TypeInfo& t) { return !t.enc.empty() && t.enc[0] == '@'; }
bool isStruct(const TypeInfo& t) { return !t.enc.empty() && (t.enc[0] == '{' || t.enc[0] == '('); }
bool returnsViaMemory(const TypeInfo& t) { return isStruct(t) && t.size > 4; }

// ---- NSMethodSignature
struct Signature : ObjC::HostObject {
    std::string types;
    TypeInfo ret;
    std::vector<TypeInfo> args;
    std::vector<uint32_t> cstrings;   // guest copies of each type, for getArgumentTypeAtIndex:
    uint32_t retCString = 0;
};
struct InvState {
    explicit InvState(Process&) {}
    Class signature = 0, invocation = 0;
    IMP forwarder = 0;
    uint32_t msgSend = 0, msgSendStret = 0;
};
InvState& IS(Ctx& x) { return x.proc.service<InvState>(); }
Signature* sig(Ctx& x, id s) { return s ? R(x).hostIf<Signature>(s) : nullptr; }

id makeSignature(Ctx& x, const std::string& types) {
    auto h = std::make_unique<Signature>();
    h->types = types;
    if (!parseSignature(types, h->ret, h->args)) return 0;
    const id o = R(x).createInstance(IS(x).signature);
    for (auto& a : h->args) h->cstrings.push_back(x.proc.allocCString(a.enc));
    h->retCString = x.proc.allocCString(h->ret.enc);
    R(x).setHost(o, std::move(h));
    return autoreleased(x, o);
}
id sigc_withTypes(Ctx& x, id, SEL, GuestPtr types) { return types ? makeSignature(x, x.proc.readCString(types)) : 0; }
uint32_t sig_count(Ctx& x, id self, SEL) { Signature* s = sig(x, self); return s ? uint32_t(s->args.size()) : 0; }
uint32_t sig_argType(Ctx& x, id self, SEL, uint32_t i) {
    Signature* s = sig(x, self);
    if (!s || i >= s->args.size()) throw std::runtime_error("*** -[NSMethodSignature getArgumentTypeAtIndex:]: index out of bounds");
    return s->cstrings[i];
}
uint32_t sig_retType(Ctx& x, id self, SEL) { Signature* s = sig(x, self); return s ? s->retCString : 0; }
uint32_t sig_retLength(Ctx& x, id self, SEL) { Signature* s = sig(x, self); return s ? s->ret.size : 0; }
uint32_t sig_frameLength(Ctx& x, id self, SEL) {
    Signature* s = sig(x, self);
    uint32_t n = 0;
    if (s) for (auto& a : s->args) n += words(a) * 4;
    return n;
}
bool sig_isOneway(Ctx& x, id self, SEL) { Signature* s = sig(x, self); return s && !s->types.empty() && s->types[0] == 'V'; }
bool sig_isEqual(Ctx& x, id self, SEL, id other) {
    Signature* a = sig(x, self);
    Signature* b = sig(x, other);
    return a && b && a->ret.enc == b->ret.enc && [&] {
        if (a->args.size() != b->args.size()) return false;
        for (size_t i = 0; i < a->args.size(); ++i) if (a->args[i].enc != b->args[i].enc) return false;
        return true;
    }();
}
id sig_typeString(Ctx& x, id self, SEL) { Signature* s = sig(x, self); return makeString(x, s ? s->types : ""); }

// NSObject: -methodSignatureForSelector:, +instanceMethodSignatureForSelector:
id o_methodSignature(Ctx& x, id self, SEL, SEL s) {
    if (!self || !s) return 0;
    std::string t = R(x).methodTypes(R(x).classOf(self), s);
    if (t.empty()) {
        const SEL ftSel = R(x).selForwardingTarget;
        const IMP ftImp = R(x).lookup(R(x).classOf(self), ftSel);
        if (ftImp) {
            const id target = id(x.proc.callGuest(x.cpu, ftImp, {self, ftSel, s}));
            if (target && target != self) {
                const SEL msigSel = R(x).sel("methodSignatureForSelector:");
                const IMP msigImp = R(x).lookup(R(x).classOf(target), msigSel);
                if (msigImp) {
                    return id(x.proc.callGuest(x.cpu, msigImp, {target, msigSel, s}));
                }
            }
        }
    }
    return t.empty() ? 0 : makeSignature(x, t);
}
id oc_instanceMethodSignature(Ctx& x, id cls, SEL, SEL s) {
    const std::string t = R(x).methodTypes(cls, s);
    return t.empty() ? 0 : makeSignature(x, t);
}

// ---- NSInvocation
struct Invocation : ObjC::HostObject {
    id signature = 0;
    std::vector<std::vector<uint8_t>> args;
    std::vector<uint8_t> ret;
    bool retained = false;
    std::vector<id> retainedObjects;
};
Invocation* inv(Ctx& x, id i) { return i ? R(x).hostIf<Invocation>(i) : nullptr; }

id newInvocation(Ctx& x, id signature) {
    Signature* s = sig(x, signature);
    if (!s) throw std::runtime_error("+[NSInvocation invocationWithMethodSignature:]: method signature argument cannot be nil");
    const id o = R(x).createInstance(IS(x).invocation);
    auto h = std::make_unique<Invocation>();
    h->signature = R(x).retain(x.cpu, signature);
    for (auto& a : s->args) h->args.emplace_back(words(a) * 4, 0);
    h->ret.assign(std::max<uint32_t>(s->ret.size, 8), 0);
    R(x).setHost(o, std::move(h));
    return autoreleased(x, o);
}
id invc_with(Ctx& x, id, SEL, id signature) { return newInvocation(x, signature); }
id inv_signature(Ctx& x, id self, SEL) { Invocation* i = inv(x, self); return i ? i->signature : 0; }
void checkIndex(Invocation* i, uint32_t idx, const char* what) {
    if (!i || idx >= i->args.size()) throw std::runtime_error(std::string("*** -[NSInvocation ") + what + "]: index (" + std::to_string(idx) + ") out of bounds");
}
void retainArg(Ctx& x, Invocation* i, Signature* s, uint32_t idx) {
    if (!i->retained || !isObject(s->args[idx])) return;
    uint32_t v;
    std::memcpy(&v, i->args[idx].data(), 4);
    if (v) i->retainedObjects.push_back(R(x).retain(x.cpu, v));
}
void inv_setArgument(Ctx& x, id self, SEL, GuestPtr buf, uint32_t idx) {
    Invocation* i = inv(x, self);
    checkIndex(i, idx, "setArgument:atIndex:");
    Signature* s = sig(x, i->signature);
    const uint32_t n = std::max<uint32_t>(s->args[idx].size, 1);
    std::fill(i->args[idx].begin(), i->args[idx].end(), 0);
    std::memcpy(i->args[idx].data(), x.mem.host(buf), std::min<size_t>(n, i->args[idx].size()));
    retainArg(x, i, s, idx);
}
void inv_getArgument(Ctx& x, id self, SEL, GuestPtr buf, uint32_t idx) {
    Invocation* i = inv(x, self);
    checkIndex(i, idx, "getArgument:atIndex:");
    Signature* s = sig(x, i->signature);
    x.mem.copyIn(buf, i->args[idx].data(), std::max<uint32_t>(s->args[idx].size, 1));
}
id inv_target(Ctx& x, id self, SEL) { Invocation* i = inv(x, self); uint32_t v = 0; if (i && !i->args.empty()) std::memcpy(&v, i->args[0].data(), 4); return v; }
void inv_setTarget(Ctx& x, id self, SEL, id t) {
    Invocation* i = inv(x, self);
    if (!i || i->args.empty()) return;
    std::memcpy(i->args[0].data(), &t, 4);
    retainArg(x, i, sig(x, i->signature), 0);
}
SEL inv_selector(Ctx& x, id self, SEL) { Invocation* i = inv(x, self); uint32_t v = 0; if (i && i->args.size() > 1) std::memcpy(&v, i->args[1].data(), 4); return v; }
void inv_setSelector(Ctx& x, id self, SEL, SEL s) { Invocation* i = inv(x, self); if (i && i->args.size() > 1) std::memcpy(i->args[1].data(), &s, 4); }
void inv_getReturn(Ctx& x, id self, SEL, GuestPtr buf) {
    Invocation* i = inv(x, self);
    Signature* s = i ? sig(x, i->signature) : nullptr;
    if (s && s->ret.size) x.mem.copyIn(buf, i->ret.data(), s->ret.size);
}
void inv_setReturn(Ctx& x, id self, SEL, GuestPtr buf) {
    Invocation* i = inv(x, self);
    Signature* s = i ? sig(x, i->signature) : nullptr;
    if (s && s->ret.size) std::memcpy(i->ret.data(), x.mem.host(buf), s->ret.size);
}
void inv_retainArguments(Ctx& x, id self, SEL) {
    Invocation* i = inv(x, self);
    if (!i || i->retained) return;
    i->retained = true;
    Signature* s = sig(x, i->signature);
    for (uint32_t k = 0; k < i->args.size(); ++k) if (k != 1) retainArg(x, i, s, k);
}
bool inv_argumentsRetained(Ctx& x, id self, SEL) { Invocation* i = inv(x, self); return i && i->retained; }

void invokeWith(Ctx& x, id self, id target) {
    Invocation* i = inv(x, self);
    if (!i) return;
    Signature* s = sig(x, i->signature);
    if (target) std::memcpy(i->args[0].data(), &target, 4);
    uint32_t t;
    std::memcpy(&t, i->args[0].data(), 4);
    if (!t) { std::fill(i->ret.begin(), i->ret.end(), 0); return; }
    const bool stret = returnsViaMemory(s->ret);
    std::vector<uint32_t> w;
    uint32_t retBuf = 0;
    if (stret) { retBuf = x.proc.heap().allocZeroed(s->ret.size); w.push_back(retBuf); }
    for (auto& a : i->args)
        for (size_t k = 0; k < a.size(); k += 4) { uint32_t v; std::memcpy(&v, a.data() + k, 4); w.push_back(v); }
    const uint64_t r = x.proc.callGuest(x.cpu, stret ? IS(x).msgSendStret : IS(x).msgSend, w);
    if (stret) {
        std::memcpy(i->ret.data(), x.mem.host(retBuf), s->ret.size);
        x.proc.heap().free(retBuf);
    } else {
        std::memcpy(i->ret.data(), &r, std::min<size_t>(8, i->ret.size()));
        // Narrow return types come back in r0; sign/zero extension doesn't matter for the stored bytes.
    }
}
void inv_invoke(Ctx& x, id self, SEL) { invokeWith(x, self, 0); }
void inv_invokeWithTarget(Ctx& x, id self, SEL, id target) { invokeWith(x, self, target); }
void inv_dealloc(Ctx& x, id self, SEL) {
    if (Invocation* i = inv(x, self)) {
        for (id o : i->retainedObjects) R(x).release(x.cpu, o);
        if (i->signature) R(x).release(x.cpu, i->signature);
    }
    R(x).disposeInstance(x.cpu, self);
}

// ---- forwarding
thread_local id t_forwardSignature = 0;

IMP forwardHook(Process& p, id self, SEL s) {
    Ctx x{p, p.currentThread().cpu, p.mem()};
    ObjC::Runtime& r = R(x);
    const Class cls = r.classOf(self);
    const SEL fwd = r.sel("forwardInvocation:");
    const IMP fimp = r.lookup(cls, fwd);
    if (!fimp || p.engine().isHleAddress(fimp)) return 0;   // NSObject's default: doesNotRecognizeSelector:
    const SEL msig = r.sel("methodSignatureForSelector:");
    if (!r.lookup(cls, msig)) return 0;
    const id signature = id(r.send(x.cpu, self, msig, {s}));
    if (!signature || !sig(x, signature)) return 0;
    t_forwardSignature = r.retain(x.cpu, signature);
    return IS(x).forwarder;
}
// Runs in place of the missing method, with the caller's registers.
void forwarder(Ctx& x) {
    const id signature = t_forwardSignature;
    t_forwardSignature = 0;
    Signature* s = sig(x, signature);
    if (!s) { x.cpu.r[0] = x.cpu.r[1] = 0; return; }
    const bool stret = returnsViaMemory(s->ret);
    const uint32_t retBuf = stret ? x.cpu.r[0] : 0;
    // The caller's arguments, word by word.
    uint32_t wi = stret ? 1 : 0;
    auto word = [&]() {
        const uint32_t v = wi < 4 ? x.cpu.r[wi] : x.mem.read<uint32_t>(x.cpu.r[CPU::kSP] + (wi - 4) * 4);
        ++wi;
        return v;
    };
    const id invocation = newInvocation(x, signature);
    Invocation* i = inv(x, invocation);
    for (auto& a : i->args)
        for (size_t k = 0; k < a.size(); k += 4) { const uint32_t v = word(); std::memcpy(a.data() + k, &v, 4); }
    const id self = inv_target(x, invocation, 0);
    R(x).send(x.cpu, self, "forwardInvocation:", {invocation});
    if (stret) x.mem.copyIn(retBuf, i->ret.data(), s->ret.size);
    else {
        uint32_t lo = 0, hi = 0;
        std::memcpy(&lo, i->ret.data(), 4);
        std::memcpy(&hi, i->ret.data() + 4, 4);
        x.cpu.r[0] = lo;
        x.cpu.r[1] = hi;
    }
    R(x).release(x.cpu, signature);
}

} // namespace

void registerInvocation(Process& p, ObjC::Runtime& r) {
    InvState& s = p.service<InvState>();
    ObjC::ClassBuilder ms = r.defineClass("NSMethodSignature", "NSObject");
    s.signature = ms.cls();
    ms.cm<&sigc_withTypes>("signatureWithObjCTypes:", "@@:r*")
      .m<&sig_count>("numberOfArguments", "I@:").m<&sig_argType>("getArgumentTypeAtIndex:", "r*@:I")
      .m<&sig_retType>("methodReturnType", "r*@:").m<&sig_retLength>("methodReturnLength", "I@:")
      .m<&sig_frameLength>("frameLength", "I@:").m<&sig_isOneway>("isOneway", "c@:").m<&sig_isEqual>("isEqual:", "c@:@")
      .m<&sig_typeString>("_typeString");
    ObjC::ClassBuilder iv = r.defineClass("NSInvocation", "NSObject");
    s.invocation = iv.cls();
    iv.cm<&invc_with>("invocationWithMethodSignature:")
      .m<&inv_signature>("methodSignature").m<&inv_setArgument>("setArgument:atIndex:", "v@:^vi")
      .m<&inv_getArgument>("getArgument:atIndex:", "v@:^vi").m<&inv_target>("target").m<&inv_setTarget>("setTarget:", "v@:@")
      .m<&inv_selector>("selector", ":@:").m<&inv_setSelector>("setSelector:", "v@::")
      .m<&inv_getReturn>("getReturnValue:", "v@:^v").m<&inv_setReturn>("setReturnValue:", "v@:^v")
      .m<&inv_retainArguments>("retainArguments", "v@:").m<&inv_argumentsRetained>("argumentsRetained", "c@:")
      .m<&inv_invoke>("invoke", "v@:").m<&inv_invokeWithTarget>("invokeWithTarget:", "v@:@")
      .m<&inv_dealloc>("dealloc", "v@:");
    r.builder(r.getClass("NSObject"))
        .m<&o_methodSignature>("methodSignatureForSelector:", "@@::")
        .cm<&o_methodSignature>("methodSignatureForSelector:", "@@::")
        .cm<&oc_instanceMethodSignature>("instanceMethodSignatureForSelector:", "@@::");
    s.forwarder = p.registerHostFunction("hvm_forward_invocation", forwarder);
    s.msgSend = *p.lookupSymbol("_objc_msgSend");
    s.msgSendStret = *p.lookupSymbol("_objc_msgSend_stret");
    Process* pp = &p;
    r.forwardHook = [pp](id self, SEL sel) { return forwardHook(*pp, self, sel); };
}

} // namespace Hydrogen::Runtime::Foundation
