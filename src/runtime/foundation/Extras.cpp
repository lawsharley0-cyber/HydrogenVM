// HydrogenVM — Foundation classes apps commonly touch at launch:
//   NSScanner, NSXMLParser (SAX, non-validating), NSURLResponse / NSHTTPURLResponse,
//   NSHTTPCookie / NSHTTPCookieStorage (in-memory), NSURLProtocol (registration only),
//   NSCache, NSDecimalNumber (double-backed approximation).
#include "Foundation.hpp"
#include <cmath>
#include <cstring>
#include <fstream>
#include <iterator>
#include <mutex>
#include <regex>

namespace Hydrogen::Runtime::Foundation {
bool characterSetContains(Ctx& x, id set, char16_t c);
namespace {

using ObjC::Class;
using ObjC::SEL;
ObjC::Runtime& R(Ctx& x) { return ObjC::rt(x); }
id retainObj(Ctx& x, id o) { return o ? R(x).retain(x.cpu, o) : 0; }
void setField(Ctx& x, id& slot, id v) { const id old = slot; slot = v ? R(x).retain(x.cpu, v) : 0; if (old) R(x).release(x.cpu, old); }

struct ExtrasState {
    explicit ExtrasState(Process&) {}
    Class scanner = 0, xmlParser = 0, urlResponse = 0, httpResponse = 0, cookie = 0, cookieStorage = 0, cache = 0;
    id sharedCookies = 0;
};
ExtrasState& ES(Ctx& x) { return x.proc.service<ExtrasState>(); }

// ------------------------------------------------------------- NSScanner ---
struct Scanner : ObjC::HostObject {
    std::u16string s;
    id string = 0, skip = 0;
    uint32_t pos = 0;
    bool caseSensitive = false;
    bool skipDefault = true;   // whitespace and newlines until the app sets its own
};
Scanner& SC(Ctx& x, id self) { return R(x).host<Scanner>(self); }
bool isWs(char16_t c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == 0x0B || c == 0x0C || c == 0x85 || c == 0xA0 || c == 0x2028 || c == 0x2029; }
void skipChars(Ctx& x, Scanner& sc) {
    while (sc.pos < sc.s.size()) {
        const char16_t c = sc.s[sc.pos];
        const bool skip = sc.skipDefault ? isWs(c) : (sc.skip && characterSetContains(x, sc.skip, c));
        if (!skip) break;
        ++sc.pos;
    }
}
id scc_with(Ctx& x, id cls, SEL, id str) {
    const id o = R(x).createInstance(cls);
    Scanner& sc = SC(x, o);
    sc.string = str ? id(R(x).send(x.cpu, str, "copy")) : makeString(x, "", true);
    sc.s = stringValue(x, sc.string);
    return autoreleased(x, o);
}
id sc_init(Ctx& x, id self, SEL, id str) {
    Scanner& sc = SC(x, self);
    sc.string = str ? id(R(x).send(x.cpu, str, "copy")) : makeString(x, "", true);
    sc.s = stringValue(x, sc.string);
    return self;
}
id sc_string(Ctx& x, id self, SEL) { return SC(x, self).string; }
uint32_t sc_location(Ctx& x, id self, SEL) { return SC(x, self).pos; }
void sc_setLocation(Ctx& x, id self, SEL, uint32_t p) {
    Scanner& sc = SC(x, self);
    if (p > sc.s.size()) throw std::runtime_error("*** -[NSScanner setScanLocation:]: Range or index out of bounds");
    sc.pos = p;
}
bool sc_atEnd(Ctx& x, id self, SEL) { Scanner& sc = SC(x, self); uint32_t keep = sc.pos; skipChars(x, sc); const bool end = sc.pos >= sc.s.size(); sc.pos = keep; return end; }
id sc_skip(Ctx& x, id self, SEL) {
    Scanner& sc = SC(x, self);
    if (sc.skipDefault) return id(R(x).send(x.cpu, R(x).getClass("NSCharacterSet"), "whitespaceAndNewlineCharacterSet"));
    return sc.skip;
}
void sc_setSkip(Ctx& x, id self, SEL, id set) { Scanner& sc = SC(x, self); sc.skipDefault = false; setField(x, sc.skip, set); }
bool sc_caseSensitive(Ctx& x, id self, SEL) { return SC(x, self).caseSensitive; }
void sc_setCaseSensitive(Ctx& x, id self, SEL, bool b) { SC(x, self).caseSensitive = b; }

// Number scanning: the longest prefix that parses, starting after skipped characters.
std::string asciiFrom(const Scanner& sc, uint32_t start) {
    std::string out;
    for (uint32_t i = start; i < sc.s.size() && sc.s[i] < 0x80 && out.size() < 400; ++i) out += char(sc.s[i]);
    return out;
}
template <typename Parse> bool scanNumber(Ctx& x, id self, Parse parse) {
    Scanner& sc = SC(x, self);
    skipChars(x, sc);
    const std::string a = asciiFrom(sc, sc.pos);
    size_t used = 0;
    if (!parse(a, used) || !used) return false;
    sc.pos += uint32_t(used);
    return true;
}
bool sc_scanInt(Ctx& x, id self, SEL, GuestPtr out) {
    return scanNumber(x, self, [&](const std::string& a, size_t& used) {
        char* end = nullptr;
        const long long v = std::strtoll(a.c_str(), &end, 10);
        used = size_t(end - a.c_str());
        if (!used) return false;
        const int32_t clamped = v > INT32_MAX ? INT32_MAX : v < INT32_MIN ? INT32_MIN : int32_t(v);
        if (out) x.mem.write<uint32_t>(out, uint32_t(clamped));
        return true;
    });
}
bool sc_scanLongLong(Ctx& x, id self, SEL, GuestPtr out) {
    return scanNumber(x, self, [&](const std::string& a, size_t& used) {
        char* end = nullptr;
        const long long v = std::strtoll(a.c_str(), &end, 10);
        used = size_t(end - a.c_str());
        if (used && out) x.mem.write<uint64_t>(out, uint64_t(v));
        return used != 0;
    });
}
bool sc_scanHexInt(Ctx& x, id self, SEL, GuestPtr out) {
    return scanNumber(x, self, [&](const std::string& a, size_t& used) {
        size_t start = 0;
        if (a.size() > 2 && a[0] == '0' && (a[1] == 'x' || a[1] == 'X') && std::isxdigit(uint8_t(a[2]))) start = 2;
        char* end = nullptr;
        const unsigned long long v = std::strtoull(a.c_str() + start, &end, 16);
        used = size_t(end - a.c_str());
        if (used <= start) return false;
        if (out) x.mem.write<uint32_t>(out, uint32_t(std::min<unsigned long long>(v, UINT32_MAX)));
        return true;
    });
}
bool sc_scanHexLongLong(Ctx& x, id self, SEL, GuestPtr out) {
    return scanNumber(x, self, [&](const std::string& a, size_t& used) {
        size_t start = 0;
        if (a.size() > 2 && a[0] == '0' && (a[1] == 'x' || a[1] == 'X') && std::isxdigit(uint8_t(a[2]))) start = 2;
        char* end = nullptr;
        const unsigned long long v = std::strtoull(a.c_str() + start, &end, 16);
        used = size_t(end - a.c_str());
        if (used <= start) return false;
        if (out) x.mem.write<uint64_t>(out, v);
        return true;
    });
}
bool doubleParse(const std::string& a, size_t& used, double& v) {
    // Decimal only (no hex floats, inf or nan), like NSScanner.
    size_t i = 0;
    if (i < a.size() && (a[i] == '+' || a[i] == '-')) ++i;
    size_t digits = 0;
    while (i < a.size() && std::isdigit(uint8_t(a[i]))) { ++i; ++digits; }
    if (i < a.size() && a[i] == '.') { ++i; while (i < a.size() && std::isdigit(uint8_t(a[i]))) { ++i; ++digits; } }
    if (!digits) return false;
    if (i < a.size() && (a[i] == 'e' || a[i] == 'E')) {
        size_t j = i + 1;
        if (j < a.size() && (a[j] == '+' || a[j] == '-')) ++j;
        if (j < a.size() && std::isdigit(uint8_t(a[j]))) { i = j; while (i < a.size() && std::isdigit(uint8_t(a[i]))) ++i; }
    }
    v = std::strtod(a.substr(0, i).c_str(), nullptr);
    used = i;
    return true;
}
bool sc_scanDouble(Ctx& x, id self, SEL, GuestPtr out) {
    return scanNumber(x, self, [&](const std::string& a, size_t& used) {
        double v;
        if (!doubleParse(a, used, v)) return false;
        if (out) x.mem.write<double>(out, v);
        return true;
    });
}
bool sc_scanFloat(Ctx& x, id self, SEL, GuestPtr out) {
    return scanNumber(x, self, [&](const std::string& a, size_t& used) {
        double v;
        if (!doubleParse(a, used, v)) return false;
        if (out) x.mem.write<float>(out, float(v));
        return true;
    });
}
bool eqChar(char16_t a, char16_t b, bool cs) {
    if (cs) return a == b;
    auto lower = [](char16_t c) -> char16_t { return c >= 'A' && c <= 'Z' ? char16_t(c + 32) : c; };
    return lower(a) == lower(b);
}
bool sc_scanString(Ctx& x, id self, SEL, id str, GuestPtr into) {
    Scanner& sc = SC(x, self);
    const std::u16string want = str ? stringValue(x, str) : std::u16string();
    const uint32_t keep = sc.pos;
    skipChars(x, sc);
    if (want.empty() || sc.pos + want.size() > sc.s.size()) { sc.pos = keep; return false; }
    for (size_t i = 0; i < want.size(); ++i)
        if (!eqChar(sc.s[sc.pos + i], want[i], sc.caseSensitive)) { sc.pos = keep; return false; }
    if (into) x.mem.write<uint32_t>(into, makeString(x, sc.s.substr(sc.pos, want.size())));
    sc.pos += uint32_t(want.size());
    return true;
}
bool sc_scanUpToString(Ctx& x, id self, SEL, id str, GuestPtr into) {
    Scanner& sc = SC(x, self);
    const std::u16string want = str ? stringValue(x, str) : std::u16string();
    skipChars(x, sc);
    const uint32_t start = sc.pos;
    uint32_t i = start;
    for (; i < sc.s.size(); ++i) {
        if (!want.empty() && i + want.size() <= sc.s.size()) {
            bool hit = true;
            for (size_t k = 0; k < want.size() && hit; ++k) hit = eqChar(sc.s[i + k], want[k], sc.caseSensitive);
            if (hit) break;
        }
    }
    if (i == start) return false;
    if (into) x.mem.write<uint32_t>(into, makeString(x, sc.s.substr(start, i - start)));
    sc.pos = i;
    return true;
}
bool scanSet(Ctx& x, id self, id set, GuestPtr into, bool member) {
    Scanner& sc = SC(x, self);
    skipChars(x, sc);
    const uint32_t start = sc.pos;
    uint32_t i = start;
    while (i < sc.s.size() && characterSetContains(x, set, sc.s[i]) == member) ++i;
    if (i == start) return false;
    if (into) x.mem.write<uint32_t>(into, makeString(x, sc.s.substr(start, i - start)));
    sc.pos = i;
    return true;
}
bool sc_scanCharacters(Ctx& x, id self, SEL, id set, GuestPtr into) { return scanSet(x, self, set, into, true); }
bool sc_scanUpToCharacters(Ctx& x, id self, SEL, id set, GuestPtr into) { return scanSet(x, self, set, into, false); }
void sc_dealloc(Ctx& x, id self, SEL) {
    if (Scanner* sc = R(x).hostIf<Scanner>(self)) for (id o : {sc->string, sc->skip}) if (o) R(x).release(x.cpu, o);
    R(x).disposeInstance(x.cpu, self);
}

// ----------------------------------------------------------- NSXMLParser ---
struct XmlParser : ObjC::HostObject {
    std::vector<uint8_t> data;
    id delegate = 0, error = 0;
    bool aborted = false, namespaces = false;
    uint32_t line = 1, column = 1;
};
XmlParser& XP(Ctx& x, id self) { return R(x).host<XmlParser>(self); }

id xml_initWithData(Ctx& x, id self, SEL, id data) {
    XmlParser& p = XP(x, self);
    p.data = data ? dataBytes(x, data) : std::vector<uint8_t>();
    return self;
}
id xml_initWithURL(Ctx& x, id self, SEL, id url) {
    XmlParser& p = XP(x, self);
    const id path = url ? id(R(x).send(x.cpu, url, "path")) : 0;
    std::ifstream f(path ? utf8(x, path) : std::string(), std::ios::binary);
    if (!f) { R(x).release(x.cpu, self); return 0; }   // remote URLs: offline
    p.data.assign(std::istreambuf_iterator<char>(f), {});
    return self;
}
id xml_initWithStream(Ctx& x, id self, SEL, id stream) {
    XmlParser& p = XP(x, self);
    if (stream) {
        R(x).send(x.cpu, stream, "open");
        const uint32_t buf = x.proc.heap().alloc(4096);
        for (;;) {
            const int32_t n = int32_t(R(x).send(x.cpu, stream, "read:maxLength:", {buf, 4096}));
            if (n <= 0) break;
            const uint8_t* b = x.mem.host(buf);
            p.data.insert(p.data.end(), b, b + n);
        }
        x.proc.heap().free(buf);
        R(x).send(x.cpu, stream, "close");
    }
    return self;
}
id xml_delegate(Ctx& x, id self, SEL) { return XP(x, self).delegate; }
void xml_setDelegate(Ctx& x, id self, SEL, id d) { XP(x, self).delegate = d; }
void xml_setNamespaces(Ctx& x, id self, SEL, bool b) { XP(x, self).namespaces = b; }
void xml_ignoreBool(Ctx&, id, SEL, bool) {}
void xml_abort(Ctx& x, id self, SEL) { XP(x, self).aborted = true; }
id xml_error(Ctx& x, id self, SEL) { return XP(x, self).error; }
int32_t xml_line(Ctx& x, id self, SEL) { return int32_t(XP(x, self).line); }
int32_t xml_column(Ctx& x, id self, SEL) { return int32_t(XP(x, self).column); }

std::string decodeEntities(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] != '&') { out += s[i]; continue; }
        const size_t semi = s.find(';', i);
        if (semi == std::string::npos || semi - i > 12) { out += s[i]; continue; }
        const std::string ent = s.substr(i + 1, semi - i - 1);
        uint32_t cp = 0;
        if (ent == "lt") cp = '<'; else if (ent == "gt") cp = '>'; else if (ent == "amp") cp = '&';
        else if (ent == "quot") cp = '"'; else if (ent == "apos") cp = '\'';
        else if (!ent.empty() && ent[0] == '#') cp = uint32_t(ent.size() > 1 && (ent[1] == 'x' || ent[1] == 'X') ? std::strtoul(ent.c_str() + 2, nullptr, 16) : std::strtoul(ent.c_str() + 1, nullptr, 10));
        if (!cp) { out += s[i]; continue; }
        if (cp < 0x80) out += char(cp);
        else if (cp < 0x800) { out += char(0xC0 | (cp >> 6)); out += char(0x80 | (cp & 0x3F)); }
        else if (cp < 0x10000) { out += char(0xE0 | (cp >> 12)); out += char(0x80 | ((cp >> 6) & 0x3F)); out += char(0x80 | (cp & 0x3F)); }
        else { out += char(0xF0 | (cp >> 18)); out += char(0x80 | ((cp >> 12) & 0x3F)); out += char(0x80 | ((cp >> 6) & 0x3F)); out += char(0x80 | (cp & 0x3F)); }
        i = semi;
    }
    return out;
}

