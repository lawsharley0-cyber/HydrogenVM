// HydrogenVM — CoreFoundation over the Foundation objects (toll-free
// bridging): CFType, CFString, CFNumber/CFBoolean, CFArray, CFDictionary,
// CFSet, CFData, CFDate & Gregorian dates, CFTimeZone/CFLocale basics,
// CFUUID, CFPropertyList, CFAllocator, CFCharacterSet.
//
// CF collections created with NULL callbacks hold raw pointers, not objects;
// those get small host-side "raw" collections that never retain.
#include "Foundation.hpp"
#include <cctype>
#include "runtime/libc/Format.hpp"
#include <cmath>
#include <ctime>

namespace Hydrogen::Runtime::Foundation {

namespace {

using U16 = std::u16string;

struct BitVector : ObjC::HostObject { std::vector<uint8_t> bytes; uint32_t count = 0; };
struct RawArray : ObjC::HostObject { std::vector<uint32_t> items; bool retains = false; };
struct Allocator : ObjC::HostObject { std::array<uint32_t, 9> context{}; bool null = false; };
struct RawDict : ObjC::HostObject {
    std::vector<std::pair<uint32_t, uint32_t>> entries;
    bool keyObjects = false, valueObjects = false, keyEqual = false;
};

struct CF {
    explicit CF(Process&) {}
    Class rawArray = 0, rawDict = 0, allocator = 0, bitVector = 0;
    id systemAllocator = 0, nullAllocator = 0;
    uint32_t arrayCallbacks = 0, dictKeyCallbacks = 0, dictValueCallbacks = 0, copyStringKeyCallbacks = 0, setCallbacks = 0,
             copyStringSetCallbacks = 0;
    uint32_t cfRetainFn = 0, cfReleaseFn = 0, cfEqualFn = 0, cfHashFn = 0, cfCopyDescFn = 0;
};
CF& cf(Ctx& x) { return x.proc.service<CF>(); }

auto& R(Ctx& x) { return rt(x); }
uint64_t send(Ctx& x, id o, const char* sel, std::initializer_list<uint32_t> a = {}) { return R(x).send(x.cpu, o, sel, a); }

BitVector* bits(Ctx& x, id object) { return R(x).hostIf<BitVector>(object); }
id bv_mutable(Ctx& x, id, int32_t capacity) {
    if (capacity < 0 || capacity > (1 << 27)) return 0;
    const id object = R(x).createInstance(cf(x).bitVector);
    R(x).setHost(object, std::make_unique<BitVector>());
    return object;
}
void bv_setCount(Ctx& x, id object, int32_t count) {
    auto* b = bits(x, object);
    if (!b || count < 0 || count > (1 << 27)) return;
    b->bytes.resize((uint32_t(count) + 7) / 8, 0); b->count = uint32_t(count);
    if ((count & 7) && !b->bytes.empty()) b->bytes.back() &= uint8_t(0xff << (8 - (count & 7)));
}
id bv_create(Ctx& x, id allocator, GuestPtr bytes, int32_t count) {
    if (count < 0 || count > (1 << 27) || (count && !bytes)) return 0;
    const id object = bv_mutable(x, allocator, count);
    bv_setCount(x, object, count);
    if (count) x.mem.copyOut(bits(x, object)->bytes.data(), bytes, (uint32_t(count) + 7) / 8);
    if ((count & 7) && count) bits(x, object)->bytes.back() &= uint8_t(0xff << (8 - (count & 7)));
    return object;
}
id bv_copy(Ctx& x, id allocator, int32_t capacity, id source) {
    auto* b = bits(x, source);
    if (!b) return 0;
    const id object = bv_mutable(x, allocator, capacity);
    if (object) *bits(x, object) = *b;
    return object;
}
uint32_t bv_count(Ctx& x, id object) { auto* b = bits(x, object); return b ? b->count : 0; }
uint32_t bv_get(Ctx& x, id object, int32_t index) {
    auto* b = bits(x, object);
    return b && index >= 0 && uint32_t(index) < b->count ? (b->bytes[index / 8] >> (7 - (index & 7))) & 1 : 0;
}
void bv_set(Ctx& x, id object, int32_t index, uint32_t value) {
    auto* b = bits(x, object);
    if (!b || index < 0 || uint32_t(index) >= b->count) return;
    const uint8_t mask = uint8_t(1 << (7 - (index & 7)));
    b->bytes[index / 8] = (b->bytes[index / 8] & ~mask) | (value ? mask : 0);
}

// ------------------------------------------------------------- CFType ---

enum TypeID : uint32_t {
    kTypeNull = 16, kTypeSet = 17, kTypeDict = 18, kTypeArray = 19, kTypeData = 20, kTypeBool = 21, kTypeNumber = 22,
    kTypeString = 7, kTypeDate = 42, kTypeURL = 29, kTypeBundle = 31, kTypeRunLoop = 43, kTypeTimer = 44, kTypeUUID = 45,
    kTypeTimeZone = 46, kTypeLocale = 47, kTypeCharSet = 48, kTypeAllocator = 2, kTypeBitVector = 49, kTypeOther = 1,
};

uint32_t cf_retain(Ctx& x, id o) {
    if (!o) { x.proc.log(LogLevel::Warning, "CFRetain(NULL) — this crashes on iOS"); return 0; }
    if (!x.mem.read<uint32_t>(o)) return o;
    return R(x).retain(x.cpu, o);
}
void cf_release(Ctx& x, id o) {
    if (!o) { x.proc.log(LogLevel::Warning, "CFRelease(NULL) — this crashes on iOS"); return; }
    if (!x.mem.read<uint32_t>(o)) return;
    R(x).release(x.cpu, o);
}
uint32_t cf_autorelease(Ctx& x, id o) { return (o && x.mem.read<uint32_t>(o)) ? R(x).autorelease(x.cpu, o) : o; }
uint32_t cf_makeCollectable(Ctx&, id o) { return o; }
int32_t cf_retainCount(Ctx& x, id o) { return int32_t(send(x, o, "retainCount")); }
bool cf_equal(Ctx& x, id a, id b) { return objectsEqual(x, a, b); }
uint32_t cf_hash(Ctx& x, id o) { return objectHash(x, o); }
uint32_t cf_copyDescription(Ctx& x, id o) { return makeString(x, describe(x, o), true); }
void cf_show(Ctx& x, id o) { x.proc.log(LogLevel::GuestStderr, describe(x, o) + "\n"); }
uint32_t cf_getAllocator(Ctx&, id) { return 0; }

uint32_t typeOf(Ctx& x, id o) {
    if (!o) return 0;
    auto& r = R(x);
    auto is = [&](const char* n) { const Class c = r.getClass(n); return c && r.isKindOf(o, c); };
    if (isString(x, o)) return kTypeString;
    if (isBoolean(x, o)) return kTypeBool;
    if (isNumber(x, o)) return kTypeNumber;
    if (isArray(x, o) || r.hostIf<RawArray>(o)) return kTypeArray;
    if (isDict(x, o) || r.hostIf<RawDict>(o)) return kTypeDict;
    if (isData(x, o)) return kTypeData;
    if (is("NSSet")) return kTypeSet;
    if (is("NSDate")) return kTypeDate;
    if (isURL(x, o)) return kTypeURL;
    if (is("NSNull")) return kTypeNull;
    if (is("NSBundle")) return kTypeBundle;
    if (is("NSRunLoop")) return kTypeRunLoop;
    if (is("NSTimer")) return kTypeTimer;
    if (is("NSUUID")) return kTypeUUID;
    if (is("NSTimeZone")) return kTypeTimeZone;
    if (is("NSLocale")) return kTypeLocale;
    if (is("NSCharacterSet")) return kTypeCharSet;
    if (r.hostIf<Allocator>(o)) return kTypeAllocator;
    if (r.hostIf<BitVector>(o)) return kTypeBitVector;
    return kTypeOther;
}
uint32_t cf_getTypeID(Ctx& x, id o) { return typeOf(x, o); }
template <uint32_t T> uint32_t cf_typeID(Ctx&) { return T; }
uint32_t cf_copyTypeIDDescription(Ctx& x, uint32_t t) {
    const char* n = t == kTypeString ? "CFString" : t == kTypeArray ? "CFArray" : t == kTypeDict ? "CFDictionary" :
                    t == kTypeNumber ? "CFNumber" : t == kTypeBool ? "CFBoolean" : t == kTypeData ? "CFData" : t == kTypeDate ? "CFDate" : "CFType";
    return makeString(x, n, true);
}

// ------------------------------------------------------------ CFString ---

// CFStringEncoding <-> NSStringEncoding
uint32_t toNS(uint32_t e) {
    switch (e) {
        case 0x0600: return kNSASCII;
        case 0x08000100: return kNSUTF8;
        case 0x0201: return kNSISOLatin1;
        case 0x0100: return kNSUnicode;
        case 0x10000100: return kNSUTF16BE;
        case 0x14000100: return kNSUTF16LE;
        case 0x0C000100: return kNSUTF32;
        case 0x0500: return kNSWindowsCP1252;
        case 0: return kNSMacOSRoman;
        case 0x0BFF: return kNSNonLossyASCII;
        case 0x0A01: return kNSShiftJIS;
        case 0x0920: return kNSJapaneseEUC;
        default: return e;
    }
}
uint32_t toCF(uint32_t e) {
    switch (e) {
        case kNSASCII: return 0x0600;
        case kNSUTF8: return 0x08000100;
        case kNSISOLatin1: return 0x0201;
        case kNSUnicode: return 0x0100;
        case kNSUTF16BE: return 0x10000100;
        case kNSUTF16LE: return 0x14000100;
        case kNSUTF32: return 0x0C000100;
        case kNSWindowsCP1252: return 0x0500;
        case kNSMacOSRoman: return 0;
        default: return 0x08000100;
    }
}

uint32_t s_createWithCString(Ctx& x, uint32_t, GuestPtr c, uint32_t enc) {
    if (!c) return 0;
    const std::string raw = x.proc.readCString(c);
    bool ok = true;
    const std::string u = decodeBytes(reinterpret_cast<const uint8_t*>(raw.data()), raw.size(), toNS(enc), &ok);
    return ok ? makeString(x, u, true) : 0;
}
uint32_t s_createWithCStringNoCopy(Ctx& x, uint32_t a, GuestPtr c, uint32_t enc, uint32_t) { return s_createWithCString(x, a, c, enc); }
uint32_t s_createWithCharacters(Ctx& x, uint32_t, GuestPtr chars, int32_t n) {
    U16 s(size_t(std::max(n, 0)), u'\0');
    for (int32_t i = 0; i < n; ++i) s[size_t(i)] = x.mem.read<uint16_t>(chars + uint32_t(i) * 2);
    return makeString(x, s, true);
}
uint32_t s_createWithCharactersNoCopy(Ctx& x, uint32_t a, GuestPtr chars, int32_t n, uint32_t) { return s_createWithCharacters(x, a, chars, n); }
uint32_t s_createWithBytes(Ctx& x, uint32_t, GuestPtr b, int32_t n, uint32_t enc, bool) {
    bool ok = true;
    const std::string u = decodeBytes(n > 0 ? x.mem.host(b) : nullptr, size_t(std::max(n, 0)), toNS(enc), &ok);
    return ok ? makeString(x, u, true) : 0;
}
uint32_t s_createWithBytesNoCopy(Ctx& x, uint32_t a, GuestPtr b, int32_t n, uint32_t enc, bool ext, uint32_t) { return s_createWithBytes(x, a, b, n, enc, ext); }
uint32_t s_createWithFormat(Ctx& x, uint32_t, id, id fmt, VarArgs va) {
    return makeString(x, formatGuest(x.proc, utf8(x, fmt), va.args, objectFormatHook(x.proc)), true);
}
uint32_t s_createWithFormatArgs(Ctx& x, uint32_t, id, id fmt, VaList ap) {
    ArgReader a(x.mem, ap.ptr);
    return makeString(x, formatGuest(x.proc, utf8(x, fmt), a, objectFormatHook(x.proc)), true);
}
uint32_t s_createCopy(Ctx& x, uint32_t, id s) { return s ? makeString(x, stringValue(x, s), true) : 0; }
uint32_t s_createMutable(Ctx& x, uint32_t, int32_t) { return makeString(x, U16(), true, true); }
uint32_t s_createMutableCopy(Ctx& x, uint32_t, int32_t, id s) { return makeString(x, s ? stringValue(x, s) : U16(), true, true); }
uint32_t s_createWithSubstring(Ctx& x, uint32_t, id s, uint32_t loc, uint32_t len) {
    const U16 v = stringValue(x, s);
    return makeString(x, loc < v.size() ? v.substr(loc, len) : U16(), true);
}
int32_t s_getLength(Ctx& x, id s) { return s ? int32_t(stringValue(x, s).size()) : 0; }
uint32_t s_getCharacterAtIndex(Ctx& x, id s, int32_t i) { const U16 v = stringValue(x, s); return i >= 0 && size_t(i) < v.size() ? v[size_t(i)] : 0; }
void s_getCharacters(Ctx& x, id s, uint32_t loc, uint32_t len, GuestPtr buf) {
    const U16 v = stringValue(x, s);
    for (uint32_t i = 0; i < len && loc + i < v.size(); ++i) x.mem.write<uint16_t>(buf + i * 2, v[loc + i]);
}
bool s_getCString(Ctx& x, id s, GuestPtr buf, int32_t size, uint32_t enc) {
    if (!s || size <= 0) return false;
    bool ok = true;
    const std::string b = encodeBytes(stringValue(x, s), toNS(enc), false, &ok);
    if (!ok || int32_t(b.size()) + 1 > size) return false;
    std::memcpy(x.mem.host(buf), b.data(), b.size());
    x.mem.write<uint8_t>(buf + uint32_t(b.size()), 0);
    return true;
}
uint32_t s_getCStringPtr(Ctx& x, id s, uint32_t) {
    if (!s) return 0;
    return uint32_t(send(x, s, "UTF8String"));
}
uint32_t s_getCharactersPtr(Ctx&, id) { return 0; }
int32_t s_getBytes(Ctx& x, id s, uint32_t loc, uint32_t len, uint32_t enc, uint8_t lossByte, bool, GuestPtr buf, int32_t max, GuestPtr used) {
    const U16 v = stringValue(x, s);
    const U16 sub = loc < v.size() ? v.substr(loc, len) : U16();
    bool ok = true;
    int32_t converted = 0;
    std::string out;
    for (char16_t c : sub) {
        std::string b = encodeBytes(U16(1, c), toNS(enc), false, &ok);
        if (!ok) { if (!lossByte) break; b = std::string(1, char(lossByte)); ok = true; }
        if (buf && int32_t(out.size() + b.size()) > max) break;
        out += b;
        ++converted;
    }
    if (buf && !out.empty()) std::memcpy(x.mem.host(buf), out.data(), out.size());
    if (used) x.mem.write<int32_t>(used, int32_t(out.size()));
    return converted;
}
int32_t s_maxSize(Ctx&, int32_t len, uint32_t enc) {
    const uint32_t ns = toNS(enc);
    return ns == kNSUTF8 ? len * 3 : ns == kNSUnicode || ns == kNSUTF16BE || ns == kNSUTF16LE ? len * 2 : ns == kNSUTF32 ? len * 4 : len;
}
uint32_t s_systemEncoding(Ctx&) { return 0; }
uint32_t s_fastestEncoding(Ctx&, id) { return 0x08000100; }
uint32_t s_smallestEncoding(Ctx&, id) { return 0x08000100; }
uint32_t s_cfToNS(Ctx&, uint32_t e) { return toNS(e); }
uint32_t s_nsToCF(Ctx&, uint32_t e) { return toCF(e); }
// CFStringConvertIANACharSetNameToEncoding: kCFStringEncodingInvalidId (0xFFFFFFFF) when unknown.
uint32_t s_ianaToEncoding(Ctx& x, id name) {
    if (!name) return 0xFFFFFFFFu;
    std::string n = utf8(x, name);
    for (char& c : n) c = char(std::tolower(uint8_t(c)));
    static const std::pair<const char*, uint32_t> map[] = {
        {"utf-8", 0x08000100}, {"utf8", 0x08000100}, {"us-ascii", 0x0600}, {"ascii", 0x0600}, {"iso-8859-1", 0x0201},
        {"latin1", 0x0201}, {"iso-latin-1", 0x0201}, {"utf-16", 0x0100}, {"utf-16be", 0x10000100}, {"utf-16le", 0x14000100},
        {"utf-32", 0x0c000100}, {"windows-1252", 0x0500}, {"cp1252", 0x0500}, {"macintosh", 0}, {"x-mac-roman", 0},
        {"shift_jis", 0x0A01}, {"sjis", 0x0A01}, {"euc-jp", 0x0920}, {"iso-2022-jp", 0x0820}, {"iso-8859-2", 0x0202},
        {"windows-1250", 0x0501}, {"windows-1251", 0x0502}, {"gb2312", 0x0930}, {"big5", 0x0A03}, {"euc-kr", 0x0940}};
    for (auto& [k, v] : map) if (n == k) return v;
    return 0xFFFFFFFFu;
}
uint32_t s_encodingToIANA(Ctx& x, uint32_t e) { return constString(x.proc, toNS(e) == kNSUTF8 ? "utf-8" : toNS(e) == kNSASCII ? "us-ascii" : "iso-8859-1"); }
bool s_isEncodingAvailable(Ctx&, uint32_t) { return true; }

void s_append(Ctx& x, id s, id other) { if (s && other) send(x, s, "appendString:", {other}); }
void s_appendCString(Ctx& x, id s, GuestPtr c, uint32_t enc) {
    const uint32_t o = s_createWithCString(x, 0, c, enc);
    if (o) { s_append(x, s, o); R(x).release(x.cpu, o); }
}
void s_appendCharacters(Ctx& x, id s, GuestPtr chars, int32_t n) {
    const uint32_t o = s_createWithCharacters(x, 0, chars, n);
    s_append(x, s, o);
    R(x).release(x.cpu, o);
}
void s_appendFormat(Ctx& x, id s, id, id fmt, VarArgs va) {
    s_append(x, s, makeString(x, formatGuest(x.proc, utf8(x, fmt), va.args, objectFormatHook(x.proc))));
}
void s_insert(Ctx& x, id s, int32_t idx, id other) { send(x, s, "insertString:atIndex:", {other, uint32_t(idx)}); }
void s_delete(Ctx& x, id s, uint32_t loc, uint32_t len) { send(x, s, "deleteCharactersInRange:", {loc, len}); }
void s_replace(Ctx& x, id s, uint32_t loc, uint32_t len, id with) { send(x, s, "replaceCharactersInRange:withString:", {loc, len, with}); }
void s_replaceAll(Ctx& x, id s, id with) { send(x, s, "setString:", {with}); }
int32_t s_findAndReplace(Ctx& x, id s, id find, id repl, uint32_t loc, uint32_t len, uint32_t opts) {
    return int32_t(send(x, s, "replaceOccurrencesOfString:withString:options:range:", {find, repl, opts, loc, len}));
}
void s_lowercase(Ctx& x, id s, uint32_t) { send(x, s, "setString:", {uint32_t(send(x, s, "lowercaseString"))}); }
void s_uppercase(Ctx& x, id s, uint32_t) { send(x, s, "setString:", {uint32_t(send(x, s, "uppercaseString"))}); }
void s_capitalize(Ctx& x, id s, uint32_t) { send(x, s, "setString:", {uint32_t(send(x, s, "capitalizedString"))}); }
void s_trimWhitespace(Ctx& x, id s) {
    const id ws = id(send(x, R(x).getClass("NSCharacterSet"), "whitespaceAndNewlineCharacterSet"));
    send(x, s, "setString:", {uint32_t(send(x, s, "stringByTrimmingCharactersInSet:", {ws}))});
}
void s_trim(Ctx& x, id s, id chars) {
    U16 v = stringValue(x, s);
    const U16 t = stringValue(x, chars);
    if (t.empty()) return;
    while (v.size() >= t.size() && v.compare(0, t.size(), t) == 0) v.erase(0, t.size());
    while (v.size() >= t.size() && v.compare(v.size() - t.size(), t.size(), t) == 0) v.erase(v.size() - t.size());
    send(x, s, "setString:", {makeString(x, v)});
}
void s_pad(Ctx& x, id s, id pad, int32_t len, int32_t idx) {
    U16 v = stringValue(x, s);
    const U16 p = stringValue(x, pad);
    if (len < int32_t(v.size())) v.resize(size_t(std::max(len, 0)));
    else for (size_t i = 0; v.size() < size_t(len) && !p.empty(); ++i) v += p[(size_t(idx) + i) % p.size()];
    send(x, s, "setString:", {makeString(x, v)});
}

// CFComparisonResult CFStringCompare(a, b, flags): flags match NSStringCompareOptions for the common bits.
uint32_t cmpFlags(uint32_t f) {
    uint32_t o = 0;
    if (f & 1) o |= 1;       // case insensitive
    if (f & 4) o |= 4;       // backwards (unused for compare)
    if (f & 8) o |= 8;       // anchored
    if (f & 16) o |= 16;     // nonliteral
    if (f & 64) o |= 64;     // numerically
    if (f & 128) o |= 128;   // diacritic insensitive
    if (f & 256) o |= 256;   // width insensitive
    return o;
}
int32_t s_compare(Ctx& x, id a, id b, uint32_t flags) { return int32_t(send(x, a, "compare:options:", {b, cmpFlags(flags)})); }
int32_t s_compareRange(Ctx& x, id a, id b, uint32_t loc, uint32_t len, uint32_t flags) {
    return int32_t(send(x, a, "compare:options:range:", {b, cmpFlags(flags), loc, len}));
}
void s_find(Ctx& x, GuestPtr ret, id s, id find, uint32_t flags) {
    const U16 v = stringValue(x, s), f = stringValue(x, find);
    size_t pos = U16::npos;
    if (!f.empty()) {
        if (flags & 1) {
            auto low = [](U16 t) { for (auto& c : t) if (c >= 'A' && c <= 'Z') c = char16_t(c + 32); return t; };
            const U16 lv = low(v), lf = low(f);
            pos = (flags & 4) ? lv.rfind(lf) : lv.find(lf);
        } else {
            pos = (flags & 4) ? v.rfind(f) : v.find(f);
        }
        if ((flags & 8) && pos != U16::npos && pos != ((flags & 4) ? v.size() - f.size() : 0)) pos = U16::npos;
    }
    writeRange(x, ret, pos == U16::npos ? Range{kNotFound, 0} : Range{uint32_t(pos), uint32_t(f.size())});
}
bool s_findWithOptions(Ctx& x, id s, id find, uint32_t loc, uint32_t len, uint32_t flags, GuestPtr result) {
    const U16 v = stringValue(x, s);
    const id sub = makeString(x, loc < v.size() ? v.substr(loc, len) : U16());
    const uint32_t tmp = x.proc.heap().alloc(8);
    s_find(x, tmp, sub, find, flags);
    const uint32_t at = x.mem.read<uint32_t>(tmp), n = x.mem.read<uint32_t>(tmp + 4);
    x.proc.heap().free(tmp);
    if (at == kNotFound) return false;
    if (result) writeRange(x, result, {at + loc, n});
    return true;
}
bool s_hasPrefix(Ctx& x, id s, id p) { const U16 v = stringValue(x, s), q = stringValue(x, p); return v.compare(0, q.size(), q) == 0 && v.size() >= q.size(); }
bool s_hasSuffix(Ctx& x, id s, id p) { const U16 v = stringValue(x, s), q = stringValue(x, p); return v.size() >= q.size() && v.compare(v.size() - q.size(), q.size(), q) == 0; }
int32_t s_intValue(Ctx& x, id s) { return int32_t(std::strtol(utf8(x, s).c_str(), nullptr, 10)); }
double s_doubleValue(Ctx& x, id s) { return std::strtod(utf8(x, s).c_str(), nullptr); }
uint32_t s_separate(Ctx& x, uint32_t, id s, id sep) { return R(x).retain(x.cpu, id(send(x, s, "componentsSeparatedByString:", {sep}))); }
uint32_t s_join(Ctx& x, uint32_t, id arr, id sep) { return R(x).retain(x.cpu, id(send(x, arr, "componentsJoinedByString:", {sep}))); }
uint32_t s_createByCombining(Ctx& x, uint32_t a, id arr, id sep) { return s_join(x, a, arr, sep); }
uint32_t s_makeConstant(Ctx& x, GuestPtr c) { return constString(x.proc, x.proc.readCString(c)); }
uint32_t s_createFromExternalRep(Ctx& x, uint32_t, id data, uint32_t enc) {
    const auto b = dataBytes(x, data);
    bool ok = true;
    const std::string u = decodeBytes(b.data(), b.size(), toNS(enc), &ok);
    return ok ? makeString(x, u, true) : 0;
}
uint32_t s_createExternalRep(Ctx& x, uint32_t, id s, uint32_t enc, uint8_t) {
    bool ok = true;
    const std::string b = encodeBytes(stringValue(x, s), toNS(enc), true, &ok);
    return makeData(x, b.data(), uint32_t(b.size()), true);
}
void s_normalize(Ctx&, id, uint32_t) {}
void s_fold(Ctx& x, id s, uint32_t flags, id) { if (flags & 1) s_lowercase(x, s, 0); }
bool s_transform(Ctx&, id, GuestPtr, id, bool) { return false; }

// ------------------------------------------------------------ CFNumber ---

enum : uint32_t {
    kSInt8 = 1, kSInt16, kSInt32, kSInt64, kFloat32, kFloat64, kChar, kShort, kInt, kLong, kLongLong, kFloat, kDouble, kCFIndex,
    kNSInteger, kCGFloat,
};
uint32_t n_create(Ctx& x, uint32_t, uint32_t type, GuestPtr p) {
    if (!p) return 0;
    switch (type) {
        case kSInt8: case kChar: return makeNumberInt(x, x.mem.read<int8_t>(p), true);
        case kSInt16: case kShort: return makeNumberInt(x, x.mem.read<int16_t>(p), true);
        case kSInt32: case kInt: case kLong: case kCFIndex: case kNSInteger: return makeNumberInt(x, x.mem.read<int32_t>(p), true);
        case kSInt64: case kLongLong: return makeNumberInt(x, x.mem.read<int64_t>(p), true);
        case kFloat32: case kFloat: case kCGFloat: return makeNumberFloat(x, x.mem.read<float>(p), true);
        case kFloat64: case kDouble: return makeNumberDouble(x, x.mem.read<double>(p), true);
        default: return 0;
    }
}
bool n_getValue(Ctx& x, id n, uint32_t type, GuestPtr p) {
    if (!n || !p) return false;
    const double d = numberDouble(x, n);
    const int64_t i = numberInt(x, n);
    switch (type) {
        case kSInt8: case kChar: x.mem.write<int8_t>(p, int8_t(i)); break;
        case kSInt16: case kShort: x.mem.write<int16_t>(p, int16_t(i)); break;
        case kSInt32: case kInt: case kLong: case kCFIndex: case kNSInteger: x.mem.write<int32_t>(p, int32_t(i)); break;
        case kSInt64: case kLongLong: x.mem.write<int64_t>(p, i); break;
        case kFloat32: case kFloat: case kCGFloat: x.mem.write<float>(p, float(d)); break;
        case kFloat64: case kDouble: x.mem.write<double>(p, d); break;
        default: return false;
    }
    const char t = numberType(x, n);
    const bool isFloat = t == 'f' || t == 'd';
    const bool wantInt = !(type == kFloat32 || type == kFloat64 || type == kFloat || type == kDouble || type == kCGFloat);
    return !(isFloat && wantInt && d != std::floor(d));   // lossy conversion returns false
}
uint32_t n_getType(Ctx& x, id n) {
    switch (numberType(x, n)) {
        case 'c': return kSInt8;
        case 's': return kSInt16;
        case 'i': case 'l': return kSInt32;
        case 'q': return kSInt64;
        case 'f': return kFloat32;
        case 'd': return kFloat64;
        default: return kSInt32;
    }
}
int32_t n_getByteSize(Ctx& x, id n) {
    switch (n_getType(x, n)) { case kSInt8: return 1; case kSInt16: return 2; case kSInt64: case kFloat64: return 8; default: return 4; }
}
bool n_isFloat(Ctx& x, id n) { const char t = numberType(x, n); return t == 'f' || t == 'd'; }
int32_t n_compare(Ctx& x, id a, id b, uint32_t) { return int32_t(send(x, a, "compare:", {b})); }
bool b_getValue(Ctx& x, id b) { return b && numberInt(x, b) != 0; }

// ------------------------------------------------------------- CFArray ---

bool objectCallbacks(Ctx& x, GuestPtr cb) { return cb && x.mem.read<uint32_t>(cb + 4) != 0; }
RawArray* rawA(Ctx& x, id a) { return a ? R(x).hostIf<RawArray>(a) : nullptr; }
id newRawArray(Ctx& x, std::vector<uint32_t> items, bool retains = false) {
    const id a = R(x).createInstance(cf(x).rawArray);
    auto h = std::make_unique<RawArray>();
    h->items = std::move(items);
    h->retains = retains;
    R(x).setHost(a, std::move(h));
    return a;
}
uint32_t a_create(Ctx& x, uint32_t, GuestPtr values, int32_t n, GuestPtr cb) {
    std::vector<uint32_t> v(size_t(std::max(n, 0)));
    for (int32_t i = 0; i < n; ++i) v[size_t(i)] = x.mem.read<uint32_t>(values + uint32_t(i) * 4);
    if (objectCallbacks(x, cb)) return makeArray(x, std::vector<id>(v.begin(), v.end()), true, false);
    return newRawArray(x, std::move(v));
}
uint32_t a_createMutable(Ctx& x, uint32_t, int32_t, GuestPtr cb) {
    if (objectCallbacks(x, cb)) return makeArray(x, {}, true, true);
    return newRawArray(x, {});
}
uint32_t a_createCopy(Ctx& x, uint32_t, id a) {
    if (RawArray* r = rawA(x, a)) return newRawArray(x, r->items);
    return uint32_t(send(x, a, "copy"));
}
uint32_t a_createMutableCopy(Ctx& x, uint32_t, int32_t, id a) {
    if (RawArray* r = rawA(x, a)) return newRawArray(x, r->items);
    return uint32_t(send(x, a, "mutableCopy"));
}
int32_t a_count(Ctx& x, id a) {
    if (!a) return 0;
    if (RawArray* r = rawA(x, a)) return int32_t(r->items.size());
    return int32_t(send(x, a, "count"));
}
uint32_t a_get(Ctx& x, id a, int32_t i) {
    if (RawArray* r = rawA(x, a)) return i >= 0 && size_t(i) < r->items.size() ? r->items[size_t(i)] : 0;
    return uint32_t(send(x, a, "objectAtIndex:", {uint32_t(i)}));
}
void a_getValues(Ctx& x, id a, uint32_t loc, uint32_t len, GuestPtr out) {
    for (uint32_t i = 0; i < len; ++i) x.mem.write<uint32_t>(out + i * 4, a_get(x, a, int32_t(loc + i)));
}
void a_append(Ctx& x, id a, uint32_t v) {
    if (RawArray* r = rawA(x, a)) {
        if (r->retains && v && R(x).classOf(v)) R(x).retain(x.cpu, v);
        r->items.push_back(v);
        return;
    }
    send(x, a, "addObject:", {v});
}
void a_insert(Ctx& x, id a, int32_t i, uint32_t v) {
    if (RawArray* r = rawA(x, a)) {
        if (r->retains && v && R(x).classOf(v)) R(x).retain(x.cpu, v);
        r->items.insert(r->items.begin() + std::min<size_t>(size_t(i), r->items.size()), v);
        return;
    }
    send(x, a, "insertObject:atIndex:", {v, uint32_t(i)});
}
void a_set(Ctx& x, id a, int32_t i, uint32_t v) {
    if (RawArray* r = rawA(x, a)) {
        if (r->retains && v && R(x).classOf(v)) R(x).retain(x.cpu, v);
        if (size_t(i) < r->items.size()) {
            const uint32_t old = r->items[size_t(i)];
            r->items[size_t(i)] = v;
            if (r->retains && old && R(x).classOf(old)) R(x).release(x.cpu, old);
        } else {
            r->items.push_back(v);
        }
        return;
    }
    if (i == a_count(x, a)) send(x, a, "addObject:", {v});
    else send(x, a, "replaceObjectAtIndex:withObject:", {uint32_t(i), v});
}
void a_remove(Ctx& x, id a, int32_t i) {
    if (RawArray* r = rawA(x, a)) {
        if (size_t(i) < r->items.size()) {
            const uint32_t old = r->items[size_t(i)];
            r->items.erase(r->items.begin() + i);
            if (r->retains && old && R(x).classOf(old)) R(x).release(x.cpu, old);
        }
        return;
    }
    send(x, a, "removeObjectAtIndex:", {uint32_t(i)});
}
void a_removeAll(Ctx& x, id a) {
    if (RawArray* r = rawA(x, a)) {
        if (r->retains) {
            for (uint32_t v : r->items) if (v && R(x).classOf(v)) R(x).release(x.cpu, v);
        }
        r->items.clear();
        return;
    }
    send(x, a, "removeAllObjects");
}
void a_exchange(Ctx& x, id a, int32_t i, int32_t j) {
    if (RawArray* r = rawA(x, a)) { std::swap(r->items[size_t(i)], r->items[size_t(j)]); return; }
    send(x, a, "exchangeObjectAtIndex:withObjectAtIndex:", {uint32_t(i), uint32_t(j)});
}
void a_appendArray(Ctx& x, id a, id other, uint32_t loc, uint32_t len) {
    for (uint32_t i = 0; i < len; ++i) a_append(x, a, a_get(x, other, int32_t(loc + i)));
}
bool valueMatch(Ctx& x, id a, uint32_t item, uint32_t v) { return rawA(x, a) ? item == v : objectsEqual(x, item, v); }
bool a_contains(Ctx& x, id a, uint32_t loc, uint32_t len, uint32_t v) {
    for (uint32_t i = 0; i < len; ++i) if (valueMatch(x, a, a_get(x, a, int32_t(loc + i)), v)) return true;
    return false;
}
int32_t a_firstIndex(Ctx& x, id a, uint32_t loc, uint32_t len, uint32_t v) {
    for (uint32_t i = 0; i < len; ++i) if (valueMatch(x, a, a_get(x, a, int32_t(loc + i)), v)) return int32_t(loc + i);
    return -1;
}
int32_t a_lastIndex(Ctx& x, id a, uint32_t loc, uint32_t len, uint32_t v) {
    for (uint32_t i = len; i-- > 0;) if (valueMatch(x, a, a_get(x, a, int32_t(loc + i)), v)) return int32_t(loc + i);
    return -1;
}
int32_t a_countOf(Ctx& x, id a, uint32_t loc, uint32_t len, uint32_t v) {
    int32_t n = 0;
    for (uint32_t i = 0; i < len; ++i) if (valueMatch(x, a, a_get(x, a, int32_t(loc + i)), v)) ++n;
    return n;
}
void a_apply(Ctx& x, id a, uint32_t loc, uint32_t len, uint32_t fn, uint32_t ctx) {
    for (uint32_t i = 0; i < len; ++i) x.proc.callGuest(x.cpu, fn, {a_get(x, a, int32_t(loc + i)), ctx});
}
void a_sort(Ctx& x, id a, uint32_t loc, uint32_t len, uint32_t cmp, uint32_t ctx) {
    std::vector<uint32_t> v;
    for (uint32_t i = 0; i < len; ++i) v.push_back(a_get(x, a, int32_t(loc + i)));
    std::stable_sort(v.begin(), v.end(), [&](uint32_t p, uint32_t q) { return int32_t(x.proc.callGuest(x.cpu, cmp, {p, q, ctx})) < 0; });
    if (RawArray* r = rawA(x, a)) { for (uint32_t i = 0; i < len; ++i) r->items[loc + i] = v[i]; return; }
    for (id o : v) R(x).retain(x.cpu, o);
    for (uint32_t i = 0; i < len; ++i) send(x, a, "replaceObjectAtIndex:withObject:", {loc + i, v[i]});
    for (id o : v) R(x).release(x.cpu, o);
}
int32_t a_bsearch(Ctx& x, id a, uint32_t loc, uint32_t len, uint32_t v, uint32_t cmp, uint32_t ctx) {
    uint32_t lo = loc, hi = loc + len;
    while (lo < hi) {
        const uint32_t mid = (lo + hi) / 2;
        if (int32_t(x.proc.callGuest(x.cpu, cmp, {a_get(x, a, int32_t(mid)), v, ctx})) < 0) lo = mid + 1; else hi = mid;
    }
    return int32_t(lo);
}
// raw array ObjC methods (so code that treats it as an NSArray/NSMutableArray/NSMutableSet still works for basics)
uint32_t ra_count(Ctx& x, id self, SEL) { return uint32_t(a_count(x, self)); }
uint32_t ra_objectAt(Ctx& x, id self, SEL, uint32_t i) { return a_get(x, self, int32_t(i)); }
void ra_addObject(Ctx& x, id self, SEL, uint32_t v) {
    if (RawArray* r = rawA(x, self)) {
        if (v && R(x).classOf(v)) R(x).retain(x.cpu, v);
        r->items.push_back(v);
        return;
    }
    a_append(x, self, v);
}
void ra_insertObject(Ctx& x, id self, SEL, uint32_t v, uint32_t i) {
    if (RawArray* r = rawA(x, self)) {
        if (v && R(x).classOf(v)) R(x).retain(x.cpu, v);
        r->items.insert(r->items.begin() + std::min<size_t>(size_t(i), r->items.size()), v);
        return;
    }
    a_insert(x, self, int32_t(i), v);
}
void ra_replaceAt(Ctx& x, id self, SEL, uint32_t i, uint32_t v) { a_set(x, self, int32_t(i), v); }
void ra_removeAt(Ctx& x, id self, SEL, uint32_t i) {
    if (RawArray* r = rawA(x, self)) {
        if (size_t(i) < r->items.size()) {
            const uint32_t v = r->items[size_t(i)];
            r->items.erase(r->items.begin() + i);
            if (v && R(x).classOf(v)) R(x).release(x.cpu, v);
        }
        return;
    }
    a_remove(x, self, int32_t(i));
}
void ra_removeObject(Ctx& x, id self, SEL, uint32_t v) {
    if (RawArray* r = rawA(x, self)) {
        auto it = std::remove(r->items.begin(), r->items.end(), v);
        const size_t count = std::distance(it, r->items.end());
        r->items.erase(it, r->items.end());
        if (v && R(x).classOf(v)) {
            for (size_t k = 0; k < count; ++k) R(x).release(x.cpu, v);
        }
    }
}
void ra_removeAll(Ctx& x, id self, SEL) {
    if (RawArray* r = rawA(x, self)) {
        for (uint32_t v : r->items) if (v && R(x).classOf(v)) R(x).release(x.cpu, v);
        r->items.clear();
        return;
    }
    a_removeAll(x, self);
}
bool ra_contains(Ctx& x, id self, SEL, uint32_t v) { return a_contains(x, self, 0, uint32_t(a_count(x, self)), v); }
uint32_t ra_member(Ctx& x, id self, SEL, uint32_t v) { return ra_contains(x, self, 0, v) ? v : 0; }
void ra_makePerf(Ctx& x, id self, SEL, SEL s) {
    RawArray* r = rawA(x, self);
    if (!r || !s) return;
    const auto copy = r->items;
    for (id o : copy) if (o) R(x).send(x.cpu, o, s);
}
void ra_makePerfObj(Ctx& x, id self, SEL, SEL s, id arg) {
    RawArray* r = rawA(x, self);
    if (!r || !s) return;
    const auto copy = r->items;
    for (id o : copy) if (o) R(x).send(x.cpu, o, s, {arg});
}
uint32_t ra_fastEnum(Ctx& x, id self, SEL, GuestPtr state, GuestPtr buf, uint32_t len) {
    RawArray* r = rawA(x, self);
    if (!r || !state) return 0;
    const uint32_t pos = x.mem.read<uint32_t>(state);
    if (pos >= r->items.size()) return 0;
    const uint32_t n = std::min<uint32_t>(len, uint32_t(r->items.size()) - pos);
    for (uint32_t i = 0; i < n; ++i) x.mem.write<uint32_t>(buf + i * 4, r->items[pos + i]);
    x.mem.write<uint32_t>(state, pos + n);
    x.mem.write<uint32_t>(state + 4, buf);
    x.mem.write<uint32_t>(state + 8, state);
    return n;
}
void ra_dealloc(Ctx& x, id self, SEL) {
    if (RawArray* r = rawA(x, self)) {
        if (r->retains) {
            for (uint32_t v : r->items) if (v && R(x).classOf(v)) R(x).release(x.cpu, v);
        }
    }
    R(x).disposeInstance(x.cpu, self);
}

// -------------------------------------------------------- CFDictionary ---

RawDict* rawD(Ctx& x, id d) { return d ? R(x).hostIf<RawDict>(d) : nullptr; }
id newRawDict(Ctx& x, bool keyObj, bool valObj, bool keyEq) {
    const id d = R(x).createInstance(cf(x).rawDict);
    auto h = std::make_unique<RawDict>();
    h->keyObjects = keyObj;
    h->valueObjects = valObj;
    h->keyEqual = keyEq;
    R(x).setHost(d, std::move(h));
    return d;
}
long rawFind(Ctx& x, RawDict& d, uint32_t key) {
    for (size_t i = 0; i < d.entries.size(); ++i) {
        const uint32_t k = d.entries[i].first;
        if (k == key || (d.keyEqual && k && key && objectsEqual(x, k, key))) return long(i);
    }
    return -1;
}
void rawSet(Ctx& x, RawDict& d, uint32_t key, uint32_t value, int mode /*0 set,1 add,2 replace*/) {
    const long i = rawFind(x, d, key);
    if ((i >= 0 && mode == 1) || (i < 0 && mode == 2)) return;
    if (d.valueObjects) R(x).retain(x.cpu, value);
    if (i >= 0) {
        if (d.valueObjects) R(x).release(x.cpu, d.entries[size_t(i)].second);
        d.entries[size_t(i)].second = value;
        return;
    }
    if (d.keyObjects) key = d.keyEqual && isString(x, key) ? id(send(x, key, "copy")) : R(x).retain(x.cpu, key);
    d.entries.emplace_back(key, value);
}
bool dictKeyEq(Ctx& x, GuestPtr kcb) { return kcb && x.mem.read<uint32_t>(kcb + 16) != 0; }
uint32_t d_create(Ctx& x, uint32_t, GuestPtr keys, GuestPtr values, int32_t n, GuestPtr kcb, GuestPtr vcb) {
    const bool ko = objectCallbacks(x, kcb), vo = objectCallbacks(x, vcb);
    if (ko && vo) {
        std::vector<std::pair<id, id>> e;
        for (int32_t i = 0; i < n; ++i) e.emplace_back(x.mem.read<uint32_t>(keys + uint32_t(i) * 4), x.mem.read<uint32_t>(values + uint32_t(i) * 4));
        return makeDict(x, e, true, false);
    }
    const id d = newRawDict(x, ko, vo, dictKeyEq(x, kcb));
    RawDict& r = *rawD(x, d);
    for (int32_t i = 0; i < n; ++i) rawSet(x, r, x.mem.read<uint32_t>(keys + uint32_t(i) * 4), x.mem.read<uint32_t>(values + uint32_t(i) * 4), 0);
    return d;
}
uint32_t d_createMutable(Ctx& x, uint32_t, int32_t, GuestPtr kcb, GuestPtr vcb) {
    const bool ko = objectCallbacks(x, kcb), vo = objectCallbacks(x, vcb);
    if (ko && vo) return makeDict(x, {}, true, true);
    return newRawDict(x, ko, vo, dictKeyEq(x, kcb));
}
uint32_t d_createCopy(Ctx& x, uint32_t, id d) {
    if (RawDict* r = rawD(x, d)) {
        const id c = newRawDict(x, r->keyObjects, r->valueObjects, r->keyEqual);
        for (auto& [k, v] : r->entries) rawSet(x, *rawD(x, c), k, v, 0);
        return c;
    }
    return uint32_t(send(x, d, "copy"));
}
uint32_t d_createMutableCopy(Ctx& x, uint32_t a, int32_t, id d) {
    if (rawD(x, d)) return d_createCopy(x, a, d);
    return uint32_t(send(x, d, "mutableCopy"));
}
int32_t d_count(Ctx& x, id d) {
    if (!d) return 0;
    if (RawDict* r = rawD(x, d)) return int32_t(r->entries.size());
    return int32_t(send(x, d, "count"));
}
uint32_t d_get(Ctx& x, id d, uint32_t key) {
    if (RawDict* r = rawD(x, d)) { const long i = rawFind(x, *r, key); return i < 0 ? 0 : r->entries[size_t(i)].second; }
    return key ? dictGet(x, d, key) : 0;
}
bool d_getIfPresent(Ctx& x, id d, uint32_t key, GuestPtr out) {
    bool present;
    uint32_t v = 0;
    if (RawDict* r = rawD(x, d)) { const long i = rawFind(x, *r, key); present = i >= 0; if (present) v = r->entries[size_t(i)].second; }
    else { v = key ? dictGet(x, d, key) : 0; present = v != 0; }
    if (present && out) x.mem.write<uint32_t>(out, v);
    return present;
}
bool d_containsKey(Ctx& x, id d, uint32_t key) {
    if (RawDict* r = rawD(x, d)) return rawFind(x, *r, key) >= 0;
    return key && dictGet(x, d, key) != 0;
}
bool d_containsValue(Ctx& x, id d, uint32_t v) {
    if (RawDict* r = rawD(x, d)) { for (auto& e : r->entries) if (e.second == v) return true; return false; }
    for (auto& [k, val] : dictEntries(x, d)) if (objectsEqual(x, val, v)) return true;
    return false;
}
void d_setMode(Ctx& x, id d, uint32_t key, uint32_t v, int mode) {
    if (RawDict* r = rawD(x, d)) { rawSet(x, *r, key, v, mode); return; }
    if (!key) return;
    const bool has = dictGet(x, d, key) != 0;
    if ((mode == 1 && has) || (mode == 2 && !has)) return;
    if (!v) { x.proc.log(LogLevel::Warning, "CFDictionarySetValue with a NULL value on an object dictionary"); return; }
    send(x, d, "setObject:forKey:", {v, key});
}
void d_set(Ctx& x, id d, uint32_t k, uint32_t v) { d_setMode(x, d, k, v, 0); }
void d_add(Ctx& x, id d, uint32_t k, uint32_t v) { d_setMode(x, d, k, v, 1); }
void d_replace(Ctx& x, id d, uint32_t k, uint32_t v) { d_setMode(x, d, k, v, 2); }
void d_remove(Ctx& x, id d, uint32_t key) {
    if (RawDict* r = rawD(x, d)) {
        const long i = rawFind(x, *r, key);
        if (i < 0) return;
        const auto e = r->entries[size_t(i)];
        r->entries.erase(r->entries.begin() + i);
        if (r->keyObjects) R(x).release(x.cpu, e.first);
        if (r->valueObjects) R(x).release(x.cpu, e.second);
        return;
    }
    if (key) send(x, d, "removeObjectForKey:", {key});
}
void rawClear(Ctx& x, RawDict& r) {
    auto entries = std::move(r.entries);
    r.entries.clear();
    for (auto& [k, v] : entries) {
        if (r.keyObjects) R(x).release(x.cpu, k);
        if (r.valueObjects) R(x).release(x.cpu, v);
    }
}
void d_removeAll(Ctx& x, id d) {
    if (RawDict* r = rawD(x, d)) { rawClear(x, *r); return; }
    send(x, d, "removeAllObjects");
}
std::vector<std::pair<uint32_t, uint32_t>> entriesOfAny(Ctx& x, id d) {
    if (RawDict* r = rawD(x, d)) return r->entries;
    std::vector<std::pair<uint32_t, uint32_t>> out;
    for (auto& e : dictEntries(x, d)) out.emplace_back(e.first, e.second);
    return out;
}
void d_getKeysAndValues(Ctx& x, id d, GuestPtr keys, GuestPtr values) {
    uint32_t i = 0;
    for (auto& [k, v] : entriesOfAny(x, d)) {
        if (keys) x.mem.write<uint32_t>(keys + i * 4, k);
        if (values) x.mem.write<uint32_t>(values + i * 4, v);
        ++i;
    }
}
void d_apply(Ctx& x, id d, uint32_t fn, uint32_t ctx) {
    for (auto& [k, v] : entriesOfAny(x, d)) x.proc.callGuest(x.cpu, fn, {k, v, ctx});
}
int32_t d_countOfValue(Ctx& x, id d, uint32_t v) {
    int32_t n = 0;
    for (auto& e : entriesOfAny(x, d)) if (e.second == v || (!rawD(x, d) && objectsEqual(x, e.second, v))) ++n;
    return n;
}
uint32_t rd_count(Ctx& x, id self, SEL) { return uint32_t(d_count(x, self)); }
uint32_t rd_objectForKey(Ctx& x, id self, SEL, uint32_t k) { return d_get(x, self, k); }
void rd_dealloc(Ctx& x, id self, SEL) {
    if (RawDict* r = rawD(x, self)) rawClear(x, *r);
    R(x).disposeInstance(x.cpu, self);
}

// --------------------------------------------------------------- CFSet ---

uint32_t set_create(Ctx& x, uint32_t, GuestPtr values, int32_t n, GuestPtr cb) {
    std::vector<id> v;
    for (int32_t i = 0; i < n; ++i) v.push_back(x.mem.read<uint32_t>(values + uint32_t(i) * 4));
    if (!objectCallbacks(x, cb)) return newRawArray(x, std::move(v));
    return makeSet(x, v, true, false);
}
uint32_t set_createMutable(Ctx& x, uint32_t, int32_t, GuestPtr cb) {
    if (!objectCallbacks(x, cb)) return newRawArray(x, {});
    return makeSet(x, {}, true, true);
}
uint32_t set_createCopy(Ctx& x, uint32_t, id s) {
    if (RawArray* r = rawA(x, s)) return newRawArray(x, r->items);
    return uint32_t(send(x, s, "copy"));
}
uint32_t set_createMutableCopy(Ctx& x, uint32_t, int32_t, id s) {
    if (RawArray* r = rawA(x, s)) return newRawArray(x, r->items);
    return uint32_t(send(x, s, "mutableCopy"));
}
int32_t set_count(Ctx& x, id s) {
    if (!s) return 0;
    if (RawArray* r = rawA(x, s)) return int32_t(r->items.size());
    return int32_t(send(x, s, "count"));
}
bool set_contains(Ctx& x, id s, uint32_t v) {
    if (RawArray* r = rawA(x, s)) return std::find(r->items.begin(), r->items.end(), v) != r->items.end();
    return (uint32_t(send(x, s, "containsObject:", {v})) & 0xFF) != 0;
}
uint32_t set_getValue(Ctx& x, id s, uint32_t v) {
    if (RawArray* r = rawA(x, s)) return std::find(r->items.begin(), r->items.end(), v) != r->items.end() ? v : 0;
    return uint32_t(send(x, s, "member:", {v}));
}
void set_add(Ctx& x, id s, uint32_t v) {
    if (RawArray* r = rawA(x, s)) {
        if (std::find(r->items.begin(), r->items.end(), v) == r->items.end()) r->items.push_back(v);
        return;
    }
    send(x, s, "addObject:", {v});
}
void set_remove(Ctx& x, id s, uint32_t v) {
    if (RawArray* r = rawA(x, s)) {
        r->items.erase(std::remove(r->items.begin(), r->items.end(), v), r->items.end());
        return;
    }
    send(x, s, "removeObject:", {v});
}
void set_removeAll(Ctx& x, id s) {
    if (RawArray* r = rawA(x, s)) { r->items.clear(); return; }
    send(x, s, "removeAllObjects");
}
void set_getValues(Ctx& x, id s, GuestPtr out) {
    uint32_t i = 0;
    if (RawArray* r = rawA(x, s)) { for (uint32_t o : r->items) x.mem.write<uint32_t>(out + 4 * i++, o); return; }
    for (id o : setItems(x, s)) x.mem.write<uint32_t>(out + 4 * i++, o);
}
void set_apply(Ctx& x, id s, uint32_t fn, uint32_t ctx) {
    if (RawArray* r = rawA(x, s)) { for (uint32_t o : std::vector<uint32_t>(r->items)) x.proc.callGuest(x.cpu, fn, {o, ctx}); return; }
    for (id o : setItems(x, s)) x.proc.callGuest(x.cpu, fn, {o, ctx});
}

// -------------------------------------------------------------- CFData ---

uint32_t dt_create(Ctx& x, uint32_t, GuestPtr p, int32_t n) { return makeData(x, n > 0 ? x.mem.host(p) : nullptr, uint32_t(std::max(n, 0)), true); }
uint32_t al_getDefault(Ctx& x);
void al_deallocate(Ctx& x, uint32_t a, uint32_t p);
uint32_t dt_createNoCopy(Ctx& x, uint32_t, GuestPtr p, int32_t n, uint32_t deallocator) {
    if (n < 0 || (!p && n)) return 0;
    const uint32_t a = R(x).retain(x.cpu, deallocator ? deallocator : al_getDefault(x));
    return makeDataNoCopy(x, p, uint32_t(n), [a,p](Ctx& y) { al_deallocate(y, a, p); R(y).release(y.cpu, a); });
}
uint32_t dt_createMutable(Ctx& x, uint32_t, int32_t) { return makeData(x, nullptr, 0, true, true); }
uint32_t dt_createCopy(Ctx& x, uint32_t, id d) { const auto b = dataBytes(x, d); return makeData(x, b.data(), uint32_t(b.size()), true); }
uint32_t dt_createMutableCopy(Ctx& x, uint32_t, int32_t, id d) { const auto b = dataBytes(x, d); return makeData(x, b.data(), uint32_t(b.size()), true, true); }
int32_t dt_length(Ctx& x, id d) { return d ? int32_t(send(x, d, "length")) : 0; }
uint32_t dt_bytePtr(Ctx& x, id d) { return d ? uint32_t(send(x, d, "bytes")) : 0; }
uint32_t dt_mutableBytePtr(Ctx& x, id d) { return d ? uint32_t(send(x, d, "mutableBytes")) : 0; }
void dt_getBytes(Ctx& x, id d, uint32_t loc, uint32_t len, GuestPtr out) { send(x, d, "getBytes:range:", {out, loc, len}); }
void dt_append(Ctx& x, id d, GuestPtr p, int32_t n) { send(x, d, "appendBytes:length:", {p, uint32_t(n)}); }
void dt_setLength(Ctx& x, id d, int32_t n) { send(x, d, "setLength:", {uint32_t(n)}); }
void dt_increaseLength(Ctx& x, id d, int32_t n) { send(x, d, "increaseLengthBy:", {uint32_t(n)}); }
void dt_replace(Ctx& x, id d, uint32_t loc, uint32_t len, GuestPtr p, int32_t n) {
    send(x, d, "replaceBytesInRange:withBytes:length:", {loc, len, p, uint32_t(n)});
}
void dt_delete(Ctx& x, id d, uint32_t loc, uint32_t len) { send(x, d, "replaceBytesInRange:withBytes:length:", {loc, len, 0, 0}); }

// ------------------------------------------------------- CFDate & time ---

uint32_t date_create(Ctx& x, uint32_t, double t) { return makeDate(x, t, true); }
double date_getAbsolute(Ctx& x, id d) { return dateInterval(x, d); }
double date_timeSince(Ctx& x, id a, id b) { return dateInterval(x, a) - dateInterval(x, b); }
int32_t date_compare(Ctx& x, id a, id b, uint32_t) { const double p = dateInterval(x, a), q = dateInterval(x, b); return p < q ? -1 : p > q ? 1 : 0; }

int32_t tzOffset(time_t t) { std::tm lt; localtime_r(&t, &lt); return int32_t(lt.tm_gmtoff); }
// CFGregorianDate { SInt32 year; SInt8 month, day, hour, minute; double second; }
void greg_fromAbsolute(Ctx& x, GuestPtr ret, double at, id tz) {
    const double unixT = at + 978307200.0;
    const time_t t = time_t(std::floor(unixT));
    std::tm tm;
    if (tz) { const time_t lt = t + tzOffset(t); gmtime_r(&lt, &tm); }
    else gmtime_r(&t, &tm);
    x.mem.write<int32_t>(ret, tm.tm_year + 1900);
    x.mem.write<int8_t>(ret + 4, int8_t(tm.tm_mon + 1));
    x.mem.write<int8_t>(ret + 5, int8_t(tm.tm_mday));
    x.mem.write<int8_t>(ret + 6, int8_t(tm.tm_hour));
    x.mem.write<int8_t>(ret + 7, int8_t(tm.tm_min));
    x.mem.write<double>(ret + 8, tm.tm_sec + (unixT - std::floor(unixT)));
}
double greg_toAbsolute(Ctx&, Words<4> g, id tz) {
    std::tm tm{};
    tm.tm_year = int32_t(g.w[0]) - 1900;
    tm.tm_mon = int8_t(g.w[1] & 0xFF) - 1;
    tm.tm_mday = int8_t((g.w[1] >> 8) & 0xFF);
    tm.tm_hour = int8_t((g.w[1] >> 16) & 0xFF);
    tm.tm_min = int8_t((g.w[1] >> 24) & 0xFF);
    uint64_t bits = uint64_t(g.w[2]) | (uint64_t(g.w[3]) << 32);
    double sec;
    std::memcpy(&sec, &bits, 8);
    time_t t = timegm(&tm);
    double unixT = double(t) + sec;
    if (tz) unixT -= tzOffset(t);
    return unixT - 978307200.0;
}
int32_t greg_dayOfWeek(Ctx&, double at, id tz) {
    time_t t = time_t(std::floor(at + 978307200.0));
    if (tz) t += tzOffset(t);
    std::tm tm;
    gmtime_r(&t, &tm);
    return tm.tm_wday == 0 ? 7 : tm.tm_wday;   // CF: Monday = 1 … Sunday = 7
}
int32_t greg_dayOfYear(Ctx&, double at, id tz) {
    time_t t = time_t(std::floor(at + 978307200.0));
    if (tz) t += tzOffset(t);
    std::tm tm;
    gmtime_r(&t, &tm);
    return tm.tm_yday + 1;
}
bool greg_isValid(Ctx&, Words<4> g, uint32_t) {
    const int m = int8_t(g.w[1] & 0xFF), d = int8_t((g.w[1] >> 8) & 0xFF);
    return m >= 1 && m <= 12 && d >= 1 && d <= 31;
}
uint32_t tz_copySystem(Ctx& x) { return R(x).retain(x.cpu, id(send(x, R(x).getClass("NSTimeZone"), "systemTimeZone"))); }
double tz_secondsFromGMT(Ctx&, id, double at) { return tzOffset(time_t(at + 978307200.0)); }
uint32_t tz_getName(Ctx& x, id tz) { return uint32_t(send(x, tz, "name")); }
uint32_t tz_createWithOffset(Ctx& x, uint32_t, double) { return tz_copySystem(x); }
uint32_t tz_createWithName(Ctx& x, uint32_t, id, bool) { return tz_copySystem(x); }
bool tz_isDST(Ctx&, id, double at) { time_t t = time_t(at + 978307200.0); std::tm lt; localtime_r(&t, &lt); return lt.tm_isdst > 0; }

uint32_t loc_copyCurrent(Ctx& x) { return R(x).retain(x.cpu, id(send(x, R(x).getClass("NSLocale"), "currentLocale"))); }
uint32_t loc_getIdentifier(Ctx& x, id) { return constString(x.proc, "en_US"); }
uint32_t loc_getValue(Ctx& x, id l, id key) { return uint32_t(send(x, l, "objectForKey:", {key})); }
uint32_t loc_preferredLanguages(Ctx& x) { return makeArray(x, {makeString(x, "en")}, true); }
uint32_t loc_createCanonical(Ctx& x, uint32_t, id s) { return R(x).retain(x.cpu, s); }

// --------------------------------------------------------------- CFUUID ---

uint32_t uuid_create(Ctx& x, uint32_t) { return R(x).retain(x.cpu, id(send(x, R(x).getClass("NSUUID"), "UUID"))); }
uint32_t uuid_createString(Ctx& x, uint32_t, id u) { return R(x).retain(x.cpu, id(send(x, u, "UUIDString"))); }
uint32_t uuid_createFromString(Ctx& x, uint32_t, id s) {
    const id u = id(send(x, id(send(x, R(x).getClass("NSUUID"), "alloc")), "initWithUUIDString:", {s}));
    return u;
}
uint32_t uuid_createFromUUIDBytes(Ctx& x, uint32_t, Words<4> b) {
    const GuestPtr tmp = x.proc.heap().alloc(16);
    for (int i = 0; i < 4; ++i) x.mem.write<uint32_t>(tmp + uint32_t(i * 4), b.w[i]);
    const id u = id(send(x, id(send(x, R(x).getClass("NSUUID"), "alloc")), "initWithUUIDBytes:", {tmp}));
    x.proc.heap().free(tmp);
    return u;
}
void uuid_createWithBytes(Ctx& x) {
    const GuestPtr tmp = x.proc.heap().alloc(16);
    x.mem.write<uint8_t>(tmp + 0, uint8_t(x.cpu.r[1]));
    x.mem.write<uint8_t>(tmp + 1, uint8_t(x.cpu.r[2]));
    x.mem.write<uint8_t>(tmp + 2, uint8_t(x.cpu.r[3]));
    const uint32_t sp = x.cpu.r[CPU::kSP];
    for (uint32_t i = 0; i < 13; ++i) x.mem.write<uint8_t>(tmp + 3 + i, uint8_t(x.mem.read<uint32_t>(sp + i * 4)));
    const id u = id(send(x, id(send(x, R(x).getClass("NSUUID"), "alloc")), "initWithUUIDBytes:", {tmp}));
    x.proc.heap().free(tmp);
    x.cpu.r[0] = u;
}
void uuid_getBytes(Ctx& x, GuestPtr ret, id u) { send(x, u, "getUUIDBytes:", {ret}); }

// ------------------------------------------------------- CFPropertyList ---

uint32_t pl_fromData(Ctx& x, id data, uint32_t opts, GuestPtr fmtOut, GuestPtr errOut, bool xmlApi) {
    const auto b = dataBytes(x, data);
    try {
        auto v = Plist::parse(b);
        if (!v) throw Plist::PlistError("empty");
        if (fmtOut) x.mem.write<uint32_t>(fmtOut, b.size() >= 6 && std::memcmp(b.data(), "bplist", 6) == 0 ? 200 : 100);
        return R(x).retain(x.cpu, plistToObject(x, *v, opts != 0));
    } catch (const std::exception& e) {
        if (errOut) {
            if (xmlApi) x.mem.write<uint32_t>(errOut, makeString(x, e.what(), true));
            else {
                const id err = id(send(x, R(x).getClass("NSError"), "errorWithDomain:code:userInfo:",
                                       {makeString(x, "NSCocoaErrorDomain"), 3840u, 0}));
                x.mem.write<uint32_t>(errOut, R(x).retain(x.cpu, err));
            }
        }
        return 0;
    }
}
uint32_t pl_createFromXMLData(Ctx& x, uint32_t, id data, uint32_t opts, GuestPtr err) { return pl_fromData(x, data, opts, 0, err, true); }
uint32_t pl_createWithData(Ctx& x, uint32_t, id data, uint32_t opts, GuestPtr fmt, GuestPtr err) { return pl_fromData(x, data, opts, fmt, err, false); }
uint32_t pl_createXMLData(Ctx& x, uint32_t, id obj) {
    auto v = objectToPlist(x, obj);
    if (!v) return 0;
    const std::string s = Plist::toXml(*v);
    return makeData(x, s.data(), uint32_t(s.size()), true);
}
uint32_t pl_createData(Ctx& x, uint32_t a, id obj, uint32_t fmt, uint32_t, GuestPtr) {
    if (fmt == 200) x.proc.reportMissing("CFPropertyList binary output (writing XML instead)");
    return pl_createXMLData(x, a, obj);
}
uint32_t pl_deepCopy(Ctx& x, uint32_t, id obj, uint32_t opts) {
    auto v = objectToPlist(x, obj);
    return v ? R(x).retain(x.cpu, plistToObject(x, *v, opts != 0)) : 0;
}
bool pl_isValid(Ctx& x, id obj, uint32_t) { return objectToPlist(x, obj) != nullptr; }

// ----------------------------------------------------------- CFAllocator ---

const char allocatorDefaultKey = 0;
uint32_t al_getDefault(Ctx& x) {
    const uint32_t a = x.proc.threadSlot(&allocatorDefaultKey);
    return a ? a : cf(x).systemAllocator;
}
Allocator* allocator(Ctx& x, uint32_t a) { return R(x).hostIf<Allocator>(a ? a : al_getDefault(x)); }
uint32_t al_allocate(Ctx& x, uint32_t a, int32_t size, uint32_t hint) {
    auto* d = allocator(x, a);
    if (!d || d->null || size <= 0) return 0;
    const auto c = d->context;
    return c[5] ? uint32_t(x.proc.callGuest(x.cpu, c[5], {uint32_t(size), hint, c[1]})) :
           d == allocator(x, cf(x).systemAllocator) ? x.proc.heap().alloc(uint32_t(size)) : 0;
}
void al_deallocate(Ctx& x, uint32_t a, uint32_t p) {
    auto* d = allocator(x, a);
    if (!p || !d || d->null) return;
    const auto c = d->context;
    if (c[7]) x.proc.callGuest(x.cpu, c[7], {p, c[1]});
    else if (d == allocator(x, cf(x).systemAllocator)) x.proc.heap().free(p);
}
uint32_t al_reallocate(Ctx& x, uint32_t a, uint32_t p, int32_t size, uint32_t hint) {
    if (!p) return al_allocate(x, a, size, hint);
    if (size <= 0) { al_deallocate(x, a, p); return 0; }
    auto* d = allocator(x, a);
    if (!d || d->null) return 0;
    const auto c = d->context;
    if (c[6]) return uint32_t(x.proc.callGuest(x.cpu, c[6], {p, uint32_t(size), hint, c[1]}));
    return d == allocator(x, cf(x).systemAllocator) ? x.proc.heap().realloc(p, uint32_t(size)) : 0;
}
void al_setDefault(Ctx& x, uint32_t a) {
    if (!a || !allocator(x, a)) return;
    // CF keeps defaults alive permanently (including previous defaults).
    R(x).retain(x.cpu, a);
    x.proc.threadSlot(&allocatorDefaultKey) = a;
}
int32_t al_preferredSize(Ctx& x, uint32_t a, int32_t size, uint32_t hint) {
    auto* d = allocator(x, a);
    if (!d || size < 0) return 0;
    const auto c = d->context;
    return c[8] ? std::max(size, int32_t(x.proc.callGuest(x.cpu, c[8], {uint32_t(size), hint, c[1]}))) : size;
}
void al_getContext(Ctx& x, uint32_t a, GuestPtr out) {
    if (auto* d = allocator(x, a)) x.mem.copyIn(out, d->context.data(), 36);
}
uint32_t al_create(Ctx& x, uint32_t parent, GuestPtr ctx) {
    if (!ctx || x.mem.read<uint32_t>(ctx) != 0) return 0;
    // The ObjC/CF bridge owns object storage; unsupported storage allocators
    // must not silently turn into the system allocator.
    if (parent && parent != cf(x).systemAllocator) {
        x.proc.reportMissing("CFAllocatorCreate with custom object-storage allocator");
        return 0;
    }
    auto d = std::make_unique<Allocator>();
    std::memcpy(d->context.data(), x.mem.host(ctx), 36);
    if (d->context[2]) d->context[1] = uint32_t(x.proc.callGuest(x.cpu, d->context[2], {d->context[1]}));
    const id o = R(x).createInstance(cf(x).allocator);
    R(x).setHost(o, std::move(d));
    return o;
}
void al_destroy(Ctx& x, id self, SEL) {
    if (auto* d = R(x).hostIf<Allocator>(self)) {
        const auto c = d->context;
        if (c[3]) x.proc.callGuest(x.cpu, c[3], {c[1]});
    }
    R(x).disposeInstance(x.cpu, self);
}

// --------------------------------------------------------- CFCharacterSet ---

uint32_t cs_predefined(Ctx& x, uint32_t which) {
    static const char* names[] = {nullptr, "controlCharacterSet", "whitespaceCharacterSet", "whitespaceAndNewlineCharacterSet",
                                  "decimalDigitCharacterSet", "letterCharacterSet", "lowercaseLetterCharacterSet",
                                  "uppercaseLetterCharacterSet", "nonBaseCharacterSet", "decomposableCharacterSet",
                                  "alphanumericCharacterSet", "punctuationCharacterSet", "illegalCharacterSet",
                                  "capitalizedLetterCharacterSet", "symbolCharacterSet", "newlineCharacterSet"};
    if (which < 1 || which > 15) return 0;
    return uint32_t(send(x, R(x).getClass("NSCharacterSet"), names[which]));
}
bool cs_isMember(Ctx& x, id set, uint32_t c) { return characterSetContains(x, set, char16_t(c)); }
uint32_t cs_createWithString(Ctx& x, uint32_t, id s) {
    return R(x).retain(x.cpu, id(send(x, R(x).getClass("NSCharacterSet"), "characterSetWithCharactersInString:", {s})));
}
uint32_t cs_createWithRange(Ctx& x, uint32_t, uint32_t loc, uint32_t len) {
    return R(x).retain(x.cpu, id(send(x, R(x).getClass("NSCharacterSet"), "characterSetWithRange:", {loc, len})));
}
uint32_t cs_createInverted(Ctx& x, uint32_t, id s) { return R(x).retain(x.cpu, id(send(x, s, "invertedSet"))); }
uint32_t cs_createMutable(Ctx& x, uint32_t) { return uint32_t(send(x, R(x).getClass("NSMutableCharacterSet"), "new")); }
uint32_t cs_createCopy(Ctx& x, uint32_t, id set) { return uint32_t(send(x, set, "copy")); }
void cs_addRange(Ctx& x, id set, uint32_t loc, uint32_t len) {
    send(x, set, "addCharactersInRange:", {loc,len});
}

} // namespace

void registerCoreFoundation(Process& p, ObjC::Runtime& r) {
    auto& c = p.service<CF>();
    c.bitVector = r.defineClass("__HVMCFBitVector", "NSObject").cls();
    p.def<&bv_create>("_CFBitVectorCreate");
    p.def<&bv_mutable>("_CFBitVectorCreateMutable");
    p.def<&bv_copy>("_CFBitVectorCreateMutableCopy");
    p.def<&bv_count>("_CFBitVectorGetCount");
    p.def<&bv_setCount>("_CFBitVectorSetCount");
    p.def<&bv_get>("_CFBitVectorGetBitAtIndex");
    p.def<&bv_set>("_CFBitVectorSetBitAtIndex");
    // CFType
    c.cfRetainFn = p.def<&cf_retain>("_CFRetain");
    c.cfReleaseFn = p.def<&cf_release>("_CFRelease");
    p.def<&cf_autorelease>("_CFAutorelease");
    p.def<&cf_makeCollectable>("_CFMakeCollectable");
    p.def<&cf_retainCount>("_CFGetRetainCount");
    c.cfEqualFn = p.def<&cf_equal>("_CFEqual");
    c.cfHashFn = p.def<&cf_hash>("_CFHash");
    c.cfCopyDescFn = p.def<&cf_copyDescription>("_CFCopyDescription");
    p.def<&cf_show>("_CFShow");
    p.def<&cf_getAllocator>("_CFGetAllocator");
    p.def<&cf_getTypeID>("_CFGetTypeID");
    p.def<&cf_typeID<kTypeBitVector>>("_CFBitVectorGetTypeID");
    p.def<&cf_copyTypeIDDescription>("_CFCopyTypeIDDescription");
    p.def<&cf_typeID<kTypeString>>("_CFStringGetTypeID");
    p.def<&cf_typeID<kTypeArray>>("_CFArrayGetTypeID");
    p.def<&cf_typeID<kTypeDict>>("_CFDictionaryGetTypeID");
    p.def<&cf_typeID<kTypeNumber>>("_CFNumberGetTypeID");
    p.def<&cf_typeID<kTypeBool>>("_CFBooleanGetTypeID");
    p.def<&cf_typeID<kTypeData>>("_CFDataGetTypeID");
    p.def<&cf_typeID<kTypeDate>>("_CFDateGetTypeID");
    p.def<&cf_typeID<kTypeSet>>("_CFSetGetTypeID");
    p.def<&cf_typeID<kTypeURL>>("_CFURLGetTypeID");
    p.def<&cf_typeID<kTypeNull>>("_CFNullGetTypeID");
    p.def<&cf_typeID<kTypeBundle>>("_CFBundleGetTypeID");
    p.def<&cf_typeID<kTypeRunLoop>>("_CFRunLoopGetTypeID");
    p.def<&cf_typeID<kTypeTimer>>("_CFRunLoopTimerGetTypeID");
    p.def<&cf_typeID<kTypeUUID>>("_CFUUIDGetTypeID");
    p.def<&cf_typeID<kTypeTimeZone>>("_CFTimeZoneGetTypeID");
    p.def<&cf_typeID<kTypeLocale>>("_CFLocaleGetTypeID");
    p.def<&cf_typeID<kTypeCharSet>>("_CFCharacterSetGetTypeID");
    p.def<&cf_typeID<kTypeAllocator>>("_CFAllocatorGetTypeID");

    // callback structs: {version, retain, release, copyDescription, equal, hash}
    auto callbacks = [&](const char* name, bool withHash, bool copyKeys) {
        const uint32_t a = p.defineDataBlock(name, withHash ? 24 : 20);
        p.mem().write<uint32_t>(a + 4, c.cfRetainFn);
        p.mem().write<uint32_t>(a + 8, c.cfReleaseFn);
        p.mem().write<uint32_t>(a + 12, c.cfCopyDescFn);
        p.mem().write<uint32_t>(a + 16, c.cfEqualFn);
        if (withHash) p.mem().write<uint32_t>(a + 20, c.cfHashFn);
        (void)copyKeys;
        return a;
    };
    c.arrayCallbacks = callbacks("_kCFTypeArrayCallBacks", false, false);
    c.dictKeyCallbacks = callbacks("_kCFTypeDictionaryKeyCallBacks", true, false);
    c.copyStringKeyCallbacks = callbacks("_kCFCopyStringDictionaryKeyCallBacks", true, true);
    c.dictValueCallbacks = callbacks("_kCFTypeDictionaryValueCallBacks", false, false);
    c.setCallbacks = callbacks("_kCFTypeSetCallBacks", true, false);
    c.copyStringSetCallbacks = callbacks("_kCFCopyStringSetCallBacks", true, true);
    c.allocator = r.defineClass("__HVMCFAllocator", "NSObject").m<&al_destroy>("dealloc", "v@:").cls();
    c.systemAllocator = r.createInstance(c.allocator);
    r.setHost(c.systemAllocator, std::make_unique<Allocator>());
    r.makeImmortal(c.systemAllocator);
    c.nullAllocator = r.createInstance(c.allocator);
    auto nullAllocator = std::make_unique<Allocator>(); nullAllocator->null = true;
    r.setHost(c.nullAllocator, std::move(nullAllocator)); r.makeImmortal(c.nullAllocator);
    for (const char* n : {"_kCFAllocatorDefault", "_kCFAllocatorSystemDefault", "_kCFAllocatorMalloc", "_kCFAllocatorMallocZone",
                          "_kCFAllocatorNull", "_kCFAllocatorUseContext"}) {
        const uint32_t slot = p.defineDataBlock(n, 4);
        p.mem().write<uint32_t>(slot, std::string(n) == "_kCFAllocatorDefault" ? 0 :
            std::string(n) == "_kCFAllocatorUseContext" ? 0xFFFFFFFFu :
            std::string(n) == "_kCFAllocatorNull" ? c.nullAllocator : c.systemAllocator);
    }

    // CFString
    p.def<&s_createWithCString>("_CFStringCreateWithCString");
    p.def<&s_createWithCStringNoCopy>("_CFStringCreateWithCStringNoCopy");
    p.def<&s_createWithCharacters>("_CFStringCreateWithCharacters");
    p.def<&s_createWithCharactersNoCopy>("_CFStringCreateWithCharactersNoCopy");
    p.def<&s_createWithBytes>("_CFStringCreateWithBytes");
    p.def<&s_createWithBytesNoCopy>("_CFStringCreateWithBytesNoCopy");
    p.def<&s_createWithFormat>("_CFStringCreateWithFormat");
    p.def<&s_createWithFormatArgs>("_CFStringCreateWithFormatAndArguments");
    p.def<&s_createCopy>("_CFStringCreateCopy");
    p.def<&s_createMutable>("_CFStringCreateMutable");
    p.def<&s_createMutableCopy>("_CFStringCreateMutableCopy");
    p.def<&s_createWithSubstring>("_CFStringCreateWithSubstring");
    p.def<&s_getLength>("_CFStringGetLength");
    p.def<&s_getCharacterAtIndex>("_CFStringGetCharacterAtIndex");
    p.def<&s_getCharacters>("_CFStringGetCharacters");
    p.def<&s_getCString>("_CFStringGetCString");
    p.def<&s_getCStringPtr>("_CFStringGetCStringPtr");
    p.def<&s_getCharactersPtr>("_CFStringGetCharactersPtr");
    p.def<&s_getBytes>("_CFStringGetBytes");
    p.def<&s_maxSize>("_CFStringGetMaximumSizeForEncoding");
    p.def<&s_systemEncoding>("_CFStringGetSystemEncoding");
    p.def<&s_fastestEncoding>("_CFStringGetFastestEncoding");
    p.def<&s_smallestEncoding>("_CFStringGetSmallestEncoding");
    p.def<&s_cfToNS>("_CFStringConvertEncodingToNSStringEncoding");
    p.def<&s_nsToCF>("_CFStringConvertNSStringEncodingToEncoding");
    p.def<&s_encodingToIANA>("_CFStringConvertEncodingToIANACharSetName");
    p.def<&s_ianaToEncoding>("_CFStringConvertIANACharSetNameToEncoding");
    p.def<&s_isEncodingAvailable>("_CFStringIsEncodingAvailable");
    p.def<&s_append>("_CFStringAppend");
    p.def<&s_appendCString>("_CFStringAppendCString");
    p.def<&s_appendCharacters>("_CFStringAppendCharacters");
    p.def<&s_appendFormat>("_CFStringAppendFormat");
    p.def<&s_insert>("_CFStringInsert");
    p.def<&s_delete>("_CFStringDelete");
    p.def<&s_replace>("_CFStringReplace");
    p.def<&s_replaceAll>("_CFStringReplaceAll");
    p.def<&s_findAndReplace>("_CFStringFindAndReplace");
    p.def<&s_lowercase>("_CFStringLowercase");
    p.def<&s_uppercase>("_CFStringUppercase");
    p.def<&s_capitalize>("_CFStringCapitalize");
    p.def<&s_trimWhitespace>("_CFStringTrimWhitespace");
    p.def<&s_trim>("_CFStringTrim");
    p.def<&s_pad>("_CFStringPad");
    p.def<&s_compare>("_CFStringCompare");
    p.def<&s_compareRange>("_CFStringCompareWithOptions");
    p.def<&s_find>("_CFStringFind");
    p.def<&s_findWithOptions>("_CFStringFindWithOptions");
    p.def<&s_hasPrefix>("_CFStringHasPrefix");
    p.def<&s_hasSuffix>("_CFStringHasSuffix");
    p.def<&s_intValue>("_CFStringGetIntValue");
    p.def<&s_doubleValue>("_CFStringGetDoubleValue");
    p.def<&s_separate>("_CFStringCreateArrayBySeparatingStrings");
    p.def<&s_createByCombining>("_CFStringCreateByCombiningStrings");
    p.def<&s_makeConstant>("___CFStringMakeConstantString");
    p.def<&s_createFromExternalRep>("_CFStringCreateFromExternalRepresentation");
    p.def<&s_createExternalRep>("_CFStringCreateExternalRepresentation");
    p.def<&s_normalize>("_CFStringNormalize");
    p.def<&s_fold>("_CFStringFold");
    p.def<&s_transform>("_CFStringTransform");

    // CFNumber / CFBoolean
    p.def<&n_create>("_CFNumberCreate");
    p.def<&n_getValue>("_CFNumberGetValue");
    p.def<&n_getType>("_CFNumberGetType");
    p.def<&n_getByteSize>("_CFNumberGetByteSize");
    p.def<&n_isFloat>("_CFNumberIsFloatType");
    p.def<&n_compare>("_CFNumberCompare");
    p.def<&b_getValue>("_CFBooleanGetValue");

    // CFArray
    ObjC::ClassBuilder ra = r.defineClass("__HVMCFRawArray", "NSObject");
    c.rawArray = ra.cls();
    ra.m<&ra_count>("count", "I@:").m<&ra_objectAt>("objectAtIndex:", "@@:I")
      .m<&ra_addObject>("addObject:", "v@:@").m<&ra_insertObject>("insertObject:atIndex:", "v@:@I")
      .m<&ra_replaceAt>("replaceObjectAtIndex:withObject:", "v@:I@").m<&ra_removeAt>("removeObjectAtIndex:", "v@:I")
      .m<&ra_removeObject>("removeObject:", "v@:@").m<&ra_removeAll>("removeAllObjects", "v@:")
      .m<&ra_contains>("containsObject:", "c@:@").m<&ra_member>("member:", "@@:@")
      .m<&ra_makePerf>("makeObjectsPerformSelector:", "v@::")
      .m<&ra_makePerfObj>("makeObjectsPerformSelector:withObject:", "v@::@")
      .m<&ra_fastEnum>("countByEnumeratingWithState:objects:count:", "I@:^v^@I")
      .m<&ra_dealloc>("dealloc", "v@:");
    p.def<&a_create>("_CFArrayCreate");
    p.def<&a_createMutable>("_CFArrayCreateMutable");
    p.def<&a_createCopy>("_CFArrayCreateCopy");
    p.def<&a_createMutableCopy>("_CFArrayCreateMutableCopy");
    p.def<&a_count>("_CFArrayGetCount");
    p.def<&a_get>("_CFArrayGetValueAtIndex");
    p.def<&a_getValues>("_CFArrayGetValues");
    p.def<&a_append>("_CFArrayAppendValue");
    p.def<&a_insert>("_CFArrayInsertValueAtIndex");
    p.def<&a_set>("_CFArraySetValueAtIndex");
    p.def<&a_remove>("_CFArrayRemoveValueAtIndex");
    p.def<&a_removeAll>("_CFArrayRemoveAllValues");
    p.def<&a_exchange>("_CFArrayExchangeValuesAtIndices");
    p.def<&a_appendArray>("_CFArrayAppendArray");
    p.def<&a_contains>("_CFArrayContainsValue");
    p.def<&a_firstIndex>("_CFArrayGetFirstIndexOfValue");
    p.def<&a_lastIndex>("_CFArrayGetLastIndexOfValue");
    p.def<&a_countOf>("_CFArrayGetCountOfValue");
    p.def<&a_apply>("_CFArrayApplyFunction");
    p.def<&a_sort>("_CFArraySortValues");
    p.def<&a_bsearch>("_CFArrayBSearchValues");

    // CFDictionary
    ObjC::ClassBuilder rd = r.defineClass("__HVMCFRawDictionary", "NSObject");
    c.rawDict = rd.cls();
    rd.m<&rd_count>("count", "I@:").m<&rd_objectForKey>("objectForKey:").m<&rd_dealloc>("dealloc", "v@:");
    p.def<&d_create>("_CFDictionaryCreate");
    p.def<&d_createMutable>("_CFDictionaryCreateMutable");
    p.def<&d_createCopy>("_CFDictionaryCreateCopy");
    p.def<&d_createMutableCopy>("_CFDictionaryCreateMutableCopy");
    p.def<&d_count>("_CFDictionaryGetCount");
    p.def<&d_get>("_CFDictionaryGetValue");
    p.def<&d_getIfPresent>("_CFDictionaryGetValueIfPresent");
    p.def<&d_containsKey>("_CFDictionaryContainsKey");
    p.def<&d_containsValue>("_CFDictionaryContainsValue");
    p.def<&d_countOfValue>("_CFDictionaryGetCountOfValue");
    p.def<&d_set>("_CFDictionarySetValue");
    p.def<&d_add>("_CFDictionaryAddValue");
    p.def<&d_replace>("_CFDictionaryReplaceValue");
    p.def<&d_remove>("_CFDictionaryRemoveValue");
    p.def<&d_removeAll>("_CFDictionaryRemoveAllValues");
    p.def<&d_getKeysAndValues>("_CFDictionaryGetKeysAndValues");
    p.def<&d_apply>("_CFDictionaryApplyFunction");

    // CFSet
    p.def<&set_create>("_CFSetCreate");
    p.def<&set_createMutable>("_CFSetCreateMutable");
    p.def<&set_createCopy>("_CFSetCreateCopy");
    p.def<&set_createMutableCopy>("_CFSetCreateMutableCopy");
    p.def<&set_count>("_CFSetGetCount");
    p.def<&set_contains>("_CFSetContainsValue");
    p.def<&set_getValue>("_CFSetGetValue");
    p.def<&set_add>("_CFSetAddValue");
    p.def<&set_add>("_CFSetSetValue");
    p.def<&set_remove>("_CFSetRemoveValue");
    p.def<&set_removeAll>("_CFSetRemoveAllValues");
    p.def<&set_getValues>("_CFSetGetValues");
    p.def<&set_apply>("_CFSetApplyFunction");

    // CFData
    p.def<&dt_create>("_CFDataCreate");
    p.def<&dt_createNoCopy>("_CFDataCreateWithBytesNoCopy");
    p.def<&dt_createMutable>("_CFDataCreateMutable");
    p.def<&dt_createCopy>("_CFDataCreateCopy");
    p.def<&dt_createMutableCopy>("_CFDataCreateMutableCopy");
    p.def<&dt_length>("_CFDataGetLength");
    p.def<&dt_bytePtr>("_CFDataGetBytePtr");
    p.def<&dt_mutableBytePtr>("_CFDataGetMutableBytePtr");
    p.def<&dt_getBytes>("_CFDataGetBytes");
    p.def<&dt_append>("_CFDataAppendBytes");
    p.def<&dt_setLength>("_CFDataSetLength");
    p.def<&dt_increaseLength>("_CFDataIncreaseLength");
    p.def<&dt_replace>("_CFDataReplaceBytes");
    p.def<&dt_delete>("_CFDataDeleteBytes");

    // CFDate, Gregorian dates, time zones, locales
    p.def<&date_create>("_CFDateCreate");
    p.def<&date_getAbsolute>("_CFDateGetAbsoluteTime");
    p.def<&date_timeSince>("_CFDateGetTimeIntervalSinceDate");
    p.def<&date_compare>("_CFDateCompare");
    p.def<&greg_fromAbsolute>("_CFAbsoluteTimeGetGregorianDate");
    p.def<&greg_toAbsolute>("_CFGregorianDateGetAbsoluteTime");
    p.def<&greg_dayOfWeek>("_CFAbsoluteTimeGetDayOfWeek");
    p.def<&greg_dayOfYear>("_CFAbsoluteTimeGetDayOfYear");
    p.def<&greg_isValid>("_CFGregorianDateIsValid");
    p.def<&tz_copySystem>("_CFTimeZoneCopySystem");
    p.def<&tz_copySystem>("_CFTimeZoneCopyDefault");
    p.def<&tz_secondsFromGMT>("_CFTimeZoneGetSecondsFromGMT");
    p.def<&tz_getName>("_CFTimeZoneGetName");
    p.def<&tz_createWithOffset>("_CFTimeZoneCreateWithTimeIntervalFromGMT");
    p.def<&tz_createWithName>("_CFTimeZoneCreateWithName");
    p.def<&tz_isDST>("_CFTimeZoneIsDaylightSavingTime");
    p.def<&loc_copyCurrent>("_CFLocaleCopyCurrent");
    p.def<&loc_copyCurrent>("_CFLocaleGetSystem");
    p.def<&loc_getIdentifier>("_CFLocaleGetIdentifier");
    p.def<&loc_getValue>("_CFLocaleGetValue");
    p.def<&loc_preferredLanguages>("_CFLocaleCopyPreferredLanguages");
    p.def<&loc_createCanonical>("_CFLocaleCreateCanonicalLanguageIdentifierFromString");
    p.def<&loc_createCanonical>("_CFLocaleCreateCanonicalLocaleIdentifierFromString");
    for (const char* n : {"kCFLocaleCountryCode", "kCFLocaleLanguageCode", "kCFLocaleCurrencyCode", "kCFLocaleCurrencySymbol",
                          "kCFLocaleDecimalSeparator", "kCFLocaleGroupingSeparator", "kCFLocaleUsesMetricSystem", "kCFLocaleIdentifier"}) {
        const uint32_t cell = p.defineDataBlock(std::string("_") + n, 4);
        p.mem().write<uint32_t>(cell, constString(p, std::string(n) + "Key"));
    }

    // CFUUID
    p.def<&uuid_create>("_CFUUIDCreate");
    p.def<&uuid_createString>("_CFUUIDCreateString");
    p.def<&uuid_createFromString>("_CFUUIDCreateFromString");
    p.def<&uuid_createFromUUIDBytes>("_CFUUIDCreateFromUUIDBytes");
    p.defineFunction("_CFUUIDCreateWithBytes", uuid_createWithBytes);
    p.def<&uuid_getBytes>("_CFUUIDGetUUIDBytes");

    // CFPropertyList
    p.def<&pl_createFromXMLData>("_CFPropertyListCreateFromXMLData");
    p.def<&pl_createWithData>("_CFPropertyListCreateWithData");
    p.def<&pl_createXMLData>("_CFPropertyListCreateXMLData");
    p.def<&pl_createData>("_CFPropertyListCreateData");
    p.def<&pl_deepCopy>("_CFPropertyListCreateDeepCopy");
    p.def<&pl_isValid>("_CFPropertyListIsValid");

    // CFAllocator
    p.def<&al_create>("_CFAllocatorCreate");
    p.def<&al_getContext>("_CFAllocatorGetContext");
    p.def<&al_allocate>("_CFAllocatorAllocate");
    p.def<&al_reallocate>("_CFAllocatorReallocate");
    p.def<&al_deallocate>("_CFAllocatorDeallocate");
    p.def<&al_getDefault>("_CFAllocatorGetDefault");
    p.def<&al_setDefault>("_CFAllocatorSetDefault");
    p.def<&al_preferredSize>("_CFAllocatorGetPreferredSizeForSize");
    p.def<&cs_createMutable>("_CFCharacterSetCreateMutable");
    p.def<&cs_createCopy>("_CFCharacterSetCreateCopy");
    p.def<&cs_addRange>("_CFCharacterSetAddCharactersInRange");

    // CFCharacterSet
    p.def<&cs_predefined>("_CFCharacterSetGetPredefined");
    p.def<&cs_isMember>("_CFCharacterSetIsCharacterMember");
    p.def<&cs_isMember>("_CFCharacterSetIsLongCharacterMember");
    p.def<&cs_createWithString>("_CFCharacterSetCreateWithCharactersInString");
    p.def<&cs_createWithRange>("_CFCharacterSetCreateWithCharactersInRange");
    p.def<&cs_createInverted>("_CFCharacterSetCreateInvertedSet");
}

} // namespace Hydrogen::Runtime::Foundation
