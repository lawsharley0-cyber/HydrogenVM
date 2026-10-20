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
}

} // namespace Hydrogen::Runtime::Foundation