bool xml_parse(Ctx& x, id self, SEL) {
    XmlParser& p = XP(x, self);
    const id d = p.delegate;
    ObjC::Runtime& r = R(x);
    auto responds = [&](const char* s) { return d && r.respondsTo(d, r.sel(s)); };
    const bool hasStart = responds("parser:didStartElement:namespaceURI:qualifiedName:attributes:");
    const bool hasEnd = responds("parser:didEndElement:namespaceURI:qualifiedName:");
    const bool hasChars = responds("parser:foundCharacters:");
    const bool hasCData = responds("parser:foundCDATA:");
    const bool hasComment = responds("parser:foundComment:");
    std::string s(p.data.begin(), p.data.end());
    if (s.size() >= 3 && uint8_t(s[0]) == 0xEF && uint8_t(s[1]) == 0xBB && uint8_t(s[2]) == 0xBF) s.erase(0, 3);
    p.aborted = false;
    p.line = 1;
    p.column = 1;
    auto fail = [&](int code, const std::string& why) {
        const id info = makeDict(x, {{constString(x.proc, "NSLocalizedDescription"), makeString(x, why)}});
        const id err = id(r.send(x.cpu, r.getClass("NSError"), "errorWithDomain:code:userInfo:", {constString(x.proc, "NSXMLParserErrorDomain"), uint32_t(code), info}));
        setField(x, p.error, err);
        if (responds("parser:parseErrorOccurred:")) r.send(x.cpu, d, "parser:parseErrorOccurred:", {self, err});
        return false;
    };
    auto pos = [&](size_t i) {
        uint32_t line = 1, col = 1;
        for (size_t k = 0; k < i && k < s.size(); ++k) { if (s[k] == '\n') { ++line; col = 1; } else ++col; }
        p.line = line; p.column = col;
    };
    if (responds("parserDidStartDocument:")) r.send(x.cpu, d, "parserDidStartDocument:", {self});
    std::vector<std::string> stack;
    size_t i = 0;
    const uint32_t pool = r.poolPush();
    auto emitText = [&](const std::string& t) {
        if (t.empty() || !hasChars) return;
        r.send(x.cpu, d, "parser:foundCharacters:", {self, makeString(x, decodeEntities(t))});
    };
    while (i < s.size() && !p.aborted) {
        const size_t lt = s.find('<', i);
        if (lt == std::string::npos) {
            if (!stack.empty()) emitText(s.substr(i));
            break;
        }
        if (lt > i && !stack.empty()) emitText(s.substr(i, lt - i));
        if (s.compare(lt, 4, "<!--") == 0) {
            const size_t e = s.find("-->", lt + 4);
            if (e == std::string::npos) { pos(lt); r.poolPop(x.cpu, pool); return fail(5, "Unterminated comment"); }
            if (hasComment) r.send(x.cpu, d, "parser:foundComment:", {self, makeString(x, s.substr(lt + 4, e - lt - 4))});
            i = e + 3;
        } else if (s.compare(lt, 9, "<![CDATA[") == 0) {
            const size_t e = s.find("]]>", lt + 9);
            if (e == std::string::npos) { pos(lt); r.poolPop(x.cpu, pool); return fail(63, "Unterminated CDATA section"); }
            const std::string body = s.substr(lt + 9, e - lt - 9);
            if (hasCData) r.send(x.cpu, d, "parser:foundCDATA:", {self, makeData(x, body.data(), uint32_t(body.size()))});
            else if (hasChars) r.send(x.cpu, d, "parser:foundCharacters:", {self, makeString(x, body)});
            i = e + 3;
        } else if (s.compare(lt, 2, "<?") == 0) {
            const size_t e = s.find("?>", lt + 2);
            if (e == std::string::npos) { pos(lt); r.poolPop(x.cpu, pool); return fail(4, "Unterminated processing instruction"); }
            i = e + 2;
        } else if (s.compare(lt, 2, "<!") == 0) {   // DOCTYPE (with an optional internal subset)
            int depth = 0;
            size_t k = lt + 2;
            for (; k < s.size(); ++k) {
                if (s[k] == '[') ++depth;
                else if (s[k] == ']') --depth;
                else if (s[k] == '>' && depth <= 0) break;
            }
            i = k + 1;
        } else if (s.compare(lt, 2, "</") == 0) {
            const size_t e = s.find('>', lt);
            if (e == std::string::npos) { pos(lt); r.poolPop(x.cpu, pool); return fail(76, "Unterminated end tag"); }
            std::string name = s.substr(lt + 2, e - lt - 2);
            while (!name.empty() && std::isspace(uint8_t(name.back()))) name.pop_back();
            if (stack.empty() || stack.back() != name) { pos(lt); r.poolPop(x.cpu, pool); return fail(76, "Tag mismatch: </" + name + ">"); }
            stack.pop_back();
            if (hasEnd) r.send(x.cpu, d, "parser:didEndElement:namespaceURI:qualifiedName:", {self, makeString(x, name), 0, 0});
            i = e + 1;
        } else {
            // Start tag: name, attributes, optional self-close. Quoted values may contain '>'.
            size_t k = lt + 1;
            while (k < s.size() && !std::isspace(uint8_t(s[k])) && s[k] != '>' && s[k] != '/') ++k;
            const std::string name = s.substr(lt + 1, k - lt - 1);
            if (name.empty()) { pos(lt); r.poolPop(x.cpu, pool); return fail(68, "Invalid start tag"); }
            std::vector<std::pair<id, id>> attrs;
            bool selfClose = false;
            for (;;) {
                while (k < s.size() && std::isspace(uint8_t(s[k]))) ++k;
                if (k >= s.size()) { pos(lt); r.poolPop(x.cpu, pool); return fail(73, "Unterminated start tag"); }
                if (s[k] == '>') { ++k; break; }
                if (s[k] == '/' && k + 1 < s.size() && s[k + 1] == '>') { selfClose = true; k += 2; break; }
                size_t a = k;
                while (k < s.size() && s[k] != '=' && !std::isspace(uint8_t(s[k])) && s[k] != '>') ++k;
                const std::string an = s.substr(a, k - a);
                while (k < s.size() && std::isspace(uint8_t(s[k]))) ++k;
                std::string av;
                if (k < s.size() && s[k] == '=') {
                    ++k;
                    while (k < s.size() && std::isspace(uint8_t(s[k]))) ++k;
                    if (k < s.size() && (s[k] == '"' || s[k] == '\'')) {
                        const char q = s[k];
                        const size_t e = s.find(q, k + 1);
                        if (e == std::string::npos) { pos(k); r.poolPop(x.cpu, pool); return fail(38, "Unterminated attribute value"); }
                        av = s.substr(k + 1, e - k - 1);
                        k = e + 1;
                    } else {
                        const size_t a2 = k;
                        while (k < s.size() && !std::isspace(uint8_t(s[k])) && s[k] != '>') ++k;
                        av = s.substr(a2, k - a2);
                    }
                }
                if (!an.empty()) attrs.push_back({makeString(x, an), makeString(x, decodeEntities(av))});
            }
            if (hasStart)
                r.send(x.cpu, d, "parser:didStartElement:namespaceURI:qualifiedName:attributes:", {self, makeString(x, name), 0, 0, makeDict(x, attrs)});
            if (selfClose) { if (hasEnd && !p.aborted) r.send(x.cpu, d, "parser:didEndElement:namespaceURI:qualifiedName:", {self, makeString(x, name), 0, 0}); }
            else stack.push_back(name);
            i = k;
        }
    }
    r.poolPop(x.cpu, pool);
    if (p.aborted) return fail(512, "Parsing aborted by the delegate");
    if (!stack.empty()) { pos(s.size()); return fail(5, "Unexpected end of document: <" + stack.back() + "> isn't closed"); }
    if (responds("parserDidEndDocument:")) r.send(x.cpu, d, "parserDidEndDocument:", {self});
    return true;
}
void xml_dealloc(Ctx& x, id self, SEL) {
    if (XmlParser* p = R(x).hostIf<XmlParser>(self)) if (p->error) R(x).release(x.cpu, p->error);
    R(x).disposeInstance(x.cpu, self);
}

