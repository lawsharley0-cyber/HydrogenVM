// HydrogenVM — NSValue, NSNumber, CFBoolean.
#include "Foundation.hpp"
#include <cmath>

namespace Hydrogen::Runtime::Foundation {
namespace {

struct NumberData : ObjC::HostObject {
    char type = 'i';      // i q d f c B (objCType)
    int64_t i = 0;
    double d = 0;
    bool isFloat() const { return type == 'd' || type == 'f'; }
    double asDouble() const { return isFloat() ? d : double(i); }
    int64_t asInt() const { return isFloat() ? int64_t(d) : i; }
};
struct ValueData : ObjC::HostObject {
    std::vector<uint8_t> bytes;
    std::string type;
};

struct Numbers {
    explicit Numbers(Process&) {}
    Class nsValue = 0, nsNumber = 0, cfNumber = 0, cfBoolean = 0, concreteValue = 0;
    id yes = 0, no = 0;
};
Numbers& N(Ctx& x) { return x.proc.service<Numbers>(); }

NumberData* nd(Ctx& x, id n) { return rt(x).hostIf<NumberData>(n); }

id newNumber(Ctx& x, char type, int64_t i, double d, bool owned) {
    const id n = rt(x).createInstance(N(x).cfNumber);
    auto data = std::make_unique<NumberData>();
    data->type = type; data->i = i; data->d = d;
    rt(x).setHost(n, std::move(data));
    return owned ? n : autoreleased(x, n);
}

std::string numberDescription(const NumberData& n) {
    char buf[64];
    switch (n.type) {
        case 'd': std::snprintf(buf, sizeof buf, "%.15g", n.d); break;
        case 'f': std::snprintf(buf, sizeof buf, "%.7g", n.d); break;
        default: std::snprintf(buf, sizeof buf, "%lld", (long long)n.i); break;
    }
    return buf;
}

// allocation: [NSNumber alloc] gives a fresh concrete number
id n_allocWithZone(Ctx& x, id self, SEL, uint32_t) {
    if (self == N(x).nsNumber || self == N(x).cfNumber) {
        const id n = rt(x).createInstance(N(x).cfNumber);
        rt(x).setHost(n, std::make_unique<NumberData>());
        return n;
    }
    if (self == N(x).nsValue) {
        const id v = rt(x).createInstance(N(x).concreteValue);
        rt(x).setHost(v, std::make_unique<ValueData>());
        return v;
    }
    return rt(x).createInstance(self);
}
id n_alloc(Ctx& x, id self, SEL s) { return n_allocWithZone(x, self, s, 0); }

template <char T, typename V>
id initNumber(Ctx& x, id self, V v) {
    NumberData* d = nd(x, self);
    if (!d) return self;
    d->type = T;
    if constexpr (std::is_floating_point_v<V>) { d->d = double(v); d->i = 0; }
    else { d->i = int64_t(v); d->d = 0; }
    return self;
}
id i_int(Ctx& x, id self, SEL, int32_t v) { return initNumber<'i'>(x, self, v); }
id i_uint(Ctx& x, id self, SEL, uint32_t v) { return initNumber<'q'>(x, self, int64_t(v)); }
id i_ll(Ctx& x, id self, SEL, int64_t v) { return initNumber<'q'>(x, self, v); }
id i_ull(Ctx& x, id self, SEL, uint64_t v) { return initNumber<'q'>(x, self, int64_t(v)); }
id i_float(Ctx& x, id self, SEL, float v) { return initNumber<'f'>(x, self, v); }
id i_double(Ctx& x, id self, SEL, double v) { return initNumber<'d'>(x, self, v); }
id i_char(Ctx& x, id self, SEL, int32_t v) { return initNumber<'c'>(x, self, int8_t(v)); }
id i_short(Ctx& x, id self, SEL, int32_t v) { return initNumber<'i'>(x, self, int16_t(v)); }
id i_bool(Ctx& x, id self, SEL, int32_t v) {
    // Like Cocoa, [NSNumber numberWithBool:] returns the shared boolean singletons.
    rt(x).release(x.cpu, self);
    return rt(x).retain(x.cpu, (v & 0xFF) ? N(x).yes : N(x).no);
}

id c_int(Ctx& x, id, SEL, int32_t v) { return newNumber(x, 'i', v, 0, false); }
id c_uint(Ctx& x, id, SEL, uint32_t v) { return newNumber(x, 'q', int64_t(v), 0, false); }
id c_ll(Ctx& x, id, SEL, int64_t v) { return newNumber(x, 'q', v, 0, false); }
id c_ull(Ctx& x, id, SEL, uint64_t v) { return newNumber(x, 'q', int64_t(v), 0, false); }
id c_float(Ctx& x, id, SEL, float v) { return newNumber(x, 'f', 0, double(v), false); }
id c_double(Ctx& x, id, SEL, double v) { return newNumber(x, 'd', 0, v, false); }
id c_char(Ctx& x, id, SEL, int32_t v) { return newNumber(x, 'c', int8_t(v), 0, false); }
id c_uchar(Ctx& x, id, SEL, int32_t v) { return newNumber(x, 'i', uint8_t(v), 0, false); }
id c_short(Ctx& x, id, SEL, int32_t v) { return newNumber(x, 'i', int16_t(v), 0, false); }
id c_ushort(Ctx& x, id, SEL, int32_t v) { return newNumber(x, 'i', uint16_t(v), 0, false); }
id c_bool(Ctx& x, id, SEL, int32_t v) { return (v & 0xFF) ? N(x).yes : N(x).no; }

NumberData numOf(Ctx& x, id self) {
    if (NumberData* d = nd(x, self)) return *d;
    NumberData z;
    return z;
}
int32_t v_int(Ctx& x, id self, SEL) { return int32_t(numOf(x, self).asInt()); }
uint32_t v_uint(Ctx& x, id self, SEL) { return uint32_t(numOf(x, self).asInt()); }
int64_t v_ll(Ctx& x, id self, SEL) { return numOf(x, self).asInt(); }
float v_float(Ctx& x, id self, SEL) { return float(numOf(x, self).asDouble()); }
double v_double(Ctx& x, id self, SEL) { return numOf(x, self).asDouble(); }
bool v_bool(Ctx& x, id self, SEL) { const auto n = numOf(x, self); return n.isFloat() ? n.d != 0 : n.i != 0; }
int32_t v_char(Ctx& x, id self, SEL) { return int8_t(numOf(x, self).asInt()); }
int32_t v_short(Ctx& x, id self, SEL) { return int16_t(numOf(x, self).asInt()); }
uint32_t v_objCType(Ctx& x, id self, SEL) { const char t[2] = {numOf(x, self).type, 0}; return x.proc.internCString(t); }
id v_stringValue(Ctx& x, id self, SEL) { return makeString(x, numberDescription(numOf(x, self))); }
int32_t v_compare(Ctx& x, id self, SEL, id other) {
    const auto a = numOf(x, self), b = numOf(x, other);
    if (a.isFloat() || b.isFloat()) { const double p = a.asDouble(), q = b.asDouble(); return p < q ? -1 : p > q ? 1 : 0; }
    return a.i < b.i ? -1 : a.i > b.i ? 1 : 0;
}
bool v_isEqualToNumber(Ctx& x, id self, SEL s, id other) { return other && isNumber(x, other) && v_compare(x, self, s, other) == 0; }
bool v_isEqual(Ctx& x, id self, SEL s, id other) { return other == self || (other && isNumber(x, other) && v_compare(x, self, s, other) == 0); }
uint32_t v_hash(Ctx& x, id self, SEL) { const auto n = numOf(x, self); return n.isFloat() && n.d != std::floor(n.d) ? uint32_t(n.d * 2654435761.0) : uint32_t(n.asInt()); }
id v_copy(Ctx& x, id self, SEL, uint32_t) { return rt(x).retain(x.cpu, self); }
id v_copyNoZone(Ctx& x, id self, SEL) { return rt(x).retain(x.cpu, self); }
id b_self(Ctx&, id self, SEL) { return self; }
void b_noop(Ctx&, id, SEL) {}
id b_desc(Ctx& x, id self, SEL) { return makeString(x, numOf(x, self).i ? "1" : "0"); }

// ----------------------------------------------------------- NSValue ---

ValueData* vd(Ctx& x, id v) { return rt(x).hostIf<ValueData>(v); }
id newValue(Ctx& x, const void* b, uint32_t n, const std::string& type, bool owned) {
    const id v = rt(x).createInstance(N(x).concreteValue);
    auto d = std::make_unique<ValueData>();
    d->bytes.assign(static_cast<const uint8_t*>(b), static_cast<const uint8_t*>(b) + n);
    d->type = type;
    rt(x).setHost(v, std::move(d));
    return owned ? v : autoreleased(x, v);
}
uint32_t typeSize(const std::string& t) {
    // Enough for the encodings apps box: scalars, pointers and simple structs.
    uint32_t size = 0;
    int depth = 0;
    for (size_t i = 0; i < t.size(); ++i) {
        const char c = t[i];
        if (c == '{') { ++depth; const size_t eq = t.find('=', i); if (eq != std::string::npos) i = eq; continue; }
        if (c == '}') { --depth; continue; }
        switch (c) {
            case 'c': case 'C': case 'B': size += 1; break;
            case 's': case 'S': size = (size + 1) & ~1u; size += 2; break;
            case 'q': case 'Q': case 'd': size = (size + 3) & ~3u; size += 8; break;
            case 'i': case 'I': case 'l': case 'L': case 'f': case '@': case '#': case ':': case '*': case '^':
                size = (size + 3) & ~3u; size += 4;
                if (c == '^' && i + 1 < t.size()) { ++i; if (t[i] == '{') { int d2 = 1; while (++i < t.size() && d2) { if (t[i] == '{') ++d2; else if (t[i] == '}') --d2; } --i; } }
                break;
            default: break;
        }
    }
    return std::max<uint32_t>((size + 3) & ~3u, 4);
}
id val_valueWithBytes(Ctx& x, id, SEL, GuestPtr bytes, GuestPtr type) {
    const std::string t = x.proc.readCString(type);
    const uint32_t n = typeSize(t);
    std::vector<uint8_t> b(n);
    std::memcpy(b.data(), x.mem.host(bytes), n);
    return newValue(x, b.data(), n, t, false);
}
id val_initWithBytes(Ctx& x, id self, SEL, GuestPtr bytes, GuestPtr type) {
    ValueData* d = vd(x, self);
    if (!d) return self;
    d->type = x.proc.readCString(type);
    const uint32_t n = typeSize(d->type);
    d->bytes.assign(x.mem.host(bytes), x.mem.host(bytes) + n);
    return self;
}
id val_valueWithPointer(Ctx& x, id, SEL, GuestPtr p) { return newValue(x, &p, 4, "^v", false); }
id val_valueWithObject(Ctx& x, id, SEL, id o) { return newValue(x, &o, 4, "@", false); }
id val_valueWithRange(Ctx& x, id, SEL, uint32_t loc, uint32_t len) { const uint32_t r[2] = {loc, len}; return newValue(x, r, 8, "{_NSRange=II}", false); }
uint32_t val_word(Ctx& x, id self, SEL) { ValueData* d = vd(x, self); uint32_t v = 0; if (d && d->bytes.size() >= 4) std::memcpy(&v, d->bytes.data(), 4); return v; }
void val_rangeValue(Ctx& x, GuestPtr out, id self, SEL) {
    ValueData* d = vd(x, self);
    uint32_t r[2] = {0, 0};
    if (d && d->bytes.size() >= 8) std::memcpy(r, d->bytes.data(), 8);
    writeRange(x, out, {r[0], r[1]});
}
void val_getValue(Ctx& x, id self, SEL, GuestPtr out) { if (ValueData* d = vd(x, self)) x.mem.copyIn(out, d->bytes.data(), d->bytes.size()); }
uint32_t val_objCType(Ctx& x, id self, SEL) { ValueData* d = vd(x, self); return x.proc.internCString(d ? d->type : ""); }
bool val_isEqual(Ctx& x, id self, SEL, id o) {
    if (o == self) return true;
    ValueData *a = vd(x, self), *b = o ? vd(x, o) : nullptr;
    return a && b && a->type == b->type && a->bytes == b->bytes;
}
uint32_t val_hash(Ctx& x, id self, SEL) {
    ValueData* d = vd(x, self);
    uint32_t h = 2166136261u;
    if (d) for (uint8_t b : d->bytes) h = (h ^ b) * 16777619u;
    return h;
}
id val_description(Ctx& x, id self, SEL) {
    ValueData* d = vd(x, self);
    std::string s = "<";
    char buf[8];
    if (d) for (size_t i = 0; i < d->bytes.size(); ++i) { std::snprintf(buf, sizeof buf, "%02x", d->bytes[i]); s += buf; if (i % 4 == 3 && i + 1 < d->bytes.size()) s += ' '; }
    return makeString(x, s + ">");
}

void num_dealloc(Ctx& x, id self, SEL) { rt(x).disposeInstance(x.cpu, self); }

} // namespace

id makeNumberInt(Ctx& x, int64_t v, bool owned) { return newNumber(x, v >= INT32_MIN && v <= INT32_MAX ? 'i' : 'q', v, 0, owned); }
id makeNumberDouble(Ctx& x, double v, bool owned) { return newNumber(x, 'd', 0, v, owned); }
id makeNumberFloat(Ctx& x, float v, bool owned) { return newNumber(x, 'f', 0, double(v), owned); }
id makeBool(Ctx& x, bool v) { return v ? N(x).yes : N(x).no; }
bool isNumber(Ctx& x, id obj) { return obj && rt(x).isKindOf(obj, N(x).nsNumber); }
double numberDouble(Ctx& x, id n) {
    if (NumberData* d = nd(x, n)) return d->asDouble();
    if (!n) return 0;
    const uint64_t bits = rt(x).send(x.cpu, n, "doubleValue");
    double d;
    std::memcpy(&d, &bits, 8);
    return d;
}
int64_t numberInt(Ctx& x, id n) {
    if (NumberData* d = nd(x, n)) return d->asInt();
    return n ? int64_t(rt(x).send(x.cpu, n, "longLongValue")) : 0;
}
id makeValue(Ctx& x, const void* bytes, uint32_t size, const std::string& type, bool owned) { return newValue(x, bytes, size, type, owned); }

// Used by the plist / description code.
std::string numberToString(Ctx& x, id n) { NumberData* d = nd(x, n); return d ? numberDescription(*d) : "0"; }
char numberType(Ctx& x, id n) { NumberData* d = nd(x, n); return d ? d->type : 'i'; }
bool isBoolean(Ctx& x, id n) { return n && (n == N(x).yes || n == N(x).no); }

void registerNumbers(Process& p, ObjC::Runtime& r) {
    auto& st = p.service<Numbers>();
    ObjC::ClassBuilder val = r.defineClass("NSValue", "NSObject");
    ObjC::ClassBuilder num = r.defineClass("NSNumber", "NSValue");
    ObjC::ClassBuilder cfn = r.defineClass("__NSCFNumber", "NSNumber");
    ObjC::ClassBuilder cfb = r.defineClass("__NSCFBoolean", "NSNumber");
    ObjC::ClassBuilder cv = r.defineClass("NSConcreteValue", "NSValue");
    st.nsValue = val.cls(); st.nsNumber = num.cls(); st.cfNumber = cfn.cls(); st.cfBoolean = cfb.cls(); st.concreteValue = cv.cls();

    val.cm<&n_alloc>("alloc").cm<&n_allocWithZone>("allocWithZone:")
       .cm<&val_valueWithBytes>("valueWithBytes:objCType:").cm<&val_valueWithBytes>("value:withObjCType:")
       .cm<&val_valueWithPointer>("valueWithPointer:").cm<&val_valueWithObject>("valueWithNonretainedObject:")
       .cm<&val_valueWithRange>("valueWithRange:");
    val.m<&val_initWithBytes>("initWithBytes:objCType:").m<&val_word>("pointerValue", "^v@:").m<&val_word>("nonretainedObjectValue")
       .m<&val_rangeValue>("rangeValue", "{_NSRange=II}@:").m<&val_getValue>("getValue:", "v@:^v").m<&val_objCType>("objCType", "r*@:")
       .m<&val_isEqual>("isEqual:", "c@:@").m<&val_isEqual>("isEqualToValue:", "c@:@").m<&val_hash>("hash", "I@:")
       .m<&val_description>("description").m<&v_copy>("copyWithZone:").m<&v_copyNoZone>("copy");
    cv.m<&num_dealloc>("dealloc", "v@:");

    num.cm<&n_alloc>("alloc").cm<&n_allocWithZone>("allocWithZone:")
       .cm<&c_int>("numberWithInt:").cm<&c_int>("numberWithInteger:").cm<&c_int>("numberWithLong:")
       .cm<&c_uint>("numberWithUnsignedInt:").cm<&c_uint>("numberWithUnsignedInteger:").cm<&c_uint>("numberWithUnsignedLong:")
       .cm<&c_ll>("numberWithLongLong:").cm<&c_ull>("numberWithUnsignedLongLong:")
       .cm<&c_float>("numberWithFloat:").cm<&c_double>("numberWithDouble:").cm<&c_bool>("numberWithBool:")
       .cm<&c_char>("numberWithChar:").cm<&c_uchar>("numberWithUnsignedChar:")
       .cm<&c_short>("numberWithShort:").cm<&c_ushort>("numberWithUnsignedShort:");
    num.m<&i_int>("initWithInt:").m<&i_int>("initWithInteger:").m<&i_int>("initWithLong:")
       .m<&i_uint>("initWithUnsignedInt:").m<&i_uint>("initWithUnsignedInteger:").m<&i_ll>("initWithLongLong:")
       .m<&i_ull>("initWithUnsignedLongLong:").m<&i_float>("initWithFloat:").m<&i_double>("initWithDouble:")
       .m<&i_bool>("initWithBool:").m<&i_char>("initWithChar:").m<&i_short>("initWithShort:")
       .m<&v_int>("intValue", "i@:").m<&v_int>("integerValue", "i@:").m<&v_int>("longValue", "l@:")
       .m<&v_uint>("unsignedIntValue", "I@:").m<&v_uint>("unsignedIntegerValue", "I@:").m<&v_uint>("unsignedLongValue", "L@:")
       .m<&v_ll>("longLongValue", "q@:").m<&v_ll>("unsignedLongLongValue", "Q@:")
       .m<&v_float>("floatValue", "f@:").m<&v_double>("doubleValue", "d@:").m<&v_bool>("boolValue", "c@:")
       .m<&v_char>("charValue", "c@:").m<&v_char>("unsignedCharValue", "C@:").m<&v_short>("shortValue", "s@:")
       .m<&v_short>("unsignedShortValue", "S@:").m<&v_objCType>("objCType", "r*@:")
       .m<&v_stringValue>("stringValue").m<&v_stringValue>("description")
       .m<&v_compare>("compare:", "i@:@").m<&v_isEqualToNumber>("isEqualToNumber:", "c@:@")
       .m<&v_isEqual>("isEqual:", "c@:@").m<&v_hash>("hash", "I@:");
    cfn.m<&num_dealloc>("dealloc", "v@:");
    cfb.m<&b_self>("retain").m<&b_noop>("release", "v@:").m<&b_self>("autorelease").m<&b_desc>("description");

    // The two CFBoolean singletons.
    auto makeB = [&](bool v) {
        const id b = r.createInstance(cfb.cls());
        auto d = std::make_unique<NumberData>();
        d->type = 'c'; d->i = v;
        r.setHost(b, std::move(d));
        r.makeImmortal(b);
        return b;
    };
    st.yes = makeB(true);
    st.no = makeB(false);
    p.mem().write<uint32_t>(p.defineDataBlock("_kCFBooleanTrue", 4), st.yes);
    p.mem().write<uint32_t>(p.defineDataBlock("_kCFBooleanFalse", 4), st.no);
}

} // namespace Hydrogen::Runtime::Foundation
