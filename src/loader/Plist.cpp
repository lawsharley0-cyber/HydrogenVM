#include "Plist.hpp"
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>

namespace Hydrogen::Plist {

// ------------------------------------------------------------------ UTF ---

std::string utf16ToUtf8(const char16_t* s, size_t n) {
    std::string out;
    out.reserve(n);
    for (size_t i = 0; i < n; ++i) {
        uint32_t c = s[i];
        if (c >= 0xD800 && c < 0xDC00 && i + 1 < n && s[i + 1] >= 0xDC00 && s[i + 1] < 0xE000) {
            c = 0x10000 + ((c - 0xD800) << 10) + (s[i + 1] - 0xDC00);
            ++i;
        } else if (c >= 0xD800 && c < 0xE000) {
            c = 0xFFFD;
        }
        if (c < 0x80) out += char(c);
        else if (c < 0x800) { out += char(0xC0 | (c >> 6)); out += char(0x80 | (c & 0x3F)); }
        else if (c < 0x10000) { out += char(0xE0 | (c >> 12)); out += char(0x80 | ((c >> 6) & 0x3F)); out += char(0x80 | (c & 0x3F)); }
        else { out += char(0xF0 | (c >> 18)); out += char(0x80 | ((c >> 12) & 0x3F)); out += char(0x80 | ((c >> 6) & 0x3F)); out += char(0x80 | (c & 0x3F)); }
    }
    return out;
}

std::u16string utf8ToUtf16(const std::string& s) {
    std::u16string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size();) {
        const uint8_t b = uint8_t(s[i]);
        uint32_t c;
        size_t len;
        if (b < 0x80) { c = b; len = 1; }
        else if ((b & 0xE0) == 0xC0) { c = b & 0x1F; len = 2; }
        else if ((b & 0xF0) == 0xE0) { c = b & 0x0F; len = 3; }
        else if ((b & 0xF8) == 0xF0) { c = b & 0x07; len = 4; }
        else { out += u'�'; ++i; continue; }
        if (i + len > s.size()) { out += u'�'; break; }
        bool ok = true;
        for (size_t k = 1; k < len; ++k) {
            const uint8_t cb = uint8_t(s[i + k]);
            if ((cb & 0xC0) != 0x80) { ok = false; break; }
            c = (c << 6) | (cb & 0x3F);
        }
        if (!ok) { out += u'�'; ++i; continue; }
        i += len;
        if (c >= 0x10000) {
            c -= 0x10000;
            out += char16_t(0xD800 + (c >> 10));
            out += char16_t(0xDC00 + (c & 0x3FF));
        } else {
            out += char16_t(c);
        }
    }
    return out;
}

namespace {

// --------------------------------------------------------------- binary ---

struct Binary {
    const std::vector<uint8_t>& d;
    uint8_t offSize = 0, refSize = 0;
    uint64_t numObjects = 0, top = 0, tableOff = 0;
    int depth = 0;