// ------------------------------------------------------------------ libxml2 ---
// Guest-visible xmlNode / xmlDoc / xmlAttr / xmlNs / xmlXPath structs (32-bit ABI)
// xmlNode (60 bytes): +4 type, +8 name, +12 children, +16 last, +20 parent, +24 next,
//                     +28 prev, +32 doc, +36 ns, +40 content, +44 properties, +48 nsDef
// xmlNs   (24 bytes): +0 next, +4 type(18), +8 href, +12 prefix, +16 _private, +20 context
// xmlXPathContext (16 bytes): +0 doc, +4 node
// xmlXPathObject  (16 bytes): +0 type(1=NODESET), +4 nodesetval
// xmlNodeSet      (12 bytes): +0 nodeNr, +4 nodeMax, +8 nodeTab

GuestPtr lx_strdupStr(Ctx& x, const std::string& s) {
    const uint32_t a = x.proc.heap().alloc(uint32_t(s.size() + 1));
    if (!a) return 0;
    if (!s.empty()) std::memcpy(x.mem.host(a), s.data(), s.size());
    x.mem.write<uint8_t>(a + uint32_t(s.size()), 0);
    return a;
}
void lx_free(Ctx& x, GuestPtr p) {
    if (p && x.proc.heap().blockSize(p)) x.proc.heap().free(p);
}
void lx_initParser(Ctx&) {}
GuestPtr lx_newNodeRaw(Ctx& x, uint32_t type, const std::string& name, const std::string& content = {}) {
    const uint32_t n = x.proc.heap().allocZeroed(64);
    x.mem.write<uint32_t>(n + 4, type);
    if (!name.empty()) x.mem.write<uint32_t>(n + 8, lx_strdupStr(x, name));
    if (!content.empty()) x.mem.write<uint32_t>(n + 40, lx_strdupStr(x, content));
    return n;
}
GuestPtr lx_addChild(Ctx& x, GuestPtr parent, GuestPtr cur) {
    if (!parent || !cur) return cur;
    const uint32_t type = x.mem.read<uint32_t>(cur + 4);
    const uint32_t doc = x.mem.read<uint32_t>(parent + 4) == 9 ? parent : x.mem.read<uint32_t>(parent + 32);
    x.mem.write<uint32_t>(cur + 20, parent);
    if (doc) x.mem.write<uint32_t>(cur + 32, doc);
    if (type == 2 /* XML_ATTRIBUTE_NODE */) {
        uint32_t p = x.mem.read<uint32_t>(parent + 44);
        if (!p) x.mem.write<uint32_t>(parent + 44, cur);
        else {
            while (x.mem.read<uint32_t>(p + 24)) p = x.mem.read<uint32_t>(p + 24);
            x.mem.write<uint32_t>(p + 24, cur);
            x.mem.write<uint32_t>(cur + 28, p);
        }
        return cur;
    }
    const uint32_t last = x.mem.read<uint32_t>(parent + 16);
    if (!last) {
        x.mem.write<uint32_t>(parent + 12, cur);
        x.mem.write<uint32_t>(parent + 16, cur);
    } else {
        x.mem.write<uint32_t>(last + 24, cur);
        x.mem.write<uint32_t>(cur + 28, last);
        x.mem.write<uint32_t>(parent + 16, cur);
    }
    return cur;
}
GuestPtr lx_newDoc(Ctx& x, GuestPtr) {
    const uint32_t doc = lx_newNodeRaw(x, 9 /* XML_DOCUMENT_NODE */, "document");
    x.mem.write<uint32_t>(doc + 32, doc);
    return doc;
}
GuestPtr lx_newNode(Ctx& x, GuestPtr ns, GuestPtr name) {
    const uint32_t n = lx_newNodeRaw(x, 1 /* XML_ELEMENT_NODE */, name ? x.proc.readCString(name) : "");
    x.mem.write<uint32_t>(n + 36, ns);
    return n;
}
GuestPtr lx_newText(Ctx& x, GuestPtr content) {
    return lx_newNodeRaw(x, 3 /* XML_TEXT_NODE */, "text", content ? x.proc.readCString(content) : "");
}
GuestPtr lx_newProp(Ctx& x, GuestPtr node, GuestPtr name, GuestPtr value) {
    const uint32_t attr = lx_newNodeRaw(x, 2 /* XML_ATTRIBUTE_NODE */, name ? x.proc.readCString(name) : "");
    const uint32_t txt = lx_newText(x, value);
    lx_addChild(x, attr, txt);
    if (node) lx_addChild(x, node, attr);
    return attr;
}
GuestPtr lx_newNs(Ctx& x, GuestPtr node, GuestPtr href, GuestPtr prefix) {
    const uint32_t ns = x.proc.heap().allocZeroed(24);
    x.mem.write<uint32_t>(ns + 4, 18 /* XML_NAMESPACE_DECL */);
    if (href) x.mem.write<uint32_t>(ns + 8, lx_strdupStr(x, x.proc.readCString(href)));
    if (prefix) x.mem.write<uint32_t>(ns + 12, lx_strdupStr(x, x.proc.readCString(prefix)));
    if (node) {
        uint32_t cur = x.mem.read<uint32_t>(node + 48);
        if (!cur) x.mem.write<uint32_t>(node + 48, ns);
        else {
            while (x.mem.read<uint32_t>(cur)) cur = x.mem.read<uint32_t>(cur);
            x.mem.write<uint32_t>(cur, ns);
        }
    }
    return ns;
}
GuestPtr lx_newNsProp(Ctx& x, GuestPtr node, GuestPtr ns, GuestPtr name, GuestPtr value) {
    const uint32_t attr = lx_newProp(x, node, name, value);
    if (attr) x.mem.write<uint32_t>(attr + 36, ns);
    return attr;
}
GuestPtr lx_docGetRoot(Ctx& x, GuestPtr doc) {
    if (!doc) return 0;
    for (uint32_t c = x.mem.read<uint32_t>(doc + 12); c; c = x.mem.read<uint32_t>(c + 24))
        if (x.mem.read<uint32_t>(c + 4) == 1) return c;
    return x.mem.read<uint32_t>(doc + 12);
}
GuestPtr lx_docSetRoot(Ctx& x, GuestPtr doc, GuestPtr root) {
    if (!doc) return 0;
    const uint32_t old = lx_docGetRoot(x, doc);
    x.mem.write<uint32_t>(doc + 12, 0);
    x.mem.write<uint32_t>(doc + 16, 0);
    if (root) lx_addChild(x, doc, root);
    return old;
}
void lx_unlinkNode(Ctx& x, GuestPtr cur) {
    if (!cur) return;
    const uint32_t parent = x.mem.read<uint32_t>(cur + 20);
    const uint32_t next = x.mem.read<uint32_t>(cur + 24);
    const uint32_t prev = x.mem.read<uint32_t>(cur + 28);
    if (prev) x.mem.write<uint32_t>(prev + 24, next);
    if (next) x.mem.write<uint32_t>(next + 28, prev);
    if (parent) {
        if (x.mem.read<uint32_t>(parent + 12) == cur) x.mem.write<uint32_t>(parent + 12, next);
        if (x.mem.read<uint32_t>(parent + 16) == cur) x.mem.write<uint32_t>(parent + 16, prev);
        if (x.mem.read<uint32_t>(parent + 44) == cur) x.mem.write<uint32_t>(parent + 44, next);
    }
    x.mem.write<uint32_t>(cur + 20, 0);
    x.mem.write<uint32_t>(cur + 24, 0);
    x.mem.write<uint32_t>(cur + 28, 0);
}
GuestPtr lx_copyNode(Ctx& x, GuestPtr node, int32_t extended) {
    if (!node) return 0;
    const uint32_t type = x.mem.read<uint32_t>(node + 4);
    const uint32_t nm = x.mem.read<uint32_t>(node + 8);
    const uint32_t ct = x.mem.read<uint32_t>(node + 40);
    const uint32_t copy = lx_newNodeRaw(x, type, nm ? x.proc.readCString(nm) : "", ct ? x.proc.readCString(ct) : "");
    x.mem.write<uint32_t>(copy + 36, x.mem.read<uint32_t>(node + 36));
    if (extended) {
        for (uint32_t a = x.mem.read<uint32_t>(node + 44); a; a = x.mem.read<uint32_t>(a + 24))
            lx_addChild(x, copy, lx_copyNode(x, a, 1));
        for (uint32_t c = x.mem.read<uint32_t>(node + 12); c; c = x.mem.read<uint32_t>(c + 24))
            lx_addChild(x, copy, lx_copyNode(x, c, 1));
    }
    return copy;
}
void lx_setTreeDoc(Ctx& x, GuestPtr tree, GuestPtr doc) {
    if (!tree) return;
    x.mem.write<uint32_t>(tree + 32, doc);
    for (uint32_t c = x.mem.read<uint32_t>(tree + 12); c; c = x.mem.read<uint32_t>(c + 24))
        lx_setTreeDoc(x, c, doc);
}
void lx_setNs(Ctx& x, GuestPtr node, GuestPtr ns) { if (node) x.mem.write<uint32_t>(node + 36, ns); }
GuestPtr lx_searchNs(Ctx& x, GuestPtr, GuestPtr node, GuestPtr prefix) {
    const std::string want = prefix ? x.proc.readCString(prefix) : "";
    for (uint32_t n = node; n; n = x.mem.read<uint32_t>(n + 20)) {
        for (uint32_t ns = x.mem.read<uint32_t>(n + 48); ns; ns = x.mem.read<uint32_t>(ns)) {
            const uint32_t pf = x.mem.read<uint32_t>(ns + 12);
            const std::string have = pf ? x.proc.readCString(pf) : "";
            if (have == want) return ns;
        }
    }
    return 0;
}
GuestPtr lx_searchNsByHref(Ctx& x, GuestPtr, GuestPtr node, GuestPtr href) {
    if (!href) return 0;
    const std::string want = x.proc.readCString(href);
    for (uint32_t n = node; n; n = x.mem.read<uint32_t>(n + 20)) {
        for (uint32_t ns = x.mem.read<uint32_t>(n + 48); ns; ns = x.mem.read<uint32_t>(ns)) {
            const uint32_t hr = x.mem.read<uint32_t>(ns + 8);
            if (hr && x.proc.readCString(hr) == want) return ns;
        }
    }
    return 0;
}
GuestPtr lx_hasProp(Ctx& x, GuestPtr node, GuestPtr name) {
    if (!node || !name) return 0;
    const std::string want = x.proc.readCString(name);
    for (uint32_t a = x.mem.read<uint32_t>(node + 44); a; a = x.mem.read<uint32_t>(a + 24)) {
        const uint32_t nm = x.mem.read<uint32_t>(a + 8);
        if (nm && x.proc.readCString(nm) == want) return a;
    }
    return 0;
}
GuestPtr lx_hasNsProp(Ctx& x, GuestPtr node, GuestPtr name, GuestPtr) { return lx_hasProp(x, node, name); }
std::string lx_collectText(Ctx& x, GuestPtr node) {
    if (!node) return {};
    const uint32_t type = x.mem.read<uint32_t>(node + 4);
    if (type == 3 /* XML_TEXT_NODE */) {
        const uint32_t ct = x.mem.read<uint32_t>(node + 40);
        return ct ? x.proc.readCString(ct) : "";
    }
    std::string out;
    for (uint32_t c = x.mem.read<uint32_t>(node + 12); c; c = x.mem.read<uint32_t>(c + 24))
        out += lx_collectText(x, c);
    return out;
}
GuestPtr lx_nodeGetContent(Ctx& x, GuestPtr node) { return lx_strdupStr(x, lx_collectText(x, node)); }
void lx_nodeSetContent(Ctx& x, GuestPtr node, GuestPtr content) {
    if (!node) return;
    const std::string s = content ? x.proc.readCString(content) : "";
    if (x.mem.read<uint32_t>(node + 4) == 3) {
        x.mem.write<uint32_t>(node + 40, lx_strdupStr(x, s));
        return;
    }
    x.mem.write<uint32_t>(node + 12, 0);
    x.mem.write<uint32_t>(node + 16, 0);
    lx_addChild(x, node, lx_newNodeRaw(x, 3, "text", s));
}
void lx_nodeSetName(Ctx& x, GuestPtr node, GuestPtr name) {
    if (node && name) x.mem.write<uint32_t>(node + 8, lx_strdupStr(x, x.proc.readCString(name)));
}
std::string lx_dumpNodeXml(Ctx& x, GuestPtr node) {
    if (!node) return {};
    const uint32_t type = x.mem.read<uint32_t>(node + 4);
    if (type == 9) return lx_dumpNodeXml(x, lx_docGetRoot(x, node));
    if (type == 3) {
        const uint32_t ct = x.mem.read<uint32_t>(node + 40);
        return ct ? x.proc.readCString(ct) : "";
    }
    if (type == 2) {
        const uint32_t nm = x.mem.read<uint32_t>(node + 8);
        return (nm ? x.proc.readCString(nm) : "") + "=\"" + lx_collectText(x, node) + "\"";
    }
    const uint32_t nm = x.mem.read<uint32_t>(node + 8);
    const std::string tag = nm ? x.proc.readCString(nm) : "node";
    std::string out = "<" + tag;
    for (uint32_t a = x.mem.read<uint32_t>(node + 44); a; a = x.mem.read<uint32_t>(a + 24))
        out += " " + lx_dumpNodeXml(x, a);
    uint32_t c = x.mem.read<uint32_t>(node + 12);
    if (!c) return out + "/>";
    out += ">";
    for (; c; c = x.mem.read<uint32_t>(c + 24)) out += lx_dumpNodeXml(x, c);
    return out + "</" + tag + ">";
}
GuestPtr lx_bufferCreate(Ctx& x) { return x.proc.heap().allocZeroed(16); }
void lx_bufferFree(Ctx& x, GuestPtr buf) {
    if (!buf) return;
    const uint32_t ptr = x.mem.read<uint32_t>(buf);
    if (ptr && x.proc.heap().blockSize(ptr)) x.proc.heap().free(ptr);
    if (x.proc.heap().blockSize(buf)) x.proc.heap().free(buf);
}
GuestPtr lx_bufferContent(Ctx& x, GuestPtr buf) { return buf ? x.mem.read<uint32_t>(buf) : 0; }
int32_t lx_bufferLength(Ctx& x, GuestPtr buf) { return buf ? int32_t(x.mem.read<uint32_t>(buf + 4)) : 0; }
int32_t lx_nodeDump(Ctx& x, GuestPtr buf, GuestPtr, GuestPtr cur, int32_t, int32_t) {
    if (!buf || !cur) return 0;
    const std::string s = lx_dumpNodeXml(x, cur);
    x.mem.write<uint32_t>(buf, lx_strdupStr(x, s));
    x.mem.write<uint32_t>(buf + 4, uint32_t(s.size()));
    return int32_t(s.size());
}
void lx_docDumpMemory(Ctx& x, GuestPtr cur, GuestPtr memOut, GuestPtr sizeOut) {
    const std::string s = "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n" + lx_dumpNodeXml(x, cur);
    if (memOut) x.mem.write<uint32_t>(memOut, lx_strdupStr(x, s));
    if (sizeOut) x.mem.write<int32_t>(sizeOut, int32_t(s.size()));
}
GuestPtr lx_readMemory(Ctx& x, GuestPtr buffer, int32_t size, GuestPtr, GuestPtr, int32_t) {
    if (!buffer || size <= 0) return 0;
    std::string s(reinterpret_cast<const char*>(x.mem.host(buffer)), size_t(size));
    if (s.size() >= 3 && uint8_t(s[0]) == 0xEF && uint8_t(s[1]) == 0xBB && uint8_t(s[2]) == 0xBF) s.erase(0, 3);
    const uint32_t doc = lx_newDoc(x, 0);
    std::vector<uint32_t> stack{doc};
    size_t i = 0;
    auto addText = [&](const std::string& raw) {
        if (stack.size() <= 1 || raw.empty()) return;
        bool allSpace = true;
        for (char c : raw) if (!std::isspace(uint8_t(c))) { allSpace = false; break; }
        if (allSpace && x.mem.read<uint32_t>(stack.back() + 12) != 0) return;
        const std::string dec = decodeEntities(raw);
        uint32_t last = x.mem.read<uint32_t>(stack.back() + 16);
        if (last && x.mem.read<uint32_t>(last + 4) == 3) {
            const uint32_t oldCt = x.mem.read<uint32_t>(last + 40);
            const std::string combined = (oldCt ? x.proc.readCString(oldCt) : "") + dec;
            x.mem.write<uint32_t>(last + 40, lx_strdupStr(x, combined));
        } else {
            lx_addChild(x, stack.back(), lx_newNodeRaw(x, 3, "text", dec));
        }
    };
    while (i < s.size()) {
        const size_t lt = s.find('<', i);
        if (lt == std::string::npos) { addText(s.substr(i)); break; }
        if (lt > i) addText(s.substr(i, lt - i));
        if (s.compare(lt, 4, "<!--") == 0) {
            const size_t e = s.find("-->", lt + 4);
            if (e == std::string::npos) break;
            i = e + 3;
        } else if (s.compare(lt, 9, "<![CDATA[") == 0) {
            const size_t e = s.find("]]>", lt + 9);
            if (e == std::string::npos) break;
            if (stack.size() > 1) lx_addChild(x, stack.back(), lx_newNodeRaw(x, 3, "text", s.substr(lt + 9, e - lt - 9)));
            i = e + 3;
        } else if (s.compare(lt, 2, "<?") == 0) {
            const size_t e = s.find("?>", lt + 2);
            if (e == std::string::npos) break;
            i = e + 2;
        } else if (s.compare(lt, 2, "<!") == 0) {
            int depth = 0;
            size_t k = lt + 2;
            for (; k < s.size(); ++k) {
                if (s[k] == '[') ++depth;
                else if (s[k] == ']') --depth;
                else if (s[k] == '>' && depth <= 0) break;
            }
            i = k + 1;
        } else if (s.compare(lt, 2, "</") == 0) {
            const size_t e = s.find('>', lt + 2);
            if (e == std::string::npos) break;
            if (stack.size() > 1) stack.pop_back();
            i = e + 1;
        } else {
            size_t k = lt + 1;
            while (k < s.size() && !std::isspace(uint8_t(s[k])) && s[k] != '>' && s[k] != '/') ++k;
            std::string rawTag = s.substr(lt + 1, k - lt - 1);
            const size_t colon = rawTag.find(':');
            const std::string localTag = colon == std::string::npos ? rawTag : rawTag.substr(colon + 1);
            const uint32_t elem = lx_newNodeRaw(x, 1, localTag);
            lx_addChild(x, stack.back(), elem);
            bool selfClose = false;
            for (;;) {
                while (k < s.size() && std::isspace(uint8_t(s[k]))) ++k;
                if (k >= s.size()) break;
                if (s[k] == '>') { ++k; break; }
                if (s[k] == '/' && k + 1 < s.size() && s[k + 1] == '>') { selfClose = true; k += 2; break; }
                const size_t a = k;
                while (k < s.size() && s[k] != '=' && !std::isspace(uint8_t(s[k])) && s[k] != '>' && s[k] != '/') ++k;
                std::string an = s.substr(a, k - a);
                while (k < s.size() && std::isspace(uint8_t(s[k]))) ++k;
                std::string av;
                if (k < s.size() && s[k] == '=') {
                    ++k;
                    while (k < s.size() && std::isspace(uint8_t(s[k]))) ++k;
                    if (k < s.size() && (s[k] == '"' || s[k] == '\'')) {
                        const char q = s[k];
                        const size_t e = s.find(q, k + 1);
                        if (e == std::string::npos) break;
                        av = decodeEntities(s.substr(k + 1, e - k - 1));
                        k = e + 1;
                    }
                }
                if (an == "xmlns" || an.compare(0, 6, "xmlns:") == 0) {
                    const std::string pf = an.size() > 6 ? an.substr(6) : "";
                    lx_newNs(x, elem, lx_strdupStr(x, av), pf.empty() ? 0 : lx_strdupStr(x, pf));
                } else if (!an.empty()) {
                    const size_t ac = an.find(':');
                    const std::string lan = ac == std::string::npos ? an : an.substr(ac + 1);
                    const uint32_t attr = lx_newNodeRaw(x, 2, lan);
                    lx_addChild(x, attr, lx_newNodeRaw(x, 3, "text", av));
                    lx_addChild(x, elem, attr);
                }
            }
            if (!selfClose) stack.push_back(elem);
            i = k;
        }
    }
    return doc;
}
int32_t lx_strEqual(Ctx& x, GuestPtr a, GuestPtr b) {
    if (a == b) return 1;
    if (!a || !b) return 0;
    return x.proc.readCString(a) == x.proc.readCString(b) ? 1 : 0;
}
int32_t lx_strcmp(Ctx& x, GuestPtr a, GuestPtr b) {
    if (a == b) return 0;
    if (!a) return -1;
    if (!b) return 1;
    const int r = x.proc.readCString(a).compare(x.proc.readCString(b));
    return r < 0 ? -1 : r > 0 ? 1 : 0;
}
GuestPtr lx_strdup(Ctx& x, GuestPtr cur) { return cur ? lx_strdupStr(x, x.proc.readCString(cur)) : 0; }
int32_t lx_strlen(Ctx& x, GuestPtr str) { return str ? int32_t(x.proc.readCString(str).size()) : 0; }
GuestPtr lx_strsub(Ctx& x, GuestPtr str, int32_t start, int32_t len) {
    if (!str || start < 0 || len < 0) return 0;
    const std::string s = x.proc.readCString(str);
    if (size_t(start) >= s.size()) return lx_strdupStr(x, "");
    return lx_strdupStr(x, s.substr(size_t(start), size_t(len)));
}
GuestPtr lx_xpathNewCtx(Ctx& x, GuestPtr doc) {
    const uint32_t ctx = x.proc.heap().allocZeroed(16);
    x.mem.write<uint32_t>(ctx, doc);
    x.mem.write<uint32_t>(ctx + 4, doc);
    return ctx;
}
int32_t lx_xpathRegNs(Ctx&, GuestPtr, GuestPtr, GuestPtr) { return 0; }
void lx_matchTagRec(Ctx& x, uint32_t node, const std::string& want, std::vector<uint32_t>& out, bool recursive) {
    for (uint32_t c = x.mem.read<uint32_t>(node + 12); c; c = x.mem.read<uint32_t>(c + 24)) {
        if (x.mem.read<uint32_t>(c + 4) != 1) continue;
        const uint32_t nm = x.mem.read<uint32_t>(c + 8);
        const std::string have = nm ? x.proc.readCString(nm) : "";
        if (want == "*" || have == want) out.push_back(c);
        if (recursive) lx_matchTagRec(x, c, want, out, true);
    }
}
GuestPtr lx_xpathEval(Ctx& x, GuestPtr exprPtr, GuestPtr ctx) {
    if (!exprPtr || !ctx) return 0;
    std::string expr = x.proc.readCString(exprPtr);
    const uint32_t doc = x.mem.read<uint32_t>(ctx);
    uint32_t startNode = x.mem.read<uint32_t>(ctx + 4);
    if (!startNode) startNode = doc;
    std::vector<uint32_t> current;
    bool nextRecursive = false;
    if (expr.compare(0, 2, "//") == 0) {
        current.push_back(doc ? doc : startNode);
        nextRecursive = true;
        expr.erase(0, 2);
    } else if (expr.compare(0, 2, "./") == 0) {
        current.push_back(startNode);
        expr.erase(0, 2);
    } else if (!expr.empty() && expr[0] == '/') {
        current.push_back(doc ? doc : startNode);
        expr.erase(0, 1);
    } else {
        current.push_back(startNode);
    }
    size_t pos = 0;
    while (pos <= expr.size()) {
        size_t slash = expr.find('/', pos);
        if (slash == std::string::npos) slash = expr.size();
        std::string step = expr.substr(pos, slash - pos);
        if (!step.empty() && step != ".") {
            const size_t colon = step.find(':');
            if (colon != std::string::npos) step = step.substr(colon + 1);
            const size_t bracket = step.find('[');
            if (bracket != std::string::npos) step = step.substr(0, bracket);
            std::vector<uint32_t> next;
            if (!step.empty() && step[0] == '@') {
                const std::string attrName = step.substr(1);
                for (uint32_t n : current)
                    for (uint32_t a = x.mem.read<uint32_t>(n + 44); a; a = x.mem.read<uint32_t>(a + 24)) {
                        const uint32_t nm = x.mem.read<uint32_t>(a + 8);
                        if (attrName == "*" || (nm && x.proc.readCString(nm) == attrName)) next.push_back(a);
                    }
            } else if (step == "..") {
                for (uint32_t n : current) if (uint32_t p = x.mem.read<uint32_t>(n + 20)) next.push_back(p);
            } else {
                for (uint32_t n : current) lx_matchTagRec(x, n, step, next, nextRecursive);
            }
            current = std::move(next);
            nextRecursive = false;
        }
        if (slash + 1 < expr.size() && expr[slash + 1] == '/') {
            nextRecursive = true;
            pos = slash + 2;
        } else {
            pos = slash + 1;
        }
    }
    const uint32_t nodeSet = x.proc.heap().allocZeroed(12);
    const uint32_t tab = x.proc.heap().allocZeroed(uint32_t(current.size() * 4 + 4));
    for (size_t i = 0; i < current.size(); ++i) x.mem.write<uint32_t>(tab + uint32_t(i * 4), current[i]);
    x.mem.write<int32_t>(nodeSet + 0, int32_t(current.size()));
    x.mem.write<int32_t>(nodeSet + 4, int32_t(current.size()));
    x.mem.write<uint32_t>(nodeSet + 8, tab);
    const uint32_t obj = x.proc.heap().allocZeroed(16);
    x.mem.write<uint32_t>(obj + 0, 1 /* XPATH_NODESET */);
    x.mem.write<uint32_t>(obj + 4, nodeSet);
    return obj;
}

