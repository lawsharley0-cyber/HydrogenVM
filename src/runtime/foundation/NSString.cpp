// HydrogenVM — NSString / NSMutableString (class cluster backed by UTF-16
// host strings) and constant (literal) strings.
#include "Foundation.hpp"
#include "runtime/libc/Format.hpp"
#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>

namespace Hydrogen::Runtime::Foundation {

namespace {

struct Strings {
    explicit Strings(Process&) {}
    Class nsString = 0, nsMutableString = 0, cfString = 0, constString = 0;
    std::mutex mu;
    std::unordered_map<uint32_t, std::u16string> constants;    // literal cache
    std::unordered_map<uint32_t, uint32_t> constUtf8;          // literal -> UTF-8 buffer
    std::unordered_map<std::string, id> hostConstants;
};
Strings& S(Process& p) { return p.service<Strings>(); }
Strings& S(Ctx& x) { return S(x.proc); }

using U16 = std::u16string;
std::string toUtf8(const U16& s) { return Plist::utf16ToUtf8(s.data(), s.size()); }
U16 fromUtf8(const std::string& s) { return Plist::utf8ToUtf16(s); }

uint32_t cfHash(const U16& s) {
    const uint32_t len = uint32_t(s.size());
    uint32_t result = len;
    auto four = [&](const char16_t* p) { result = result * 67503105u + p[0] * 16974593u + p[1] * 66049u + p[2] * 257u + p[3]; };
    auto one = [&](const char16_t* p) { result = result * 257u + p[0]; };
    const char16_t* c = s.data();
    if (len <= 96) {
        uint32_t i = 0;
        for (; i + 4 <= len; i += 4) four(c + i);
        for (; i < len; ++i) one(c + i);
    } else {
        for (uint32_t i = 0; i < 32; i += 4) four(c + i);
        for (uint32_t i = len / 2 - 16; i < len / 2 + 16; i += 4) four(c + i);
        for (uint32_t i = len - 32; i < len; i += 4) four(c + i);
    }
    return result + (result << (len & 31));
}

char16_t lower(char16_t c) { return (c >= 'A' && c <= 'Z') ? char16_t(c + 32) : (c >= 0xC0 && c <= 0xDE && c != 0xD7) ? char16_t(c + 32) : c; }
char16_t upper(char16_t c) { return (c >= 'a' && c <= 'z') ? char16_t(c - 32) : (c >= 0xE0 && c <= 0xFE && c != 0xF7) ? char16_t(c - 32) : c; }

enum : uint32_t { kCaseInsensitive = 1, kLiteral = 2, kBackwards = 4, kAnchored = 8, kNumeric = 64, kDiacriticInsensitive = 128 };

int compareStrings(const U16& a, const U16& b, uint32_t opts) {
    size_t i = 0, j = 0;
    while (i < a.size() && j < b.size()) {
        if ((opts & kNumeric) && a[i] >= '0' && a[i] <= '9' && b[j] >= '0' && b[j] <= '9') {
            size_t ie = i, je = j;
            while (ie < a.size() && a[ie] >= '0' && a[ie] <= '9') ++ie;
            while (je < b.size() && b[je] >= '0' && b[je] <= '9') ++je;
            U16 na = a.substr(i, ie - i), nb = b.substr(j, je - j);
            while (na.size() > 1 && na[0] == '0') na.erase(0, 1);
            while (nb.size() > 1 && nb[0] == '0') nb.erase(0, 1);
            if (na.size() != nb.size()) return na.size() < nb.size() ? -1 : 1;
            if (na != nb) return na < nb ? -1 : 1;
            i = ie; j = je;
            continue;
        }
        char16_t ca = a[i], cb = b[j];
        if (opts & kCaseInsensitive) { ca = lower(ca); cb = lower(cb); }
        if (ca != cb) return ca < cb ? -1 : 1;
        ++i; ++j;
    }
    if (i < a.size()) return 1;
    if (j < b.size()) return -1;
    return 0;
}

// Returns position or npos.
size_t findString(const U16& hay, const U16& needle, uint32_t opts, size_t from, size_t to) {
    if (needle.empty() || to > hay.size() || from > to || needle.size() > to - from) return U16::npos;
    auto eq = [&](size_t p) {
        for (size_t k = 0; k < needle.size(); ++k) {
            char16_t a = hay[p + k], b = needle[k];
            if (opts & kCaseInsensitive) { a = lower(a); b = lower(b); }
            if (a != b) return false;
        }
        return true;
    };
    const size_t last = to - needle.size();
    if (opts & kBackwards) {
        if (opts & kAnchored) return eq(last) ? last : U16::npos;
        for (size_t p = last + 1; p-- > from;) if (eq(p)) return p;
        return U16::npos;
    }
    if (opts & kAnchored) return eq(from) ? from : U16::npos;
    for (size_t p = from; p <= last; ++p) if (eq(p)) return p;
    return U16::npos;
}

StringData* dataOf(Ctx& x, id s) { return rt(x).hostIf<StringData>(s); }

bool isConstant(Ctx& x, id s) { return rt(x).classOf(s) == S(x).constString; }

U16 constantValue(Ctx& x, id s) {
    auto& st = S(x);
    {
        std::lock_guard<std::mutex> lk(st.mu);
        auto it = st.constants.find(s);
        if (it != st.constants.end()) return it->second;
    }
    const uint32_t flags = x.mem.read<uint32_t>(s + 4), ptr = x.mem.read<uint32_t>(s + 8), len = x.mem.read<uint32_t>(s + 12);
    U16 v;
    if (flags & 0x10) {
        v.resize(len);
        for (uint32_t i = 0; i < len; ++i) v[i] = x.mem.read<uint16_t>(ptr + i * 2);
    } else {
        const std::string raw(reinterpret_cast<const char*>(x.mem.host(ptr)), len);
        bool ok = true;
        v = fromUtf8(decodeBytes(reinterpret_cast<const uint8_t*>(raw.data()), raw.size(), kNSUTF8, &ok));
        if (!ok) v = fromUtf8(decodeBytes(reinterpret_cast<const uint8_t*>(raw.data()), raw.size(), kNSMacOSRoman));
    }
    std::lock_guard<std::mutex> lk(st.mu);
    st.constants[s] = v;
    return v;
}

void setValue(Ctx& x, id s, U16 v) {
    if (StringData* d = dataOf(x, s)) { d->s = std::move(v); return; }
    auto d = std::make_unique<StringData>();
    d->proc = &x.proc;
    d->s = std::move(v);
    rt(x).setHost(s, std::move(d));
}

id newStringObject(Ctx& x, bool isMutable) {
    const id s = rt(x).createInstance(S(x).cfString);
    auto d = std::make_unique<StringData>();
    d->proc = &x.proc;
    d->isMutable = isMutable;
    rt(x).setHost(s, std::move(d));
    return s;
}

// --------------------------------------------------------- string args ---

U16 sv(Ctx& x, id s) { return s ? stringValue(x, s) : U16(); }
std::string fmtString(Ctx& x, id fmt, ArgReader& args) { return formatGuest(x.proc, utf8(x, fmt), args); }

int64_t parseInt(const U16& s) {
    size_t i = 0;
    while (i < s.size() && (s[i] == ' ' || s[i] == '\t' || s[i] == '\n' || s[i] == '\r')) ++i;
    bool neg = false;
    if (i < s.size() && (s[i] == '+' || s[i] == '-')) { neg = s[i] == '-'; ++i; }
    int64_t v = 0;
    while (i < s.size() && s[i] >= '0' && s[i] <= '9') { v = v * 10 + (s[i] - '0'); if (v > (int64_t(1) << 62)) break; ++i; }
    return neg ? -v : v;
}
double parseDouble(const U16& s) { return std::strtod(toUtf8(s).c_str(), nullptr); }

// ---------------------------------------------------------- allocation ---

id ns_allocWithZone(Ctx& x, id self, SEL, uint32_t) {
    auto& st = S(x);
    if (self == st.nsString || self == st.cfString) return newStringObject(x, false);
    if (self == st.nsMutableString) return newStringObject(x, true);
    return rt(x).createInstance(self);   // guest subclass
}
id ns_alloc(Ctx& x, id self, SEL s) { return ns_allocWithZone(x, self, s, 0); }

// init* (return self)
id i_init(Ctx& x, id self, SEL) { setValue(x, self, U16()); return self; }
id i_initWithString(Ctx& x, id self, SEL, id other) { setValue(x, self, sv(x, other)); return self; }
id i_initWithUTF8String(Ctx& x, id self, SEL, GuestPtr c) {
    if (!c) { rt(x).release(x.cpu, self); return 0; }
    setValue(x, self, fromUtf8(x.proc.readCString(c)));
    return self;
}
id i_initWithCString(Ctx& x, id self, SEL, GuestPtr c, uint32_t enc) {
    if (!c) { rt(x).release(x.cpu, self); return 0; }
    const std::string raw = x.proc.readCString(c);
    bool ok = true;
    const std::string u = decodeBytes(reinterpret_cast<const uint8_t*>(raw.data()), raw.size(), enc, &ok);
    if (!ok) { rt(x).release(x.cpu, self); return 0; }
    setValue(x, self, fromUtf8(u));
    return self;
}
id i_initWithCStringDefault(Ctx& x, id self, SEL s, GuestPtr c) { return i_initWithCString(x, self, s, c, kNSMacOSRoman); }
id i_initWithFormat(Ctx& x, id self, SEL, id fmt, VarArgs va) { setValue(x, self, fromUtf8(fmtString(x, fmt, va.args))); return self; }
id i_initWithFormatArgs(Ctx& x, id self, SEL, id fmt, VaList ap) { ArgReader a(x.mem, ap.ptr); setValue(x, self, fromUtf8(fmtString(x, fmt, a))); return self; }
id i_initWithCharacters(Ctx& x, id self, SEL, GuestPtr chars, uint32_t len) {
    U16 v(len, u'\0');
    for (uint32_t i = 0; i < len; ++i) v[i] = x.mem.read<uint16_t>(chars + i * 2);
    setValue(x, self, std::move(v));
    return self;
}
id i_initWithBytes(Ctx& x, id self, SEL, GuestPtr bytes, uint32_t len, uint32_t enc) {
    bool ok = true;
    const std::string u = decodeBytes(x.mem.host(bytes), len, enc, &ok);
    if (!ok) { rt(x).release(x.cpu, self); return 0; }
    setValue(x, self, fromUtf8(u));
    return self;
}
id i_initWithBytesNoCopy(Ctx& x, id self, SEL s, GuestPtr bytes, uint32_t len, uint32_t enc, bool freeWhenDone) {
    const id r = i_initWithBytes(x, self, s, bytes, len, enc);
    if (freeWhenDone) x.proc.heap().free(bytes);
    return r;
}
id i_initWithData(Ctx& x, id self, SEL, id data, uint32_t enc) {
    const auto b = dataBytes(x, data);
    bool ok = true;
    const std::string u = decodeBytes(b.data(), b.size(), enc, &ok);
    if (!ok) { rt(x).release(x.cpu, self); return 0; }
    setValue(x, self, fromUtf8(u));
    return self;
}
bool readFileText(const std::string& path, std::string& out) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return false;
    out.assign((std::istreambuf_iterator<char>(in)), {});
    return true;
}
id i_initWithContentsOfFile(Ctx& x, id self, SEL, id path, uint32_t enc, GuestPtr err) {
    std::string raw;
    if (!readFileText(utf8(x, path), raw)) { if (err) x.mem.write<uint32_t>(err, 0); rt(x).release(x.cpu, self); return 0; }
    uint32_t e = enc;
    if (raw.size() >= 2 && ((uint8_t(raw[0]) == 0xFF && uint8_t(raw[1]) == 0xFE) || (uint8_t(raw[0]) == 0xFE && uint8_t(raw[1]) == 0xFF))) e = kNSUnicode;
    bool ok = true;
    const std::string u = decodeBytes(reinterpret_cast<const uint8_t*>(raw.data()), raw.size(), e, &ok);
    if (!ok) { rt(x).release(x.cpu, self); return 0; }
    setValue(x, self, fromUtf8(u));
    return self;
}
id i_initWithContentsOfFileUsed(Ctx& x, id self, SEL s, id path, GuestPtr usedEnc, GuestPtr err) {
    if (usedEnc) x.mem.write<uint32_t>(usedEnc, kNSUTF8);
    return i_initWithContentsOfFile(x, self, s, path, kNSUTF8, err);
}
id i_initWithContentsOfFile1(Ctx& x, id self, SEL s, id path) { return i_initWithContentsOfFile(x, self, s, path, kNSUTF8, 0); }
id i_initWithCapacity(Ctx& x, id self, SEL, uint32_t) { setValue(x, self, U16()); return self; }

// convenience constructors: [[[self alloc] initXxx] autorelease]
template <typename... A, typename F>
id construct(Ctx& x, id cls, F init, A... a) {
    id obj = ns_allocWithZone(x, cls, 0, 0);
    obj = init(x, obj, 0, a...);
    return obj ? autoreleased(x, obj) : 0;
}
id c_string(Ctx& x, id self, SEL) { return construct(x, self, i_init); }
id c_stringWithString(Ctx& x, id self, SEL, id o) { return construct(x, self, i_initWithString, o); }
id c_stringWithUTF8String(Ctx& x, id self, SEL, GuestPtr c) { return construct(x, self, i_initWithUTF8String, c); }
id c_stringWithCString(Ctx& x, id self, SEL, GuestPtr c, uint32_t e) { return construct(x, self, i_initWithCString, c, e); }
id c_stringWithCStringDefault(Ctx& x, id self, SEL, GuestPtr c) { return construct(x, self, i_initWithCStringDefault, c); }
id c_stringWithFormat(Ctx& x, id self, SEL, id fmt, VarArgs va) {
    id obj = ns_allocWithZone(x, self, 0, 0);
    setValue(x, obj, fromUtf8(fmtString(x, fmt, va.args)));
    return autoreleased(x, obj);
}
id c_stringWithCharacters(Ctx& x, id self, SEL, GuestPtr c, uint32_t n) { return construct(x, self, i_initWithCharacters, c, n); }
id c_stringWithContentsOfFile(Ctx& x, id self, SEL, id path, uint32_t enc, GuestPtr err) { return construct(x, self, i_initWithContentsOfFile, path, enc, err); }
id c_stringWithContentsOfFileUsed(Ctx& x, id self, SEL, id path, GuestPtr used, GuestPtr err) { return construct(x, self, i_initWithContentsOfFileUsed, path, used, err); }
id c_stringWithContentsOfFile1(Ctx& x, id self, SEL, id path) { return construct(x, self, i_initWithContentsOfFile1, path); }
id c_stringWithCapacity(Ctx& x, id self, SEL, uint32_t n) { return construct(x, self, i_initWithCapacity, n); }
id c_pathWithComponents(Ctx& x, id, SEL, id arr) {
    std::string out;
    for (id c : arrayItems(x, arr)) {
        std::string p = utf8(x, c);
        if (out.empty()) out = p;
        else if (p == "/") continue;
        else out += (out.back() == '/' ? "" : "/") + p;
    }
    return makeString(x, out);
}

// ------------------------------------------------------------- queries ---

uint32_t s_length(Ctx& x, id self, SEL) { return uint32_t(sv(x, self).size()); }
uint32_t s_characterAtIndex(Ctx& x, id self, SEL, uint32_t i) {
    const U16 v = sv(x, self);
    if (i >= v.size()) throw ObjC::ObjCError("-[NSString characterAtIndex:]: index " + std::to_string(i) + " out of bounds");
    return v[i];
}
void s_getCharactersRange(Ctx& x, id self, SEL, GuestPtr buf, uint32_t loc, uint32_t len) {
    const U16 v = sv(x, self);
    for (uint32_t i = 0; i < len && loc + i < v.size(); ++i) x.mem.write<uint16_t>(buf + i * 2, v[loc + i]);
}
void s_getCharacters(Ctx& x, id self, SEL, GuestPtr buf) {
    const U16 v = sv(x, self);
    for (size_t i = 0; i < v.size(); ++i) x.mem.write<uint16_t>(buf + uint32_t(i) * 2, v[i]);
}
uint32_t utf8Pointer(Ctx& x, id self) {
    auto& st = S(x);
    if (isConstant(x, self)) {
        std::lock_guard<std::mutex> lk(st.mu);
        auto it = st.constUtf8.find(self);
        if (it != st.constUtf8.end()) return it->second;
    }
    const std::string u = toUtf8(sv(x, self));
    if (isConstant(x, self)) {
        const uint32_t flags = x.mem.read<uint32_t>(self + 4);
        uint32_t ptr = x.mem.read<uint32_t>(self + 8);
        bool ascii = !(flags & 0x10);
        if (ascii) for (char c : u) if (uint8_t(c) >= 0x80) { ascii = false; break; }
        if (!ascii) ptr = x.proc.internCString(u);
        std::lock_guard<std::mutex> lk(st.mu);
        st.constUtf8[self] = ptr;
        return ptr;
    }
    if (StringData* d = dataOf(x, self)) {
        if (d->utf8Buffer && d->utf8Len == u.size() && std::memcmp(x.mem.host(d->utf8Buffer), u.c_str(), u.size() + 1) == 0) return d->utf8Buffer;
        // Keep the old buffer alive (callers may still hold it); it's freed with the string.
        const uint32_t buf = x.proc.allocCString(u);
        if (d->utf8Buffer) { x.proc.heap().free(d->utf8Buffer); }
        d->utf8Buffer = buf;
        d->utf8Len = uint32_t(u.size());
        return buf;
    }
    return x.proc.allocCString(u);   // guest subclass: leaks a little, like an autoreleased buffer
}
uint32_t s_UTF8String(Ctx& x, id self, SEL) { return utf8Pointer(x, self); }
uint32_t s_cStringUsingEncoding(Ctx& x, id self, SEL, uint32_t enc) {
    if (enc == kNSUTF8 || enc == kNSASCII) return utf8Pointer(x, self);
    bool ok = true;
    const std::string b = encodeBytes(sv(x, self), enc, false, &ok);
    if (!ok) return 0;
    return x.proc.allocCString(b);
}
bool s_getCString(Ctx& x, id self, SEL, GuestPtr buf, uint32_t max, uint32_t enc) {
    bool ok = true;
    const std::string b = encodeBytes(sv(x, self), enc, false, &ok);
    if (!ok || b.size() + 1 > max) return false;
    x.mem.copyIn(buf, b.c_str(), b.size() + 1);
    return true;
}
bool s_canConvert(Ctx& x, id self, SEL, uint32_t enc) { bool ok = true; encodeBytes(sv(x, self), enc, false, &ok); return ok; }
uint32_t s_lengthOfBytes(Ctx& x, id self, SEL, uint32_t enc) { bool ok = true; const std::string b = encodeBytes(sv(x, self), enc, false, &ok); return ok ? uint32_t(b.size()) : 0; }
uint32_t s_maxLengthOfBytes(Ctx& x, id self, SEL, uint32_t enc) { return uint32_t(sv(x, self).size()) * (enc == kNSUTF8 ? 3 : enc == kNSUnicode ? 2 : 1); }
id s_dataUsingEncoding(Ctx& x, id self, SEL, uint32_t enc) {
    bool ok = true;
    const std::string b = encodeBytes(sv(x, self), enc, false, &ok);
    return ok ? makeData(x, b.data(), uint32_t(b.size())) : 0;
}
id s_dataUsingEncodingLossy(Ctx& x, id self, SEL, uint32_t enc, bool lossy) {
    bool ok = true;
    const std::string b = encodeBytes(sv(x, self), enc, lossy, &ok);
    return ok ? makeData(x, b.data(), uint32_t(b.size())) : 0;
}
bool s_isEqualToString(Ctx& x, id self, SEL, id other) { return other == self || (other && sv(x, self) == sv(x, other)); }
bool s_isEqual(Ctx& x, id self, SEL, id other) { return other == self || (other && isString(x, other) && sv(x, self) == sv(x, other)); }
uint32_t s_hash(Ctx& x, id self, SEL) { return cfHash(sv(x, self)); }
int32_t s_compare(Ctx& x, id self, SEL, id other) { return compareStrings(sv(x, self), sv(x, other), 0); }
int32_t s_compareOptions(Ctx& x, id self, SEL, id other, uint32_t opts) { return compareStrings(sv(x, self), sv(x, other), opts); }
int32_t s_compareOptionsRange(Ctx& x, id self, SEL, id other, uint32_t opts, uint32_t loc, uint32_t len) {
    const U16 v = sv(x, self);
    return compareStrings(v.substr(std::min<size_t>(loc, v.size()), len), sv(x, other), opts);
}
int32_t s_caseInsensitiveCompare(Ctx& x, id self, SEL, id other) { return compareStrings(sv(x, self), sv(x, other), kCaseInsensitive); }
int32_t s_localizedCompare(Ctx& x, id self, SEL, id other) { return compareStrings(sv(x, self), sv(x, other), 0); }
bool s_hasPrefix(Ctx& x, id self, SEL, id p) { const U16 v = sv(x, self), q = sv(x, p); return !q.empty() && v.compare(0, q.size(), q) == 0; }
bool s_hasSuffix(Ctx& x, id self, SEL, id p) { const U16 v = sv(x, self), q = sv(x, p); return !q.empty() && v.size() >= q.size() && v.compare(v.size() - q.size(), q.size(), q) == 0; }
void s_rangeOfStringOptionsRange(Ctx& x, GuestPtr out, id self, SEL, id needle, uint32_t opts, uint32_t loc, uint32_t len) {
    const U16 v = sv(x, self);
    const size_t p = findString(v, sv(x, needle), opts, loc, std::min<size_t>(v.size(), size_t(loc) + len));
    writeRange(x, out, p == U16::npos ? Range{kNotFound, 0} : Range{uint32_t(p), uint32_t(sv(x, needle).size())});
}
void s_rangeOfStringOptions(Ctx& x, GuestPtr out, id self, SEL s, id needle, uint32_t opts) { s_rangeOfStringOptionsRange(x, out, self, s, needle, opts, 0, uint32_t(sv(x, self).size())); }
void s_rangeOfString(Ctx& x, GuestPtr out, id self, SEL s, id needle) { s_rangeOfStringOptions(x, out, self, s, needle, 0); }
bool s_containsString(Ctx& x, id self, SEL, id needle) { return findString(sv(x, self), sv(x, needle), 0, 0, sv(x, self).size()) != U16::npos; }

// character sets are host predicates (see Misc.cpp)
}   // namespace
namespace {

void s_rangeOfCharacterFromSet(Ctx& x, GuestPtr out, id self, SEL, id set) {
    const U16 v = sv(x, self);
    for (size_t i = 0; i < v.size(); ++i)
        if (characterSetContains(x, set, v[i])) { writeRange(x, out, {uint32_t(i), 1}); return; }
    writeRange(x, out, {kNotFound, 0});
}
void s_rangeOfCharacterFromSetOptions(Ctx& x, GuestPtr out, id self, SEL, id set, uint32_t opts) {
    const U16 v = sv(x, self);
    if (opts & kBackwards) {
        for (size_t i = v.size(); i-- > 0;) if (characterSetContains(x, set, v[i])) { writeRange(x, out, {uint32_t(i), 1}); return; }
    } else {
        for (size_t i = 0; i < v.size(); ++i) if (characterSetContains(x, set, v[i])) { writeRange(x, out, {uint32_t(i), 1}); return; }
    }
    writeRange(x, out, {kNotFound, 0});
}

void checkRange(const U16& v, uint32_t loc, uint32_t len, const char* what) {
    if (uint64_t(loc) + len > v.size())
        throw ObjC::ObjCError(std::string("-[NSString ") + what + "]: range {" + std::to_string(loc) + ", " + std::to_string(len) + "} out of bounds; string length " + std::to_string(v.size()));
}
id s_substringFromIndex(Ctx& x, id self, SEL, uint32_t i) { const U16 v = sv(x, self); checkRange(v, i, 0, "substringFromIndex:"); return makeString(x, v.substr(i)); }
id s_substringToIndex(Ctx& x, id self, SEL, uint32_t i) { const U16 v = sv(x, self); checkRange(v, 0, i, "substringToIndex:"); return makeString(x, v.substr(0, i)); }
id s_substringWithRange(Ctx& x, id self, SEL, uint32_t loc, uint32_t len) { const U16 v = sv(x, self); checkRange(v, loc, len, "substringWithRange:"); return makeString(x, v.substr(loc, len)); }
id s_appending(Ctx& x, id self, SEL, id o) { return makeString(x, sv(x, self) + sv(x, o)); }
id s_appendingFormat(Ctx& x, id self, SEL, id fmt, VarArgs va) { return makeString(x, sv(x, self) + fromUtf8(fmtString(x, fmt, va.args))); }
id s_replacingOccurrences(Ctx& x, id self, SEL, id target, id repl) {
    U16 v = sv(x, self);
    const U16 t = sv(x, target), r = sv(x, repl);
    if (t.empty()) return makeString(x, v);
    U16 out;
    size_t pos = 0, p;
    while ((p = v.find(t, pos)) != U16::npos) { out += v.substr(pos, p - pos) + r; pos = p + t.size(); }
    out += v.substr(pos);
    return makeString(x, out);
}
id s_replacingOccurrencesOptions(Ctx& x, id self, SEL, id target, id repl, uint32_t opts, uint32_t loc, uint32_t len) {
    const U16 v = sv(x, self), t = sv(x, target), r = sv(x, repl);
    if (t.empty()) return makeString(x, v);
    U16 out = v.substr(0, loc);
    size_t pos = loc, end = std::min<size_t>(v.size(), size_t(loc) + len), p;
    while ((p = findString(v, t, opts & ~(kBackwards | kAnchored), pos, end)) != U16::npos) { out += v.substr(pos, p - pos) + r; pos = p + t.size(); }
    out += v.substr(pos);
    return makeString(x, out);
}
id s_replacingCharactersInRange(Ctx& x, id self, SEL, uint32_t loc, uint32_t len, id repl) {
    U16 v = sv(x, self);
    checkRange(v, loc, len, "stringByReplacingCharactersInRange:withString:");
    v.replace(loc, len, sv(x, repl));
    return makeString(x, v);
}
id s_trimming(Ctx& x, id self, SEL, id set) {
    const U16 v = sv(x, self);
    size_t a = 0, b = v.size();
    while (a < b && characterSetContains(x, set, v[a])) ++a;
    while (b > a && characterSetContains(x, set, v[b - 1])) --b;
    return makeString(x, v.substr(a, b - a));
}
id s_componentsSeparatedByString(Ctx& x, id self, SEL, id sep) {
    const U16 v = sv(x, self), s = sv(x, sep);
    std::vector<id> parts;
    if (s.empty()) { parts.push_back(makeString(x, v)); return makeArray(x, parts); }
    size_t pos = 0, p;
    while ((p = v.find(s, pos)) != U16::npos) { parts.push_back(makeString(x, v.substr(pos, p - pos))); pos = p + s.size(); }
    parts.push_back(makeString(x, v.substr(pos)));
    return makeArray(x, parts);
}
id s_componentsSeparatedByCharacters(Ctx& x, id self, SEL, id set) {
    const U16 v = sv(x, self);
    std::vector<id> parts;
    size_t start = 0;
    for (size_t i = 0; i <= v.size(); ++i)
        if (i == v.size() || characterSetContains(x, set, v[i])) { parts.push_back(makeString(x, v.substr(start, i - start))); start = i + 1; }
    return makeArray(x, parts);
}
id s_lowercase(Ctx& x, id self, SEL) { U16 v = sv(x, self); for (auto& c : v) c = lower(c); return makeString(x, v); }
id s_uppercase(Ctx& x, id self, SEL) { U16 v = sv(x, self); for (auto& c : v) c = upper(c); return makeString(x, v); }
id s_capitalized(Ctx& x, id self, SEL) {
    U16 v = sv(x, self);
    bool start = true;
    for (auto& c : v) {
        const bool alpha = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c >= 0xC0;
        c = alpha ? (start ? upper(c) : lower(c)) : c;
        start = !alpha && !(c >= '0' && c <= '9');
    }
    return makeString(x, v);
}
int32_t s_intValue(Ctx& x, id self, SEL) { const int64_t v = parseInt(sv(x, self)); return v > INT32_MAX ? INT32_MAX : v < INT32_MIN ? INT32_MIN : int32_t(v); }
int64_t s_longLongValue(Ctx& x, id self, SEL) { return parseInt(sv(x, self)); }
float s_floatValue(Ctx& x, id self, SEL) { return float(parseDouble(sv(x, self))); }
double s_doubleValue(Ctx& x, id self, SEL) { return parseDouble(sv(x, self)); }
bool s_boolValue(Ctx& x, id self, SEL) {
    const U16 v = sv(x, self);
    size_t i = 0;
    while (i < v.size() && (v[i] == ' ' || v[i] == '\t' || v[i] == '\n')) ++i;
    if (i < v.size() && (v[i] == '+' || v[i] == '-')) ++i;
    while (i < v.size() && v[i] == '0') ++i;
    if (i >= v.size()) return false;
    const char16_t c = v[i];
    return c == 'Y' || c == 'y' || c == 'T' || c == 't' || (c >= '1' && c <= '9');
}
id s_self(Ctx&, id self, SEL) { return self; }
id s_copyWithZone(Ctx& x, id self, SEL, uint32_t) {
    StringData* d = dataOf(x, self);
    if (isConstant(x, self)) return self;
    if (d && !d->isMutable) return rt(x).retain(x.cpu, self);
    return makeString(x, sv(x, self), true, false);
}
id s_mutableCopyWithZone(Ctx& x, id self, SEL, uint32_t) { return makeString(x, sv(x, self), true, true); }
id s_copy(Ctx& x, id self, SEL s) { return s_copyWithZone(x, self, s, 0); }
id s_mutableCopy(Ctx& x, id self, SEL s) { return s_mutableCopyWithZone(x, self, s, 0); }

// ------------------------------------------------------------- paths ---

namespace fsys = std::filesystem;
std::string pathOf(Ctx& x, id s) { return toUtf8(sv(x, s)); }
std::string trimSlash(std::string p) { while (p.size() > 1 && p.back() == '/') p.pop_back(); return p; }
id s_appendingPathComponent(Ctx& x, id self, SEL, id comp) {
    std::string a = trimSlash(pathOf(x, self)), b = pathOf(x, comp);
    while (!b.empty() && b[0] == '/') b.erase(0, 1);
    if (a.empty()) return makeString(x, b);
    if (b.empty()) return makeString(x, a);
    return makeString(x, a == "/" ? "/" + b : a + "/" + b);
}
id s_appendingPathExtension(Ctx& x, id self, SEL, id ext) { return makeString(x, trimSlash(pathOf(x, self)) + "." + pathOf(x, ext)); }
id s_lastPathComponent(Ctx& x, id self, SEL) {
    const std::string p = trimSlash(pathOf(x, self));
    if (p == "/") return makeString(x, "/");
    const size_t s = p.rfind('/');
    return makeString(x, s == std::string::npos ? p : p.substr(s + 1));
}
id s_pathExtension(Ctx& x, id self, SEL) {
    const std::string p = trimSlash(pathOf(x, self));
    const size_t s = p.rfind('/'), d = p.rfind('.');
    if (d == std::string::npos || (s != std::string::npos && d < s) || d == (s == std::string::npos ? 0 : s + 1)) return makeString(x, "");
    return makeString(x, p.substr(d + 1));
}
id s_deletingLastPathComponent(Ctx& x, id self, SEL) {
    const std::string p = trimSlash(pathOf(x, self));
    const size_t s = p.rfind('/');
    if (s == std::string::npos) return makeString(x, "");
    if (s == 0) return makeString(x, "/");
    return makeString(x, p.substr(0, s));
}
id s_deletingPathExtension(Ctx& x, id self, SEL) {
    const std::string p = trimSlash(pathOf(x, self));
    const size_t s = p.rfind('/'), d = p.rfind('.');
    if (d == std::string::npos || (s != std::string::npos && d < s) || d == (s == std::string::npos ? 0 : s + 1)) return makeString(x, p);
    return makeString(x, p.substr(0, d));
}
id s_pathComponents(Ctx& x, id self, SEL) {
    const std::string p = pathOf(x, self);
    std::vector<id> parts;
    if (!p.empty() && p[0] == '/') parts.push_back(makeString(x, "/"));
    size_t i = 0;
    while (i < p.size()) {
        while (i < p.size() && p[i] == '/') ++i;
        size_t j = i;
        while (j < p.size() && p[j] != '/') ++j;
        if (j > i) parts.push_back(makeString(x, p.substr(i, j - i)));
        i = j;
    }
    return makeArray(x, parts);
}
bool s_isAbsolutePath(Ctx& x, id self, SEL) { const std::string p = pathOf(x, self); return !p.empty() && (p[0] == '/' || p[0] == '~'); }
id s_expandingTilde(Ctx& x, id self, SEL) {
    std::string p = pathOf(x, self);
    if (!p.empty() && p[0] == '~') p = x.proc.homePath() + p.substr(1);
    return makeString(x, p);
}
id s_standardizing(Ctx& x, id self, SEL) {
    std::string p = pathOf(x, self);
    if (!p.empty() && p[0] == '~') p = x.proc.homePath() + p.substr(1);
    return makeString(x, trimSlash(fsys::path(p).lexically_normal().string()));
}
uint32_t s_fileSystemRepresentation(Ctx& x, id self, SEL) { return utf8Pointer(x, s_expandingTilde(x, self, 0)); }
bool s_getFileSystemRepresentation(Ctx& x, id self, SEL, GuestPtr buf, uint32_t max) {
    const std::string p = pathOf(x, self);
    if (p.size() + 1 > max) return false;
    x.mem.copyIn(buf, p.c_str(), p.size() + 1);
    return true;
}
bool s_writeToFileEnc(Ctx& x, id self, SEL, id path, bool, uint32_t enc, GuestPtr err) {
    bool ok = true;
    const std::string b = encodeBytes(sv(x, self), enc, false, &ok);
    std::ofstream out(utf8(x, path), std::ios::binary | std::ios::trunc);
    if (!ok || !out) { if (err) x.mem.write<uint32_t>(err, 0); return false; }
    out.write(b.data(), std::streamsize(b.size()));
    return bool(out);
}
bool s_writeToFile(Ctx& x, id self, SEL s, id path, bool atomically) { return s_writeToFileEnc(x, self, s, path, atomically, kNSUTF8, 0); }
id s_addingPercentEscapes(Ctx& x, id self, SEL, uint32_t) {
    const std::string u = utf8(x, self);
    std::string out;
    static const char* safe = "!$&'()*+,-./:;=?@_~";
    char buf[4];
    for (unsigned char c : u) {
        if (std::isalnum(c) || std::strchr(safe, c)) out += char(c);
        else { std::snprintf(buf, sizeof buf, "%%%02X", c); out += buf; }
    }
    return makeString(x, out);
}
id s_replacingPercentEscapes(Ctx& x, id self, SEL, uint32_t) {
    const std::string u = utf8(x, self);
    std::string out;
    for (size_t i = 0; i < u.size(); ++i) {
        if (u[i] == '%' && i + 2 < u.size() && std::isxdigit(uint8_t(u[i + 1])) && std::isxdigit(uint8_t(u[i + 2]))) {
            out += char(std::stoi(u.substr(i + 1, 2), nullptr, 16));
            i += 2;
        } else out += u[i];
    }
    return makeString(x, out);
}

// ------------------------------------------------------------- mutation ---

U16& mutableValue(Ctx& x, id self) {
    StringData* d = dataOf(x, self);
    if (!d) throw ObjC::ObjCError("attempt to mutate a string that isn't an NSMutableString (" + rt(x).objectClassName(self) + ")");
    return d->s;
}
void m_appendString(Ctx& x, id self, SEL, id o) { if (o) mutableValue(x, self) += sv(x, o); }
void m_appendFormat(Ctx& x, id self, SEL, id fmt, VarArgs va) { mutableValue(x, self) += fromUtf8(fmtString(x, fmt, va.args)); }
void m_setString(Ctx& x, id self, SEL, id o) { mutableValue(x, self) = sv(x, o); }
void m_insertString(Ctx& x, id self, SEL, id o, uint32_t at) {
    U16& v = mutableValue(x, self);
    checkRange(v, at, 0, "insertString:atIndex:");
    v.insert(at, sv(x, o));
}
void m_deleteCharacters(Ctx& x, id self, SEL, uint32_t loc, uint32_t len) {
    U16& v = mutableValue(x, self);
    checkRange(v, loc, len, "deleteCharactersInRange:");
    v.erase(loc, len);
}
void m_replaceCharacters(Ctx& x, id self, SEL, uint32_t loc, uint32_t len, id o) {
    U16& v = mutableValue(x, self);
    checkRange(v, loc, len, "replaceCharactersInRange:withString:");
    v.replace(loc, len, sv(x, o));
}
uint32_t m_replaceOccurrences(Ctx& x, id self, SEL, id target, id repl, uint32_t opts, uint32_t loc, uint32_t len) {
    U16& v = mutableValue(x, self);
    const U16 t = sv(x, target), r = sv(x, repl);
    if (t.empty()) return 0;
    uint32_t n = 0;
    size_t pos = loc, end = std::min<size_t>(v.size(), size_t(loc) + len), p;
    while ((p = findString(v, t, opts & ~(kBackwards | kAnchored), pos, end)) != U16::npos) {
        v.replace(p, t.size(), r);
        end = end - t.size() + r.size();
        pos = p + r.size();
        ++n;
    }
    return n;
}

void str_dealloc(Ctx& x, id self, SEL) { rt(x).disposeInstance(x.cpu, self); }
void const_noop(Ctx&, id, SEL) {}
id const_self(Ctx&, id self, SEL) { return self; }
uint32_t const_retainCount(Ctx&, id, SEL) { return 0xFFFFFFFFu; }

} // namespace

