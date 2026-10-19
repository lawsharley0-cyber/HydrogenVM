#include "Format.hpp"
#include "loader/Plist.hpp"
#include <cstdio>

namespace Hydrogen::Runtime {
namespace {

struct FormatState {
    explicit FormatState(Process&) {}
    std::function<std::string(uint32_t)> hook;
};

} // namespace

void setObjectFormatHook(Process& p, std::function<std::string(uint32_t)> hook) { p.service<FormatState>().hook = std::move(hook); }
const std::function<std::string(uint32_t)>& objectFormatHook(Process& p) { return p.service<FormatState>().hook; }

std::string formatGuest(Process& p, const std::string& fmt, ArgReader& args, const std::function<std::string(uint32_t)>& objectHook) {
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

        // Positional arguments (%1$d) are rare in apps; not supported.
        std::string flags;
        while (i < fmt.size() && std::strchr("-+ #0'", fmt[i])) flags += fmt[i++];
        int width = -1, prec = -1;
        bool widthStar = false, precStar = false;
        if (i < fmt.size() && fmt[i] == '*') { widthStar = true; ++i; }
        else { while (i < fmt.size() && std::isdigit(uint8_t(fmt[i]))) { width = (width < 0 ? 0 : width) * 10 + (fmt[i] - '0'); ++i; } }
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