// -------------------------------------------- NSURLResponse / NSHTTPURLResponse ---
struct Response : ObjC::HostObject { id url = 0, mime = 0, encoding = 0, headers = 0; int64_t length = -1; int32_t status = 0; };
Response& RS(Ctx& x, id self) { return R(x).host<Response>(self); }
id resp_init(Ctx& x, id self, SEL, id url, id mime, int32_t length, id enc) {
    Response& r = RS(x, self);
    setField(x, r.url, url); setField(x, r.mime, mime); setField(x, r.encoding, enc);
    r.length = length;
    return self;
}
id http_init(Ctx& x, id self, SEL, id url, int32_t status, id, id headers) {
    Response& r = RS(x, self);
    setField(x, r.url, url);
    r.status = status;
    r.headers = headers ? id(R(x).send(x.cpu, headers, "copy")) : makeDict(x, {}, true);
    if (const id ct = headers ? dictGet(x, headers, "Content-Type") : 0) setField(x, r.mime, ct);
    return self;
}
id resp_url(Ctx& x, id self, SEL) { return RS(x, self).url; }
id resp_mime(Ctx& x, id self, SEL) { return RS(x, self).mime; }
id resp_encoding(Ctx& x, id self, SEL) { return RS(x, self).encoding; }
int64_t resp_length(Ctx& x, id self, SEL) { return RS(x, self).length; }
id resp_filename(Ctx& x, id self, SEL) {
    const id url = RS(x, self).url;
    const id last = url ? id(R(x).send(x.cpu, url, "lastPathComponent")) : 0;
    return last ? last : makeString(x, "Unknown");
}
int32_t http_status(Ctx& x, id self, SEL) { return RS(x, self).status; }
id http_headers(Ctx& x, id self, SEL) { Response& r = RS(x, self); return r.headers ? r.headers : makeDict(x, {}); }
id httpc_statusString(Ctx& x, id, SEL, int32_t code) {
    static const std::pair<int, const char*> names[] = {{200, "no error"}, {201, "created"}, {204, "no content"}, {301, "moved permanently"},
        {302, "found"}, {304, "not modified"}, {400, "bad request"}, {401, "unauthorized"}, {403, "forbidden"}, {404, "not found"},
        {500, "internal server error"}, {502, "bad gateway"}, {503, "service unavailable"}};
    for (auto& [c, n] : names) if (c == code) return makeString(x, n);
    return makeString(x, code >= 500 ? "server error" : code >= 400 ? "client error" : code >= 300 ? "redirected" : "success");
}
void resp_dealloc(Ctx& x, id self, SEL) {
    if (Response* r = R(x).hostIf<Response>(self)) for (id o : {r->url, r->mime, r->encoding, r->headers}) if (o) R(x).release(x.cpu, o);
    R(x).disposeInstance(x.cpu, self);
}