// ------------------------------------------------------------ public API ---

StringData::~StringData() {
    if (utf8Buffer && proc) proc->heap().free(utf8Buffer);
}

id makeString(Ctx& x, const std::u16string& s, bool owned, bool isMutable) {
    const id obj = newStringObject(x, isMutable);
    dataOf(x, obj)->s = s;
    return owned ? obj : autoreleased(x, obj);
}
id makeString(Ctx& x, const std::string& utf8s, bool owned, bool isMutable) { return makeString(x, fromUtf8(utf8s), owned, isMutable); }

bool isString(Ctx& x, id obj) {
    if (!obj) return false;
    auto& st = S(x);
    const Class c = rt(x).classOf(obj);
    if (c == st.cfString || c == st.constString) return true;
    return rt(x).isSubclass(c, st.nsString);
}

std::u16string stringValue(Ctx& x, id str) {
    if (!str) return {};
    auto& st = S(x);
    const Class c = rt(x).classOf(str);
    if (c == st.constString) return constantValue(x, str);
    if (StringData* d = dataOf(x, str)) return d->s;
    if (rt(x).isSubclass(c, st.nsString)) {
        // A guest NSString subclass: use its primitive methods.
        const uint32_t n = uint32_t(rt(x).send(x.cpu, str, "length"));
        U16 v(n, u'\0');
        for (uint32_t i = 0; i < n; ++i) v[i] = char16_t(rt(x).send(x.cpu, str, "characterAtIndex:", {i}));
        return v;
    }
    return fromUtf8(describe(x, str));
}

