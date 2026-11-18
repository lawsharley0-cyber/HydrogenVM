#include "Format.hpp"
#include "loader/Plist.hpp"
#include <cstdio>
#include <optional>
#include <stdexcept>

namespace Hydrogen::Runtime {
namespace {

struct FormatState {
    explicit FormatState(Process&) {}
    std::function<std::string(uint32_t)> hook;
};

// Indexed formats need the types of every preceding logical argument before
// their ARMv7 word offsets can be known. Gather those types, snapshot the
// values once, then feed an ordinary argument stream to the existing formatter.
// POSIX requires indexed arguments to be contiguous and forbids mixing indexed
// and sequential argument consumers. Never guess a missing argument's width.
std::optional<std::string> positionalFormat(Process& p, const std::string& fmt, ArgReader& args,
                                          const std::function<std::string(uint32_t)>& hook) {
    struct Use { uint32_t value, width, precision; bool wide, widthStar, precisionStar, consumes; };
    std::vector<Use> uses;
    std::vector<uint8_t> widths(4097,0);
    uint32_t highest = 0;
    bool indexed = false;
    std::string plain;
    size_t i = 0;
    auto index = [&]() -> uint32_t {
        size_t end = i; uint32_t n = 0;
        while (end < fmt.size() && std::isdigit(uint8_t(fmt[end]))) {
            n = std::min(4097u,n*10+uint32_t(fmt[end++]-'0'));
        }
        if (end == i || end == fmt.size() || fmt[end] != '$') return 0;
        if (!n || n > 4096) throw std::invalid_argument("invalid positional format index");
        i = end+1; indexed = true; return n;
    };
    auto type = [&](uint32_t n, uint8_t words) {
        if (!n) return;
        if (widths[n] && widths[n] != words) throw std::invalid_argument("conflicting positional argument widths");
        widths[n] = words; highest = std::max(highest,n);
    };
    while (i < fmt.size()) {
        if (fmt[i] != '%') { plain += fmt[i++]; continue; }
        plain += fmt[i++];
        if (i < fmt.size() && fmt[i] == '%') { plain += fmt[i++]; continue; }
        Use use{}; use.value = index();
        bool widthSeen = false;
        while (i < fmt.size()) {
            if (std::strchr("-+ #0'",fmt[i])) plain += fmt[i++];
            else if (fmt[i] == '*') {
                if (use.widthStar) throw std::invalid_argument("repeated positional width");
                use.widthStar = true; plain += fmt[i++]; use.width = index();
            } else if (std::isdigit(uint8_t(fmt[i])) && !widthSeen) {
                widthSeen = true;
                while (i < fmt.size() && std::isdigit(uint8_t(fmt[i]))) plain += fmt[i++];
            } else break;
        }
        if (i < fmt.size() && fmt[i] == '.') {
            plain += fmt[i++];
            if (i < fmt.size() && fmt[i] == '*') {
                use.precisionStar = true; plain += fmt[i++]; use.precision = index();
            } else while (i < fmt.size() && std::isdigit(uint8_t(fmt[i]))) plain += fmt[i++];
        }
        std::string length;
        while (i < fmt.size() && std::strchr("hlqjztL",fmt[i])) { length += fmt[i]; plain += fmt[i++]; }
        if (i == fmt.size()) {
            if (indexed) throw std::invalid_argument("incomplete positional format");
            return std::nullopt;
        }
        const char conv = fmt[i++]; plain += conv;
        use.consumes = std::strchr("diuoxXDUOcCsSpfFeEgGaA@n",conv) != nullptr;
        use.wide = std::strchr("fFeEgGaA",conv) || ((length == "ll" || length == "q" || length == "j") && std::strchr("diuoxX",conv));
        if (!use.consumes && use.value) throw std::invalid_argument("unknown positional conversion");
        if (use.consumes) type(use.value,use.wide ? 2 : 1);
        if (use.widthStar) type(use.width,1);
        if (use.precisionStar) type(use.precision,1);
        uses.push_back(use);
    }
    if (!indexed) return std::nullopt;
    for (const auto use : uses)
        if ((use.consumes && !use.value) || (use.widthStar && !use.width) || (use.precisionStar && !use.precision))
            throw std::invalid_argument("mixed indexed and sequential format arguments");
    for (uint32_t n=1;n<=highest;++n)
        if (!widths[n]) throw std::invalid_argument("gap in positional format arguments");
    auto reader = args;
    std::vector<uint64_t> values(highest+1);
    for (uint32_t n=1;n<=highest;++n) values[n] = widths[n] == 2 ? reader.dword() : reader.word();
    args = reader;
    std::vector<uint32_t> ordered;
    for (const auto use : uses) {
        if (use.widthStar) ordered.push_back(uint32_t(values[use.width]));
        if (use.precisionStar) ordered.push_back(uint32_t(values[use.precision]));
        if (use.consumes) {
            ordered.push_back(uint32_t(values[use.value]));
            if (use.wide) ordered.push_back(uint32_t(values[use.value]>>32));
        }
    }
    if (ordered.size() > (UINT32_MAX-16)/4) throw std::length_error("positional format is too large");
    struct Buffer { Process& p; uint32_t address; ~Buffer() { if (address) p.heap().free(address); } };
    Buffer buffer{p,p.heap().allocZeroed(uint32_t(ordered.size()*4)+16)};
    if (!buffer.address) throw std::bad_alloc();
    if (!ordered.empty()) p.mem().copyIn(buffer.address,ordered.data(),ordered.size()*4);
    ArgReader reordered(p.mem(),buffer.address);
    return formatGuest(p,plain,reordered,hook);
}

} // namespace

void setObjectFormatHook(Process& p, std::function<std::string(uint32_t)> hook) { p.service<FormatState>().hook = std::move(hook); }
const std::function<std::string(uint32_t)>& objectFormatHook(Process& p) { return p.service<FormatState>().hook; }

std::string formatGuest(Process& p, const std::string& fmt, ArgReader& args, const std::function<std::string(uint32_t)>& objectHook) {
    if (fmt.find('$') != std::string::npos)
        if (auto indexed = positionalFormat(p,fmt,args,objectHook)) return std::move(*indexed);
    std::string out;
    out.reserve(fmt.size() + 16);
    const auto& hook = objectHook ? objectHook : objectFormatHook(p);
    char buf[512];
    size_t i = 0;
    while (i < fmt.size()) {
        const char ch = fmt[i];
        if (ch != '%') { out += ch; ++i; continue; }
        const size_t start = i++;
        if (i < fmt.size() && fmt[i] == '%') { out += '%'; ++i; continue; }

        // Flags and width can appear in flexible order (e.g. %10#llx in PLCrashReporter).
        std::string flags;
        int width = -1, prec = -1;
        bool widthStar = false, precStar = false;
        while (i < fmt.size()) {
            if (std::strchr("-+ #0'", fmt[i])) flags += fmt[i++];
            else if (fmt[i] == '*') { widthStar = true; ++i; }
            else if (std::isdigit(uint8_t(fmt[i])) && width < 0) {
                width = 0;
                while (i < fmt.size() && std::isdigit(uint8_t(fmt[i]))) width = width * 10 + (fmt[i++] - '0');
            } else break;
        }
        if (i < fmt.size() && fmt[i] == '.') {
            ++i;
            prec = 0;
            if (i < fmt.size() && fmt[i] == '*') { precStar = true; ++i; }
            else while (i < fmt.size() && std::isdigit(uint8_t(fmt[i]))) { prec = prec * 10 + (fmt[i] - '0'); ++i; }
        }
        std::string len;
        while (i < fmt.size() && std::strchr("hlqjztL", fmt[i])) len += fmt[i++];
        if (i >= fmt.size()) { out += fmt.substr(start); break; }
        const char conv = fmt[i++];

        if (widthStar) { int w = int(args.word()); if (w < 0) { flags += '-'; w = -w; } width = w; }
        if (precStar) { const int pr = int(args.word()); prec = pr < 0 ? -1 : pr; }

        // Host format spec without length modifiers; we pick the host type.
        std::string spec = "%" + flags;
        if (width >= 0) spec += std::to_string(width);
        if (prec >= 0) spec += "." + std::to_string(prec);
        const bool is64 = len == "ll" || len == "q" || len == "j";
        auto pad = [&](const std::string& s) {
            if (width < 0 || int(s.size()) >= width) { out += s; return; }
            const std::string fill(size_t(width) - s.size(), ' ');
            if (flags.find('-') != std::string::npos) out += s + fill; else out += fill + s;
        };

        switch (conv) {
            case 'd': case 'i': {
                long long v;
                if (is64) v = (long long)int64_t(args.dword());
                else {
                    const uint32_t w = args.word();
                    if (len == "hh") v = int8_t(w);
                    else if (len == "h") v = int16_t(w);
                    else v = int32_t(w);
                }
                std::snprintf(buf, sizeof buf, (spec + "lld").c_str(), v);
                out += buf;
                break;
            }
            case 'u': case 'o': case 'x': case 'X': {
                unsigned long long v;
                if (is64) v = args.dword();
                else {
                    const uint32_t w = args.word();
                    if (len == "hh") v = uint8_t(w);
                    else if (len == "h") v = uint16_t(w);
                    else v = w;
                }
                std::snprintf(buf, sizeof buf, (spec + "ll" + conv).c_str(), v);
                out += buf;
                break;
            }
            case 'D': case 'U': case 'O': {   // obsolete long forms
                const uint32_t w = args.word();
                std::snprintf(buf, sizeof buf, (spec + (conv == 'D' ? "d" : conv == 'U' ? "u" : "o")).c_str(), w);
                out += buf;
                break;
            }
            case 'c': {
                const char cc = char(args.word());
                std::snprintf(buf, sizeof buf, (spec + "c").c_str(), cc);
                out += buf;
                break;
            }
            case 'C': {   // unichar
                const char16_t u = char16_t(args.word());
                pad(Plist::utf16ToUtf8(&u, 1));
                break;
            }
            case 's': {
                const uint32_t ptr = args.word();
                std::string s;
                if (!ptr) s = "(null)";
                else if (len == "l") {   // wchar_t* (32-bit)
                    std::u16string u;
                    for (uint32_t a = ptr; prec < 0 || int(u.size()) < prec; a += 4) {
                        const uint32_t wc = p.mem().read<uint32_t>(a);
                        if (!wc) break;
                        if (wc >= 0x10000) { u += char16_t(0xD800 + ((wc - 0x10000) >> 10)); u += char16_t(0xDC00 + ((wc - 0x10000) & 0x3FF)); }
                        else u += char16_t(wc);
                    }
                    s = Plist::utf16ToUtf8(u.data(), u.size());
                } else {
                    s = p.readCString(ptr, prec >= 0 ? size_t(prec) : (1u << 20));
                }
                std::string spec2 = "%" + flags + (width >= 0 ? std::to_string(width) : "") + "s";
                if (s.size() < sizeof buf - 1) { std::snprintf(buf, sizeof buf, spec2.c_str(), s.c_str()); out += buf; }
                else pad(s);
                break;
            }
            case 'S': {   // UTF-16 string (Cocoa)
                const uint32_t ptr = args.word();
                std::u16string u;
                if (ptr) for (uint32_t a = ptr; prec < 0 || int(u.size()) < prec; a += 2) {
                    const char16_t c16 = p.mem().read<uint16_t>(a);
                    if (!c16) break;
                    u += c16;
                }
                pad(ptr ? Plist::utf16ToUtf8(u.data(), u.size()) : "(null)");
                break;
            }
            case 'p': {
                const uint32_t v = args.word();
                std::snprintf(buf, sizeof buf, "0x%x", v);
                pad(buf);
                break;
            }
            case 'f': case 'F': case 'e': case 'E': case 'g': case 'G': case 'a': case 'A': {
                const double d = args.get<double>();   // floats are promoted; long double == double
                std::snprintf(buf, sizeof buf, (spec + conv).c_str(), d);
                out += buf;
                break;
            }
            case '@': {
                const uint32_t obj = args.word();
                std::string s;
                if (!obj) s = "(null)";
                else if (hook) s = hook(obj);
                else { std::snprintf(buf, sizeof buf, "<object 0x%x>", obj); s = buf; }
                if (prec >= 0 && int(s.size()) > prec) s.resize(size_t(prec));
                pad(s);
                break;
            }
            case 'n': {
                const uint32_t ptr = args.word();
                if (ptr) p.mem().write<uint32_t>(ptr, uint32_t(out.size()));
                break;
            }
            default:
                out += fmt.substr(start, i - start);   // unknown: print literally
                break;
        }
    }
    return out;
}

} // namespace Hydrogen::Runtime