// ----------------------------------------- NSHTTPCookie / NSHTTPCookieStorage ---
struct Cookie : ObjC::HostObject { id props = 0; };
id cookie_prop(Ctx& x, id self, const char* key) { Cookie* c = R(x).hostIf<Cookie>(self); return c && c->props ? dictGet(x, c->props, key) : 0; }
id cookiec_with(Ctx& x, id cls, SEL, id props) {
    if (!props || !dictGet(x, props, "Name") || !dictGet(x, props, "Value")) return 0;
    const id o = R(x).createInstance(cls);
    R(x).host<Cookie>(o).props = id(R(x).send(x.cpu, props, "copy"));
    return autoreleased(x, o);
}
id cookie_init(Ctx& x, id self, SEL, id props) {
    if (!props || !dictGet(x, props, "Name") || !dictGet(x, props, "Value")) { R(x).release(x.cpu, self); return 0; }
    R(x).host<Cookie>(self).props = id(R(x).send(x.cpu, props, "copy"));
    return self;
}
id cookie_name(Ctx& x, id self, SEL) { return cookie_prop(x, self, "Name"); }
id cookie_value(Ctx& x, id self, SEL) { return cookie_prop(x, self, "Value"); }
id cookie_domain(Ctx& x, id self, SEL) { return cookie_prop(x, self, "Domain"); }
id cookie_path(Ctx& x, id self, SEL) { const id p = cookie_prop(x, self, "Path"); return p ? p : makeString(x, "/"); }
id cookie_expires(Ctx& x, id self, SEL) { return cookie_prop(x, self, "Expires"); }
bool cookie_secure(Ctx& x, id self, SEL) { return cookie_prop(x, self, "Secure") != 0; }
bool cookie_httpOnly(Ctx& x, id self, SEL) { return cookie_prop(x, self, "HttpOnly") != 0; }
bool cookie_session(Ctx& x, id self, SEL) { return cookie_prop(x, self, "Expires") == 0; }
id cookie_props(Ctx& x, id self, SEL) { Cookie* c = R(x).hostIf<Cookie>(self); return c ? c->props : 0; }
void cookie_dealloc(Ctx& x, id self, SEL) {
    if (Cookie* c = R(x).hostIf<Cookie>(self)) if (c->props) R(x).release(x.cpu, c->props);
    R(x).disposeInstance(x.cpu, self);
}
id cookiec_fromHeaders(Ctx& x, id, SEL, id, id) { return makeArray(x, {}); }
id cookiec_requestHeaders(Ctx& x, id, SEL, id cookies) {
    std::string h;
    for (id c : cookies ? arrayItems(x, cookies) : std::vector<id>()) {
        const id n = cookie_prop(x, c, "Name"), v = cookie_prop(x, c, "Value");
        if (!n || !v) continue;
        if (!h.empty()) h += "; ";
        h += utf8(x, n) + "=" + utf8(x, v);
    }
    return h.empty() ? makeDict(x, {}) : makeDict(x, {{constString(x.proc, "Cookie"), makeString(x, h)}});
}