std::string utf8(Ctx& x, id str) { return toUtf8(stringValue(x, str)); }

id constString(Process& p, const std::string& s) {
    auto& st = S(p);
    {
        std::lock_guard<std::mutex> lk(st.mu);
        auto it = st.hostConstants.find(s);
        if (it != st.hostConstants.end()) return it->second;
    }
    // Same layout as a compiler-emitted literal.
    const uint32_t obj = p.allocStatic(16);
    const uint32_t bytes = p.internCString(s);
    bool ascii = true;
    for (char c : s) if (uint8_t(c) >= 0x80) ascii = false;
    p.mem().write<uint32_t>(obj, st.constString);
    if (ascii) {
        p.mem().write<uint32_t>(obj + 4, 0x7C8);
        p.mem().write<uint32_t>(obj + 8, bytes);
        p.mem().write<uint32_t>(obj + 12, uint32_t(s.size()));
    } else {
        const U16 u = fromUtf8(s);
        const uint32_t buf = p.allocStatic(uint32_t(u.size() * 2 + 2), 2);
        for (size_t i = 0; i < u.size(); ++i) p.mem().write<uint16_t>(buf + uint32_t(i) * 2, u[i]);
        p.mem().write<uint32_t>(obj + 4, 0x7D0);
        p.mem().write<uint32_t>(obj + 8, buf);
        p.mem().write<uint32_t>(obj + 12, uint32_t(u.size()));
    }
    std::lock_guard<std::mutex> lk(st.mu);
    st.hostConstants[s] = obj;
    return obj;
}