    uint64_t be(size_t off, size_t n) const {
        if (off + n > d.size()) throw PlistError("bplist: truncated");
        uint64_t v = 0;
        for (size_t i = 0; i < n; ++i) v = (v << 8) | d[off + i];
        return v;
    }
    size_t objectOffset(uint64_t ref) const {
        if (ref >= numObjects) throw PlistError("bplist: bad object reference");
        return size_t(be(size_t(tableOff + ref * offSize), offSize));
    }
    // Count for markers with 0xF meaning "an int object follows".
    uint64_t count(size_t& off, uint8_t marker) const {
        uint64_t n = marker & 0x0F;
        if (n == 0x0F) {
            const uint8_t im = d.at(off);
            if ((im >> 4) != 1) throw PlistError("bplist: bad count");
            const size_t sz = size_t(1) << (im & 0x0F);
            n = be(off + 1, sz);
            off += 1 + sz;
        }
        return n;
    }
    ValuePtr obj(uint64_t ref) {
        if (++depth > 512) throw PlistError("bplist: nesting too deep");
        size_t off = objectOffset(ref);
        const uint8_t m = d.at(off++);
        ValuePtr v;
        switch (m >> 4) {
            case 0x0:
                if (m == 0x08 || m == 0x09) v = Value::makeBool(m == 0x09);
                else v = std::make_shared<Value>();
                break;
            case 0x1: {
                const size_t sz = size_t(1) << (m & 0x0F);
                uint64_t raw = be(off, std::min<size_t>(sz, 8));
                int64_t x = sz == 8 ? int64_t(raw) : (sz == 16 ? int64_t(be(off + 8, 8)) : int64_t(raw));
                v = Value::makeInt(x);
                break;
            }
            case 0x2: {
                const size_t sz = size_t(1) << (m & 0x0F);
                if (sz == 4) { uint32_t b = uint32_t(be(off, 4)); float f; std::memcpy(&f, &b, 4); v = Value::makeReal(f); }
                else { uint64_t b = be(off, 8); double f; std::memcpy(&f, &b, 8); v = Value::makeReal(f); }
                break;
            }
            case 0x3: { uint64_t b = be(off, 8); double f; std::memcpy(&f, &b, 8); v = Value::makeDate(f); break; }
            case 0x4: {
                const uint64_t n = count(off, m);
                if (off + n > d.size()) throw PlistError("bplist: data truncated");
                v = Value::makeData(std::string(reinterpret_cast<const char*>(&d[off]), size_t(n)));
                break;
            }
            case 0x5: {
                const uint64_t n = count(off, m);
                if (off + n > d.size()) throw PlistError("bplist: string truncated");
                v = Value::makeString(std::string(reinterpret_cast<const char*>(&d[off]), size_t(n)));
                break;
            }
            case 0x6: {
                const uint64_t n = count(off, m);
                if (off + n * 2 > d.size()) throw PlistError("bplist: string truncated");
                std::u16string u(size_t(n), u'\0');
                for (size_t i = 0; i < n; ++i) u[i] = char16_t(be(off + i * 2, 2));
                v = Value::makeString(utf16ToUtf8(u.data(), u.size()));
                break;
            }
            case 0x8: {
                auto u = std::make_shared<Value>();
                u->type = Value::Type::Uid;
                u->i = int64_t(be(off, (m & 0x0F) + 1));
                v = u;
                break;
            }
            case 0xA: case 0xC: {
                const uint64_t n = count(off, m);
                v = Value::makeArray();
                for (uint64_t i = 0; i < n; ++i) v->array.push_back(obj(be(size_t(off + i * refSize), refSize)));
                break;
            }
            case 0xD: {
                const uint64_t n = count(off, m);
                v = Value::makeDict();
                for (uint64_t i = 0; i < n; ++i) {
                    ValuePtr k = obj(be(size_t(off + i * refSize), refSize));
                    ValuePtr val = obj(be(size_t(off + (n + i) * refSize), refSize));
                    v->dict.emplace_back(k->type == Value::Type::String ? k->s : std::to_string(k->i), val);
                }
                break;
            }
            default: throw PlistError("bplist: unknown object type");
        }
        --depth;
        return v;
    }
};

ValuePtr parseBinary(const std::vector<uint8_t>& d) {
    if (d.size() < 40) throw PlistError("bplist: too small");
    Binary b{d};
    const size_t t = d.size() - 32;
    b.offSize = d[t + 6];
    b.refSize = d[t + 7];
    b.numObjects = b.be(t + 8, 8);
    b.top = b.be(t + 16, 8);
    b.tableOff = b.be(t + 24, 8);
    if (!b.offSize || !b.refSize || b.offSize > 8 || b.refSize > 8 || b.tableOff + b.numObjects * b.offSize > t)
        throw PlistError("bplist: bad trailer");
    return b.obj(b.top);
}

// ------------------------------------------------------------------ XML ---

struct Xml {
    const std::string& t;
    size_t p = 0;