struct CookieJar : ObjC::HostObject { id cookies = 0; uint32_t policy = 2; };   // NSHTTPCookieAcceptPolicyOnlyFromMainDocumentDomain
CookieJar& CJ(Ctx& x, id self) { return R(x).host<CookieJar>(self); }
id jar_array(Ctx& x, id self) {
    CookieJar& j = CJ(x, self);
    if (!j.cookies) j.cookies = makeArray(x, {}, true, true);
    return j.cookies;
}
id jarc_shared(Ctx& x, id cls, SEL) {
    auto& s = ES(x);
    if (!s.sharedCookies) { s.sharedCookies = R(x).createInstance(cls); R(x).makeImmortal(s.sharedCookies); }
    return s.sharedCookies;
}
id jar_cookies(Ctx& x, id self, SEL) { return id(R(x).send(x.cpu, jar_array(x, self), "copy")); }
void jar_setCookie(Ctx& x, id self, SEL, id c) {
    if (!c) return;
    const id arr = jar_array(x, self);
    const std::string name = utf8(x, cookie_prop(x, c, "Name")), dom = cookie_prop(x, c, "Domain") ? utf8(x, cookie_prop(x, c, "Domain")) : "";
    std::vector<id> keep;
    for (id o : arrayItems(x, arr)) {
        const bool same = utf8(x, cookie_prop(x, o, "Name")) == name && (cookie_prop(x, o, "Domain") ? utf8(x, cookie_prop(x, o, "Domain")) : "") == dom;
        if (!same) keep.push_back(o);
    }
    keep.push_back(c);
    R(x).send(x.cpu, arr, "setArray:", {makeArray(x, keep)});
}
void jar_delete(Ctx& x, id self, SEL, id c) { if (c) R(x).send(x.cpu, jar_array(x, self), "removeObject:", {c}); }
id jar_forURL(Ctx& x, id self, SEL, id url) {
    const id hostObj = url ? id(R(x).send(x.cpu, url, "host")) : 0;
    const std::string host = hostObj ? utf8(x, hostObj) : "";
    std::vector<id> out;
    for (id c : arrayItems(x, jar_array(x, self))) {
        const id d = cookie_prop(x, c, "Domain");
        std::string dom = d ? utf8(x, d) : "";
        if (!dom.empty() && dom[0] == '.') dom.erase(0, 1);
        if (dom.empty() || host == dom || (host.size() > dom.size() && host.compare(host.size() - dom.size(), dom.size(), dom) == 0 && host[host.size() - dom.size() - 1] == '.'))
            out.push_back(c);
    }
    return makeArray(x, out);
}
void jar_setCookiesForURL(Ctx& x, id self, SEL, id cookies, id, id) { for (id c : cookies ? arrayItems(x, cookies) : std::vector<id>()) jar_setCookie(x, self, 0, c); }
uint32_t jar_policy(Ctx& x, id self, SEL) { return CJ(x, self).policy; }
void jar_setPolicy(Ctx& x, id self, SEL, uint32_t p) { CJ(x, self).policy = p; }

// ---------------------------------------------------------- NSURLProtocol ---
bool proto_yes(Ctx&, id, SEL, id) { return true; }
bool proto_no(Ctx&, id, SEL, id) { return false; }
void proto_ignore(Ctx&, id, SEL, id) {}
id proto_nil2(Ctx&, id, SEL, id, id) { return 0; }
void proto_ignore3(Ctx&, id, SEL, id, id, id) {}

// ---------------------------------------------------------------- NSCache ---
struct CacheData : ObjC::HostObject { id dict = 0, name = 0, delegate = 0; uint32_t countLimit = 0, costLimit = 0; std::vector<id> order; };
CacheData& CD(Ctx& x, id self) {
    CacheData& c = R(x).host<CacheData>(self);
    if (!c.dict) c.dict = makeDict(x, {}, true, true);
    return c;
}
id cache_object(Ctx& x, id self, SEL, id key) { return key ? dictGet(x, CD(x, self).dict, key) : 0; }
void cache_set(Ctx& x, id self, SEL, id obj, id key) {
    if (!key) return;
    CacheData& c = CD(x, self);
    if (!obj) { R(x).send(x.cpu, c.dict, "removeObjectForKey:", {key}); return; }
    R(x).send(x.cpu, c.dict, "setObject:forKey:", {obj, key});
    if (c.countLimit) {
        const uint32_t n = uint32_t(R(x).send(x.cpu, c.dict, "count"));
        if (n > c.countLimit) {
            // Evict an arbitrary entry other than the one just added.
            for (id k : arrayItems(x, id(R(x).send(x.cpu, c.dict, "allKeys")))) {
                if (k != key) { R(x).send(x.cpu, c.dict, "removeObjectForKey:", {k}); break; }
            }
        }
    }
}
void cache_setCost(Ctx& x, id self, SEL s, id obj, id key, uint32_t) { cache_set(x, self, s, obj, key); }
void cache_remove(Ctx& x, id self, SEL, id key) { if (key) R(x).send(x.cpu, CD(x, self).dict, "removeObjectForKey:", {key}); }
void cache_removeAll(Ctx& x, id self, SEL) { R(x).send(x.cpu, CD(x, self).dict, "removeAllObjects"); }
uint32_t cache_countLimit(Ctx& x, id self, SEL) { return CD(x, self).countLimit; }
void cache_setCountLimit(Ctx& x, id self, SEL, uint32_t n) { CD(x, self).countLimit = n; }
uint32_t cache_costLimit(Ctx& x, id self, SEL) { return CD(x, self).costLimit; }
void cache_setCostLimit(Ctx& x, id self, SEL, uint32_t n) { CD(x, self).costLimit = n; }
id cache_name(Ctx& x, id self, SEL) { CacheData& c = CD(x, self); return c.name ? c.name : makeString(x, ""); }
void cache_setName(Ctx& x, id self, SEL, id n) { setField(x, CD(x, self).name, n); }
id cache_delegate(Ctx& x, id self, SEL) { return CD(x, self).delegate; }
void cache_setDelegate(Ctx& x, id self, SEL, id d) { CD(x, self).delegate = d; }
void cache_ignoreBool(Ctx&, id, SEL, bool) {}
bool cache_yes(Ctx&, id, SEL) { return true; }
void cache_dealloc(Ctx& x, id self, SEL) {
    if (CacheData* c = R(x).hostIf<CacheData>(self)) for (id o : {c->dict, c->name}) if (o) R(x).release(x.cpu, o);
    R(x).disposeInstance(x.cpu, self);
}

// ------------------------------------------------------- NSDecimalNumber ---
// Backed by double: exact enough for prices and scores, not for accounting.
double numArg(Ctx& x, id n) { return n ? numberDouble(x, n) : 0.0; }
id dec_fromString(Ctx& x, id, SEL, id s) {
    if (!s) return makeNumberDouble(x, NAN);
    const std::string v = utf8(x, s);
    char* end = nullptr;
    const double d = std::strtod(v.c_str(), &end);
    return makeNumberDouble(x, end == v.c_str() ? NAN : d);
}
id dec_fromStringLocale(Ctx& x, id c, SEL s, id str, id) { return dec_fromString(x, c, s, str); }
// +decimalNumberWithMantissa:(unsigned long long) exponent:(short) isNegative:(BOOL) — mantissa in r2:r3.
void dec_mantissa(Ctx& x) {
    const uint64_t m = (uint64_t(x.cpu.r[3]) << 32) | x.cpu.r[2];
    const uint32_t sp = x.cpu.r[CPU::kSP];
    const int16_t e = int16_t(x.mem.read<uint32_t>(sp));
    const bool neg = (x.mem.read<uint32_t>(sp + 4) & 0xFF) != 0;
    double v = double(m) * std::pow(10.0, double(e));
    x.cpu.r[0] = makeNumberDouble(x, neg ? -v : v);
}
id dec_zero(Ctx& x, id, SEL) { return makeNumberDouble(x, 0); }
id dec_one(Ctx& x, id, SEL) { return makeNumberDouble(x, 1); }
id dec_nan(Ctx& x, id, SEL) { return makeNumberDouble(x, NAN); }
id dec_max(Ctx& x, id, SEL) { return makeNumberDouble(x, 1e38); }
id dec_min(Ctx& x, id, SEL) { return makeNumberDouble(x, -1e38); }
id dec_add(Ctx& x, id self, SEL, id o) { return makeNumberDouble(x, numArg(x, self) + numArg(x, o)); }
id dec_sub(Ctx& x, id self, SEL, id o) { return makeNumberDouble(x, numArg(x, self) - numArg(x, o)); }
id dec_mul(Ctx& x, id self, SEL, id o) { return makeNumberDouble(x, numArg(x, self) * numArg(x, o)); }
id dec_div(Ctx& x, id self, SEL, id o) {
    const double b = numArg(x, o);
    if (b == 0) throw std::runtime_error("NSDecimalNumberDivideByZeroException: NSDecimalNumber divide by zero exception");
    return makeNumberDouble(x, numArg(x, self) / b);
}
id dec_add2(Ctx& x, id self, SEL s, id o, id) { return dec_add(x, self, s, o); }
id dec_sub2(Ctx& x, id self, SEL s, id o, id) { return dec_sub(x, self, s, o); }
id dec_mul2(Ctx& x, id self, SEL s, id o, id) { return dec_mul(x, self, s, o); }
id dec_div2(Ctx& x, id self, SEL s, id o, id) { return dec_div(x, self, s, o); }
id dec_pow(Ctx& x, id self, SEL, uint32_t p) { return makeNumberDouble(x, std::pow(numArg(x, self), double(p))); }
id dec_mulPow10(Ctx& x, id self, SEL, int32_t p) { return makeNumberDouble(x, numArg(x, self) * std::pow(10.0, double(int16_t(p)))); }
id dec_round(Ctx& x, id self, SEL, id behavior) {
    // NSDecimalNumberHandler: scale (short) and rounding mode; default: plain rounding to `scale` places.
    int scale = 0;
    if (behavior && R(x).respondsTo(behavior, R(x).sel("scale"))) scale = int16_t(R(x).send(x.cpu, behavior, "scale"));
    const double f = std::pow(10.0, scale);
    return makeNumberDouble(x, std::round(numArg(x, self) * f) / f);
}
id dec_self(Ctx&, id self, SEL) { return self; }
id dech_default(Ctx& x, id cls, SEL) { return autoreleased(x, R(x).createInstance(cls)); }
id dech_init(Ctx&, id self, SEL, uint32_t, int32_t, bool, bool, bool, bool) { return self; }
int16_t dech_scale(Ctx&, id, SEL) { return 2; }

} // namespace