std::string decodeBytes(const uint8_t* p, size_t n, uint32_t enc, bool* ok) {
    if (ok) *ok = true;
    switch (enc) {
        case kNSUTF8: case kNSASCII: case kNSNonLossyASCII: {
            // Validate UTF-8; ASCII decoding also accepts it.
            size_t i = 0;
            while (i < n) {
                const uint8_t b = p[i];
                size_t len = b < 0x80 ? 1 : (b & 0xE0) == 0xC0 ? 2 : (b & 0xF0) == 0xE0 ? 3 : (b & 0xF8) == 0xF0 ? 4 : 0;
                if (!len || i + len > n) { if (ok) *ok = false; break; }
                for (size_t k = 1; k < len; ++k) if ((p[i + k] & 0xC0) != 0x80) { len = 0; break; }
                if (!len) { if (ok) *ok = false; break; }
                i += len;
            }
            if (ok && !*ok && enc == kNSASCII) { *ok = true; std::string s; for (size_t k = 0; k < n; ++k) s += char(p[k] & 0x7F); return s; }
            size_t end = n;
            while (end && p[end - 1] == 0) --end;   // tolerate trailing NULs
            return std::string(reinterpret_cast<const char*>(p), end);
        }
        case kNSUnicode: case kNSUTF16BE: case kNSUTF16LE: {
            bool be = enc == kNSUTF16BE;
            size_t i = 0;
            if (enc == kNSUnicode && n >= 2) {
                if (p[0] == 0xFE && p[1] == 0xFF) { be = true; i = 2; }
                else if (p[0] == 0xFF && p[1] == 0xFE) { be = false; i = 2; }
            }
            U16 u;
            for (; i + 1 < n; i += 2) u += char16_t(be ? (p[i] << 8 | p[i + 1]) : (p[i + 1] << 8 | p[i]));
            return toUtf8(u);
        }
        case kNSUTF32: {
            U16 u;
            for (size_t i = 0; i + 3 < n; i += 4) {
                const uint32_t c = uint32_t(p[i]) | uint32_t(p[i + 1]) << 8 | uint32_t(p[i + 2]) << 16 | uint32_t(p[i + 3]) << 24;
                if (c >= 0x10000) { u += char16_t(0xD800 + ((c - 0x10000) >> 10)); u += char16_t(0xDC00 + ((c - 0x10000) & 0x3FF)); }
                else u += char16_t(c);
            }
            return toUtf8(u);
        }
        case kNSMacOSRoman: {
            static const char16_t hi[128] = {
                0xC4,0xC5,0xC7,0xC9,0xD1,0xD6,0xDC,0xE1,0xE0,0xE2,0xE4,0xE3,0xE5,0xE7,0xE9,0xE8,0xEA,0xEB,0xED,0xEC,0xEE,0xEF,0xF1,0xF3,0xF2,0xF4,0xF6,0xF5,0xFA,0xF9,0xFB,0xFC,
                0x2020,0xB0,0xA2,0xA3,0xA7,0x2022,0xB6,0xDF,0xAE,0xA9,0x2122,0xB4,0xA8,0x2260,0xC6,0xD8,0x221E,0xB1,0x2264,0x2265,0xA5,0xB5,0x2202,0x2211,0x220F,0x3C0,0x222B,0xAA,0xBA,0x3A9,0xE6,0xF8,
                0xBF,0xA1,0xAC,0x221A,0x192,0x2248,0x2206,0xAB,0xBB,0x2026,0xA0,0xC0,0xC3,0xD5,0x152,0x153,0x2013,0x2014,0x201C,0x201D,0x2018,0x2019,0xF7,0x25CA,0xFF,0x178,0x2044,0x20AC,0x2039,0x203A,0xFB01,0xFB02,
                0x2021,0xB7,0x201A,0x201E,0x2030,0xC2,0xCA,0xC1,0xCB,0xC8,0xCD,0xCE,0xCF,0xCC,0xD3,0xD4,0xF8FF,0xD2,0xDA,0xDB,0xD9,0x131,0x2C6,0x2DC,0xAF,0x2D8,0x2D9,0x2DA,0xB8,0x2DD,0x2DB,0x2C7};
            U16 u;
            for (size_t i = 0; i < n && p[i]; ++i) u += p[i] < 0x80 ? char16_t(p[i]) : hi[p[i] - 0x80];
            return toUtf8(u);
        }
        default: {   // Latin-1 and friends
            U16 u;
            for (size_t i = 0; i < n && p[i]; ++i) u += char16_t(p[i]);
            return toUtf8(u);
        }
    }
}