    void ws() { while (p < t.size() && std::isspace(uint8_t(t[p]))) ++p; }
    [[noreturn]] void fail(const char* what) { throw PlistError(std::string("XML plist: ") + what + " at offset " + std::to_string(p)); }

    // Skip <?...?>, <!...>, comments.
    void skipMisc() {
        for (;;) {
            ws();
            if (t.compare(p, 4, "<!--") == 0) { size_t e = t.find("-->", p); if (e == std::string::npos) fail("unterminated comment"); p = e + 3; }
            else if (t.compare(p, 2, "<?") == 0 || t.compare(p, 2, "<!") == 0) { size_t e = t.find('>', p); if (e == std::string::npos) fail("bad declaration"); p = e + 1; }
            else return;
        }
    }
    // Reads "<name ...>" or "<name/>". Returns name; sets selfClosing.
    std::string openTag(bool& selfClosing) {
        skipMisc();
        if (p >= t.size() || t[p] != '<') fail("expected tag");
        size_t e = t.find('>', p);
        if (e == std::string::npos) fail("unterminated tag");
        std::string inner = t.substr(p + 1, e - p - 1);
        p = e + 1;
        selfClosing = !inner.empty() && inner.back() == '/';
        if (selfClosing) inner.pop_back();
        const size_t sp = inner.find_first_of(" \t\r\n");
        return sp == std::string::npos ? inner : inner.substr(0, sp);
    }
    void closeTag(const std::string& name) {
        skipMisc();
        const std::string want = "</" + name;
        if (t.compare(p, want.size(), want) != 0) fail(("expected </" + name + ">").c_str());
        size_t e = t.find('>', p);
        p = e + 1;
    }
    std::string text(const std::string& name) {
        const std::string end = "</" + name;
        size_t e = t.find(end, p);
        if (e == std::string::npos) fail("unterminated element");
        std::string raw = t.substr(p, e - p);
        p = e;
        closeTag(name);
        return decodeEntities(raw);
    }
    static std::string decodeEntities(const std::string& s) {
        std::string out;
        for (size_t i = 0; i < s.size(); ++i) {
            if (s[i] != '&') { out += s[i]; continue; }
            const size_t e = s.find(';', i);
            if (e == std::string::npos) { out += s[i]; continue; }
            const std::string ent = s.substr(i + 1, e - i - 1);
            if (ent == "amp") out += '&';
            else if (ent == "lt") out += '<';
            else if (ent == "gt") out += '>';
            else if (ent == "quot") out += '"';
            else if (ent == "apos") out += '\'';
            else if (!ent.empty() && ent[0] == '#') {
                const uint32_t c = ent.size() > 1 && (ent[1] == 'x' || ent[1] == 'X') ? uint32_t(std::strtoul(ent.c_str() + 2, nullptr, 16))
                                                                                  : uint32_t(std::strtoul(ent.c_str() + 1, nullptr, 10));
                char16_t u[2]; size_t n = 1;
                if (c >= 0x10000) { u[0] = char16_t(0xD800 + ((c - 0x10000) >> 10)); u[1] = char16_t(0xDC00 + ((c - 0x10000) & 0x3FF)); n = 2; }
                else u[0] = char16_t(c);
                out += utf16ToUtf8(u, n);
            } else { out += '&'; out += ent; out += ';'; }
            i = e;
        }
        return out;
    }
    static std::string base64(const std::string& s) {
        static const std::string chars = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
        std::string out;
        uint32_t acc = 0; int bits = 0;
        for (char ch : s) {
            const size_t k = chars.find(ch);
            if (k == std::string::npos) continue;
            acc = (acc << 6) | uint32_t(k);
            bits += 6;
            if (bits >= 8) { bits -= 8; out += char((acc >> bits) & 0xFF); }
        }
        return out;
    }
    ValuePtr value(int depth = 0) {
        if (depth > 512) fail("nesting too deep");
        bool sc;
        const std::string tag = openTag(sc);
        if (tag == "true") { if (!sc) closeTag(tag); return Value::makeBool(true); }
        if (tag == "false") { if (!sc) closeTag(tag); return Value::makeBool(false); }
        if (tag == "string") return Value::makeString(sc ? "" : text(tag));
        if (tag == "integer") return Value::makeInt(sc ? 0 : std::strtoll(text(tag).c_str(), nullptr, 0));
        if (tag == "real") return Value::makeReal(sc ? 0 : std::strtod(text(tag).c_str(), nullptr));
        if (tag == "data") return Value::makeData(sc ? "" : base64(text(tag)));
        if (tag == "date") {
            const std::string s = sc ? "" : text(tag);
            int Y = 2001, M = 1, D = 1, h = 0, mi = 0, se = 0;
            std::sscanf(s.c_str(), "%d-%d-%dT%d:%d:%d", &Y, &M, &D, &h, &mi, &se);
            // days from civil (Howard Hinnant), relative to 2001-01-01
            auto days = [](int y, int m, int d) {
                y -= m <= 2;
                const int era = (y >= 0 ? y : y - 399) / 400;
                const unsigned yoe = unsigned(y - era * 400);
                const unsigned doy = (153 * unsigned(m + (m > 2 ? -3 : 9)) + 2) / 5 + unsigned(d) - 1;
                const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
                return era * 146097 + int(doe) - 719468;
            };
            return Value::makeDate(double(days(Y, M, D) - days(2001, 1, 1)) * 86400.0 + h * 3600 + mi * 60 + se);
        }
        if (tag == "array") {
            auto a = Value::makeArray();
            if (sc) return a;
            for (;;) {
                skipMisc();
                if (t.compare(p, 2, "</") == 0) { closeTag(tag); return a; }
                a->array.push_back(value(depth + 1));
            }
        }
        if (tag == "dict") {
            auto dct = Value::makeDict();
            if (sc) return dct;
            for (;;) {
                skipMisc();
                if (t.compare(p, 2, "</") == 0) { closeTag(tag); return dct; }
                bool ksc;
                if (openTag(ksc) != "key") fail("expected <key>");
                std::string k = ksc ? "" : text("key");
                dct->dict.emplace_back(std::move(k), value(depth + 1));
            }
        }
        fail(("unknown element <" + tag + ">").c_str());
    }
    ValuePtr root() {
        bool sc;
        skipMisc();
        size_t save = p;
        const std::string tag = openTag(sc);
        if (tag != "plist") { p = save; return value(); }
        ValuePtr v = sc ? Value::makeDict() : value();
        return v;
    }
};

// ------------------------------------------------- OpenStep / .strings ---

struct Ascii {
    const std::string& t;
    size_t p = 0;
    [[noreturn]] void fail(const char* what) { throw PlistError(std::string("text plist: ") + what + " at offset " + std::to_string(p)); }
    void ws() {
        for (;;) {
            while (p < t.size() && std::isspace(uint8_t(t[p]))) ++p;
            if (t.compare(p, 2, "//") == 0) { while (p < t.size() && t[p] != '\n') ++p; }
            else if (t.compare(p, 2, "/*") == 0) { size_t e = t.find("*/", p + 2); p = e == std::string::npos ? t.size() : e + 2; }
            else return;
        }
    }
    std::string quoted() {
        const char q = t[p++];
        std::string out;
        while (p < t.size() && t[p] != q) {
            if (t[p] == '\\' && p + 1 < t.size()) {
                const char e = t[++p];
                switch (e) {
                    case 'n': out += '\n'; break;
                    case 't': out += '\t'; break;
                    case 'r': out += '\r'; break;
                    case '"': out += '"'; break;
                    case '\'': out += '\''; break;
                    case '\\': out += '\\'; break;
                    case 'U': case 'u': {
                        const uint32_t c = uint32_t(std::strtoul(t.substr(p + 1, 4).c_str(), nullptr, 16));
                        char16_t u = char16_t(c);
                        out += utf16ToUtf8(&u, 1);
                        p += 4;
                        break;
                    }
                    default: out += e;
                }
                ++p;
            } else {
                out += t[p++];
            }
        }
        if (p >= t.size()) fail("unterminated string");
        ++p;
        return out;
    }
    std::string bare() {
        const size_t s = p;
        while (p < t.size() && (std::isalnum(uint8_t(t[p])) || std::strchr("_$+/:.-", t[p]))) ++p;
        if (s == p) fail("expected a value");
        return t.substr(s, p - s);
    }
    ValuePtr value(int depth = 0) {
        if (depth > 512) fail("nesting too deep");
        ws();
        if (p >= t.size()) fail("unexpected end");
        if (t[p] == '{') {
            ++p;
            auto d = Value::makeDict();
            for (;;) {
                ws();
                if (p < t.size() && t[p] == '}') { ++p; return d; }
                std::string k = (t[p] == '"' || t[p] == '\'') ? quoted() : bare();
                ws();
                if (p >= t.size() || t[p] != '=') fail("expected '='");
                ++p;
                d->dict.emplace_back(std::move(k), value(depth + 1));
                ws();
                if (p < t.size() && t[p] == ';') ++p;
            }
        }
        if (t[p] == '(') {
            ++p;
            auto a = Value::makeArray();
            for (;;) {
                ws();
                if (p < t.size() && t[p] == ')') { ++p; return a; }
                a->array.push_back(value(depth + 1));
                ws();
                if (p < t.size() && t[p] == ',') ++p;
            }
        }
        if (t[p] == '"' || t[p] == '\'') return Value::makeString(quoted());
        return Value::makeString(bare());
    }
    // A .strings file is a dictionary without the braces.
    ValuePtr root() {
        ws();
        if (p < t.size() && (t[p] == '{' || t[p] == '(')) return value();
        auto d = Value::makeDict();
        for (;;) {
            ws();
            if (p >= t.size()) return d;
            std::string k = (t[p] == '"' || t[p] == '\'') ? quoted() : bare();
            ws();
            if (p < t.size() && t[p] == ';') { ++p; d->dict.emplace_back(k, Value::makeString(k)); continue; }
            if (p >= t.size() || t[p] != '=') fail("expected '='");
            ++p;
            d->dict.emplace_back(std::move(k), value());
            ws();
            if (p < t.size() && t[p] == ';') ++p;
        }
    }
};

std::string decodeText(const std::vector<uint8_t>& d) {
    if (d.size() >= 2 && ((d[0] == 0xFF && d[1] == 0xFE) || (d[0] == 0xFE && d[1] == 0xFF))) {
        const bool bigEndian = d[0] == 0xFE;
        std::u16string u;
        for (size_t i = 2; i + 1 < d.size(); i += 2)
            u += char16_t(bigEndian ? (d[i] << 8 | d[i + 1]) : (d[i + 1] << 8 | d[i]));
        return utf16ToUtf8(u.data(), u.size());
    }
    size_t start = (d.size() >= 3 && d[0] == 0xEF && d[1] == 0xBB && d[2] == 0xBF) ? 3 : 0;
    return std::string(d.begin() + long(start), d.end());
}

void xmlEscape(std::string& out, const std::string& s) {
    for (char c : s) {
        switch (c) {
            case '&': out += "&amp;"; break;
            case '<': out += "&lt;"; break;
            case '>': out += "&gt;"; break;
            default: out += c;
        }
    }
}

void writeXml(std::string& out, const Value& v, int indent) {
    const std::string pad(size_t(indent), '\t');
    char buf[64];
    switch (v.type) {
        case Value::Type::Null: out += pad + "<string></string>\n"; break;
        case Value::Type::Bool: out += pad + (v.b ? "<true/>\n" : "<false/>\n"); break;
        case Value::Type::Integer: case Value::Type::Uid:
            std::snprintf(buf, sizeof buf, "%lld", (long long)v.i); out += pad + "<integer>" + buf + "</integer>\n"; break;
        case Value::Type::Real: std::snprintf(buf, sizeof buf, "%.17g", v.r); out += pad + "<real>" + buf + "</real>\n"; break;
        case Value::Type::Date: {
            const long long secs = (long long)std::floor(v.r) + 978307200LL;
            const long long days = secs / 86400 - (secs % 86400 < 0);
            long long rem = secs - days * 86400;
            // civil from days (Howard Hinnant)
            long long z = days + 719468;
            const long long era = (z >= 0 ? z : z - 146096) / 146097;
            const unsigned doe = unsigned(z - era * 146097);
            const unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
            const long long y = (long long)yoe + era * 400;
            const unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
            const unsigned mp = (5 * doy + 2) / 153;
            const unsigned d = doy - (153 * mp + 2) / 5 + 1;
            const unsigned m = mp < 10 ? mp + 3 : mp - 9;
            std::snprintf(buf, sizeof buf, "%04lld-%02u-%02uT%02lld:%02lld:%02lldZ", y + (m <= 2), m, d, rem / 3600, rem / 60 % 60, rem % 60);
            out += pad + "<date>" + buf + "</date>\n";
            break;
        }
        case Value::Type::Data: {
            static const char* chars = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
            std::string b64;
            const std::string& s = v.s;
            for (size_t i = 0; i < s.size(); i += 3) {
                uint32_t n = uint32_t(uint8_t(s[i])) << 16;
                if (i + 1 < s.size()) n |= uint32_t(uint8_t(s[i + 1])) << 8;
                if (i + 2 < s.size()) n |= uint8_t(s[i + 2]);
                b64 += chars[(n >> 18) & 63];
                b64 += chars[(n >> 12) & 63];
                b64 += i + 1 < s.size() ? chars[(n >> 6) & 63] : '=';
                b64 += i + 2 < s.size() ? chars[n & 63] : '=';
            }
            out += pad + "<data>" + b64 + "</data>\n";
            break;
        }
        case Value::Type::String: out += pad + "<string>"; xmlEscape(out, v.s); out += "</string>\n"; break;
        case Value::Type::Array:
            if (v.array.empty()) { out += pad + "<array/>\n"; break; }
            out += pad + "<array>\n";
            for (const auto& e : v.array) writeXml(out, *e, indent + 1);
            out += pad + "</array>\n";
            break;
        case Value::Type::Dict:
            if (v.dict.empty()) { out += pad + "<dict/>\n"; break; }
            out += pad + "<dict>\n";
            for (const auto& [k, e] : v.dict) {
                out += pad + "\t<key>"; xmlEscape(out, k); out += "</key>\n";
                writeXml(out, *e, indent + 1);
            }
            out += pad + "</dict>\n";
            break;
    }
}

} // namespace

