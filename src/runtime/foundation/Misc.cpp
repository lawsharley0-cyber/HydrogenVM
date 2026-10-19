// HydrogenVM — assorted Foundation: NSLog, NSException, NSError, NSNull,
// NSAutoreleasePool, NSCharacterSet, KVC on NSObject, NSProcessInfo, locks,
// NSIndexPath / NSIndexSet, NSUUID, NSLocale / NSTimeZone basics and the
// NSString/NSClass/NSSelector conversion functions.
#include "Foundation.hpp"
#include "runtime/libc/Format.hpp"
#include <chrono>
#include <random>
#include <thread>
#include <unistd.h>

namespace Hydrogen::Runtime::Foundation {

namespace {

using U16 = std::u16string;

struct MiscState {
    explicit MiscState(Process&) {}
    Class nsException = 0, nsError = 0, nsNull = 0, pool = 0, charSet = 0, mutableCharSet = 0, processInfo = 0,
          nsLock = 0, nsRecursiveLock = 0, nsCondition = 0, nsConditionLock = 0, indexPath = 0, indexSet = 0,
          mutableIndexSet = 0, uuid = 0, locale = 0, timeZone = 0, assertionHandler = 0;
    id nullObj = 0, processInfoObj = 0, assertionHandlerObj = 0, currentLocale = 0, localTZ = 0;
    std::mutex mu;
    std::unordered_map<int, id> standardSets;
    uint32_t uncaughtHandler = 0;
};
MiscState& M(Process& p) { return p.service<MiscState>(); }
MiscState& M(Ctx& x) { return M(x.proc); }

void obj_dealloc(Ctx& x, id self, SEL) { rt(x).disposeInstance(x.cpu, self); }
id retainedCopy(Ctx& x, id self, SEL, uint32_t) { return rt(x).retain(x.cpu, self); }
std::string fmt(Ctx& x, id f, ArgReader& a) { return formatGuest(x.proc, utf8(x, f), a, objectFormatHook(x.proc)); }

// ------------------------------------------------------------------ NSLog ---

void logLine(Ctx& x, std::string s) {
    if (s.empty() || s.back() != '\n') s += '\n';
    x.proc.log(LogLevel::GuestStderr, s);
}
void ns_log(Ctx& x, id f, VarArgs va) { logLine(x, fmt(x, f, va.args)); }
void ns_logv(Ctx& x, id f, VaList ap) { ArgReader a(x.mem, ap.ptr); logLine(x, fmt(x, f, a)); }

// ------------------------------------------------------------- NSObject ---

id o_description(Ctx& x, id self, SEL) {
    char buf[160];
    std::snprintf(buf, sizeof buf, "<%s: 0x%x>", rt(x).objectClassName(self).c_str(), self);
    return makeString(x, std::string(buf));
}
id oc_description(Ctx& x, id self, SEL) { return makeString(x, rt(x).className(self)); }

// Return type code of a method (first char of its type encoding, skipping qualifiers).
char returnType(Ctx& x, Class cls, SEL s, std::string* full = nullptr) {
    for (Class c = cls; c; c = rt(x).superclass(c)) {
        ObjC::ClassInfo* ci = rt(x).info(c);
        if (!ci) continue;
        auto it = ci->types.find(s);
        if (it == ci->types.end()) continue;
        const std::string& t = it->second;
        if (full) *full = t;
        for (char ch : t) if (!std::strchr("rnNoORV", ch)) return ch;
        return '@';
    }
    return '@';
}
// Type code of the first explicit argument.
char firstArgType(const std::string& t) {
    // skip return type and offsets: crude parse of "v12@0:4@8" or "v@:@"
    int seen = 0;
    for (size_t i = 0; i < t.size(); ++i) {
        const char ch = t[i];
        if (std::strchr("rnNoORV0123456789", ch)) continue;
        if (ch == '{') { int d = 0; for (; i < t.size(); ++i) { if (t[i] == '{') ++d; else if (t[i] == '}' && --d == 0) break; } if (seen++ == 3) return '{'; continue; }
        if (seen++ == 3) return ch;
        if (ch == '^') ++i;
    }
    return '@';
}

id boxScalar(Ctx& x, char type, uint64_t v) {
    switch (type) {
        case '@': case '#': return id(v);
        case 'c': case 'B': {
            // BOOL is 'c' on 32-bit iOS; box as a number holding 0/1
            return makeNumberInt(x, int8_t(v));
        }
        case 'C': return makeNumberInt(x, uint8_t(v));
        case 's': return makeNumberInt(x, int16_t(v));
        case 'S': return makeNumberInt(x, uint16_t(v));
        case 'i': case 'l': return makeNumberInt(x, int32_t(v));
        case 'I': case 'L': return makeNumberInt(x, uint32_t(v));
        case 'q': case 'Q': return makeNumberInt(x, int64_t(v));
        case 'f': { float f; const uint32_t b = uint32_t(v); std::memcpy(&f, &b, 4); return makeNumberDouble(x, f); }
        case 'd': { double d; std::memcpy(&d, &v, 8); return makeNumberDouble(x, d); }
        default: return id(v);
    }
}

std::string capitalized(const std::string& k) { std::string s = k; if (!s.empty()) s[0] = char(std::toupper(uint8_t(s[0]))); return s; }

id kvcGet(Ctx& x, id self, const std::string& key) {
    if (!self) return 0;
    auto& r = rt(x);
    if (isDict(x, self)) return dictGet(x, self, makeString(x, key));
    if (isArray(x, self)) {
        std::vector<id> out;
        for (id e : arrayItems(x, self)) { const id v = kvcGet(x, e, key); out.push_back(v ? v : M(x).nullObj); }
        return makeArray(x, out);
    }
    const Class cls = r.classOf(self);
    for (const std::string& name : {key, "get" + capitalized(key), "is" + capitalized(key), "_" + key}) {
        const SEL s = r.sel(name);
        if (r.lookup(cls, s)) {
            const char t = returnType(x, cls, s);
            if (t == '{') { x.proc.reportMissing("valueForKey: struct-valued key " + key); return 0; }
            return boxScalar(x, t, r.send(x.cpu, self, s));
        }
    }
    for (const std::string& name : {"_" + key, "_is" + capitalized(key), key, "is" + capitalized(key)}) {
        if (const ObjC::Ivar* iv = r.findIvar(cls, name)) {
            const uint32_t addr = self + iv->offset;
            const char t = iv->type.empty() ? '@' : iv->type[0];
            if (t == 'd' || t == 'q' || t == 'Q') return boxScalar(x, t, x.mem.read<uint64_t>(addr));
            if (t == 'c' || t == 'C' || t == 'B') return boxScalar(x, t, x.mem.read<uint8_t>(addr));
            if (t == 's' || t == 'S') return boxScalar(x, t, x.mem.read<uint16_t>(addr));
            return boxScalar(x, t, x.mem.read<uint32_t>(addr));
        }
    }
    return id(r.send(x.cpu, self, "valueForUndefinedKey:", {makeString(x, key)}));
}

void kvcSet(Ctx& x, id self, const std::string& key, id value) {
    if (!self) return;
    auto& r = rt(x);
    if (isDict(x, self)) {
        if (value) r.send(x.cpu, self, "setObject:forKey:", {value, makeString(x, key)});
        else r.send(x.cpu, self, "removeObjectForKey:", {makeString(x, key)});
        return;
    }
    const Class cls = r.classOf(self);
    for (const std::string& name : {"set" + capitalized(key) + ":", "_set" + capitalized(key) + ":"}) {
        const SEL s = r.sel(name);
        if (!r.lookup(cls, s)) continue;
        std::string types;
        returnType(x, cls, s, &types);
        const char t = types.empty() ? '@' : firstArgType(types);
        switch (t) {
            case '@': case '#': r.send(x.cpu, self, s, {value}); return;
            case 'f': { const float f = float(numberDouble(x, value)); uint32_t b; std::memcpy(&b, &f, 4); r.send(x.cpu, self, s, {b}); return; }
            case 'd': { const double d = numberDouble(x, value); uint64_t b; std::memcpy(&b, &d, 8); r.send(x.cpu, self, s, {uint32_t(b), uint32_t(b >> 32)}); return; }
            case 'q': case 'Q': { const uint64_t v = uint64_t(numberInt(x, value)); r.send(x.cpu, self, s, {uint32_t(v), uint32_t(v >> 32)}); return; }
            case '{': x.proc.reportMissing("setValue:forKey: struct-valued key " + key); return;
            default: r.send(x.cpu, self, s, {uint32_t(numberInt(x, value))}); return;
        }
    }
    for (const std::string& name : {"_" + key, "_is" + capitalized(key), key, "is" + capitalized(key)}) {
        const ObjC::Ivar* iv = r.findIvar(cls, name);
        if (!iv) continue;
        const uint32_t addr = self + iv->offset;
        const char t = iv->type.empty() ? '@' : iv->type[0];
        switch (t) {
            case '@': { const id old = x.mem.read<uint32_t>(addr); x.mem.write<uint32_t>(addr, r.retain(x.cpu, value)); r.release(x.cpu, old); return; }
            case 'c': case 'C': case 'B': x.mem.write<uint8_t>(addr, uint8_t(numberInt(x, value))); return;
            case 's': case 'S': x.mem.write<uint16_t>(addr, uint16_t(numberInt(x, value))); return;
            case 'f': x.mem.write<float>(addr, float(numberDouble(x, value))); return;
            case 'd': x.mem.write<double>(addr, numberDouble(x, value)); return;
            case 'q': case 'Q': x.mem.write<uint64_t>(addr, uint64_t(numberInt(x, value))); return;
            default: x.mem.write<uint32_t>(addr, uint32_t(numberInt(x, value))); return;
        }
    }
    r.send(x.cpu, self, "setValue:forUndefinedKey:", {value, makeString(x, key)});
}

id o_valueForKey(Ctx& x, id self, SEL, id key) { return kvcGet(x, self, utf8(x, key)); }
void o_setValueForKey(Ctx& x, id self, SEL, id value, id key) { kvcSet(x, self, utf8(x, key), value); }
id o_valueForKeyPath(Ctx& x, id self, SEL, id path) {
    const std::string p = utf8(x, path);
    id cur = self;
    size_t start = 0;
    while (cur) {
        const size_t dot = p.find('.', start);
        const std::string k = p.substr(start, dot == std::string::npos ? std::string::npos : dot - start);
        if (!k.empty() && k[0] == '@') {   // collection operators
            const std::vector<id> items = arrayItems(x, cur);
            const std::string rest = dot == std::string::npos ? "" : p.substr(dot + 1);
            if (k == "@count") return makeNumberInt(x, int64_t(items.size()));
            double sum = 0, mn = 1e300, mx = -1e300;
            for (id e : items) { const double v = numberDouble(x, rest.empty() ? e : kvcGet(x, e, rest)); sum += v; mn = std::min(mn, v); mx = std::max(mx, v); }
            if (k == "@sum") return makeNumberDouble(x, sum);
            if (k == "@avg") return makeNumberDouble(x, items.empty() ? 0 : sum / double(items.size()));
            if (k == "@min") return items.empty() ? 0 : makeNumberDouble(x, mn);
            if (k == "@max") return items.empty() ? 0 : makeNumberDouble(x, mx);
            x.proc.reportMissing("KVC collection operator " + k);
            return 0;
        }
        cur = kvcGet(x, cur, k);
        if (dot == std::string::npos) return cur;
        start = dot + 1;
    }
    return 0;
}
void o_setValueForKeyPath(Ctx& x, id self, SEL, id value, id path) {
    const std::string p = utf8(x, path);
    const size_t dot = p.rfind('.');
    if (dot == std::string::npos) { kvcSet(x, self, p, value); return; }
    const id target = o_valueForKeyPath(x, self, 0, makeString(x, p.substr(0, dot)));
    kvcSet(x, target, p.substr(dot + 1), value);
}
void o_setValuesForKeys(Ctx& x, id self, SEL, id dict) {
    for (auto& [k, v] : dictEntries(x, dict)) kvcSet(x, self, utf8(x, k), v == M(x).nullObj ? 0 : v);
}
id o_dictionaryWithValuesForKeys(Ctx& x, id self, SEL, id keys) {
    std::vector<std::pair<id, id>> e;
    for (id k : arrayItems(x, keys)) { const id v = kvcGet(x, self, utf8(x, k)); e.emplace_back(k, v ? v : M(x).nullObj); }
    return makeDict(x, e);
}
id o_valueForUndefinedKey(Ctx& x, id self, SEL, id key) {
    throw ObjC::ObjCError("[<" + rt(x).objectClassName(self) + "> valueForUndefinedKey:]: this class is not key value coding-compliant for the key " + utf8(x, key));
}
void o_setValueForUndefinedKey(Ctx& x, id self, SEL, id, id key) {
    throw ObjC::ObjCError("[<" + rt(x).objectClassName(self) + "> setValue:forUndefinedKey:]: this class is not key value coding-compliant for the key " + utf8(x, key));
}
void o_setNilValueForKey(Ctx& x, id self, SEL, id key) { kvcSet(x, self, utf8(x, key), makeNumberInt(x, 0)); }
id o_mutableArrayValueForKey(Ctx& x, id self, SEL s, id key) {
    // Not a live proxy — good enough for read-modify-write patterns that set it back.
    const id v = o_valueForKey(x, self, s, key);
    return autoreleased(x, id(rt(x).send(x.cpu, v ? v : makeArray(x, {}), "mutableCopy")));
}
// KVO: observers are accepted but never notified (no isa-swizzling yet).
void o_kvoNoop2(Ctx&, id, SEL, id, id) {}
void o_kvoAdd(Ctx& x, id, SEL, id, id key, uint32_t, uint32_t) { x.proc.reportMissing("KVO notifications (observing " + utf8(x, key) + ")"); }
void o_kvoRemove3(Ctx&, id, SEL, id, id, uint32_t) {}
void o_kvoChange(Ctx&, id, SEL, id) {}
bool oc_automaticallyNotifies(Ctx&, id, SEL, id) { return true; }
id o_observationInfo(Ctx&, id, SEL) { return 0; }

// ----------------------------------------------------------- NSException ---

struct ExcData : ObjC::HostObject { id name = 0, reason = 0, userInfo = 0; };

std::string exceptionText(Ctx& x, id e) {
    if (!e) return "nil";
    auto& r = rt(x);
    if (r.isKindOf(e, M(x).nsException)) {
        const id n = id(r.send(x.cpu, e, "name")), rs = id(r.send(x.cpu, e, "reason"));
        return "'" + (n ? utf8(x, n) : std::string("NSException")) + "', reason: '" + (rs ? utf8(x, rs) : std::string()) + "'";
    }
    return r.objectClassName(e) + ": " + describe(x, e);
}
id exc_init(Ctx& x, id self, SEL, id name, id reason, id userInfo) {
    auto h = std::make_unique<ExcData>();
    h->name = id(rt(x).send(x.cpu, name, "copy"));
    h->reason = id(rt(x).send(x.cpu, reason, "copy"));
    h->userInfo = rt(x).retain(x.cpu, userInfo);
    rt(x).setHost(self, std::move(h));
    return self;
}
id excc_with(Ctx& x, id self, SEL, id name, id reason, id userInfo) {
    const id e = id(rt(x).send(x.cpu, self, rt(x).selAlloc));
    return autoreleased(x, exc_init(x, e, 0, name, reason, userInfo));
}
[[noreturn]] void throwException(Ctx& x, id e) {
    if (M(x).uncaughtHandler) x.proc.callGuest(x.cpu, M(x).uncaughtHandler, {e});
    throw ObjC::ObjCError("Terminating app due to uncaught exception " + exceptionText(x, e));
}
void excc_raiseFormat(Ctx& x, id self, SEL, id name, id f, VarArgs va) {
    const id reason = makeString(x, fmt(x, f, va.args));
    throwException(x, excc_with(x, self, 0, name, reason, 0));
}
void excc_raiseFormatArgs(Ctx& x, id self, SEL, id name, id f, VaList ap) {
    ArgReader a(x.mem, ap.ptr);
    throwException(x, excc_with(x, self, 0, name, makeString(x, fmt(x, f, a)), 0));
}
void exc_raise(Ctx& x, id self, SEL) { throwException(x, self); }
id exc_name(Ctx& x, id self, SEL) { ExcData* d = rt(x).hostIf<ExcData>(self); return d ? d->name : 0; }
id exc_reason(Ctx& x, id self, SEL) { ExcData* d = rt(x).hostIf<ExcData>(self); return d ? d->reason : 0; }
id exc_userInfo(Ctx& x, id self, SEL) { ExcData* d = rt(x).hostIf<ExcData>(self); return d ? d->userInfo : 0; }
id exc_callStack(Ctx& x, id, SEL) { return makeArray(x, {}); }
id exc_description(Ctx& x, id self, SEL) { const id r = exc_reason(x, self, 0); return r ? r : exc_name(x, self, 0); }
void exc_dealloc(Ctx& x, id self, SEL) {
    if (ExcData* d = rt(x).hostIf<ExcData>(self)) {
        const id a = d->name, b = d->reason, c = d->userInfo;
        d->name = d->reason = d->userInfo = 0;
        rt(x).release(x.cpu, a); rt(x).release(x.cpu, b); rt(x).release(x.cpu, c);
    }
    rt(x).disposeInstance(x.cpu, self);
}
void set_uncaught(Ctx& x, uint32_t fn) { M(x).uncaughtHandler = fn; }
uint32_t get_uncaught(Ctx& x) { return M(x).uncaughtHandler; }

// NSAssertionHandler
id ah_current(Ctx& x, id, SEL) {
    auto& m = M(x);
    if (!m.assertionHandlerObj) { m.assertionHandlerObj = rt(x).createInstance(m.assertionHandler); rt(x).makeImmortal(m.assertionHandlerObj); }
    return m.assertionHandlerObj;
}
void ah_failMethod(Ctx& x, id, SEL, SEL sel, id obj, id file, int32_t line, id f, VarArgs va) {
    const std::string msg = f ? fmt(x, f, va.args) : std::string();
    throw ObjC::ObjCError("Assertion failure in -[" + rt(x).objectClassName(obj) + " " + rt(x).selName(sel) + "], " +
                          utf8(x, file) + ":" + std::to_string(line) + ": " + msg);
}
void ah_failFunction(Ctx& x, id, SEL, id func, id file, int32_t line, id f, VarArgs va) {
    const std::string msg = f ? fmt(x, f, va.args) : std::string();
    throw ObjC::ObjCError("Assertion failure in " + utf8(x, func) + "(), " + utf8(x, file) + ":" + std::to_string(line) + ": " + msg);
}

// --------------------------------------------------------------- NSError ---

struct ErrData : ObjC::HostObject { id domain = 0, userInfo = 0; int32_t code = 0; };
id err_init(Ctx& x, id self, SEL, id domain, int32_t code, id userInfo) {
    auto h = std::make_unique<ErrData>();
    h->domain = id(rt(x).send(x.cpu, domain, "copy"));
    h->code = code;
    h->userInfo = id(rt(x).send(x.cpu, userInfo, "copy"));
    rt(x).setHost(self, std::move(h));
    return self;
}
id errc_with(Ctx& x, id self, SEL, id domain, int32_t code, id userInfo) {
    return autoreleased(x, err_init(x, id(rt(x).send(x.cpu, self, rt(x).selAlloc)), 0, domain, code, userInfo));
}
id err_domain(Ctx& x, id self, SEL) { ErrData* d = rt(x).hostIf<ErrData>(self); return d ? d->domain : 0; }
int32_t err_code(Ctx& x, id self, SEL) { ErrData* d = rt(x).hostIf<ErrData>(self); return d ? d->code : 0; }
id err_userInfo(Ctx& x, id self, SEL) { ErrData* d = rt(x).hostIf<ErrData>(self); return d && d->userInfo ? d->userInfo : makeDict(x, {}); }
id err_localizedDescription(Ctx& x, id self, SEL) {
    ErrData* d = rt(x).hostIf<ErrData>(self);
    if (!d) return makeString(x, "Unknown error");
    if (d->userInfo) if (const id v = dictGet(x, d->userInfo, "NSLocalizedDescription")) return v;
    return makeString(x, "The operation couldn’t be completed. (" + (d->domain ? utf8(x, d->domain) : std::string("?")) + " error " + std::to_string(d->code) + ".)");
}
id err_userInfoKey(Ctx& x, id self, SEL, const char* key) { ErrData* d = rt(x).hostIf<ErrData>(self); return d && d->userInfo ? dictGet(x, d->userInfo, key) : 0; }
id err_failureReason(Ctx& x, id self, SEL s) { return err_userInfoKey(x, self, s, "NSLocalizedFailureReason"); }
id err_recovery(Ctx& x, id self, SEL s) { return err_userInfoKey(x, self, s, "NSLocalizedRecoverySuggestion"); }
id err_description(Ctx& x, id self, SEL) {
    ErrData* d = rt(x).hostIf<ErrData>(self);
    return makeString(x, "Error Domain=" + (d && d->domain ? utf8(x, d->domain) : std::string()) + " Code=" + std::to_string(d ? d->code : 0) +
                             " \"" + utf8(x, err_localizedDescription(x, self, 0)) + "\"");
}
void err_dealloc(Ctx& x, id self, SEL) {
    if (ErrData* d = rt(x).hostIf<ErrData>(self)) { const id a = d->domain, b = d->userInfo; d->domain = d->userInfo = 0; rt(x).release(x.cpu, a); rt(x).release(x.cpu, b); }
    rt(x).disposeInstance(x.cpu, self);
}

// ---------------------------------------------------------------- NSNull ---

id null_null(Ctx& x, id, SEL) { return M(x).nullObj; }
id null_description(Ctx& x, id, SEL) { return makeString(x, "<null>"); }
id null_self(Ctx&, id self, SEL) { return self; }
void null_noop(Ctx&, id, SEL) {}

// ------------------------------------------------------ NSAutoreleasePool ---

struct PoolData : ObjC::HostObject { uint32_t token = 0; bool popped = false; };
id pool_init(Ctx& x, id self, SEL) {
    auto h = std::make_unique<PoolData>();
    h->token = rt(x).poolPush();
    rt(x).setHost(self, std::move(h));
    return self;
}
void pool_drain(Ctx& x, id self, SEL) {
    PoolData* d = rt(x).hostIf<PoolData>(self);
    if (d && !d->popped) { d->popped = true; rt(x).poolPop(x.cpu, d->token); }
    rt(x).disposeInstance(x.cpu, self);
}
id pool_retain(Ctx& x, id, SEL) { throw ObjC::ObjCError("*** -[NSAutoreleasePool retain]: Cannot retain an autorelease pool"); }
void poolc_addObject(Ctx& x, id, SEL, id obj) { rt(x).addToPool(obj); }
void pool_addObject(Ctx& x, id, SEL, id obj) { rt(x).addToPool(obj); }

// ---------------------------------------------------------- NSCharacterSet ---

struct CharSetData : ObjC::HostObject {
    std::vector<uint64_t> bits = std::vector<uint64_t>(1024, 0);   // BMP only
    bool test(char16_t c) const { return (bits[c >> 6] >> (c & 63)) & 1; }
    void set(char16_t c, bool v = true) { if (v) bits[c >> 6] |= uint64_t(1) << (c & 63); else bits[c >> 6] &= ~(uint64_t(1) << (c & 63)); }
    void range(uint32_t a, uint32_t b, bool v = true) { for (uint32_t c = a; c <= b && c < 0x10000; ++c) set(char16_t(c), v); }
};

bool isWhitespace(char16_t c) { return c == ' ' || c == '\t' || c == 0xA0 || c == 0x1680 || (c >= 0x2000 && c <= 0x200A) || c == 0x202F || c == 0x205F || c == 0x3000; }
bool isNewline(char16_t c) { return (c >= 0x0A && c <= 0x0D) || c == 0x85 || c == 0x2028 || c == 0x2029; }
bool isDigit(char16_t c) { return (c >= '0' && c <= '9') || (c >= 0x660 && c <= 0x669) || (c >= 0x6F0 && c <= 0x6F9) || (c >= 0x966 && c <= 0x96F) || (c >= 0xFF10 && c <= 0xFF19); }
bool isUpper(char16_t c) {
    return (c >= 'A' && c <= 'Z') || (c >= 0xC0 && c <= 0xDE && c != 0xD7) || (c >= 0x391 && c <= 0x3AB) || (c >= 0x400 && c <= 0x42F) ||
           (c >= 0x100 && c <= 0x17F && !(c & 1)) || (c >= 0xFF21 && c <= 0xFF3A);
}
bool isLower(char16_t c) {
    return (c >= 'a' && c <= 'z') || (c >= 0xDF && c <= 0xFF && c != 0xF7) || (c >= 0x3B1 && c <= 0x3CB) || (c >= 0x430 && c <= 0x45F) ||
           (c >= 0x100 && c <= 0x17F && (c & 1)) || (c >= 0xFF41 && c <= 0xFF5A);
}
bool isLetter(char16_t c) {
    return isUpper(c) || isLower(c) || (c >= 0x180 && c <= 0x24F) || (c >= 0x370 && c <= 0x3FF && c != 0x37E && c != 0x387) ||
           (c >= 0x400 && c <= 0x52F) || (c >= 0x5D0 && c <= 0x5EA) || (c >= 0x620 && c <= 0x64A) || (c >= 0x904 && c <= 0x939) ||
           (c >= 0xE01 && c <= 0xE30) || (c >= 0x1100 && c <= 0x11FF) || (c >= 0x3041 && c <= 0x3096) || (c >= 0x30A1 && c <= 0x30FA) ||
           (c >= 0x3400 && c <= 0x4DBF) || (c >= 0x4E00 && c <= 0x9FFF) || (c >= 0xAC00 && c <= 0xD7A3) || (c >= 0xF900 && c <= 0xFAFF) ||
           c == 0xAA || c == 0xB5 || c == 0xBA;
}
bool isPunct(char16_t c) {
    return (c < 0x80 && std::strchr("!\"#%&'()*,-./:;?@[\\]_{}", char(c)) && c) || c == 0xA1 || c == 0xA7 || c == 0xAB || c == 0xB6 || c == 0xB7 ||
           c == 0xBB || c == 0xBF || (c >= 0x2010 && c <= 0x2027) || (c >= 0x2030 && c <= 0x205E) || (c >= 0x3001 && c <= 0x3003) ||
           (c >= 0x3008 && c <= 0x3011) || (c >= 0xFF01 && c <= 0xFF0F && !std::strchr("$+<=>", char(c - 0xFF00 + 0x20)));
}
bool isSymbol(char16_t c) {
    return (c < 0x80 && c && std::strchr("$+<=>^`|~", char(c))) || c == 0xA2 || c == 0xA3 || c == 0xA4 || c == 0xA5 || c == 0xA6 || c == 0xA8 ||
           c == 0xA9 || c == 0xAC || c == 0xAE || c == 0xAF || c == 0xB0 || c == 0xB1 || c == 0xB4 || c == 0xB8 || c == 0xD7 || c == 0xF7 ||
           (c >= 0x20A0 && c <= 0x20CF) || (c >= 0x2100 && c <= 0x214F) || (c >= 0x2190 && c <= 0x23FF) || (c >= 0x2500 && c <= 0x27BF);
}
bool isControl(char16_t c) { return c < 0x20 || (c >= 0x7F && c <= 0x9F); }
bool isNonBase(char16_t c) { return (c >= 0x300 && c <= 0x36F) || (c >= 0x483 && c <= 0x489) || (c >= 0x591 && c <= 0x5BD) || (c >= 0x20D0 && c <= 0x20FF) || (c >= 0xFE20 && c <= 0xFE2F); }
bool isAlnum(char16_t c) { return isLetter(c) || isDigit(c) || isNonBase(c); }
bool urlUnreserved(char16_t c) { return c < 0x80 && (std::isalnum(c) || std::strchr("-._~", char(c))); }

enum StdSet {
    kWhitespace, kWhitespaceNewline, kNewline, kDigits, kLetters, kLower, kUpper, kAlnum, kPunct, kSymbols, kControl,
    kNonBase, kDecomposable, kIllegal, kCapitalized, kURLQuery, kURLPath, kURLHost, kURLFragment, kURLUser, kURLPassword,
};

id newCharSet(Ctx& x, bool isMutable) {
    const id o = rt(x).createInstance(isMutable ? M(x).mutableCharSet : M(x).charSet);
    rt(x).setHost(o, std::make_unique<CharSetData>());
    return o;
}
CharSetData* csd(Ctx& x, id s) { return s ? rt(x).hostIf<CharSetData>(s) : nullptr; }

id standardSet(Ctx& x, int which) {
    auto& m = M(x);
    {
        std::lock_guard<std::mutex> lk(m.mu);
        auto it = m.standardSets.find(which);
        if (it != m.standardSets.end()) return it->second;
    }
    std::function<bool(char16_t)> pred;
    switch (which) {
        case kWhitespace: pred = isWhitespace; break;
        case kWhitespaceNewline: pred = [](char16_t c) { return isWhitespace(c) || isNewline(c); }; break;
        case kNewline: pred = isNewline; break;
        case kDigits: pred = isDigit; break;
        case kLetters: pred = [](char16_t c) { return isLetter(c) || isNonBase(c); }; break;
        case kLower: pred = isLower; break;
        case kUpper: case kCapitalized: pred = isUpper; break;
        case kAlnum: pred = isAlnum; break;
        case kPunct: pred = isPunct; break;
        case kSymbols: pred = isSymbol; break;
        case kControl: pred = isControl; break;
        case kNonBase: pred = isNonBase; break;
        case kDecomposable: pred = [](char16_t c) { return (c >= 0xC0 && c <= 0x17F && c != 0xD7 && c != 0xF7) || (c >= 0xAC00 && c <= 0xD7A3); }; break;
        case kIllegal: pred = [](char16_t c) { return (c >= 0xFDD0 && c <= 0xFDEF) || c == 0xFFFE || c == 0xFFFF; }; break;
        case kURLQuery: case kURLFragment: pred = [](char16_t c) { return urlUnreserved(c) || (c < 0x80 && c && std::strchr("!$&'()*+,;=:@/?", char(c))); }; break;
        case kURLPath: pred = [](char16_t c) { return urlUnreserved(c) || (c < 0x80 && c && std::strchr("!$&'()*+,;=:@/", char(c))); }; break;
        case kURLHost: pred = [](char16_t c) { return urlUnreserved(c) || (c < 0x80 && c && std::strchr("!$&'()*+,;=:[]", char(c))); }; break;
        case kURLUser: case kURLPassword: pred = [](char16_t c) { return urlUnreserved(c) || (c < 0x80 && c && std::strchr("!$&'()*+,;=", char(c))); }; break;
        default: pred = [](char16_t) { return false; };
    }
    const id s = newCharSet(x, false);
    CharSetData* d = csd(x, s);
    for (uint32_t c = 0; c < 0x10000; ++c) if (pred(char16_t(c))) d->set(char16_t(c));
    rt(x).makeImmortal(s);
    std::lock_guard<std::mutex> lk(m.mu);
    return m.standardSets.emplace(which, s).first->second;
}

template <int W> id csc_std(Ctx& x, id self, SEL) {
    const id s = standardSet(x, W);
    if (self == M(x).mutableCharSet) {
        const id c = newCharSet(x, true);
        csd(x, c)->bits = csd(x, s)->bits;
        return autoreleased(x, c);
    }
    return s;
}
id csc_withString(Ctx& x, id self, SEL, id str) {
    const id s = newCharSet(x, self == M(x).mutableCharSet);
    CharSetData* d = csd(x, s);
    for (char16_t c : stringValue(x, str)) d->set(c);
    return autoreleased(x, s);
}
id csc_withRange(Ctx& x, id self, SEL, uint32_t loc, uint32_t len) {
    const id s = newCharSet(x, self == M(x).mutableCharSet);
    if (len) csd(x, s)->range(loc, loc + len - 1);
    return autoreleased(x, s);
}
id csc_new(Ctx& x, id self, SEL) { return autoreleased(x, newCharSet(x, self == M(x).mutableCharSet)); }
id cs_allocWithZone(Ctx& x, id self, SEL, uint32_t) {
    if (self == M(x).charSet || self == M(x).mutableCharSet) return newCharSet(x, self == M(x).mutableCharSet);
    return rt(x).createInstance(self);
}
id cs_alloc(Ctx& x, id self, SEL s) { return cs_allocWithZone(x, self, s, 0); }
id cs_init(Ctx&, id self, SEL) { return self; }
bool cs_isMember(Ctx& x, id self, SEL, uint32_t c) { CharSetData* d = csd(x, self); return d && c < 0x10000 && d->test(char16_t(c)); }
bool cs_longIsMember(Ctx& x, id self, SEL s, uint32_t c) { return cs_isMember(x, self, s, c); }
id cs_inverted(Ctx& x, id self, SEL) {
    const id s = newCharSet(x, false);
    if (CharSetData* d = csd(x, self)) { auto& b = csd(x, s)->bits; for (size_t i = 0; i < b.size(); ++i) b[i] = ~d->bits[i]; }
    return autoreleased(x, s);
}
id cs_copy(Ctx& x, id self, SEL, uint32_t) {
    const id s = newCharSet(x, false);
    if (CharSetData* d = csd(x, self)) csd(x, s)->bits = d->bits;
    return s;
}
id cs_mutableCopy(Ctx& x, id self, SEL, uint32_t) {
    const id s = newCharSet(x, true);
    if (CharSetData* d = csd(x, self)) csd(x, s)->bits = d->bits;
    return s;
}
bool cs_isSuperset(Ctx& x, id self, SEL, id other) {
    CharSetData *a = csd(x, self), *b = csd(x, other);
    if (!a || !b) return false;
    for (size_t i = 0; i < a->bits.size(); ++i) if ((b->bits[i] & ~a->bits[i]) != 0) return false;
    return true;
}
void mcs_addString(Ctx& x, id self, SEL, id str) { if (CharSetData* d = csd(x, self)) for (char16_t c : stringValue(x, str)) d->set(c); }
void mcs_removeString(Ctx& x, id self, SEL, id str) { if (CharSetData* d = csd(x, self)) for (char16_t c : stringValue(x, str)) d->set(c, false); }
void mcs_addRange(Ctx& x, id self, SEL, uint32_t loc, uint32_t len) { if (CharSetData* d = csd(x, self); d && len) d->range(loc, loc + len - 1); }
void mcs_removeRange(Ctx& x, id self, SEL, uint32_t loc, uint32_t len) { if (CharSetData* d = csd(x, self); d && len) d->range(loc, loc + len - 1, false); }
void mcs_union(Ctx& x, id self, SEL, id other) { CharSetData *a = csd(x, self), *b = csd(x, other); if (a && b) for (size_t i = 0; i < a->bits.size(); ++i) a->bits[i] |= b->bits[i]; }
void mcs_intersect(Ctx& x, id self, SEL, id other) { CharSetData *a = csd(x, self), *b = csd(x, other); if (a && b) for (size_t i = 0; i < a->bits.size(); ++i) a->bits[i] &= b->bits[i]; }
void mcs_invert(Ctx& x, id self, SEL) { if (CharSetData* a = csd(x, self)) for (auto& w : a->bits) w = ~w; }

// --------------------------------------------------------- NSProcessInfo ---

id pi_processInfo(Ctx& x, id, SEL) {
    auto& m = M(x);
    if (!m.processInfoObj) { m.processInfoObj = rt(x).createInstance(m.processInfo); rt(x).makeImmortal(m.processInfoObj); }
    return m.processInfoObj;
}
id pi_processName(Ctx& x, id, SEL) {
    const std::string& p = x.proc.executablePath();
    return makeString(x, p.substr(p.rfind('/') + 1));
}
id pi_arguments(Ctx& x, id, SEL) {
    std::vector<id> args;
    for (uint32_t i = 0; i < x.proc.argc(); ++i) args.push_back(makeString(x, x.proc.readCString(x.mem.read<uint32_t>(x.proc.argvAddr() + i * 4))));
    return makeArray(x, args);
}
id pi_environment(Ctx& x, id, SEL) {
    std::vector<std::pair<id, id>> e;
    for (uint32_t p = x.proc.envpAddr(); p; p += 4) {
        const uint32_t s = x.mem.read<uint32_t>(p);
        if (!s) break;
        const std::string kv = x.proc.readCString(s);
        const size_t eq = kv.find('=');
        if (eq != std::string::npos) e.emplace_back(makeString(x, kv.substr(0, eq)), makeString(x, kv.substr(eq + 1)));
    }
    return makeDict(x, e);
}
std::string randomUuid() {
    static std::mt19937_64 rng{std::random_device{}()};
    static std::mutex mu;
    uint8_t b[16];
    {
        std::lock_guard<std::mutex> lk(mu);
        for (int i = 0; i < 16; i += 8) { const uint64_t v = rng(); std::memcpy(b + i, &v, 8); }
    }
    b[6] = (b[6] & 0x0F) | 0x40;
    b[8] = (b[8] & 0x3F) | 0x80;
    char s[37];
    std::snprintf(s, sizeof s, "%02X%02X%02X%02X-%02X%02X-%02X%02X-%02X%02X-%02X%02X%02X%02X%02X%02X",
                  b[0], b[1], b[2], b[3], b[4], b[5], b[6], b[7], b[8], b[9], b[10], b[11], b[12], b[13], b[14], b[15]);
    return s;
}
id pi_guid(Ctx& x, id, SEL) { return makeString(x, randomUuid()); }
id pi_hostName(Ctx& x, id, SEL) { return makeString(x, "localhost"); }
id pi_osVersionString(Ctx& x, id, SEL) { return makeString(x, "Version " + x.proc.config().systemVersion + " (Build 10B141)"); }
int32_t pi_pid(Ctx&, id, SEL) { return int32_t(getpid() & 0x7FFF); }
uint64_t pi_physicalMemory(Ctx&, id, SEL) { return 1024ull << 20; }
uint32_t pi_processorCount(Ctx&, id, SEL) { return 2; }
double pi_systemUptime(Ctx&, id, SEL) {
    using namespace std::chrono;
    return duration<double>(steady_clock::now().time_since_epoch()).count();
}
void parseVersion(const std::string& v, int32_t out[3]) {
    out[0] = out[1] = out[2] = 0;
    std::sscanf(v.c_str(), "%d.%d.%d", &out[0], &out[1], &out[2]);
}
void pi_osVersion(Ctx& x, GuestPtr ret, id, SEL) {
    int32_t v[3];
    parseVersion(x.proc.config().systemVersion, v);
    for (int i = 0; i < 3; ++i) x.mem.write<int32_t>(ret + i * 4, v[i]);
}
bool pi_atLeast(Ctx& x, id, SEL, int32_t maj, int32_t min, int32_t patch) {
    int32_t v[3];
    parseVersion(x.proc.config().systemVersion, v);
    return std::make_tuple(v[0], v[1], v[2]) >= std::make_tuple(maj, min, patch);
}
void pi_noop(Ctx&, id, SEL) {}
uint32_t pi_thermal(Ctx&, id, SEL) { return 0; }
bool pi_false(Ctx&, id, SEL) { return false; }

// ----------------------------------------------------------------- locks ---

struct LockData : ObjC::HostObject {
    std::mutex m;
    std::condition_variable cv;
    GuestThread* owner = nullptr;
    uint32_t depth = 0;
    bool recursive = false;
    int32_t condition = 0;
    id name = 0;
};
LockData& ld(Ctx& x, id self) { return rt(x).host<LockData>(self); }

// Acquire with periodic stop checks; returns false on timeout.
bool acquire(Ctx& x, LockData& d, double timeout = -1, std::function<bool()> extra = {}) {
    GuestThread* me = &x.proc.currentThread();
    std::unique_lock<std::mutex> lk(d.m);
    if (d.owner == me && d.recursive) { ++d.depth; return true; }
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::duration_cast<std::chrono::steady_clock::duration>(std::chrono::duration<double>(timeout < 0 ? 1e7 : timeout));
    while (d.owner || (extra && !extra())) {
        if (d.owner == me && !d.recursive) x.proc.logf(LogLevel::Warning, "*** -[NSLock lock]: deadlock (lock already held by this thread)");
        if (x.proc.stopping()) throw ProcessExit(0, "stopping");
        if (std::chrono::steady_clock::now() >= deadline) return false;
        d.cv.wait_for(lk, std::chrono::milliseconds(50));
    }
    d.owner = me;
    d.depth = 1;
    return true;
}
void releaseLock(Ctx& x, LockData& d) {
    {
        std::lock_guard<std::mutex> lk(d.m);
        if (d.owner != &x.proc.currentThread()) { if (d.owner) x.proc.logf(LogLevel::Warning, "*** unlock of a lock owned by another thread"); }
        if (d.depth > 1) { --d.depth; return; }
        d.owner = nullptr;
        d.depth = 0;
    }
    d.cv.notify_all();
}
id lock_init(Ctx& x, id self, SEL) { ld(x, self); return self; }
id rlock_init(Ctx& x, id self, SEL) { ld(x, self).recursive = true; return self; }
id clock_initWith(Ctx& x, id self, SEL, int32_t c) { ld(x, self).condition = c; return self; }
void lock_lock(Ctx& x, id self, SEL) { acquire(x, ld(x, self)); }
void lock_unlock(Ctx& x, id self, SEL) { releaseLock(x, ld(x, self)); }
bool lock_try(Ctx& x, id self, SEL) { return acquire(x, ld(x, self), 0); }
bool lock_before(Ctx& x, id self, SEL, id date) {
    const double until = dateInterval(x, date);
    return acquire(x, ld(x, self), std::max(0.0, until - now()));
}
id lock_name(Ctx& x, id self, SEL) { return ld(x, self).name; }
void lock_setName(Ctx& x, id self, SEL, id n) { auto& d = ld(x, self); const id old = d.name; d.name = id(rt(x).send(x.cpu, n, "copy")); rt(x).release(x.cpu, old); }
void lock_dealloc(Ctx& x, id self, SEL) {
    if (LockData* d = rt(x).hostIf<LockData>(self)) { const id n = d->name; d->name = 0; rt(x).release(x.cpu, n); }
    rt(x).disposeInstance(x.cpu, self);
}
// NSCondition: wait releases the lock, waits for signal, reacquires.
bool condWait(Ctx& x, LockData& d, double timeout) {
    GuestThread* me = &x.proc.currentThread();
    std::unique_lock<std::mutex> lk(d.m);
    const uint32_t depth = d.depth;
    d.owner = nullptr;
    d.depth = 0;
    d.cv.notify_all();
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::duration_cast<std::chrono::steady_clock::duration>(std::chrono::duration<double>(timeout < 0 ? 1e7 : timeout));
    bool signalled = false;
    // Wait for one signal (spurious wakeups are allowed by NSCondition's contract).
    if (x.proc.stopping()) throw ProcessExit(0, "stopping");
    const auto step = std::chrono::milliseconds(50);
    while (!signalled) {
        const auto now2 = std::chrono::steady_clock::now();
        if (now2 >= deadline) break;
        const std::chrono::steady_clock::time_point until = std::min<std::chrono::steady_clock::time_point>(deadline, now2 + step);
        signalled = d.cv.wait_until(lk, until) == std::cv_status::no_timeout;
        if (x.proc.stopping()) throw ProcessExit(0, "stopping");
    }
    while (d.owner) {
        if (x.proc.stopping()) throw ProcessExit(0, "stopping");
        d.cv.wait_for(lk, step);
    }
    d.owner = me;
    d.depth = depth ? depth : 1;
    return signalled;
}
void cond_wait(Ctx& x, id self, SEL) { condWait(x, ld(x, self), -1); }
bool cond_waitUntil(Ctx& x, id self, SEL, id date) { return condWait(x, ld(x, self), std::max(0.0, dateInterval(x, date) - now())); }
void cond_signal(Ctx& x, id self, SEL) { ld(x, self).cv.notify_one(); }
void cond_broadcast(Ctx& x, id self, SEL) { ld(x, self).cv.notify_all(); }
int32_t clock_condition(Ctx& x, id self, SEL) { return ld(x, self).condition; }
void clock_lockWhen(Ctx& x, id self, SEL, int32_t c) { auto& d = ld(x, self); acquire(x, d, -1, [&] { return d.condition == c; }); }
bool clock_tryWhen(Ctx& x, id self, SEL, int32_t c) { auto& d = ld(x, self); return acquire(x, d, 0, [&] { return d.condition == c; }); }
bool clock_lockWhenBefore(Ctx& x, id self, SEL, int32_t c, id date) {
    auto& d = ld(x, self);
    return acquire(x, d, std::max(0.0, dateInterval(x, date) - now()), [&] { return d.condition == c; });
}
void clock_unlockWith(Ctx& x, id self, SEL, int32_t c) { auto& d = ld(x, self); { std::lock_guard<std::mutex> lk(d.m); d.condition = c; } releaseLock(x, d); }

// ----------------------------------------------------------- NSIndexPath ---

struct IndexPathData : ObjC::HostObject { std::vector<uint32_t> idx; };
std::vector<uint32_t>& ipv(Ctx& x, id self) { return rt(x).host<IndexPathData>(self).idx; }
id newIndexPath(Ctx& x, std::vector<uint32_t> v) {
    const id o = rt(x).createInstance(M(x).indexPath);
    auto h = std::make_unique<IndexPathData>();
    h->idx = std::move(v);
    rt(x).setHost(o, std::move(h));
    return autoreleased(x, o);
}
id ipc_withIndex(Ctx& x, id, SEL, uint32_t i) { return newIndexPath(x, {i}); }
id ipc_withIndexes(Ctx& x, id, SEL, GuestPtr p, uint32_t n) {
    std::vector<uint32_t> v(n);
    for (uint32_t i = 0; i < n; ++i) v[i] = x.mem.read<uint32_t>(p + i * 4);
    return newIndexPath(x, v);
}
id ipc_rowSection(Ctx& x, id, SEL, uint32_t row, uint32_t section) { return newIndexPath(x, {section, row}); }
id ip_init(Ctx& x, id self, SEL) { ipv(x, self); return self; }
id ip_initWithIndexes(Ctx& x, id self, SEL, GuestPtr p, uint32_t n) {
    auto& v = ipv(x, self);
    v.resize(n);
    for (uint32_t i = 0; i < n; ++i) v[i] = x.mem.read<uint32_t>(p + i * 4);
    return self;
}
id ip_initWithIndex(Ctx& x, id self, SEL, uint32_t i) { ipv(x, self) = {i}; return self; }
uint32_t ip_length(Ctx& x, id self, SEL) { return uint32_t(ipv(x, self).size()); }
uint32_t ip_indexAt(Ctx& x, id self, SEL, uint32_t pos) { auto& v = ipv(x, self); return pos < v.size() ? v[pos] : kNotFound; }
uint32_t ip_section(Ctx& x, id self, SEL) { auto& v = ipv(x, self); return v.empty() ? kNotFound : v[0]; }
uint32_t ip_row(Ctx& x, id self, SEL) { auto& v = ipv(x, self); return v.size() < 2 ? kNotFound : v[1]; }
id ip_adding(Ctx& x, id self, SEL, uint32_t i) { auto v = ipv(x, self); v.push_back(i); return newIndexPath(x, v); }
id ip_removingLast(Ctx& x, id self, SEL) { auto v = ipv(x, self); if (!v.empty()) v.pop_back(); return newIndexPath(x, v); }
void ip_getIndexes(Ctx& x, id self, SEL, GuestPtr out) { auto& v = ipv(x, self); for (size_t i = 0; i < v.size(); ++i) x.mem.write<uint32_t>(out + uint32_t(i) * 4, v[i]); }
int32_t ip_compare(Ctx& x, id self, SEL, id other) {
    const auto& a = ipv(x, self);
    const auto b = other ? ipv(x, other) : std::vector<uint32_t>();
    return a < b ? -1 : b < a ? 1 : 0;
}
bool ip_isEqual(Ctx& x, id self, SEL, id other) {
    return other && rt(x).isKindOf(other, M(x).indexPath) && ipv(x, self) == ipv(x, other);
}
uint32_t ip_hash(Ctx& x, id self, SEL) { uint32_t h = 0; for (uint32_t i : ipv(x, self)) h = h * 31 + i; return h; }
id ip_description(Ctx& x, id self, SEL) {
    std::string s;
    char buf[96];
    std::snprintf(buf, sizeof buf, "<NSIndexPath: 0x%x> {length = %zu, path = ", self, ipv(x, self).size());
    s = buf;
    bool first = true;
    for (uint32_t i : ipv(x, self)) { if (!first) s += " - "; s += std::to_string(i); first = false; }
    return makeString(x, s + "}");
}

// ------------------------------------------------------------ NSIndexSet ---

struct IndexSetData : ObjC::HostObject { std::set<uint32_t> idx; };
std::set<uint32_t>& isv(Ctx& x, id self) { return rt(x).host<IndexSetData>(self).idx; }
id newIndexSet(Ctx& x, Class cls, std::set<uint32_t> v) {
    const id o = rt(x).createInstance(cls);
    auto h = std::make_unique<IndexSetData>();
    h->idx = std::move(v);
    rt(x).setHost(o, std::move(h));
    return o;
}
Class concreteSetClass(Ctx& x, id self) { return rt(x).isSubclass(self, M(x).mutableIndexSet) ? M(x).mutableIndexSet : M(x).indexSet; }
id isc_set(Ctx& x, id self, SEL) { return autoreleased(x, newIndexSet(x, concreteSetClass(x, self), {})); }
id isc_withIndex(Ctx& x, id self, SEL, uint32_t i) { return autoreleased(x, newIndexSet(x, concreteSetClass(x, self), {i})); }
id isc_withRange(Ctx& x, id self, SEL, uint32_t loc, uint32_t len) {
    std::set<uint32_t> s;
    for (uint32_t i = 0; i < len; ++i) s.insert(loc + i);
    return autoreleased(x, newIndexSet(x, concreteSetClass(x, self), s));
}
id is_init(Ctx& x, id self, SEL) { isv(x, self); return self; }
id is_initWithIndex(Ctx& x, id self, SEL, uint32_t i) { isv(x, self) = {i}; return self; }
id is_initWithRange(Ctx& x, id self, SEL, uint32_t loc, uint32_t len) { auto& s = isv(x, self); for (uint32_t i = 0; i < len; ++i) s.insert(loc + i); return self; }
id is_initWithSet(Ctx& x, id self, SEL, id other) { if (other) isv(x, self) = isv(x, other); return self; }
uint32_t is_count(Ctx& x, id self, SEL) { return uint32_t(isv(x, self).size()); }
uint32_t is_first(Ctx& x, id self, SEL) { auto& s = isv(x, self); return s.empty() ? kNotFound : *s.begin(); }
uint32_t is_last(Ctx& x, id self, SEL) { auto& s = isv(x, self); return s.empty() ? kNotFound : *s.rbegin(); }
bool is_contains(Ctx& x, id self, SEL, uint32_t i) { return isv(x, self).count(i) != 0; }
bool is_containsRange(Ctx& x, id self, SEL, uint32_t loc, uint32_t len) { auto& s = isv(x, self); for (uint32_t i = 0; i < len; ++i) if (!s.count(loc + i)) return false; return true; }
bool is_containsSet(Ctx& x, id self, SEL, id o) { auto& s = isv(x, self); for (uint32_t i : isv(x, o)) if (!s.count(i)) return false; return true; }
uint32_t is_greaterThan(Ctx& x, id self, SEL, uint32_t i) { auto& s = isv(x, self); auto it = s.upper_bound(i); return it == s.end() ? kNotFound : *it; }
uint32_t is_greaterOrEqual(Ctx& x, id self, SEL, uint32_t i) { auto& s = isv(x, self); auto it = s.lower_bound(i); return it == s.end() ? kNotFound : *it; }
uint32_t is_lessThan(Ctx& x, id self, SEL, uint32_t i) { auto& s = isv(x, self); auto it = s.lower_bound(i); return it == s.begin() ? kNotFound : *--it; }
uint32_t is_lessOrEqual(Ctx& x, id self, SEL, uint32_t i) { auto& s = isv(x, self); auto it = s.upper_bound(i); return it == s.begin() ? kNotFound : *--it; }
void is_enumerate(Ctx& x, id self, SEL, id block) {
    const std::vector<uint32_t> v(isv(x, self).begin(), isv(x, self).end());
    const uint32_t invoke = x.mem.read<uint32_t>(block + 12);
    const uint32_t stop = x.proc.heap().allocZeroed(4);
    for (uint32_t i : v) {
        x.proc.callGuest(x.cpu, invoke, {block, i, stop});
        if (x.mem.read<uint8_t>(stop)) break;
    }
    x.proc.heap().free(stop);
}
uint32_t is_getIndexes(Ctx& x, id self, SEL, GuestPtr buf, uint32_t max, GuestPtr range) {
    auto& s = isv(x, self);
    uint32_t lo = 0, hi = 0xFFFFFFFF;
    if (range) { lo = x.mem.read<uint32_t>(range); hi = lo + x.mem.read<uint32_t>(range + 4); }
    uint32_t n = 0, last = lo;
    for (auto it = s.lower_bound(lo); it != s.end() && *it < hi && n < max; ++it) { x.mem.write<uint32_t>(buf + n * 4, *it); ++n; last = *it + 1; }
    if (range) { const uint32_t end = hi; x.mem.write<uint32_t>(range, last); x.mem.write<uint32_t>(range + 4, end - last); }
    return n;
}
id is_copy(Ctx& x, id self, SEL, uint32_t) { return newIndexSet(x, M(x).indexSet, isv(x, self)); }
id is_mutableCopy(Ctx& x, id self, SEL, uint32_t) { return newIndexSet(x, M(x).mutableIndexSet, isv(x, self)); }
bool is_isEqual(Ctx& x, id self, SEL, id o) { return o && rt(x).isKindOf(o, M(x).indexSet) && isv(x, self) == isv(x, o); }
void mis_add(Ctx& x, id self, SEL, uint32_t i) { isv(x, self).insert(i); }
void mis_remove(Ctx& x, id self, SEL, uint32_t i) { isv(x, self).erase(i); }
void mis_addRange(Ctx& x, id self, SEL, uint32_t loc, uint32_t len) { auto& s = isv(x, self); for (uint32_t i = 0; i < len; ++i) s.insert(loc + i); }
void mis_removeRange(Ctx& x, id self, SEL, uint32_t loc, uint32_t len) { auto& s = isv(x, self); for (uint32_t i = 0; i < len; ++i) s.erase(loc + i); }
void mis_addSet(Ctx& x, id self, SEL, id o) { if (o) for (uint32_t i : isv(x, o)) isv(x, self).insert(i); }
void mis_removeSet(Ctx& x, id self, SEL, id o) { if (o) for (uint32_t i : std::set<uint32_t>(isv(x, o))) isv(x, self).erase(i); }
void mis_removeAll(Ctx& x, id self, SEL) { isv(x, self).clear(); }
void mis_shift(Ctx& x, id self, SEL, uint32_t start, int32_t delta) {
    auto& s = isv(x, self);
    std::set<uint32_t> out;
    for (uint32_t i : s) {
        if (i < start) out.insert(i);
        else if (int64_t(i) + delta >= int64_t(start) || delta > 0) { const int64_t n = int64_t(i) + delta; if (n >= 0) out.insert(uint32_t(n)); }
    }
    s = std::move(out);
}

// --------------------------------------------------------------- NSUUID ---

struct UuidData : ObjC::HostObject { uint8_t b[16] = {}; };
void parseUuid(const std::string& s, uint8_t out[16], bool* ok) {
    int n = 0;
    *ok = true;
    for (size_t i = 0; i < s.size() && n < 32; ++i) {
        if (s[i] == '-') continue;
        if (!std::isxdigit(uint8_t(s[i]))) { *ok = false; return; }
        const int v = std::isdigit(uint8_t(s[i])) ? s[i] - '0' : (std::tolower(s[i]) - 'a' + 10);
        if (n & 1) out[n / 2] |= uint8_t(v); else out[n / 2] = uint8_t(v << 4);
        ++n;
    }
    if (n != 32 || s.size() != 36) *ok = false;
}
id uuid_init(Ctx& x, id self, SEL) {
    auto h = std::make_unique<UuidData>();
    bool ok;
    parseUuid(randomUuid(), h->b, &ok);
    rt(x).setHost(self, std::move(h));
    return self;
}
id uuidc_uuid(Ctx& x, id self, SEL) { return autoreleased(x, uuid_init(x, rt(x).createInstance(self), 0)); }
id uuid_initWithString(Ctx& x, id self, SEL, id str) {
    auto h = std::make_unique<UuidData>();
    bool ok;
    parseUuid(utf8(x, str), h->b, &ok);
    if (!ok) { rt(x).release(x.cpu, self); return 0; }
    rt(x).setHost(self, std::move(h));
    return self;
}
id uuid_initWithBytes(Ctx& x, id self, SEL, GuestPtr p) {
    auto h = std::make_unique<UuidData>();
    std::memcpy(h->b, x.mem.host(p), 16);
    rt(x).setHost(self, std::move(h));
    return self;
}
id uuid_string(Ctx& x, id self, SEL) {
    UuidData& d = rt(x).host<UuidData>(self);
    const uint8_t* b = d.b;
    char s[37];
    std::snprintf(s, sizeof s, "%02X%02X%02X%02X-%02X%02X-%02X%02X-%02X%02X-%02X%02X%02X%02X%02X%02X",
                  b[0], b[1], b[2], b[3], b[4], b[5], b[6], b[7], b[8], b[9], b[10], b[11], b[12], b[13], b[14], b[15]);
    return makeString(x, std::string(s));
}
void uuid_getBytes(Ctx& x, id self, SEL, GuestPtr out) { std::memcpy(x.mem.host(out), rt(x).host<UuidData>(self).b, 16); }
bool uuid_isEqual(Ctx& x, id self, SEL, id o) { return o && rt(x).hostIf<UuidData>(o) && std::memcmp(rt(x).host<UuidData>(self).b, rt(x).host<UuidData>(o).b, 16) == 0; }
uint32_t uuid_hash(Ctx& x, id self, SEL) { uint32_t h; std::memcpy(&h, rt(x).host<UuidData>(self).b, 4); return h; }

// ------------------------------------------------- NSLocale / NSTimeZone ---

id singleton(Ctx& x, id& slot, Class cls) {
    if (!slot) { slot = rt(x).createInstance(cls); rt(x).makeImmortal(slot); }
    return slot;
}
id loc_current(Ctx& x, id, SEL) { return singleton(x, M(x).currentLocale, M(x).locale); }
id loc_identifier(Ctx& x, id, SEL) { return makeString(x, "en_US"); }
id locc_preferredLanguages(Ctx& x, id, SEL) { return makeArray(x, {makeString(x, "en")}); }
id locc_available(Ctx& x, id, SEL) { return makeArray(x, {makeString(x, "en_US")}); }
id loc_objectForKey(Ctx& x, id, SEL, id key) {
    const std::string k = utf8(x, key);
    if (k == "kCFLocaleCountryCodeKey" || k == "NSLocaleCountryCode") return makeString(x, "US");
    if (k == "kCFLocaleLanguageCodeKey" || k == "NSLocaleLanguageCode") return makeString(x, "en");
    if (k == "kCFLocaleCurrencyCodeKey" || k == "NSLocaleCurrencyCode") return makeString(x, "USD");
    if (k == "kCFLocaleCurrencySymbolKey" || k == "NSLocaleCurrencySymbol") return makeString(x, "$");
    if (k == "kCFLocaleDecimalSeparatorKey" || k == "NSLocaleDecimalSeparator") return makeString(x, ".");
    if (k == "kCFLocaleGroupingSeparatorKey" || k == "NSLocaleGroupingSeparator") return makeString(x, ",");
    if (k == "kCFLocaleUsesMetricSystemKey" || k == "NSLocaleUsesMetricSystem") return makeBool(x, false);
    if (k == "kCFLocaleIdentifierKey" || k == "NSLocaleIdentifier") return makeString(x, "en_US");
    return 0;
}
id loc_initWithIdentifier(Ctx&, id self, SEL, id) { return self; }
id locc_withIdentifier(Ctx& x, id, SEL, id) { return loc_current(x, 0, 0); }
id loc_displayName(Ctx& x, id, SEL, id, id value) { return value; }

id tz_local(Ctx& x, id, SEL) { return singleton(x, M(x).localTZ, M(x).timeZone); }
int32_t tz_secondsFromGMT(Ctx&, id, SEL) {
    const time_t t = time(nullptr);
    std::tm lt;
    localtime_r(&t, &lt);
    return int32_t(lt.tm_gmtoff);
}
int32_t tz_secondsFromGMTForDate(Ctx& x, id self, SEL s, id) { return tz_secondsFromGMT(x, self, s); }
id tz_name(Ctx& x, id, SEL) {
    const char* tz = std::getenv("TZ");
    return makeString(x, std::string(tz && *tz ? tz : "GMT"));
}
id tz_abbreviation(Ctx& x, id, SEL) {
    const time_t t = time(nullptr);
    std::tm lt;
    localtime_r(&t, &lt);
    return makeString(x, std::string(lt.tm_zone ? lt.tm_zone : "GMT"));
}
id tzc_withName(Ctx& x, id, SEL, id) { return tz_local(x, 0, 0); }
bool tz_isDST(Ctx&, id, SEL) { const time_t t = time(nullptr); std::tm lt; localtime_r(&t, &lt); return lt.tm_isdst > 0; }

// ---------------------------------------------- conversion functions ---

id f_stringFromClass(Ctx& x, Class c) { return c ? makeString(x, rt(x).className(c)) : 0; }
Class f_classFromString(Ctx& x, id s) { return s ? rt(x).getClass(utf8(x, s)) : 0; }
id f_stringFromSelector(Ctx& x, SEL s) { return s ? makeString(x, rt(x).selName(s)) : 0; }
SEL f_selectorFromString(Ctx& x, id s) { return s ? rt(x).sel(utf8(x, s)) : 0; }
id f_stringFromProtocol(Ctx& x, uint32_t p) { return p ? makeString(x, rt(x).protocolName(p)) : 0; }
uint32_t f_protocolFromString(Ctx& x, id s) { return s ? rt(x).getProtocol(utf8(x, s)) : 0; }
id f_stringFromRange(Ctx& x, uint32_t loc, uint32_t len) { return makeString(x, "{" + std::to_string(loc) + ", " + std::to_string(len) + "}"); }
void f_rangeFromString(Ctx& x, GuestPtr ret, id s) {
    const std::string v = utf8(x, s);
    uint32_t a = 0, b = 0;
    const char* p = v.c_str();
    while (*p && !std::isdigit(uint8_t(*p))) ++p;
    a = uint32_t(std::strtoul(p, const_cast<char**>(&p), 10));
    while (*p && !std::isdigit(uint8_t(*p))) ++p;
    b = uint32_t(std::strtoul(p, nullptr, 10));
    writeRange(x, ret, {a, b});
}
void f_unionRange(Ctx& x, GuestPtr ret, uint32_t l1, uint32_t n1, uint32_t l2, uint32_t n2) {
    const uint32_t lo = std::min(l1, l2), hi = std::max(l1 + n1, l2 + n2);
    writeRange(x, ret, {lo, hi - lo});
}
void f_intersectionRange(Ctx& x, GuestPtr ret, uint32_t l1, uint32_t n1, uint32_t l2, uint32_t n2) {
    const uint32_t lo = std::max(l1, l2), hi = std::min(l1 + n1, l2 + n2);
    writeRange(x, ret, hi > lo ? Range{lo, hi - lo} : Range{0, 0});
}
id f_allocateObject(Ctx& x, Class c, uint32_t extra, uint32_t) { return rt(x).createInstance(c, extra); }
void f_deallocateObject(Ctx& x, id o) { rt(x).disposeInstance(x.cpu, o); }
id f_copyObject(Ctx& x, id o, uint32_t extra, uint32_t) {
    const id c = rt(x).createInstance(rt(x).classOf(o), extra);
    const uint32_t size = rt(x).info(rt(x).classOf(o)) ? rt(x).info(rt(x).classOf(o))->instanceSize : 4;
    if (size > 4) std::memcpy(x.mem.host(c + 4), x.mem.host(o + 4), size - 4);
    return c;
}
uint32_t f_defaultZone(Ctx&) { return 0; }
uint32_t f_zoneMalloc(Ctx& x, uint32_t, uint32_t n) { return x.proc.heap().alloc(n); }
uint32_t f_zoneCalloc(Ctx& x, uint32_t, uint32_t n, uint32_t s) { return x.proc.heap().allocZeroed(n * s); }
uint32_t f_zoneRealloc(Ctx& x, uint32_t, uint32_t p, uint32_t n) { return x.proc.heap().realloc(p, n); }
void f_zoneFree(Ctx& x, uint32_t, uint32_t p) { x.proc.heap().free(p); }
uint32_t f_zoneFromPointer(Ctx&, uint32_t) { return 0; }
void f_incExtra(Ctx& x, id o) { rt(x).rootRetain(o); }
bool f_decExtraWasZero(Ctx& x, id o) { return rt(x).rootRelease(o); }
uint32_t f_extraRefCount(Ctx& x, id o) { const uint32_t n = rt(x).retainCount(o); return n ? n - 1 : 0; }
bool f_shouldRetainWithZone(Ctx&, id, uint32_t) { return true; }
uint32_t f_pageSize(Ctx&) { return 4096; }
uint32_t f_roundUpToPage(Ctx&, uint32_t n) { return (n + 4095) & ~4095u; }
uint32_t f_roundDownToPage(Ctx&, uint32_t n) { return n & ~4095u; }
uint32_t f_allocateMemoryPages(Ctx& x, uint32_t n) { return x.proc.heap().allocZeroed((n + 4095) & ~4095u); }
void f_deallocateMemoryPages(Ctx& x, uint32_t p, uint32_t) { x.proc.heap().free(p); }
uint32_t f_realMemory(Ctx&) { return 1024u << 20; }
id f_fullUserName(Ctx& x) { return makeString(x, "Mobile User"); }
id f_userName(Ctx& x) { return makeString(x, "mobile"); }

} // namespace

// Exported helpers ------------------------------------------------------------

bool characterSetContains(Ctx& x, id set, char16_t c) {
    if (CharSetData* d = csd(x, set)) return d->test(c);
    if (!set) return false;
    return (uint32_t(rt(x).send(x.cpu, set, "characterIsMember:", {uint32_t(c)})) & 0xFF) != 0;
}
id nullObject(Process& p) { return M(p).nullObj; }

void registerMisc(Process& p, ObjC::Runtime& r) {
    auto& m = M(p);
    p.def<&ns_log>("_NSLog");
    p.def<&ns_logv>("_NSLogv");

    ObjC::ClassBuilder obj = r.builder(r.nsObject);
    obj.m<&o_description>("description").m<&o_description>("debugDescription").cm<&oc_description>("description")
       .cm<&oc_description>("debugDescription")
       .m<&o_valueForKey>("valueForKey:").m<&o_setValueForKey>("setValue:forKey:", "v@:@@")
       .m<&o_valueForKeyPath>("valueForKeyPath:").m<&o_setValueForKeyPath>("setValue:forKeyPath:", "v@:@@")
       .m<&o_setValuesForKeys>("setValuesForKeysWithDictionary:", "v@:@").m<&o_dictionaryWithValuesForKeys>("dictionaryWithValuesForKeys:")
       .m<&o_valueForUndefinedKey>("valueForUndefinedKey:").m<&o_setValueForUndefinedKey>("setValue:forUndefinedKey:", "v@:@@")
       .m<&o_setNilValueForKey>("setNilValueForKey:", "v@:@").m<&o_mutableArrayValueForKey>("mutableArrayValueForKey:")
       .m<&o_kvoAdd>("addObserver:forKeyPath:options:context:", "v@:@@I^v").m<&o_kvoNoop2>("removeObserver:forKeyPath:", "v@:@@")
       .m<&o_kvoRemove3>("removeObserver:forKeyPath:context:", "v@:@@^v").m<&o_kvoChange>("willChangeValueForKey:", "v@:@")
       .m<&o_kvoChange>("didChangeValueForKey:", "v@:@").cm<&oc_automaticallyNotifies>("automaticallyNotifiesObserversForKey:", "c@:@")
       .m<&o_observationInfo>("observationInfo", "^v@:");

    ObjC::ClassBuilder exc = r.defineClass("NSException", "NSObject");
    m.nsException = exc.cls();
    exc.cm<&excc_with>("exceptionWithName:reason:userInfo:").cm<&excc_raiseFormat>("raise:format:", "v@:@@")
       .cm<&excc_raiseFormatArgs>("raise:format:arguments:", "v@:@@^v");
    exc.m<&exc_init>("initWithName:reason:userInfo:").m<&exc_raise>("raise", "v@:").m<&exc_name>("name").m<&exc_reason>("reason")
       .m<&exc_userInfo>("userInfo").m<&exc_callStack>("callStackSymbols").m<&exc_callStack>("callStackReturnAddresses")
       .m<&exc_description>("description").m<&exc_dealloc>("dealloc", "v@:");
    r.describeException = [&p](id e) {
        Ctx x{p, p.currentThread().cpu, p.mem()};
        return exceptionText(x, e);
    };
    p.def<&set_uncaught>("_NSSetUncaughtExceptionHandler");
    p.def<&get_uncaught>("_NSGetUncaughtExceptionHandler");
    for (const char* n : {"NSGenericException", "NSRangeException", "NSInvalidArgumentException", "NSInternalInconsistencyException",
                          "NSMallocException", "NSObjectInaccessibleException", "NSObjectNotAvailableException",
                          "NSDestinationInvalidException", "NSPortTimeoutException", "NSInvalidSendPortException",
                          "NSInvalidReceivePortException", "NSPortSendException", "NSPortReceiveException",
                          "NSCharacterConversionException", "NSFileHandleOperationException", "NSParseErrorException",
                          "NSUndefinedKeyException", "NSInvalidArchiveOperationException", "NSInvalidUnarchiveOperationException",
                          "NSCocoaErrorDomain", "NSPOSIXErrorDomain", "NSOSStatusErrorDomain", "NSMachErrorDomain",
                          "NSURLErrorDomain", "NSLocalizedDescriptionKey", "NSLocalizedFailureReasonErrorKey",
                          "NSLocalizedRecoverySuggestionErrorKey", "NSUnderlyingErrorKey", "NSFilePathErrorKey",
                          "NSStringEncodingErrorKey", "NSURLErrorKey", "NSErrorFailingURLStringKey", "NSURLErrorFailingURLErrorKey",
                          "NSURLErrorFailingURLStringErrorKey", "NSHelpAnchorErrorKey", "NSLocalizedRecoveryOptionsErrorKey",
                          "NSRecoveryAttempterErrorKey"}) {
        // The string values match Apple's: the constant name, except for the error keys.
        std::string v = n;
        if (v == "NSLocalizedDescriptionKey") v = "NSLocalizedDescription";
        else if (v == "NSLocalizedFailureReasonErrorKey") v = "NSLocalizedFailureReason";
        else if (v == "NSLocalizedRecoverySuggestionErrorKey") v = "NSLocalizedRecoverySuggestion";
        else if (v == "NSUnderlyingErrorKey") v = "NSUnderlyingError";
        else if (v == "NSFilePathErrorKey") v = "NSFilePath";
        else if (v == "NSURLErrorKey") v = "NSURL";
        else if (v == "NSErrorFailingURLStringKey" || v == "NSURLErrorFailingURLStringErrorKey") v = "NSErrorFailingURLStringKey";
        else if (v == "NSURLErrorFailingURLErrorKey") v = "NSErrorFailingURLKey";
        else if (v == "NSStringEncodingErrorKey") v = "NSStringEncoding";
        else if (v == "NSUndefinedKeyException") v = "NSUnknownKeyException";
        const uint32_t cell = p.defineDataBlock(std::string("_") + n, 4);
        p.mem().write<uint32_t>(cell, constString(p, v));
    }

    ObjC::ClassBuilder ah = r.defineClass("NSAssertionHandler", "NSObject");
    m.assertionHandler = ah.cls();
    ah.cm<&ah_current>("currentHandler")
      .m<&ah_failMethod>("handleFailureInMethod:object:file:lineNumber:description:", "v@::@@i@")
      .m<&ah_failFunction>("handleFailureInFunction:file:lineNumber:description:", "v@:@@i@");

    ObjC::ClassBuilder err = r.defineClass("NSError", "NSObject");
    m.nsError = err.cls();
    err.cm<&errc_with>("errorWithDomain:code:userInfo:");
    err.m<&err_init>("initWithDomain:code:userInfo:").m<&err_domain>("domain").m<&err_code>("code", "i@:")
       .m<&err_userInfo>("userInfo").m<&err_localizedDescription>("localizedDescription")
       .m<&err_failureReason>("localizedFailureReason").m<&err_recovery>("localizedRecoverySuggestion")
       .m<&err_description>("description").m<&retainedCopy>("copyWithZone:").m<&err_dealloc>("dealloc", "v@:");

    ObjC::ClassBuilder nul = r.defineClass("NSNull", "NSObject");
    m.nsNull = nul.cls();
    m.nullObj = r.createInstance(m.nsNull);
    r.makeImmortal(m.nullObj);
    nul.cm<&null_null>("null").cm<&null_null>("alloc").cm<&null_null>("allocWithZone:");
    nul.m<&null_description>("description").m<&null_self>("init").m<&null_self>("retain").m<&null_noop>("release", "v@:")
       .m<&null_self>("autorelease").m<&retainedCopy>("copyWithZone:").m<&null_noop>("dealloc", "v@:");
    const uint32_t nullCell = p.defineDataBlock("_kCFNull", 4);
    p.mem().write<uint32_t>(nullCell, m.nullObj);

    ObjC::ClassBuilder pool = r.defineClass("NSAutoreleasePool", "NSObject");
    m.pool = pool.cls();
    pool.m<&pool_init>("init").m<&pool_drain>("drain", "v@:").m<&pool_drain>("release", "v@:").m<&pool_retain>("retain")
        .m<&pool_retain>("autorelease").m<&pool_addObject>("addObject:", "v@:@").cm<&poolc_addObject>("addObject:", "v@:@");

    ObjC::ClassBuilder cs = r.defineClass("NSCharacterSet", "NSObject");
    ObjC::ClassBuilder mcs = r.defineClass("NSMutableCharacterSet", "NSCharacterSet");
    m.charSet = cs.cls(); m.mutableCharSet = mcs.cls();
    cs.cm<&cs_alloc>("alloc").cm<&cs_allocWithZone>("allocWithZone:")
      .cm<&csc_std<kWhitespace>>("whitespaceCharacterSet").cm<&csc_std<kWhitespaceNewline>>("whitespaceAndNewlineCharacterSet")
      .cm<&csc_std<kNewline>>("newlineCharacterSet").cm<&csc_std<kDigits>>("decimalDigitCharacterSet")
      .cm<&csc_std<kLetters>>("letterCharacterSet").cm<&csc_std<kLower>>("lowercaseLetterCharacterSet")
      .cm<&csc_std<kUpper>>("uppercaseLetterCharacterSet").cm<&csc_std<kAlnum>>("alphanumericCharacterSet")
      .cm<&csc_std<kPunct>>("punctuationCharacterSet").cm<&csc_std<kSymbols>>("symbolCharacterSet")
      .cm<&csc_std<kControl>>("controlCharacterSet").cm<&csc_std<kNonBase>>("nonBaseCharacterSet")
      .cm<&csc_std<kDecomposable>>("decomposableCharacterSet").cm<&csc_std<kIllegal>>("illegalCharacterSet")
      .cm<&csc_std<kCapitalized>>("capitalizedLetterCharacterSet")
      .cm<&csc_std<kURLQuery>>("URLQueryAllowedCharacterSet").cm<&csc_std<kURLPath>>("URLPathAllowedCharacterSet")
      .cm<&csc_std<kURLHost>>("URLHostAllowedCharacterSet").cm<&csc_std<kURLFragment>>("URLFragmentAllowedCharacterSet")
      .cm<&csc_std<kURLUser>>("URLUserAllowedCharacterSet").cm<&csc_std<kURLPassword>>("URLPasswordAllowedCharacterSet")
      .cm<&csc_withString>("characterSetWithCharactersInString:").cm<&csc_withRange>("characterSetWithRange:")
      .cm<&csc_new>("new");
    cs.m<&cs_init>("init").m<&cs_isMember>("characterIsMember:", "c@:S").m<&cs_longIsMember>("longCharacterIsMember:", "c@:I")
      .m<&cs_inverted>("invertedSet").m<&cs_copy>("copyWithZone:").m<&cs_mutableCopy>("mutableCopyWithZone:")
      .m<&cs_isSuperset>("isSupersetOfSet:", "c@:@").m<&obj_dealloc>("dealloc", "v@:");
    mcs.m<&mcs_addString>("addCharactersInString:", "v@:@").m<&mcs_removeString>("removeCharactersInString:", "v@:@")
       .m<&mcs_addRange>("addCharactersInRange:", "v@:{_NSRange=II}").m<&mcs_removeRange>("removeCharactersInRange:", "v@:{_NSRange=II}")
       .m<&mcs_union>("formUnionWithCharacterSet:", "v@:@").m<&mcs_intersect>("formIntersectionWithCharacterSet:", "v@:@")
       .m<&mcs_invert>("invert", "v@:");

    ObjC::ClassBuilder pi = r.defineClass("NSProcessInfo", "NSObject");
    m.processInfo = pi.cls();
    pi.cm<&pi_processInfo>("processInfo");
    pi.m<&pi_processName>("processName").m<&pi_arguments>("arguments").m<&pi_environment>("environment")
      .m<&pi_guid>("globallyUniqueString").m<&pi_hostName>("hostName").m<&pi_osVersionString>("operatingSystemVersionString")
      .m<&pi_pid>("processIdentifier", "i@:").m<&pi_physicalMemory>("physicalMemory", "Q@:")
      .m<&pi_processorCount>("processorCount", "I@:").m<&pi_processorCount>("activeProcessorCount", "I@:")
      .m<&pi_systemUptime>("systemUptime", "d@:").m<&pi_osVersion>("operatingSystemVersion", "{?=iii}@:")
      .m<&pi_atLeast>("isOperatingSystemAtLeastVersion:", "c@:{?=iii}")
      .m<&pi_noop>("disableSuddenTermination", "v@:").m<&pi_noop>("enableSuddenTermination", "v@:")
      .m<&pi_thermal>("thermalState", "I@:").m<&pi_false>("isLowPowerModeEnabled", "c@:");

    ObjC::ClassBuilder lock = r.defineClass("NSLock", "NSObject");
    ObjC::ClassBuilder rlock = r.defineClass("NSRecursiveLock", "NSObject");
    ObjC::ClassBuilder cond = r.defineClass("NSCondition", "NSObject");
    ObjC::ClassBuilder clock = r.defineClass("NSConditionLock", "NSObject");
    m.nsLock = lock.cls(); m.nsRecursiveLock = rlock.cls(); m.nsCondition = cond.cls(); m.nsConditionLock = clock.cls();
    for (ObjC::ClassBuilder* b : {&lock, &rlock, &cond, &clock}) {
        b->m<&lock_lock>("lock", "v@:").m<&lock_unlock>("unlock", "v@:").m<&lock_try>("tryLock", "c@:")
          .m<&lock_before>("lockBeforeDate:", "c@:@").m<&lock_name>("name").m<&lock_setName>("setName:", "v@:@")
          .m<&lock_dealloc>("dealloc", "v@:").m<&lock_init>("init");
    }
    rlock.m<&rlock_init>("init");
    cond.m<&cond_wait>("wait", "v@:").m<&cond_waitUntil>("waitUntilDate:", "c@:@").m<&cond_signal>("signal", "v@:")
        .m<&cond_broadcast>("broadcast", "v@:");
    clock.m<&clock_initWith>("initWithCondition:").m<&clock_condition>("condition", "i@:").m<&clock_lockWhen>("lockWhenCondition:", "v@:i")
         .m<&clock_tryWhen>("tryLockWhenCondition:", "c@:i").m<&clock_lockWhenBefore>("lockWhenCondition:beforeDate:", "c@:i@")
         .m<&clock_unlockWith>("unlockWithCondition:", "v@:i");

    ObjC::ClassBuilder ip = r.defineClass("NSIndexPath", "NSObject");
    m.indexPath = ip.cls();
    ip.cm<&ipc_withIndex>("indexPathWithIndex:").cm<&ipc_withIndexes>("indexPathWithIndexes:length:")
      .cm<&ipc_rowSection>("indexPathForRow:inSection:").cm<&ipc_rowSection>("indexPathForItem:inSection:");
    ip.m<&ip_init>("init").m<&ip_initWithIndexes>("initWithIndexes:length:").m<&ip_initWithIndex>("initWithIndex:")
      .m<&ip_length>("length", "I@:").m<&ip_indexAt>("indexAtPosition:", "I@:I").m<&ip_section>("section", "i@:")
      .m<&ip_row>("row", "i@:").m<&ip_row>("item", "i@:").m<&ip_adding>("indexPathByAddingIndex:")
      .m<&ip_removingLast>("indexPathByRemovingLastIndex").m<&ip_getIndexes>("getIndexes:", "v@:^I")
      .m<&ip_compare>("compare:", "i@:@").m<&ip_isEqual>("isEqual:", "c@:@").m<&ip_hash>("hash", "I@:")
      .m<&ip_description>("description").m<&retainedCopy>("copyWithZone:").m<&obj_dealloc>("dealloc", "v@:");

    ObjC::ClassBuilder is = r.defineClass("NSIndexSet", "NSObject");
    ObjC::ClassBuilder mis = r.defineClass("NSMutableIndexSet", "NSIndexSet");
    m.indexSet = is.cls(); m.mutableIndexSet = mis.cls();
    is.cm<&isc_set>("indexSet").cm<&isc_withIndex>("indexSetWithIndex:").cm<&isc_withRange>("indexSetWithIndexesInRange:");
    is.m<&is_init>("init").m<&is_initWithIndex>("initWithIndex:").m<&is_initWithRange>("initWithIndexesInRange:")
      .m<&is_initWithSet>("initWithIndexSet:").m<&is_count>("count", "I@:").m<&is_first>("firstIndex", "I@:")
      .m<&is_last>("lastIndex", "I@:").m<&is_contains>("containsIndex:", "c@:I").m<&is_containsRange>("containsIndexesInRange:", "c@:{_NSRange=II}")
      .m<&is_containsSet>("containsIndexes:", "c@:@").m<&is_greaterThan>("indexGreaterThanIndex:", "I@:I")
      .m<&is_greaterOrEqual>("indexGreaterThanOrEqualToIndex:", "I@:I").m<&is_lessThan>("indexLessThanIndex:", "I@:I")
      .m<&is_lessOrEqual>("indexLessThanOrEqualToIndex:", "I@:I").m<&is_enumerate>("enumerateIndexesUsingBlock:", "v@:@?")
      .m<&is_getIndexes>("getIndexes:maxCount:inIndexRange:", "I@:^II^{_NSRange=II}")
      .m<&is_copy>("copyWithZone:").m<&is_mutableCopy>("mutableCopyWithZone:").m<&is_isEqual>("isEqual:", "c@:@")
      .m<&is_isEqual>("isEqualToIndexSet:", "c@:@").m<&obj_dealloc>("dealloc", "v@:");
    mis.m<&mis_add>("addIndex:", "v@:I").m<&mis_remove>("removeIndex:", "v@:I").m<&mis_addRange>("addIndexesInRange:", "v@:{_NSRange=II}")
       .m<&mis_removeRange>("removeIndexesInRange:", "v@:{_NSRange=II}").m<&mis_addSet>("addIndexes:", "v@:@")
       .m<&mis_removeSet>("removeIndexes:", "v@:@").m<&mis_removeAll>("removeAllIndexes", "v@:")
       .m<&mis_shift>("shiftIndexesStartingAtIndex:by:", "v@:Ii");

    ObjC::ClassBuilder uu = r.defineClass("NSUUID", "NSObject");
    m.uuid = uu.cls();
    uu.cm<&uuidc_uuid>("UUID");
    uu.m<&uuid_init>("init").m<&uuid_initWithString>("initWithUUIDString:").m<&uuid_initWithBytes>("initWithUUIDBytes:")
      .m<&uuid_string>("UUIDString").m<&uuid_getBytes>("getUUIDBytes:", "v@:^C").m<&uuid_isEqual>("isEqual:", "c@:@")
      .m<&uuid_hash>("hash", "I@:").m<&uuid_string>("description").m<&retainedCopy>("copyWithZone:").m<&obj_dealloc>("dealloc", "v@:");

    ObjC::ClassBuilder loc = r.defineClass("NSLocale", "NSObject");
    m.locale = loc.cls();
    loc.cm<&loc_current>("currentLocale").cm<&loc_current>("systemLocale").cm<&loc_current>("autoupdatingCurrentLocale")
       .cm<&locc_preferredLanguages>("preferredLanguages").cm<&locc_available>("availableLocaleIdentifiers")
       .cm<&locc_withIdentifier>("localeWithLocaleIdentifier:");
    loc.m<&loc_identifier>("localeIdentifier").m<&loc_objectForKey>("objectForKey:").m<&loc_initWithIdentifier>("initWithLocaleIdentifier:")
       .m<&loc_displayName>("displayNameForKey:value:").m<&retainedCopy>("copyWithZone:");
    for (const char* n : {"NSLocaleCountryCode", "NSLocaleLanguageCode", "NSLocaleCurrencyCode", "NSLocaleCurrencySymbol",
                          "NSLocaleDecimalSeparator", "NSLocaleGroupingSeparator", "NSLocaleUsesMetricSystem", "NSLocaleIdentifier",
                          "NSCurrentLocaleDidChangeNotification"}) {
        const uint32_t cell = p.defineDataBlock(std::string("_") + n, 4);
        p.mem().write<uint32_t>(cell, constString(p, n));
    }
    ObjC::ClassBuilder tz = r.defineClass("NSTimeZone", "NSObject");
    m.timeZone = tz.cls();
    tz.cm<&tz_local>("localTimeZone").cm<&tz_local>("systemTimeZone").cm<&tz_local>("defaultTimeZone")
      .cm<&tzc_withName>("timeZoneWithName:").cm<&tzc_withName>("timeZoneWithAbbreviation:");
    tz.m<&tz_secondsFromGMT>("secondsFromGMT", "i@:").m<&tz_secondsFromGMTForDate>("secondsFromGMTForDate:", "i@:@")
      .m<&tz_name>("name").m<&tz_abbreviation>("abbreviation").m<&tz_isDST>("isDaylightSavingTime", "c@:")
      .m<&retainedCopy>("copyWithZone:");

    p.def<&f_stringFromClass>("_NSStringFromClass");
    p.def<&f_classFromString>("_NSClassFromString");
    p.def<&f_stringFromSelector>("_NSStringFromSelector");
    p.def<&f_selectorFromString>("_NSSelectorFromString");
    p.def<&f_stringFromProtocol>("_NSStringFromProtocol");
    p.def<&f_protocolFromString>("_NSProtocolFromString");
    p.def<&f_stringFromRange>("_NSStringFromRange");
    p.def<&f_rangeFromString>("_NSRangeFromString");
    p.def<&f_unionRange>("_NSUnionRange");
    p.def<&f_intersectionRange>("_NSIntersectionRange");
    p.def<&f_allocateObject>("_NSAllocateObject");
    p.def<&f_deallocateObject>("_NSDeallocateObject");
    p.def<&f_copyObject>("_NSCopyObject");
    p.def<&f_defaultZone>("_NSDefaultMallocZone");
    p.def<&f_defaultZone>("_NSCreateZone");
    p.def<&f_zoneMalloc>("_NSZoneMalloc");
    p.def<&f_zoneCalloc>("_NSZoneCalloc");
    p.def<&f_zoneRealloc>("_NSZoneRealloc");
    p.def<&f_zoneFree>("_NSZoneFree");
    p.def<&f_zoneFromPointer>("_NSZoneFromPointer");
    p.def<&f_incExtra>("_NSIncrementExtraRefCount");
    p.def<&f_decExtraWasZero>("_NSDecrementExtraRefCountWasZero");
    p.def<&f_extraRefCount>("_NSExtraRefCount");
    p.def<&f_shouldRetainWithZone>("_NSShouldRetainWithZone");
    p.def<&f_pageSize>("_NSPageSize");
    p.def<&f_pageSize>("_NSLogPageSize");
    p.def<&f_roundUpToPage>("_NSRoundUpToMultipleOfPageSize");
    p.def<&f_roundDownToPage>("_NSRoundDownToMultipleOfPageSize");
    p.def<&f_allocateMemoryPages>("_NSAllocateMemoryPages");
    p.def<&f_deallocateMemoryPages>("_NSDeallocateMemoryPages");
    p.def<&f_realMemory>("_NSRealMemoryAvailable");
    p.def<&f_fullUserName>("_NSFullUserName");
    p.def<&f_userName>("_NSUserName");
    // NSFoundationVersionNumber (double): iOS 6.1 = 993.00
    const uint32_t ver = p.defineDataBlock("_NSFoundationVersionNumber", 8);
    p.mem().write<double>(ver, 993.0);
}

} // namespace Hydrogen::Runtime::Foundation