std::string encodeBytes(const std::u16string& s, uint32_t enc, bool lossy, bool* ok) {
    if (ok) *ok = true;
    switch (enc) {
        case kNSUTF8: case kNSNonLossyASCII: return toUtf8(s);
        case kNSUnicode: { std::string out("\xFF\xFE", 2); for (char16_t c : s) { out += char(c & 0xFF); out += char(c >> 8); } return out; }
        case kNSUTF16LE: { std::string out; for (char16_t c : s) { out += char(c & 0xFF); out += char(c >> 8); } return out; }
        case kNSUTF16BE: { std::string out; for (char16_t c : s) { out += char(c >> 8); out += char(c & 0xFF); } return out; }
        case kNSASCII: {
            std::string out;
            for (char16_t c : s) { if (c >= 0x80) { if (!lossy) { if (ok) *ok = false; return {}; } out += '?'; } else out += char(c); }
            return out;
        }
        default: {
            std::string out;
            for (char16_t c : s) { if (c >= 0x100) { if (!lossy) { if (ok) *ok = false; return {}; } out += '?'; } else out += char(c); }
            return out;
        }
    }
}

uint32_t c_defaultCStringEncoding(Ctx&, id, SEL) { return 4; }   // NSUTF8StringEncoding

void registerStrings(Process& p, ObjC::Runtime& r) {
    auto& st = S(p);
    ObjC::ClassBuilder ns = r.defineClass("NSString", "NSObject");
    ObjC::ClassBuilder nms = r.defineClass("NSMutableString", "NSString");
    ObjC::ClassBuilder cf = r.defineClass("__NSCFString", "NSMutableString");
    ObjC::ClassBuilder cs = r.defineClass("__NSCFConstantString", "__NSCFString");
    st.nsString = ns.cls(); st.nsMutableString = nms.cls(); st.cfString = cf.cls(); st.constString = cs.cls();
    p.defineData("___CFConstantStringClassReference", cs.cls());
    p.defineData("_OBJC_CLASS_$___NSCFConstantString", cs.cls());
    r.makeImmortal(cs.cls());

    ns.cm<&ns_alloc>("alloc").cm<&ns_allocWithZone>("allocWithZone:")
      .cm<&c_string>("string").cm<&c_stringWithString>("stringWithString:").cm<&c_stringWithUTF8String>("stringWithUTF8String:")
      .cm<&c_stringWithCString>("stringWithCString:encoding:").cm<&c_stringWithCStringDefault>("stringWithCString:")
      .cm<&c_defaultCStringEncoding>("defaultCStringEncoding", "I@:")
      .cm<&c_stringWithFormat>("stringWithFormat:").cm<&c_stringWithFormat>("localizedStringWithFormat:")
      .cm<&c_stringWithCharacters>("stringWithCharacters:length:")
      .cm<&c_stringWithContentsOfFile>("stringWithContentsOfFile:encoding:error:")
      .cm<&c_stringWithContentsOfFileUsed>("stringWithContentsOfFile:usedEncoding:error:")
      .cm<&c_stringWithContentsOfFile1>("stringWithContentsOfFile:")
      .cm<&c_pathWithComponents>("pathWithComponents:");
    ns.m<&i_init>("init").m<&i_initWithString>("initWithString:").m<&i_initWithUTF8String>("initWithUTF8String:")
      .m<&i_initWithCString>("initWithCString:encoding:").m<&i_initWithCStringDefault>("initWithCString:")
      .m<&i_initWithFormat>("initWithFormat:").m<&i_initWithFormatArgs>("initWithFormat:arguments:")
      .m<&i_initWithCharacters>("initWithCharacters:length:").m<&i_initWithBytes>("initWithBytes:length:encoding:")
      .m<&i_initWithBytesNoCopy>("initWithBytesNoCopy:length:encoding:freeWhenDone:")
      .m<&i_initWithData>("initWithData:encoding:").m<&i_initWithContentsOfFile>("initWithContentsOfFile:encoding:error:")
      .m<&i_initWithContentsOfFileUsed>("initWithContentsOfFile:usedEncoding:error:").m<&i_initWithContentsOfFile1>("initWithContentsOfFile:")
      .m<&s_length>("length", "I@:").m<&s_characterAtIndex>("characterAtIndex:", "S@:I")
      .m<&s_getCharactersRange>("getCharacters:range:", "v@:^S{_NSRange=II}").m<&s_getCharacters>("getCharacters:")
      .m<&s_UTF8String>("UTF8String", "*@:").m<&s_UTF8String>("cString").m<&s_UTF8String>("lossyCString")
      .m<&s_cStringUsingEncoding>("cStringUsingEncoding:").m<&s_getCString>("getCString:maxLength:encoding:")
      .m<&s_canConvert>("canBeConvertedToEncoding:", "c@:I").m<&s_lengthOfBytes>("lengthOfBytesUsingEncoding:").m<&s_maxLengthOfBytes>("maximumLengthOfBytesUsingEncoding:")
      .m<&s_dataUsingEncoding>("dataUsingEncoding:").m<&s_dataUsingEncodingLossy>("dataUsingEncoding:allowLossyConversion:")
      .m<&s_isEqualToString>("isEqualToString:", "c@:@").m<&s_isEqual>("isEqual:", "c@:@").m<&s_hash>("hash", "I@:")
      .m<&s_compare>("compare:", "i@:@").m<&s_compareOptions>("compare:options:").m<&s_compareOptionsRange>("compare:options:range:")
      .m<&s_caseInsensitiveCompare>("caseInsensitiveCompare:").m<&s_localizedCompare>("localizedCompare:")
      .m<&s_caseInsensitiveCompare>("localizedCaseInsensitiveCompare:").m<&s_localizedCompare>("localizedStandardCompare:")
      .m<&s_hasPrefix>("hasPrefix:", "c@:@").m<&s_hasSuffix>("hasSuffix:", "c@:@")
      .m<&s_rangeOfString>("rangeOfString:", "{_NSRange=II}@:@").m<&s_rangeOfStringOptions>("rangeOfString:options:")
      .m<&s_rangeOfStringOptionsRange>("rangeOfString:options:range:").m<&s_containsString>("containsString:")
      .m<&s_rangeOfCharacterFromSet>("rangeOfCharacterFromSet:").m<&s_rangeOfCharacterFromSetOptions>("rangeOfCharacterFromSet:options:")
      .m<&s_substringFromIndex>("substringFromIndex:").m<&s_substringToIndex>("substringToIndex:").m<&s_substringWithRange>("substringWithRange:")
      .m<&s_appending>("stringByAppendingString:").m<&s_appendingFormat>("stringByAppendingFormat:")
      .m<&s_replacingOccurrences>("stringByReplacingOccurrencesOfString:withString:")
      .m<&s_replacingOccurrencesOptions>("stringByReplacingOccurrencesOfString:withString:options:range:")
      .m<&s_replacingCharactersInRange>("stringByReplacingCharactersInRange:withString:")
      .m<&s_trimming>("stringByTrimmingCharactersInSet:")
      .m<&s_componentsSeparatedByString>("componentsSeparatedByString:").m<&s_componentsSeparatedByCharacters>("componentsSeparatedByCharactersInSet:")
      .m<&s_lowercase>("lowercaseString").m<&s_uppercase>("uppercaseString").m<&s_capitalized>("capitalizedString")
      .m<&s_intValue>("intValue", "i@:").m<&s_intValue>("integerValue", "i@:").m<&s_longLongValue>("longLongValue", "q@:")
      .m<&s_floatValue>("floatValue", "f@:").m<&s_doubleValue>("doubleValue", "d@:").m<&s_boolValue>("boolValue", "c@:")
      .m<&s_self>("description").m<&s_self>("stringValue")
      .m<&s_copyWithZone>("copyWithZone:").m<&s_mutableCopyWithZone>("mutableCopyWithZone:")
      .m<&s_copy>("copy").m<&s_mutableCopy>("mutableCopy")
      .m<&s_appendingPathComponent>("stringByAppendingPathComponent:").m<&s_appendingPathExtension>("stringByAppendingPathExtension:")
      .m<&s_lastPathComponent>("lastPathComponent").m<&s_pathExtension>("pathExtension")
      .m<&s_deletingLastPathComponent>("stringByDeletingLastPathComponent").m<&s_deletingPathExtension>("stringByDeletingPathExtension")
      .m<&s_pathComponents>("pathComponents").m<&s_isAbsolutePath>("isAbsolutePath", "c@:")
      .m<&s_expandingTilde>("stringByExpandingTildeInPath").m<&s_standardizing>("stringByStandardizingPath")
      .m<&s_standardizing>("stringByResolvingSymlinksInPath")
      .m<&s_fileSystemRepresentation>("fileSystemRepresentation", "r*@:").m<&s_getFileSystemRepresentation>("getFileSystemRepresentation:maxLength:")
      .m<&s_writeToFileEnc>("writeToFile:atomically:encoding:error:").m<&s_writeToFile>("writeToFile:atomically:")
      .m<&s_addingPercentEscapes>("stringByAddingPercentEscapesUsingEncoding:")
      .m<&s_replacingPercentEscapes>("stringByReplacingPercentEscapesUsingEncoding:");
    nms.cm<&c_stringWithCapacity>("stringWithCapacity:")
       .m<&i_initWithCapacity>("initWithCapacity:")
       .m<&m_appendString>("appendString:", "v@:@").m<&m_appendFormat>("appendFormat:", "v@:@")
       .m<&m_setString>("setString:", "v@:@").m<&m_insertString>("insertString:atIndex:")
       .m<&m_deleteCharacters>("deleteCharactersInRange:").m<&m_replaceCharacters>("replaceCharactersInRange:withString:")
       .m<&m_replaceOccurrences>("replaceOccurrencesOfString:withString:options:range:");
    cf.m<&str_dealloc>("dealloc", "v@:");
    cs.m<&const_self>("retain").m<&const_noop>("release", "v@:").m<&const_self>("autorelease")
      .m<&const_retainCount>("retainCount", "I@:").m<&const_noop>("dealloc", "v@:");
    // %@ in format strings
    setObjectFormatHook(p, [&p](uint32_t obj) {
        Ctx x{p, p.currentThread().cpu, p.mem()};
        return describe(x, obj);
    });
}

} // namespace Hydrogen::Runtime::Foundation