ValuePtr parse(const std::vector<uint8_t>& data) {
    if (data.size() >= 8 && std::memcmp(data.data(), "bplist00", 8) == 0) return parseBinary(data);
    const std::string text = decodeText(data);
    size_t i = 0;
    while (i < text.size() && std::isspace(uint8_t(text[i]))) ++i;
    if (i < text.size() && text[i] == '<') { Xml x{text}; return x.root(); }
    Ascii a{text};
    return a.root();
}

ValuePtr parseFile(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) throw PlistError("can't open " + path);
    std::vector<uint8_t> d((std::istreambuf_iterator<char>(in)), {});
    return parse(d);
}

std::string toXml(const Value& v) {
    std::string out = "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
                      "<!DOCTYPE plist PUBLIC \"-//Apple//DTD PLIST 1.0//EN\" \"http://www.apple.com/DTDs/PropertyList-1.0.dtd\">\n"
                      "<plist version=\"1.0\">\n";
    writeXml(out, v, 0);
    out += "</plist>\n";
    return out;
}

void writeXmlFile(const Value& v, const std::string& path) {
    const std::string s = toXml(v);
    const std::string tmp = path + ".tmp";
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        if (!out) throw PlistError("can't write " + path);
        out.write(s.data(), std::streamsize(s.size()));
    }
    std::rename(tmp.c_str(), path.c_str());
}

} // namespace Hydrogen::Plist
