// HydrogenVM — serialization: NSJSONSerialization, NSPropertyListSerialization,
// NSCoder / NSKeyedArchiver / NSKeyedUnarchiver (the real $archiver plist
// format, so archives move between HydrogenVM and iOS).
#include "Foundation.hpp"
#include <cmath>
#include <fstream>

namespace Hydrogen::Runtime::Foundation {

namespace {

using PV = Plist::ValuePtr;
using T = Plist::Value::Type;

void setError(Ctx& x, GuestPtr out, int32_t code, const std::string& msg) {
    if (!out) return;
    const id err = id(rt(x).send(x.cpu, rt(x).getClass("NSError"), "errorWithDomain:code:userInfo:",
                                 {makeString(x, "NSCocoaErrorDomain"), uint32_t(code),
                                  makeDict(x, {{makeString(x, "NSDebugDescription"), makeString(x, msg)}})}));
    x.mem.write<uint32_t>(out, err);
}

// ================================================================ JSON ===

struct JsonParser {
    Ctx& x;
    const std::string& s;
    size_t p = 0;
    bool mutableContainers, mutableLeaves;
    std::string error;

    void ws() { while (p < s.size() && (s[p] == ' ' || s[p] == '\t' || s[p] == '\n' || s[p] == '\r')) ++p; }
    bool fail(const std::string& e) { if (error.empty()) error = e + " around character " + std::to_string(p); return false; }
    bool lit(const char* w) { const size_t n = std::strlen(w); if (s.compare(p, n, w) == 0) { p += n; return true; } return false; }