void registerFoundationExtras(Process& p, ObjC::Runtime& r) {
    ExtrasState& s = p.service<ExtrasState>();
    ObjC::ClassBuilder sc = r.defineClass("NSScanner", "NSObject");
    s.scanner = sc.cls();
    sc.cm<&scc_with>("scannerWithString:").cm<&scc_with>("localizedScannerWithString:")
      .m<&sc_init>("initWithString:").m<&sc_string>("string")
      .m<&sc_location>("scanLocation", "I@:").m<&sc_setLocation>("setScanLocation:", "v@:I").m<&sc_atEnd>("isAtEnd", "c@:")
      .m<&sc_skip>("charactersToBeSkipped").m<&sc_setSkip>("setCharactersToBeSkipped:", "v@:@")
      .m<&sc_caseSensitive>("caseSensitive", "c@:").m<&sc_setCaseSensitive>("setCaseSensitive:", "v@:c")
      .m<&sc_scanInt>("scanInt:", "c@:^i").m<&sc_scanInt>("scanInteger:", "c@:^i").m<&sc_scanLongLong>("scanLongLong:", "c@:^q")
      .m<&sc_scanHexInt>("scanHexInt:", "c@:^I").m<&sc_scanHexLongLong>("scanHexLongLong:", "c@:^Q")
      .m<&sc_scanFloat>("scanFloat:", "c@:^f").m<&sc_scanDouble>("scanDouble:", "c@:^d")
      .m<&sc_scanString>("scanString:intoString:", "c@:@^@").m<&sc_scanUpToString>("scanUpToString:intoString:", "c@:@^@")
      .m<&sc_scanCharacters>("scanCharactersFromSet:intoString:", "c@:@^@")
      .m<&sc_scanUpToCharacters>("scanUpToCharactersFromSet:intoString:", "c@:@^@")
      .m<&sc_dealloc>("dealloc", "v@:");

    ObjC::ClassBuilder xp = r.defineClass("NSXMLParser", "NSObject");
    s.xmlParser = xp.cls();
    xp.m<&xml_initWithData>("initWithData:").m<&xml_initWithURL>("initWithContentsOfURL:").m<&xml_initWithStream>("initWithStream:")
      .m<&xml_delegate>("delegate").m<&xml_setDelegate>("setDelegate:", "v@:@")
      .m<&xml_setNamespaces>("setShouldProcessNamespaces:", "v@:c").m<&xml_ignoreBool>("setShouldReportNamespacePrefixes:", "v@:c")
      .m<&xml_ignoreBool>("setShouldResolveExternalEntities:", "v@:c")
      .m<&xml_parse>("parse", "c@:").m<&xml_abort>("abortParsing", "v@:").m<&xml_error>("parserError")
      .m<&xml_line>("lineNumber", "i@:").m<&xml_column>("columnNumber", "i@:").m<&xml_dealloc>("dealloc", "v@:");
    p.mem().write<uint32_t>(p.defineDataBlock("_NSXMLParserErrorDomain", 4), constString(p, "NSXMLParserErrorDomain"));

    ObjC::ClassBuilder ur = r.defineClass("NSURLResponse", "NSObject");
    s.urlResponse = ur.cls();
    ur.m<&resp_init>("initWithURL:MIMEType:expectedContentLength:textEncodingName:", "@@:@@i@")
      .m<&resp_url>("URL").m<&resp_mime>("MIMEType").m<&resp_encoding>("textEncodingName")
      .m<&resp_length>("expectedContentLength", "q@:").m<&resp_filename>("suggestedFilename").m<&resp_dealloc>("dealloc", "v@:");
    ObjC::ClassBuilder hr = r.defineClass("NSHTTPURLResponse", "NSURLResponse");
    s.httpResponse = hr.cls();
    hr.m<&http_init>("initWithURL:statusCode:HTTPVersion:headerFields:", "@@:@i@@").m<&http_status>("statusCode", "i@:")
      .m<&http_headers>("allHeaderFields").cm<&httpc_statusString>("localizedStringForStatusCode:", "@@:i");
    r.defineClass("NSCachedURLResponse", "NSObject");

    ObjC::ClassBuilder ck = r.defineClass("NSHTTPCookie", "NSObject");
    s.cookie = ck.cls();
    ck.cm<&cookiec_with>("cookieWithProperties:").cm<&cookiec_fromHeaders>("cookiesWithResponseHeaderFields:forURL:")
      .cm<&cookiec_requestHeaders>("requestHeaderFieldsWithCookies:")
      .m<&cookie_init>("initWithProperties:").m<&cookie_name>("name").m<&cookie_value>("value").m<&cookie_domain>("domain")
      .m<&cookie_path>("path").m<&cookie_expires>("expiresDate").m<&cookie_secure>("isSecure", "c@:")
      .m<&cookie_httpOnly>("isHTTPOnly", "c@:").m<&cookie_session>("isSessionOnly", "c@:").m<&cookie_props>("properties")
      .m<&cookie_dealloc>("dealloc", "v@:");
    ObjC::ClassBuilder cs = r.defineClass("NSHTTPCookieStorage", "NSObject");
    s.cookieStorage = cs.cls();
    cs.cm<&jarc_shared>("sharedHTTPCookieStorage").m<&jar_cookies>("cookies").m<&jar_setCookie>("setCookie:", "v@:@")
      .m<&jar_delete>("deleteCookie:", "v@:@").m<&jar_forURL>("cookiesForURL:")
      .m<&jar_setCookiesForURL>("setCookies:forURL:mainDocumentURL:", "v@:@@@")
      .m<&jar_policy>("cookieAcceptPolicy", "I@:").m<&jar_setPolicy>("setCookieAcceptPolicy:", "v@:I");
    for (const char* n : {"_NSHTTPCookieComment", "_NSHTTPCookieCommentURL", "_NSHTTPCookieDiscard", "_NSHTTPCookieMaximumAge",
                          "_NSHTTPCookiePort", "_NSHTTPCookieManagerCookiesChangedNotification",
                          "_NSHTTPCookieManagerAcceptPolicyChangedNotification"})
        p.mem().write<uint32_t>(p.defineDataBlock(n, 4), constString(p, n + 1));

    ObjC::ClassBuilder pr = r.defineClass("NSURLProtocol", "NSObject");
    pr.cm<&proto_yes>("registerClass:", "c@:#").cm<&proto_ignore>("unregisterClass:", "v@:#").cm<&proto_no>("canInitWithRequest:", "c@:@")
      .cm<&proto_nil2>("propertyForKey:inRequest:").cm<&proto_ignore3>("setProperty:forKey:inRequest:", "v@:@@@")
      .cm<&proto_nil2>("removePropertyForKey:inRequest:");

    ObjC::ClassBuilder ca = r.defineClass("NSCache", "NSObject");
    s.cache = ca.cls();
    ca.m<&cache_object>("objectForKey:").m<&cache_set>("setObject:forKey:", "v@:@@").m<&cache_setCost>("setObject:forKey:cost:", "v@:@@I")
      .m<&cache_remove>("removeObjectForKey:", "v@:@").m<&cache_removeAll>("removeAllObjects", "v@:")
      .m<&cache_countLimit>("countLimit", "I@:").m<&cache_setCountLimit>("setCountLimit:", "v@:I")
      .m<&cache_costLimit>("totalCostLimit", "I@:").m<&cache_setCostLimit>("setTotalCostLimit:", "v@:I")
      .m<&cache_name>("name").m<&cache_setName>("setName:", "v@:@").m<&cache_delegate>("delegate").m<&cache_setDelegate>("setDelegate:", "v@:@")
      .m<&cache_yes>("evictsObjectsWithDiscardedContent", "c@:").m<&cache_ignoreBool>("setEvictsObjectsWithDiscardedContent:", "v@:c")
      .m<&cache_dealloc>("dealloc", "v@:");

    ObjC::ClassBuilder dn = r.defineClass("NSDecimalNumber", "NSNumber");
    dn.cm<&dec_fromString>("decimalNumberWithString:").cm<&dec_fromStringLocale>("decimalNumberWithString:locale:")
      .cm<&dec_zero>("zero").cm<&dec_one>("one").cm<&dec_nan>("notANumber").cm<&dec_max>("maximumDecimalNumber")
      .cm<&dec_min>("minimumDecimalNumber");
    dn.classMethod("decimalNumberWithMantissa:exponent:isNegative:", dec_mantissa, "@@:Qsc");
    // Arithmetic on any NSNumber (our decimal numbers are NSNumbers underneath).
    r.builder(r.getClass("NSNumber"))
        .m<&dec_add>("decimalNumberByAdding:").m<&dec_sub>("decimalNumberBySubtracting:")
        .m<&dec_mul>("decimalNumberByMultiplyingBy:").m<&dec_div>("decimalNumberByDividingBy:")
        .m<&dec_add2>("decimalNumberByAdding:withBehavior:").m<&dec_sub2>("decimalNumberBySubtracting:withBehavior:")
        .m<&dec_mul2>("decimalNumberByMultiplyingBy:withBehavior:").m<&dec_div2>("decimalNumberByDividingBy:withBehavior:")
        .m<&dec_pow>("decimalNumberByRaisingToPower:", "@@:I").m<&dec_mulPow10>("decimalNumberByMultiplyingByPowerOf10:", "@@:s")
        .m<&dec_round>("decimalNumberByRoundingAccordingToBehavior:");
    ObjC::ClassBuilder dh = r.defineClass("NSDecimalNumberHandler", "NSObject");
    dh.cm<&dech_default>("defaultDecimalNumberHandler")
      .m<&dech_init>("initWithRoundingMode:scale:raiseOnExactness:raiseOnOverflow:raiseOnUnderflow:raiseOnDivideByZero:", "@@:Isccccc")
      .m<&dech_scale>("scale", "s@:");
    for (const char* n : {"_NSDecimalNumberExactnessException", "_NSDecimalNumberOverflowException", "_NSDecimalNumberUnderflowException",
                          "_NSDecimalNumberDivideByZeroException"})
        p.mem().write<uint32_t>(p.defineDataBlock(n, 4), constString(p, n + 1));
    (void)dec_self;

    // NSRegularExpression & NSTextCheckingResult
    struct TextMatchData : ObjC::HostObject {
        id regex = 0;
        std::vector<Range> ranges;
    };
    struct RegexData : ObjC::HostObject {
        id pattern = 0;
        uint32_t options = 0;
        std::regex re;
        uint32_t groups = 0;
    };
    ObjC::ClassBuilder tcr = r.defineClass("NSTextCheckingResult", "NSObject");
    tcr.method("range", [](Ctx& x) {
        const GuestPtr sret = x.cpu.r[0];
        const id self = x.cpu.r[1];
        auto* d = R(x).hostIf<TextMatchData>(self);
        writeRange(x, sret, (d && !d->ranges.empty()) ? d->ranges[0] : Range{kNotFound, 0});
    }, "{_NSRange=II}@:");
    tcr.method("rangeAtIndex:", [](Ctx& x) {
        const GuestPtr sret = x.cpu.r[0];
        const id self = x.cpu.r[1];
        const uint32_t idx = x.cpu.r[3];
        auto* d = R(x).hostIf<TextMatchData>(self);
        writeRange(x, sret, (d && idx < d->ranges.size()) ? d->ranges[idx] : Range{kNotFound, 0});
    }, "{_NSRange=II}@:I");
    tcr.method("numberOfRanges", [](Ctx& x) {
        auto* d = R(x).hostIf<TextMatchData>(x.cpu.r[0]);
        x.cpu.r[0] = d ? uint32_t(d->ranges.size()) : 0;
    }, "I@:");
    tcr.method("resultType", [](Ctx& x) { x.cpu.r[0] = 1024; x.cpu.r[1] = 0; }, "Q@:");
    tcr.method("regularExpression", [](Ctx& x) {
        auto* d = R(x).hostIf<TextMatchData>(x.cpu.r[0]);
        x.cpu.r[0] = d ? d->regex : 0;
    }, "@@:");
    tcr.method("dealloc", [](Ctx& x) {
        const id self = x.cpu.r[0];
        auto* d = R(x).hostIf<TextMatchData>(self);
        if (d && d->regex) R(x).release(x.cpu, d->regex);
        R(x).disposeInstance(x.cpu, self);
    }, "v@:");

    ObjC::ClassBuilder rx = r.defineClass("NSRegularExpression", "NSObject");
    const Class tcrCls = tcr.cls();
    auto initRegex = [](Ctx& x, id self, id pat, uint32_t opts, GuestPtr errOut) -> id {
        if (errOut) x.mem.write<uint32_t>(errOut, 0);
        if (!pat) { R(x).release(x.cpu, self); return 0; }
        auto& d = R(x).host<RegexData>(self);
        d.pattern = id(R(x).send(x.cpu, pat, "copy"));
        d.options = opts;
        std::regex_constants::syntax_option_type flags = std::regex::ECMAScript;
        if (opts & 1 /* NSRegularExpressionCaseInsensitive */) flags |= std::regex::icase;
        try {
            d.re = std::regex(utf8(x, pat), flags);
            d.groups = uint32_t(d.re.mark_count());
        } catch (...) {
            d.groups = 0;
        }
        return self;
    };
    auto collectMatches = [tcrCls](Ctx& x, id self, id strObj, uint32_t loc, uint32_t len) -> std::vector<id> {
        std::vector<id> out;
        auto* d = R(x).hostIf<RegexData>(self);
        if (!d || !strObj) return out;
        const std::string s = utf8(x, strObj);
        const size_t start = std::min<size_t>(loc, s.size());
        const size_t end = std::min<size_t>(start + len, s.size());
        try {
            auto begin = std::sregex_iterator(s.begin() + long(start), s.begin() + long(end), d->re);
            auto finish = std::sregex_iterator();
            for (auto it = begin; it != finish; ++it) {
                const std::smatch& m = *it;
                const id res = R(x).createInstance(tcrCls);
                auto& md = R(x).host<TextMatchData>(res);
                md.regex = R(x).retain(x.cpu, self);
                for (size_t i = 0; i < m.size(); ++i) {
                    if (m[i].matched) md.ranges.push_back({uint32_t(start + size_t(m.position(i))), uint32_t(m.length(i))});
                    else md.ranges.push_back({kNotFound, 0});
                }
                out.push_back(autoreleased(x, res));
            }
        } catch (...) {}
        return out;
    };
    rx.classMethod("regularExpressionWithPattern:options:error:", [initRegex](Ctx& x) {
        const id cls = x.cpu.r[0];
        const id inst = R(x).createInstance(cls);
        const GuestPtr errOut = x.mem.read<uint32_t>(x.cpu.r[CPU::kSP]);
        const id r = initRegex(x, inst, x.cpu.r[2], x.cpu.r[3], errOut);
        x.cpu.r[0] = r ? autoreleased(x, r) : 0;
    }, "@@:@I^@");
    rx.classMethod("escapedPatternForString:", [](Ctx& x) { x.cpu.r[0] = x.cpu.r[2]; }, "@@:@");
    rx.classMethod("escapedTemplateForString:", [](Ctx& x) { x.cpu.r[0] = x.cpu.r[2]; }, "@@:@");
    rx.method("initWithPattern:options:error:", [initRegex](Ctx& x) {
        const GuestPtr errOut = x.mem.read<uint32_t>(x.cpu.r[CPU::kSP]);
        x.cpu.r[0] = initRegex(x, x.cpu.r[0], x.cpu.r[2], x.cpu.r[3], errOut);
    }, "@@:@I^@");
    // NSRegularExpression is immutable: an owning copy can share the compiled
    // expression, but must outlive the original owner's autorelease pool.
    rx.method("copyWithZone:", [](Ctx& x) { x.cpu.r[0] = retainObj(x, x.cpu.r[0]); }, "@@:^v");
    rx.method("dealloc", [](Ctx& x) {
        const id self = x.cpu.r[0];
        auto* d = R(x).hostIf<RegexData>(self);
        if (d && d->pattern) R(x).release(x.cpu, d->pattern);
        R(x).disposeInstance(x.cpu, self);
    }, "v@:");
    rx.method("pattern", [](Ctx& x) {
        auto* d = R(x).hostIf<RegexData>(x.cpu.r[0]);
        x.cpu.r[0] = d ? d->pattern : makeString(x, "");
    }, "@@:");
    rx.method("options", [](Ctx& x) {
        auto* d = R(x).hostIf<RegexData>(x.cpu.r[0]);
        x.cpu.r[0] = d ? d->options : 0;
    }, "I@:");
    rx.method("numberOfCaptureGroups", [](Ctx& x) {
        auto* d = R(x).hostIf<RegexData>(x.cpu.r[0]);
        x.cpu.r[0] = d ? d->groups : 0;
    }, "I@:");
    rx.method("matchesInString:options:range:", [collectMatches](Ctx& x) {
        const uint32_t sp = x.cpu.r[CPU::kSP];
        const auto m = collectMatches(x, x.cpu.r[0], x.cpu.r[2], x.mem.read<uint32_t>(sp), x.mem.read<uint32_t>(sp + 4));
        x.cpu.r[0] = makeArray(x, m);
    }, "@@:@I{_NSRange=II}");
    rx.method("numberOfMatchesInString:options:range:", [collectMatches](Ctx& x) {
        const uint32_t sp = x.cpu.r[CPU::kSP];
        const auto m = collectMatches(x, x.cpu.r[0], x.cpu.r[2], x.mem.read<uint32_t>(sp), x.mem.read<uint32_t>(sp + 4));
        x.cpu.r[0] = uint32_t(m.size());
    }, "I@:@I{_NSRange=II}");
    rx.method("firstMatchInString:options:range:", [collectMatches](Ctx& x) {
        const uint32_t sp = x.cpu.r[CPU::kSP];
        const auto m = collectMatches(x, x.cpu.r[0], x.cpu.r[2], x.mem.read<uint32_t>(sp), x.mem.read<uint32_t>(sp + 4));
        x.cpu.r[0] = m.empty() ? 0 : m.front();
    }, "@@:@I{_NSRange=II}");
    rx.method("rangeOfFirstMatchInString:options:range:", [collectMatches](Ctx& x) {
        const GuestPtr sret = x.cpu.r[0];
        const id self = x.cpu.r[1];
        const id strObj = x.cpu.r[3];
        const uint32_t sp = x.cpu.r[CPU::kSP];
        const auto m = collectMatches(x, self, strObj, x.mem.read<uint32_t>(sp + 4), x.mem.read<uint32_t>(sp + 8));
        if (m.empty()) writeRange(x, sret, {kNotFound, 0});
        else {
            auto* md = R(x).hostIf<TextMatchData>(m.front());
            writeRange(x, sret, (md && !md->ranges.empty()) ? md->ranges[0] : Range{kNotFound, 0});
        }
    }, "{_NSRange=II}@:@I{_NSRange=II}");
    rx.method("stringByReplacingMatchesInString:options:range:withTemplate:", [](Ctx& x) {
        const id self = x.cpu.r[0];
        const id strObj = x.cpu.r[2];
        const uint32_t sp = x.cpu.r[CPU::kSP];
        const uint32_t loc = x.mem.read<uint32_t>(sp);
        const uint32_t len = x.mem.read<uint32_t>(sp + 4);
        const id templObj = x.mem.read<uint32_t>(sp + 8);
        auto* d = R(x).hostIf<RegexData>(self);
        if (!d || !strObj) { x.cpu.r[0] = strObj; return; }
        const std::string s = utf8(x, strObj);
        const std::string t = templObj ? utf8(x, templObj) : "";
        const size_t start = std::min<size_t>(loc, s.size());
        const size_t end = std::min<size_t>(start + len, s.size());
        try {
            const std::string mid = std::regex_replace(s.substr(start, end - start), d->re, t);
            x.cpu.r[0] = makeString(x, s.substr(0, start) + mid + s.substr(end));
        } catch (...) {
            x.cpu.r[0] = strObj;
        }
    }, "@@:@I{_NSRange=II}@");
    rx.method("replaceMatchesInString:options:range:withTemplate:", [](Ctx& x) {
        const id self = x.cpu.r[0];
        const id mstr = x.cpu.r[2];
        const uint32_t sp = x.cpu.r[CPU::kSP];
        const uint32_t loc = x.mem.read<uint32_t>(sp);
        const uint32_t len = x.mem.read<uint32_t>(sp + 4);
        const id templObj = x.mem.read<uint32_t>(sp + 8);
        const id rep = id(R(x).send(x.cpu, self, "stringByReplacingMatchesInString:options:range:withTemplate:", {mstr, x.cpu.r[3], loc, len, templObj}));
        if (rep && mstr) R(x).send(x.cpu, mstr, "setString:", {rep});
        x.cpu.r[0] = 1;
    }, "I@:@I{_NSRange=II}@");

    // libxml2 C API
    p.def<&lx_initParser>("_xmlInitParser");
    p.def<&lx_initParser>("_xmlCleanupParser");
    const uint32_t xmlFreeFn = p.def<&lx_free>("_xmlFree");
    p.mem().write<uint32_t>(p.defineDataBlock("_xmlFree", 4), xmlFreeFn);
    p.def<&lx_free>("_xmlFreeDoc");
    p.def<&lx_free>("_xmlFreeNode");
    p.def<&lx_free>("_xmlFreeNs");
    p.def<&lx_free>("_xmlFreeNsList");
    p.def<&lx_newDoc>("_xmlNewDoc");
    p.def<&lx_newNode>("_xmlNewNode");
    p.def<&lx_newText>("_xmlNewText");
    p.def<&lx_newProp>("_xmlNewProp");
    p.def<&lx_newNs>("_xmlNewNs");
    p.def<&lx_newNsProp>("_xmlNewNsProp");
    p.def<&lx_addChild>("_xmlAddChild");
    p.def<&lx_docGetRoot>("_xmlDocGetRootElement");
    p.def<&lx_docSetRoot>("_xmlDocSetRootElement");
    p.def<&lx_unlinkNode>("_xmlUnlinkNode");
    p.def<&lx_copyNode>("_xmlCopyNode");
    p.def<&lx_setTreeDoc>("_xmlSetTreeDoc");
    p.def<&lx_setNs>("_xmlSetNs");
    p.def<&lx_searchNs>("_xmlSearchNs");
    p.def<&lx_searchNsByHref>("_xmlSearchNsByHref");
    p.def<&lx_hasProp>("_xmlHasProp");
    p.def<&lx_hasNsProp>("_xmlHasNsProp");
    p.def<&lx_nodeGetContent>("_xmlNodeGetContent");
    p.def<&lx_nodeSetContent>("_xmlNodeSetContent");
    p.def<&lx_nodeSetName>("_xmlNodeSetName");
    p.def<&lx_bufferCreate>("_xmlBufferCreate");
    p.def<&lx_bufferFree>("_xmlBufferFree");
    p.def<&lx_bufferContent>("_xmlBufferContent");
    p.def<&lx_bufferLength>("_xmlBufferLength");
    p.def<&lx_nodeDump>("_xmlNodeDump");
    p.def<&lx_docDumpMemory>("_xmlDocDumpMemory");
    p.def<&lx_readMemory>("_xmlReadMemory");
    p.def<&lx_strEqual>("_xmlStrEqual");
    p.def<&lx_strcmp>("_xmlStrcmp");
    p.def<&lx_strdup>("_xmlStrdup");
    p.def<&lx_strlen>("_xmlStrlen");
    p.def<&lx_strsub>("_xmlStrsub");
    p.def<&lx_xpathNewCtx>("_xmlXPathNewContext");
    p.def<&lx_free>("_xmlXPathFreeContext");
    p.def<&lx_xpathRegNs>("_xmlXPathRegisterNs");
    p.def<&lx_xpathEval>("_xmlXPathEval");
    p.def<&lx_xpathEval>("_xmlXPathEvalExpression");
    p.def<&lx_free>("_xmlXPathFreeObject");
}

} // namespace Hydrogen::Runtime::Foundation