    bool hex4(uint32_t& v) {
        if (p + 4 > s.size()) return false;
        v = 0;
        for (int i = 0; i < 4; ++i) {
            const char c = s[p++];
            v <<= 4;
            if (c >= '0' && c <= '9') v |= uint32_t(c - '0');
            else if (c >= 'a' && c <= 'f') v |= uint32_t(c - 'a' + 10);
            else if (c >= 'A' && c <= 'F') v |= uint32_t(c - 'A' + 10);
            else return false;
        }
        return true;
    }
    bool str(std::u16string& out) {
        if (s[p] != '"') return fail("expected string");
        ++p;
        std::string run;
        auto flush = [&] { if (!run.empty()) { out += Plist::utf8ToUtf16(run); run.clear(); } };
        while (p < s.size()) {
            const char c = s[p];
            if (c == '"') { ++p; flush(); return true; }
            if (uint8_t(c) < 0x20) return fail("unescaped control character in string");
            if (c != '\\') { run += c; ++p; continue; }
            ++p;
            if (p >= s.size()) break;
            const char e = s[p++];
            switch (e) {
                case '"': run += '"'; break;
                case '\\': run += '\\'; break;
                case '/': run += '/'; break;
                case 'b': run += '\b'; break;
                case 'f': run += '\f'; break;
                case 'n': run += '\n'; break;
                case 'r': run += '\r'; break;
                case 't': run += '\t'; break;
                case 'u': {
                    uint32_t v;
                    if (!hex4(v)) return fail("bad \\u escape");
                    flush();
                    out += char16_t(v);   // surrogate pairs pass through as two UTF-16 units
                    break;
                }
                default: return fail("bad escape");
            }
        }
        return fail("unterminated string");
    }
    id value(int depth) {
        if (depth > 512) { fail("nesting too deep"); return 0; }
        ws();
        if (p >= s.size()) { fail("unexpected end of data"); return 0; }
        const char c = s[p];
        if (c == '{') {
            ++p;
            std::vector<std::pair<id, id>> e;
            ws();
            if (p < s.size() && s[p] == '}') { ++p; return makeDict(x, e, false, mutableContainers); }
            for (;;) {
                ws();
                std::u16string k;
                if (p >= s.size() || !str(k)) { fail("expected key string"); return 0; }
                ws();
                if (p >= s.size() || s[p] != ':') { fail("expected ':'"); return 0; }
                ++p;
                const id v = value(depth + 1);
                if (!v) return 0;
                e.emplace_back(makeString(x, k), v);
                ws();
                if (p < s.size() && s[p] == ',') { ++p; continue; }
                if (p < s.size() && s[p] == '}') { ++p; break; }
                fail("expected ',' or '}'");
                return 0;
            }
            return makeDict(x, e, false, mutableContainers);
        }
        if (c == '[') {
            ++p;
            std::vector<id> items;
            ws();
            if (p < s.size() && s[p] == ']') { ++p; return makeArray(x, items, false, mutableContainers); }
            for (;;) {
                const id v = value(depth + 1);
                if (!v) return 0;
                items.push_back(v);
                ws();
                if (p < s.size() && s[p] == ',') { ++p; continue; }
                if (p < s.size() && s[p] == ']') { ++p; break; }
                fail("expected ',' or ']'");
                return 0;
            }
            return makeArray(x, items, false, mutableContainers);
        }
        if (c == '"') {
            std::u16string v;
            if (!str(v)) return 0;
            return makeString(x, v, false, mutableLeaves);
        }
        if (lit("true")) return makeBool(x, true);
        if (lit("false")) return makeBool(x, false);
        if (lit("null")) return nullObject(x.proc);
        if (c == '-' || (c >= '0' && c <= '9')) {
            const size_t start = p;
            if (s[p] == '-') ++p;
            bool isFloat = false;
            while (p < s.size() && (std::isdigit(uint8_t(s[p])) || s[p] == '.' || s[p] == 'e' || s[p] == 'E' || s[p] == '+' || s[p] == '-')) {
                if (s[p] == '.' || s[p] == 'e' || s[p] == 'E') isFloat = true;
                ++p;
            }
            const std::string num = s.substr(start, p - start);
            if (!isFloat) {
                errno = 0;
                const long long v = std::strtoll(num.c_str(), nullptr, 10);
                if (errno != ERANGE) return makeNumberInt(x, v);
            }
            return makeNumberDouble(x, std::strtod(num.c_str(), nullptr));
        }
        fail("invalid value");
        return 0;
    }
};

void jsonEscape(std::string& out, const std::u16string& s) {
    out += '"';
    std::string run;
    for (char16_t c : s) {
        switch (c) {
            case '"': out += "\\\""; continue;
            case '\\': out += "\\\\"; continue;
            case '/': out += "\\/"; continue;      // Foundation escapes '/'
            case '\n': out += "\\n"; continue;
            case '\r': out += "\\r"; continue;
            case '\t': out += "\\t"; continue;
            case '\b': out += "\\b"; continue;
            case '\f': out += "\\f"; continue;
            default: break;
        }
        if (c < 0x20) { char b[8]; std::snprintf(b, sizeof b, "\\u%04x", c); out += b; continue; }
        out += Plist::utf16ToUtf8(&c, 1);
    }
    out += '"';
}
std::string formatDouble(double d) {
    if (std::isfinite(d) && d == std::floor(d) && std::fabs(d) < 1e15) { char b[32]; std::snprintf(b, sizeof b, "%.0f", d); return b; }
    char b[40];
    for (int prec = 15; prec <= 17; ++prec) {
        std::snprintf(b, sizeof b, "%.*g", prec, d);
        if (std::strtod(b, nullptr) == d) break;
    }
    return b;
}
bool jsonWrite(Ctx& x, id o, std::string& out, bool pretty, int indent, std::string& err) {
    auto nl = [&](int lvl) { if (pretty) { out += '\n'; out.append(size_t(lvl) * 2, ' '); } };
    if (!o) { err = "nil value"; return false; }
    if (o == nullObject(x.proc)) { out += "null"; return true; }
    if (isString(x, o)) { jsonEscape(out, stringValue(x, o)); return true; }
    if (isBoolean(x, o)) { out += numberInt(x, o) ? "true" : "false"; return true; }
    if (isNumber(x, o)) {
        const char t = numberType(x, o);
        if (t == 'f' || t == 'd') {
            const double d = numberDouble(x, o);
            if (!std::isfinite(d)) { err = "Invalid number value (NaN or infinity) in JSON write"; return false; }
            out += formatDouble(d);
        } else out += std::to_string(numberInt(x, o));
        return true;
    }
    if (isArray(x, o)) {
        const auto items = arrayItems(x, o);
        out += '[';
        for (size_t i = 0; i < items.size(); ++i) {
            if (i) out += ',';
            nl(indent + 1);
            if (!jsonWrite(x, items[i], out, pretty, indent + 1, err)) return false;
        }
        if (!items.empty()) nl(indent);
        out += ']';
        return true;
    }
    if (isDict(x, o)) {
        const auto entries = dictEntries(x, o);
        out += '{';
        bool first = true;
        for (auto& [k, v] : entries) {
            if (!isString(x, k)) { err = "Invalid (non-string) key in JSON dictionary"; return false; }
            if (!first) out += ',';
            first = false;
            nl(indent + 1);
            jsonEscape(out, stringValue(x, k));
            out += pretty ? " : " : ":";
            if (!jsonWrite(x, v, out, pretty, indent + 1, err)) return false;
        }
        if (!entries.empty()) nl(indent);
        out += '}';
        return true;
    }
    err = "Invalid type in JSON write (" + rt(x).objectClassName(o) + ")";
    return false;
}

std::string decodeJsonText(const std::vector<uint8_t>& b) {
    // UTF-8 (with optional BOM) or UTF-16/32 detected from the leading nulls (RFC 4627).
    if (b.size() >= 3 && b[0] == 0xEF && b[1] == 0xBB && b[2] == 0xBF) return std::string(b.begin() + 3, b.end());
    if (b.size() >= 2 && ((b[0] == 0 && b[1] != 0) || (b[0] == 0xFE && b[1] == 0xFF)))
        return decodeBytes(b.data(), b.size(), kNSUTF16BE);
    if (b.size() >= 2 && ((b[0] != 0 && b[1] == 0) || (b[0] == 0xFF && b[1] == 0xFE)))
        return decodeBytes(b.data(), b.size(), kNSUTF16LE);
    return std::string(b.begin(), b.end());
}

id js_parse(Ctx& x, id, SEL, id data, uint32_t opts, GuestPtr err) {
    if (!data) { throw ObjC::ObjCError("*** +[NSJSONSerialization JSONObjectWithData:options:error:]: data parameter is nil"); }
    const std::string text = decodeJsonText(dataBytes(x, data));
    JsonParser jp{x, text, 0, (opts & 1) != 0, (opts & 2) != 0, {}};
    const id v = jp.value(0);
    if (v) {
        jp.ws();
        if (jp.p != text.size()) { jp.fail("garbage at end"); }
        else if (!(opts & 4) && !isArray(x, v) && !isDict(x, v)) jp.fail("JSON text did not start with array or object and option to allow fragments not set.");
        else return v;
    }
    setError(x, err, 3840, jp.error.empty() ? "invalid JSON" : jp.error);
    return 0;
}
id js_write(Ctx& x, id, SEL, id obj, uint32_t opts, GuestPtr err) {
    std::string out, e;
    if (!isArray(x, obj) && !isDict(x, obj)) e = "Invalid top-level type in JSON write";
    else if (jsonWrite(x, obj, out, opts & 1, 0, e)) return makeData(x, out.data(), uint32_t(out.size()));
    // Foundation throws for invalid objects rather than returning an error.
    (void)err;
    throw ObjC::ObjCError("*** +[NSJSONSerialization dataWithJSONObject:options:error:]: " + e);
}
bool js_isValid(Ctx& x, id, SEL, id obj) {
    std::string out, e;
    return (isArray(x, obj) || isDict(x, obj)) && jsonWrite(x, obj, out, false, 0, e);
}
id js_readStream(Ctx& x, id, SEL, id, uint32_t, GuestPtr err) {
    x.proc.reportMissing("NSJSONSerialization JSONObjectWithStream:");
    setError(x, err, 3840, "streams not supported");
    return 0;
}

// =================================================== property lists ===

id pls_dataWith(Ctx& x, id, SEL, id plist, uint32_t format, uint32_t, GuestPtr err) {
    PV v = objectToPlist(x, plist);
    if (!v) { setError(x, err, 3851, "Property list invalid for format"); return 0; }
    if (format == 200) x.proc.reportMissing("binary property list output (wrote XML)");
    const std::string s = Plist::toXml(*v);
    return makeData(x, s.data(), uint32_t(s.size()));
}
id pls_dataFrom(Ctx& x, id self, SEL s, id plist, uint32_t format, GuestPtr errStr) {
    const id d = pls_dataWith(x, self, s, plist, format, 0, 0);
    if (!d && errStr) x.mem.write<uint32_t>(errStr, makeString(x, "Property list invalid for format"));
    return d;
}
id pls_parse(Ctx& x, id, SEL, id data, uint32_t opts, GuestPtr fmt, GuestPtr err) {
    const auto b = dataBytes(x, data);
    try {
        PV v = Plist::parse(b);
        if (!v) throw Plist::PlistError("empty");
        if (fmt) x.mem.write<uint32_t>(fmt, b.size() >= 6 && std::memcmp(b.data(), "bplist", 6) == 0 ? 200 : 100);
        return plistToObject(x, *v, opts != 0);
    } catch (const std::exception& e) {
        setError(x, err, 3840, e.what());
        return 0;
    }
}
id pls_parseOld(Ctx& x, id self, SEL s, id data, uint32_t opts, GuestPtr fmt, GuestPtr errStr) {
    const id r = pls_parse(x, self, s, data, opts, fmt, 0);
    if (!r && errStr) x.mem.write<uint32_t>(errStr, makeString(x, "Unable to parse property list"));
    return r;
}
bool pls_isValid(Ctx& x, id, SEL, id plist, uint32_t) { return objectToPlist(x, plist) != nullptr; }

// ================================================= keyed archiving ===

PV uidValue(uint64_t n) { auto u = std::make_shared<Plist::Value>(); u->type = T::Uid; u->i = int64_t(n); return u; }

struct ArchiverData : ObjC::HostObject {
    std::vector<PV> objects;                       // $objects
    std::unordered_map<id, uint64_t> uids;         // object -> uid
    std::unordered_map<std::string, uint64_t> classUids;
    std::vector<PV> stack;                         // dicts being filled
    id data = 0;                                   // NSMutableData given to initForWritingWithMutableData:
    bool finished = false;
    PV root;                                       // $top
};

struct UnarchiverData : ObjC::HostObject {
    PV archive;
    std::vector<PV> objects;
    std::unordered_map<uint64_t, id> decoded;      // uid -> object (+1, released at dealloc)
    std::vector<const Plist::Value*> stack;        // current object dicts
    const Plist::Value* top = nullptr;
    std::unordered_map<std::string, std::string> classSubs;   // setClass:forClassName:
};

struct Archiving {
    explicit Archiving(Process&) {}
    Class coder = 0, archiver = 0, unarchiver = 0;
};
Archiving& A(Ctx& x) { return x.proc.service<Archiving>(); }

ArchiverData* ad(Ctx& x, id a) { return rt(x).hostIf<ArchiverData>(a); }
UnarchiverData* ud(Ctx& x, id u) { return rt(x).hostIf<UnarchiverData>(u); }

uint64_t classUid(Ctx& x, ArchiverData& a, Class cls, const std::string& overrideName = {}) {
    const std::string name = overrideName.empty() ? rt(x).className(cls) : overrideName;
    auto it = a.classUids.find(name);
    if (it != a.classUids.end()) return it->second;
    auto d = Plist::Value::makeDict();
    d->set("$classname", Plist::Value::makeString(name));
    auto chain = Plist::Value::makeArray();
    chain->array.push_back(Plist::Value::makeString(name));
    for (Class c = rt(x).superclass(cls); c; c = rt(x).superclass(c)) chain->array.push_back(Plist::Value::makeString(rt(x).className(c)));
    d->set("$classes", chain);
    a.objects.push_back(d);
    const uint64_t uid = a.objects.size() - 1;
    a.classUids[name] = uid;
    return uid;
}

uint64_t encodeObject(Ctx& x, id archiver, id o);

std::string publicClassName(Ctx& x, id o, const char* immutable, const char* mut) {
    // __NSArrayM -> NSMutableArray etc., like the real class clusters' classForCoder
    const std::string n = rt(x).objectClassName(o);
    const bool isMutable = n.find("M") != std::string::npos && n.rfind("__NS", 0) == 0 ? n.back() == 'M' : n.find("Mutable") != std::string::npos;
    return isMutable ? mut : immutable;
}

uint64_t encodeObject(Ctx& x, id archiver, id o) {
    ArchiverData& a = *ad(x, archiver);
    if (!o) return 0;
    auto it = a.uids.find(o);
    if (it != a.uids.end()) return it->second;

    // Plain property-list leaves are stored inline in $objects.
    if (isString(x, o)) {
        a.objects.push_back(Plist::Value::makeString(utf8(x, o)));
        return a.uids[o] = a.objects.size() - 1;
    }
    if (isNumber(x, o)) {
        PV v = objectToPlist(x, o);
        a.objects.push_back(v ? v : Plist::Value::makeInt(0));
        return a.uids[o] = a.objects.size() - 1;
    }
    if (isData(x, o)) {
        const auto b = dataBytes(x, o);
        a.objects.push_back(Plist::Value::makeData(std::string(b.begin(), b.end())));
        return a.uids[o] = a.objects.size() - 1;
    }

    auto dict = Plist::Value::makeDict();
    a.objects.push_back(dict);
    const uint64_t uid = a.objects.size() - 1;
    a.uids[o] = uid;
    rt(x).retain(x.cpu, o);   // keep it alive (and its address unique) while archiving

    auto& r = rt(x);
    if (isArray(x, o) || (r.getClass("NSSet") && r.isKindOf(o, r.getClass("NSSet")))) {
        const bool set = !isArray(x, o);
        auto objs = Plist::Value::makeArray();
        for (id e : set ? setItems(x, o) : arrayItems(x, o)) objs->array.push_back(uidValue(encodeObject(x, archiver, e)));
        dict->set("NS.objects", objs);
        dict->set("$class", uidValue(classUid(x, a, r.classOf(o), set ? publicClassName(x, o, "NSSet", "NSMutableSet")
                                                                      : publicClassName(x, o, "NSArray", "NSMutableArray"))));
        return uid;
    }
    if (isDict(x, o)) {
        auto keys = Plist::Value::makeArray(), vals = Plist::Value::makeArray();
        for (auto& [k, v] : dictEntries(x, o)) {
            keys->array.push_back(uidValue(encodeObject(x, archiver, k)));
            vals->array.push_back(uidValue(encodeObject(x, archiver, v)));
        }
        dict->set("NS.keys", keys);
        dict->set("NS.objects", vals);
        dict->set("$class", uidValue(classUid(x, a, r.classOf(o), publicClassName(x, o, "NSDictionary", "NSMutableDictionary"))));
        return uid;
    }
    if (r.getClass("NSDate") && r.isKindOf(o, r.getClass("NSDate"))) {
        dict->set("NS.time", Plist::Value::makeReal(dateInterval(x, o)));
        dict->set("$class", uidValue(classUid(x, a, r.getClass("NSDate"))));
        return uid;
    }
    if (isURL(x, o)) {
        dict->set("NS.base", uidValue(0));
        dict->set("NS.relative", uidValue(encodeObject(x, archiver, makeString(x, urlString(x, o)))));
        dict->set("$class", uidValue(classUid(x, a, r.getClass("NSURL"))));
        return uid;
    }
    if (o == nullObject(x.proc)) {
        dict->set("$class", uidValue(classUid(x, a, r.getClass("NSNull"))));
        return uid;
    }
    // Arbitrary object: let it encode itself.
    Class cls = r.classOf(o);
    if (r.respondsTo(o, r.sel("classForKeyedArchiver"))) if (const Class c = Class(r.send(x.cpu, o, "classForKeyedArchiver"))) cls = c;
    if (!r.respondsTo(o, r.sel("encodeWithCoder:")))
        throw ObjC::ObjCError("*** -[NSKeyedArchiver encodeObject:forKey:]: " + r.className(cls) + " does not implement encodeWithCoder:");
    a.stack.push_back(dict);
    r.send(x.cpu, o, "encodeWithCoder:", {archiver});
    a.stack.pop_back();
    dict->set("$class", uidValue(classUid(x, a, cls)));
    return uid;
}

Plist::Value& currentDict(Ctx& x, id archiver) {
    ArchiverData* a = ad(x, archiver);
    if (!a || a->stack.empty()) throw ObjC::ObjCError("NSKeyedArchiver: encode called outside encodeWithCoder:");
    return *a->stack.back();
}
std::string K(Ctx& x, id key) { return key ? utf8(x, key) : std::string("$null"); }

PV buildArchive(Ctx& x, id archiver) {
    ArchiverData& a = *ad(x, archiver);
    auto root = Plist::Value::makeDict();
    root->set("$version", Plist::Value::makeInt(100000));
    auto objs = Plist::Value::makeArray();
    objs->array = a.objects;
    root->set("$objects", objs);
    root->set("$archiver", Plist::Value::makeString("NSKeyedArchiver"));
    root->set("$top", a.root ? a.root : Plist::Value::makeDict());
    return root;
}

id newArchiver(Ctx& x) {
    const id a = rt(x).createInstance(A(x).archiver);
    auto h = std::make_unique<ArchiverData>();
    h->objects.push_back(Plist::Value::makeString("$null"));
    h->root = Plist::Value::makeDict();
    h->stack.push_back(h->root);   // top-level encodes go into $top
    rt(x).setHost(a, std::move(h));
    return a;
}
void releaseArchived(Ctx& x, ArchiverData& a) {
    for (auto& [o, uid] : a.uids) {
        const PV& v = a.objects[size_t(uid)];
        if (v->type == T::Dict) rt(x).release(x.cpu, o);
    }
    a.uids.clear();
}
id archiveData(Ctx& x, id archiver) {
    const std::string s = Plist::toXml(*buildArchive(x, archiver));
    return makeData(x, s.data(), uint32_t(s.size()));
}

id ka_archivedData(Ctx& x, id, SEL, id root) {
    const id a = newArchiver(x);
    ad(x, a)->root->set("root", uidValue(encodeObject(x, a, root)));
    const id d = archiveData(x, a);
    releaseArchived(x, *ad(x, a));
    rt(x).release(x.cpu, a);
    return d;
}
bool ka_archiveToFile(Ctx& x, id self, SEL s, id root, id path) {
    const id d = ka_archivedData(x, self, s, root);
    const auto b = dataBytes(x, d);
    std::ofstream out(x.proc.hostPath(utf8(x, path)), std::ios::binary | std::ios::trunc);
    out.write(reinterpret_cast<const char*>(b.data()), std::streamsize(b.size()));
    return bool(out);
}
id ka_initForWriting(Ctx& x, id self, SEL, id data) {
    rt(x).disposeInstance(x.cpu, self);
    const id a = newArchiver(x);
    ad(x, a)->data = rt(x).retain(x.cpu, data);
    return a;
}
id ka_initRequiringSecure(Ctx& x, id self, SEL, bool) {
    rt(x).disposeInstance(x.cpu, self);
    return newArchiver(x);
}
id ka_init(Ctx& x, id self, SEL) { rt(x).disposeInstance(x.cpu, self); return newArchiver(x); }
void ka_finish(Ctx& x, id self, SEL) {
    ArchiverData* a = ad(x, self);
    if (!a || a->finished) return;
    a->finished = true;
    if (a->data) {
        const id d = archiveData(x, self);
        rt(x).send(x.cpu, a->data, "setData:", {d});
    }
}
id ka_encodedData(Ctx& x, id self, SEL s) { ka_finish(x, self, s); return archiveData(x, self); }
void ka_encodeObject(Ctx& x, id self, SEL, id o, id key) {
    const uint64_t u = encodeObject(x, self, o);
    currentDict(x, self).set(K(x, key), uidValue(u));
}
void ka_encodeInt(Ctx& x, id self, SEL, int32_t v, id key) { currentDict(x, self).set(K(x, key), Plist::Value::makeInt(v)); }
void ka_encodeInt64(Ctx& x, id self, SEL, int64_t v, id key) { currentDict(x, self).set(K(x, key), Plist::Value::makeInt(v)); }
void ka_encodeBool(Ctx& x, id self, SEL, bool v, id key) { currentDict(x, self).set(K(x, key), Plist::Value::makeBool(v)); }
void ka_encodeFloat(Ctx& x, id self, SEL, float v, id key) { currentDict(x, self).set(K(x, key), Plist::Value::makeReal(v)); }
void ka_encodeDouble(Ctx& x, id self, SEL, double v, id key) { currentDict(x, self).set(K(x, key), Plist::Value::makeReal(v)); }
void ka_encodeBytes(Ctx& x, id self, SEL, GuestPtr p, uint32_t n, id key) {
    currentDict(x, self).set(K(x, key), Plist::Value::makeData(std::string(reinterpret_cast<const char*>(x.mem.host(p)), n)));
}
void ka_encodeRoot(Ctx& x, id self, SEL, id o) { ka_encodeObject(x, self, 0, o, makeString(x, "root")); }
bool ka_allowsKeyed(Ctx&, id, SEL) { return true; }
void ka_setFormat(Ctx&, id, SEL, uint32_t) {}
void ka_setDelegate(Ctx&, id, SEL, id) {}
void ka_dealloc(Ctx& x, id self, SEL) {
    if (ArchiverData* a = ad(x, self)) {
        releaseArchived(x, *a);
        const id d = a->data;
        a->data = 0;
        rt(x).release(x.cpu, d);
    }
    rt(x).disposeInstance(x.cpu, self);
}

// ---------------------------------------------------------- unarchiving ---

const Plist::Value* objAt(UnarchiverData& u, uint64_t uid) { return uid < u.objects.size() ? u.objects[size_t(uid)].get() : nullptr; }

id decodeUid(Ctx& x, id unarch, uint64_t uid);

id decodeValue(Ctx& x, id unarch, const Plist::Value* v) {
    if (!v) return 0;
    if (v->type == T::Uid) return decodeUid(x, unarch, uint64_t(v->i));
    return plistToObject(x, *v);
}

id decodeUid(Ctx& x, id unarch, uint64_t uid) {
    UnarchiverData& u = *ud(x, unarch);
    if (uid == 0) return 0;
    auto it = u.decoded.find(uid);
    if (it != u.decoded.end()) return it->second;
    const Plist::Value* v = objAt(u, uid);
    if (!v) return 0;
    if (v->type != T::Dict || !v->get("$class")) {
        if (v->type == T::String && v->s == "$null") return 0;
        const id o = rt(x).retain(x.cpu, plistToObject(x, *v));
        u.decoded[uid] = o;
        return o;
    }
    const Plist::Value* cref = v->get("$class");
    const Plist::Value* cdict = cref && cref->type == T::Uid ? objAt(u, uint64_t(cref->i)) : nullptr;
    std::string cname = cdict ? cdict->getString("$classname") : "";
    if (auto sub = u.classSubs.find(cname); sub != u.classSubs.end()) cname = sub->second;
    auto& r = rt(x);
    auto items = [&](const char* key) {
        std::vector<id> out;
        if (const Plist::Value* a = v->get(key); a && a->type == T::Array)
            for (auto& e : a->array) { const id o = decodeValue(x, unarch, e.get()); out.push_back(o ? o : nullObject(x.proc)); }
        return out;
    };
    id result = 0;
    const bool isMutable = cname.find("Mutable") != std::string::npos;
    if (cname == "NSArray" || cname == "NSMutableArray") result = makeArray(x, items("NS.objects"), true, isMutable);
    else if (cname == "NSSet" || cname == "NSMutableSet") result = makeSet(x, items("NS.objects"), true, isMutable);
    else if (cname == "NSDictionary" || cname == "NSMutableDictionary") {
        const auto ks = items("NS.keys"), vs = items("NS.objects");
        std::vector<std::pair<id, id>> e;
        for (size_t i = 0; i < ks.size() && i < vs.size(); ++i) e.emplace_back(ks[i], vs[i]);
        result = makeDict(x, e, true, isMutable);
    } else if (cname == "NSString" || cname == "NSMutableString") {
        const Plist::Value* s = v->get("NS.string");
        result = makeString(x, s ? s->s : std::string(), true, isMutable);
    } else if (cname == "NSData" || cname == "NSMutableData") {
        const Plist::Value* d = v->get("NS.data");
        const id inner = d ? decodeValue(x, unarch, d) : 0;
        const auto b = inner ? dataBytes(x, inner) : std::vector<uint8_t>();
        result = makeData(x, b.data(), uint32_t(b.size()), true, isMutable);
    } else if (cname == "NSDate") {
        const Plist::Value* t = v->get("NS.time");
        result = makeDate(x, t ? (t->type == T::Real ? t->r : double(t->i)) : 0, true);
    } else if (cname == "NSURL") {
        const Plist::Value* rel = v->get("NS.relative");
        const id s = rel ? decodeValue(x, unarch, rel) : 0;
        const std::string str = s ? utf8(x, s) : "";
        result = r.retain(x.cpu, id(r.send(x.cpu, r.getClass("NSURL"), "URLWithString:", {makeString(x, str)})));
    } else if (cname == "NSNull") {
        result = nullObject(x.proc);
    } else {
        Class cls = r.getClass(cname);
        if (!cls && cdict) {   // fall back along $classes
            if (const Plist::Value* chain = cdict->get("$classes"))
                for (auto& c : chain->array) if (c->type == T::String && (cls = r.getClass(c->s))) break;
        }
        if (!cls) throw ObjC::ObjCError("*** -[NSKeyedUnarchiver decodeObjectForKey:]: cannot decode object of class (" + cname + ")");
        const id obj = id(r.send(x.cpu, cls, r.selAlloc));
        u.decoded[uid] = obj;   // allows cycles to resolve to the allocated object
        u.stack.push_back(v);
        id inited = 0;
        try { inited = id(r.send(x.cpu, obj, "initWithCoder:", {unarch})); } catch (...) { u.stack.pop_back(); throw; }
        u.stack.pop_back();
        if (inited && r.respondsTo(inited, r.sel("awakeAfterUsingCoder:"))) {
            const id replaced = id(r.send(x.cpu, inited, "awakeAfterUsingCoder:", {unarch}));
            if (replaced && replaced != inited) { r.retain(x.cpu, replaced); r.release(x.cpu, inited); inited = replaced; }
        }
        result = inited;
    }
    u.decoded[uid] = result;
    return result;
}

const Plist::Value* field(Ctx& x, id unarch, id key) {
    UnarchiverData* u = ud(x, unarch);
    if (!u) return nullptr;
    const Plist::Value* d = u->stack.empty() ? u->top : u->stack.back();
    return d ? d->get(K(x, key)) : nullptr;
}

id newUnarchiver(Ctx& x, const std::vector<uint8_t>& bytes) {
    PV root;
    try { root = Plist::parse(bytes); } catch (const std::exception&) { return 0; }
    if (!root || root->type != T::Dict) return 0;
    const Plist::Value* objs = root->get("$objects");
    if (!objs || objs->type != T::Array) return 0;
    const id un = rt(x).createInstance(A(x).unarchiver);
    auto h = std::make_unique<UnarchiverData>();
    h->archive = root;
    h->objects = objs->array;
    h->top = root->get("$top");
    rt(x).setHost(un, std::move(h));
    return un;
}

id ku_unarchiveData(Ctx& x, id, SEL, id data) {
    if (!data) return 0;
    const id un = newUnarchiver(x, dataBytes(x, data));
    if (!un) throw ObjC::ObjCError("*** -[NSKeyedUnarchiver initForReadingWithData:]: incomprehensible archive");
    id root = 0;
    if (const Plist::Value* r = field(x, un, makeString(x, "root"))) root = decodeValue(x, un, r);
    rt(x).retain(x.cpu, root);
    rt(x).release(x.cpu, un);
    return autoreleased(x, root);
}
id ku_unarchiveFile(Ctx& x, id self, SEL s, id path) {
    std::ifstream in(x.proc.hostPath(utf8(x, path)), std::ios::binary);
    if (!in) return 0;
    std::vector<uint8_t> b((std::istreambuf_iterator<char>(in)), {});
    return ku_unarchiveData(x, self, s, makeData(x, b.data(), uint32_t(b.size())));
}
id ku_initForReading(Ctx& x, id self, SEL, id data) {
    rt(x).disposeInstance(x.cpu, self);
    const id un = data ? newUnarchiver(x, dataBytes(x, data)) : 0;
    if (!un) throw ObjC::ObjCError("*** -[NSKeyedUnarchiver initForReadingWithData:]: incomprehensible archive");
    return un;
}
id ku_decodeObject(Ctx& x, id self, SEL, id key) { return decodeValue(x, self, field(x, self, key)); }
id ku_decodeObjectOfClass(Ctx& x, id self, SEL s, Class, id key) { return ku_decodeObject(x, self, s, key); }
bool ku_contains(Ctx& x, id self, SEL, id key) { return field(x, self, key) != nullptr; }
int64_t intOf(const Plist::Value* v) {
    if (!v) return 0;
    switch (v->type) { case T::Integer: return v->i; case T::Real: return int64_t(v->r); case T::Bool: return v->b; default: return 0; }
}
double doubleOf(const Plist::Value* v) {
    if (!v) return 0;
    switch (v->type) { case T::Integer: return double(v->i); case T::Real: return v->r; case T::Bool: return v->b; default: return 0; }
}
int32_t ku_decodeInt(Ctx& x, id self, SEL, id key) { return int32_t(intOf(field(x, self, key))); }
int64_t ku_decodeInt64(Ctx& x, id self, SEL, id key) { return intOf(field(x, self, key)); }
bool ku_decodeBool(Ctx& x, id self, SEL, id key) { const Plist::Value* v = field(x, self, key); return v && (v->type == T::Bool ? v->b : intOf(v) != 0); }
float ku_decodeFloat(Ctx& x, id self, SEL, id key) { return float(doubleOf(field(x, self, key))); }
double ku_decodeDouble(Ctx& x, id self, SEL, id key) { return doubleOf(field(x, self, key)); }
uint32_t ku_decodeBytes(Ctx& x, id self, SEL, id key, GuestPtr lenOut) {
    const Plist::Value* v = field(x, self, key);
    if (!v || v->type != T::Data) { if (lenOut) x.mem.write<uint32_t>(lenOut, 0); return 0; }
    // Valid until the unarchiver is gone: an autoreleased NSData owns the bytes.
    const id d = makeData(x, v->s.data(), uint32_t(v->s.size()));
    if (lenOut) x.mem.write<uint32_t>(lenOut, uint32_t(v->s.size()));
    return uint32_t(rt(x).send(x.cpu, d, "bytes"));
}
void ku_finish(Ctx&, id, SEL) {}
void ku_setClass(Ctx& x, id self, SEL, Class cls, id name) {
    if (UnarchiverData* u = ud(x, self)) u->classSubs[utf8(x, name)] = rt(x).className(cls);
}
void kuc_setClass(Ctx&, id, SEL, Class, id) {}
void ku_dealloc(Ctx& x, id self, SEL) {
    if (UnarchiverData* u = ud(x, self)) {
        auto decoded = std::move(u->decoded);
        u->decoded.clear();
        for (auto& [uid, o] : decoded) rt(x).release(x.cpu, o);
    }
    rt(x).disposeInstance(x.cpu, self);
}

// NSCoder base: abstract, but the common entry points exist so subclasses work.
bool coder_allowsKeyed(Ctx&, id, SEL) { return false; }
uint32_t coder_systemVersion(Ctx&, id, SEL) { return 1000; }
int32_t coder_versionForClass(Ctx&, id, SEL, id) { return 0; }
id coder_abstract(Ctx& x, id self, SEL s) {
    throw ObjC::ObjCError("*** -[" + rt(x).objectClassName(self) + " " + rt(x).selName(s) + "]: NSCoder is abstract");
}

} // namespace

void registerArchiving(Process& p, ObjC::Runtime& r) {
    auto& a = p.service<Archiving>();
    ObjC::ClassBuilder js = r.defineClass("NSJSONSerialization", "NSObject");
    js.cm<&js_parse>("JSONObjectWithData:options:error:").cm<&js_write>("dataWithJSONObject:options:error:")
      .cm<&js_isValid>("isValidJSONObject:", "c@:@").cm<&js_readStream>("JSONObjectWithStream:options:error:");
    ObjC::ClassBuilder pls = r.defineClass("NSPropertyListSerialization", "NSObject");
    pls.cm<&pls_dataWith>("dataWithPropertyList:format:options:error:").cm<&pls_dataFrom>("dataFromPropertyList:format:errorDescription:")
       .cm<&pls_parse>("propertyListWithData:options:format:error:")
       .cm<&pls_parseOld>("propertyListFromData:mutabilityOption:format:errorDescription:")
       .cm<&pls_isValid>("propertyList:isValidForFormat:", "c@:@I");

    ObjC::ClassBuilder coder = r.defineClass("NSCoder", "NSObject");
    ObjC::ClassBuilder ka = r.defineClass("NSKeyedArchiver", "NSCoder");
    ObjC::ClassBuilder ku = r.defineClass("NSKeyedUnarchiver", "NSCoder");
    a.coder = coder.cls(); a.archiver = ka.cls(); a.unarchiver = ku.cls();
    coder.m<&coder_allowsKeyed>("allowsKeyedCoding", "c@:").m<&coder_systemVersion>("systemVersion", "I@:")
         .m<&coder_versionForClass>("versionForClassName:", "i@:@").m<&coder_abstract>("decodeObject");
    ka.cm<&ka_archivedData>("archivedDataWithRootObject:").cm<&ka_archiveToFile>("archiveRootObject:toFile:", "c@:@@");
    ka.m<&ka_init>("init").m<&ka_initForWriting>("initForWritingWithMutableData:").m<&ka_initRequiringSecure>("initRequiringSecureCoding:")
      .m<&ka_finish>("finishEncoding", "v@:").m<&ka_encodedData>("encodedData")
      .m<&ka_encodeObject>("encodeObject:forKey:", "v@:@@").m<&ka_encodeObject>("encodeConditionalObject:forKey:", "v@:@@")
      .m<&ka_encodeInt>("encodeInt:forKey:", "v@:i@").m<&ka_encodeInt>("encodeInt32:forKey:", "v@:i@")
      .m<&ka_encodeInt>("encodeInteger:forKey:", "v@:i@").m<&ka_encodeInt64>("encodeInt64:forKey:", "v@:q@")
      .m<&ka_encodeBool>("encodeBool:forKey:", "v@:c@").m<&ka_encodeFloat>("encodeFloat:forKey:", "v@:f@")
      .m<&ka_encodeDouble>("encodeDouble:forKey:", "v@:d@").m<&ka_encodeBytes>("encodeBytes:length:forKey:", "v@:r*I@")
      .m<&ka_encodeRoot>("encodeRootObject:", "v@:@").m<&ka_encodeRoot>("encodeObject:", "v@:@")
      .m<&ka_allowsKeyed>("allowsKeyedCoding", "c@:").m<&ka_setFormat>("setOutputFormat:", "v@:I")
      .m<&ka_setDelegate>("setDelegate:", "v@:@").m<&ka_setDelegate>("setRequiresSecureCoding:", "v@:c")
      .m<&ka_dealloc>("dealloc", "v@:");
    ku.cm<&ku_unarchiveData>("unarchiveObjectWithData:").cm<&ku_unarchiveFile>("unarchiveObjectWithFile:")
      .cm<&kuc_setClass>("setClass:forClassName:", "v@:#@");
    ku.m<&ku_initForReading>("initForReadingWithData:").m<&ku_decodeObject>("decodeObjectForKey:")
      .m<&ku_decodeObjectOfClass>("decodeObjectOfClass:forKey:").m<&ku_contains>("containsValueForKey:", "c@:@")
      .m<&ku_decodeInt>("decodeIntForKey:", "i@:@").m<&ku_decodeInt>("decodeInt32ForKey:", "i@:@")
      .m<&ku_decodeInt>("decodeIntegerForKey:", "i@:@").m<&ku_decodeInt64>("decodeInt64ForKey:", "q@:@")
      .m<&ku_decodeBool>("decodeBoolForKey:", "c@:@").m<&ku_decodeFloat>("decodeFloatForKey:", "f@:@")
      .m<&ku_decodeDouble>("decodeDoubleForKey:", "d@:@").m<&ku_decodeBytes>("decodeBytesForKey:returnedLength:", "r*@:@^I")
      .m<&ka_allowsKeyed>("allowsKeyedCoding", "c@:").m<&ku_finish>("finishDecoding", "v@:")
      .m<&ku_setClass>("setClass:forClassName:", "v@:#@").m<&ka_setDelegate>("setDelegate:", "v@:@")
      .m<&ka_setDelegate>("setRequiresSecureCoding:", "v@:c").m<&ku_dealloc>("dealloc", "v@:");
    const uint32_t rootKey = p.defineDataBlock("_NSKeyedArchiveRootObjectKey", 4);
    p.mem().write<uint32_t>(rootKey, constString(p, "root"));
}

} // namespace Hydrogen::Runtime::Foundation
