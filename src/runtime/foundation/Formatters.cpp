// HydrogenVM — NSFormatter, NSNumberFormatter and NSDateFormatter (en_US
// behaviour: what games use them for is scores, money and dates on screen).
#include "Foundation.hpp"
#include <cmath>
#include <cstring>
#include <ctime>
#include <optional>

namespace Hydrogen::Runtime::Foundation {
namespace {

using ObjC::id;
using ObjC::Runtime;

// ------------------------------------------------------- number formatter ---

enum NumberStyle { kNone = 0, kDecimal = 1, kCurrency = 2, kPercent = 3, kScientific = 4, kSpellOut = 5 };

struct NumberFormatterData : ObjC::HostObject {
    int style = kNone;
    std::optional<bool> grouping;
    std::string groupingSeparator = ",", decimalSeparator = ".", currencySymbol = "$", percentSymbol = "%";
    std::optional<uint32_t> minFraction, maxFraction;
    uint32_t minInteger = 1, groupingSize = 3;
    std::optional<std::string> positivePrefix, positiveSuffix, negativePrefix, negativeSuffix;
    std::optional<std::string> zeroSymbol, nilSymbol;
    double multiplier = 0;   // 0 = style default
    bool allowsFloats = true;
    uint32_t behavior = 0, roundingMode = 4;
};
NumberFormatterData& nf(Ctx& x, id self) { return ObjC::rt(x).host<NumberFormatterData>(self); }

bool usesGrouping(const NumberFormatterData& d) { return d.grouping.value_or(d.style == kDecimal || d.style == kCurrency || d.style == kPercent); }
uint32_t maxFraction(const NumberFormatterData& d) {
    if (d.maxFraction) return *d.maxFraction;
    return d.style == kDecimal ? 3 : d.style == kCurrency ? 2 : d.style == kScientific ? 6 : 0;
}
uint32_t minFraction(const NumberFormatterData& d) {
    if (d.minFraction) return std::min(*d.minFraction, std::max(maxFraction(d), *d.minFraction));
    return d.style == kCurrency ? 2 : 0;
}

std::string formatNumber(const NumberFormatterData& d, double v) {
    if (std::isnan(v)) return "NaN";
    if (v == 0 && d.zeroSymbol) return *d.zeroSymbol;
    const double mult = d.multiplier ? d.multiplier : (d.style == kPercent ? 100 : 1);
    v *= mult;
    const bool neg = v < 0 || (v == 0 && std::signbit(v) && false);
    double a = std::fabs(v);
    if (std::isinf(a)) return std::string(neg ? "-" : "") + "∞";
    std::string body;
    if (d.style == kScientific) {
        char buf[64];
        std::snprintf(buf, sizeof buf, "%.*E", int(maxFraction(d)), a);
        body = buf;
    } else {
        const uint32_t maxF = std::min<uint32_t>(maxFraction(d), 15), minF = std::min(minFraction(d), maxF);
        char buf[512];
        std::snprintf(buf, sizeof buf, "%.*f", int(maxF), a);
        std::string s = buf, ip = s, fp;
        if (const size_t dot = s.find('.'); dot != std::string::npos) { ip = s.substr(0, dot); fp = s.substr(dot + 1); }
        while (fp.size() > minF && !fp.empty() && fp.back() == '0') fp.pop_back();
        while (ip.size() < d.minInteger) ip.insert(ip.begin(), '0');
        if (d.minInteger == 0 && ip == "0" && !fp.empty()) ip.clear();
        if (usesGrouping(d) && d.groupingSize > 0) {
            std::string g;
            const size_t n = ip.size();
            for (size_t i = 0; i < n; ++i) {
                g += ip[i];
                const size_t left = n - 1 - i;
                if (left && left % d.groupingSize == 0) g += d.groupingSeparator;
            }
            ip = g;
        }
        body = fp.empty() ? ip : ip + d.decimalSeparator + fp;
    }
    const bool isZero = body.find_first_not_of("0.,") == std::string::npos;
    const bool showNeg = neg && !isZero;
    std::string prefix, suffix;
    if (d.style == kCurrency) prefix = d.currencySymbol;
    if (d.style == kPercent) suffix = d.percentSymbol;
    if (showNeg) {
        prefix = d.negativePrefix ? *d.negativePrefix : "-" + (d.positivePrefix ? *d.positivePrefix : prefix);
        suffix = d.negativeSuffix ? *d.negativeSuffix : (d.positiveSuffix ? *d.positiveSuffix : suffix);
    } else {
        if (d.positivePrefix) prefix = *d.positivePrefix;
        if (d.positiveSuffix) suffix = *d.positiveSuffix;
    }
    return prefix + body + suffix;
}

std::optional<double> parseNumber(const NumberFormatterData& d, std::string s) {
    auto eraseAll = [&](const std::string& t) {
        if (t.empty()) return;
        for (size_t p; (p = s.find(t)) != std::string::npos;) s.erase(p, t.size());
    };
    for (const auto& t : {d.positivePrefix, d.positiveSuffix}) if (t && !t->empty() && *t != "-") eraseAll(*t);
    eraseAll(d.currencySymbol);
    eraseAll(d.percentSymbol);
    if (usesGrouping(d) || d.groupingSeparator == ",") eraseAll(d.groupingSeparator);
    if (d.decimalSeparator != ".") { const size_t p = s.find(d.decimalSeparator); if (p != std::string::npos) s.replace(p, d.decimalSeparator.size(), "."); }
    eraseAll(" ");
    if (s.empty()) return std::nullopt;
    char* end = nullptr;
    double v = std::strtod(s.c_str(), &end);
    if (!end || *end) return std::nullopt;
    const double mult = d.multiplier ? d.multiplier : (d.style == kPercent ? 100 : 1);
    v /= mult;
    if (!d.allowsFloats && v != std::floor(v)) return std::nullopt;
    return v;
}

id numberObject(Ctx& x, double v) {
    if (v == std::floor(v) && std::fabs(v) < 9.0e15) return makeNumberInt(x, int64_t(v));
    return makeNumberDouble(x, v);
}

double numberArg(Ctx& x, id n) {
    if (!n) return std::nan("");
    if (isNumber(x, n)) return numberDouble(x, n);
    // NSDecimalNumber and other NSNumber-alikes
    return std::bit_cast<double>(ObjC::rt(x).send(x.cpu, n, "doubleValue"));
}

// "#,##0.00", "0.###", "$#,##0.00;-$#,##0.00": fraction digits, grouping, prefix/suffix.
void applyPattern(NumberFormatterData& d, const std::string& pat, bool negative) {
    const std::string p = pat.substr(0, pat.find(';'));
    const size_t first = p.find_first_of("#0,."), last = p.find_last_of("#0,.");
    if (first == std::string::npos) return;
    const std::string pre = p.substr(0, first), core = p.substr(first, last - first + 1), suf = p.substr(last + 1);
    if (negative) { d.negativePrefix = pre; d.negativeSuffix = suf; return; }
    d.positivePrefix = pre;
    d.positiveSuffix = suf;
    d.grouping = core.find(',') != std::string::npos;
    if (const size_t comma = core.rfind(','); comma != std::string::npos) {
        const size_t end = core.find('.') == std::string::npos ? core.size() : core.find('.');
        if (end > comma + 1) d.groupingSize = uint32_t(end - comma - 1);
    }
    const size_t dot = core.find('.');
    if (dot == std::string::npos) { d.minFraction = 0; d.maxFraction = 0; }
    else {
        const std::string frac = core.substr(dot + 1);
        d.minFraction = uint32_t(std::count(frac.begin(), frac.end(), '0'));
        d.maxFraction = uint32_t(frac.size());
    }
    const std::string ip = core.substr(0, dot == std::string::npos ? core.size() : dot);
    d.minInteger = uint32_t(std::count(ip.begin(), ip.end(), '0'));
}

// --------------------------------------------------------- date formatter ---

struct DateFormatterData : ObjC::HostObject {
    std::string format;
    int dateStyle = 0, timeStyle = 0;
    bool utc = false;
};
DateFormatterData& df(Ctx& x, id self) { return ObjC::rt(x).host<DateFormatterData>(self); }

constexpr double kRefEpoch = 978307200.0;   // 2001-01-01 in Unix time
const char* kMonths[] = {"January", "February", "March", "April", "May", "June", "July", "August", "September", "October", "November", "December"};
const char* kDays[] = {"Sunday", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday"};

std::string styleFormat(int dateStyle, int timeStyle) {
    static const char* dates[] = {"", "M/d/yy", "MMM d, y", "MMMM d, y", "EEEE, MMMM d, y"};
    static const char* times[] = {"", "h:mm a", "h:mm:ss a", "h:mm:ss a z", "h:mm:ss a zzzz"};
    const std::string dpart = dates[std::clamp(dateStyle, 0, 4)], tpart = times[std::clamp(timeStyle, 0, 4)];
    if (dpart.empty()) return tpart;
    if (tpart.empty()) return dpart;
    return dpart + (dateStyle >= 3 ? " 'at' " : ", ") + tpart;
}

std::string effectiveFormat(const DateFormatterData& d) {
    return d.format.empty() ? styleFormat(d.dateStyle, d.timeStyle) : d.format;
}

std::string formatDate(const DateFormatterData& d, double ref) {
    const std::time_t t = std::time_t(std::floor(ref + kRefEpoch));
    const double frac = (ref + kRefEpoch) - std::floor(ref + kRefEpoch);
    std::tm tm{};
    if (d.utc) gmtime_r(&t, &tm); else localtime_r(&t, &tm);
    const std::string f = effectiveFormat(d);
    std::string out;
    char buf[32];
    for (size_t i = 0; i < f.size();) {
        const char c = f[i];
        if (c == '\'') {
            const size_t j = f.find('\'', i + 1);
            if (j == i + 1) { out += '\''; i += 2; continue; }
            out += f.substr(i + 1, (j == std::string::npos ? f.size() : j) - i - 1);
            i = j == std::string::npos ? f.size() : j + 1;
            continue;
        }
        if (!std::isalpha(uint8_t(c))) { out += c; ++i; continue; }
        size_t n = 1;
        while (i + n < f.size() && f[i + n] == c) ++n;
        auto num = [&](int v, size_t width) { std::snprintf(buf, sizeof buf, "%0*d", int(width), v); out += buf; };
        switch (c) {
            case 'y': case 'Y': case 'u':
                if (n == 2) num((tm.tm_year + 1900) % 100, 2); else num(tm.tm_year + 1900, n); break;
            case 'M': case 'L':
                if (n >= 4) out += kMonths[tm.tm_mon];
                else if (n == 3) out += std::string(kMonths[tm.tm_mon]).substr(0, 3);
                else num(tm.tm_mon + 1, n); break;
            case 'd': num(tm.tm_mday, n); break;
            case 'D': num(tm.tm_yday + 1, n); break;
            case 'E': case 'e': case 'c':
                if (n >= 4) out += kDays[tm.tm_wday]; else out += std::string(kDays[tm.tm_wday]).substr(0, 3); break;
            case 'H': num(tm.tm_hour, n); break;
            case 'k': num(tm.tm_hour ? tm.tm_hour : 24, n); break;
            case 'h': num(tm.tm_hour % 12 ? tm.tm_hour % 12 : 12, n); break;
            case 'K': num(tm.tm_hour % 12, n); break;
            case 'm': num(tm.tm_min, n); break;
            case 's': num(tm.tm_sec, n); break;
            case 'S': { std::snprintf(buf, sizeof buf, "%0*d", int(n), int(frac * std::pow(10.0, double(n)))); out += buf; break; }
            case 'a': out += tm.tm_hour < 12 ? "AM" : "PM"; break;
            case 'z': case 'Z': case 'v': case 'V': case 'O': case 'x': case 'X': {
                const long off = d.utc ? 0 : tm.tm_gmtoff;
                if (c == 'z' && n < 4 && !d.utc && tm.tm_zone) { out += tm.tm_zone; break; }
                std::snprintf(buf, sizeof buf, (c == 'Z' && n < 4) ? "%c%02ld%02ld" : "GMT%c%02ld:%02ld", off < 0 ? '-' : '+', std::labs(off) / 3600, (std::labs(off) % 3600) / 60);
                out += buf;
                break;
            }
            case 'G': out += tm.tm_year >= -1900 ? "AD" : "BC"; break;
            default: out.append(n, c); break;
        }
        i += n;
    }
    return out;
}

std::optional<double> parseDate(const DateFormatterData& d, const std::string& s) {
    const std::string f = effectiveFormat(d);
    std::tm tm{};
    tm.tm_mday = 1;
    tm.tm_year = 70;
    tm.tm_isdst = -1;
    int hour12 = -1;
    bool pm = false;
    size_t p = 0;
    auto readInt = [&](size_t maxDigits, int& v) {
        size_t k = 0;
        v = 0;
        bool neg = false;
        if (p < s.size() && (s[p] == '-' || s[p] == '+')) { neg = s[p] == '-'; ++p; }
        while (p < s.size() && k < maxDigits && std::isdigit(uint8_t(s[p]))) { v = v * 10 + (s[p] - '0'); ++p; ++k; }
        if (neg) v = -v;
        return k > 0;
    };
    long tzOffset = 0;
    bool haveTz = false;
    for (size_t i = 0; i < f.size();) {
        const char c = f[i];
        if (c == '\'') {
            const size_t j = f.find('\'', i + 1);
            const std::string lit = f.substr(i + 1, (j == std::string::npos ? f.size() : j) - i - 1);
            if (s.compare(p, lit.size(), lit) != 0) return std::nullopt;
            p += lit.size();
            i = j == std::string::npos ? f.size() : j + 1;
            continue;
        }
        if (!std::isalpha(uint8_t(c))) {
            if (p < s.size() && s[p] == c) ++p;
            else if (c != ' ') return std::nullopt;
            ++i;
            continue;
        }
        size_t n = 1;
        while (i + n < f.size() && f[i + n] == c) ++n;
        int v = 0;
        const size_t maxDigits = n >= 2 ? std::max<size_t>(n, 2) : 9;
        switch (c) {
            case 'y': case 'Y': case 'u':
                if (!readInt(n == 2 ? 2 : 9, v)) return std::nullopt;
                tm.tm_year = (n == 2 ? (v < 70 ? 2000 + v : 1900 + v) : v) - 1900; break;
            case 'M': case 'L':
                if (n >= 3) {
                    int found = -1;
                    for (int m = 0; m < 12 && found < 0; ++m) {
                        const std::string full = kMonths[m], abbr = full.substr(0, 3);
                        if (strncasecmp(s.c_str() + p, full.c_str(), full.size()) == 0) { found = m; p += full.size(); }
                        else if (strncasecmp(s.c_str() + p, abbr.c_str(), 3) == 0) { found = m; p += 3; }
                    }
                    if (found < 0) return std::nullopt;
                    tm.tm_mon = found;
                } else { if (!readInt(maxDigits, v)) return std::nullopt; tm.tm_mon = v - 1; }
                break;
            case 'd': if (!readInt(maxDigits, v)) return std::nullopt; tm.tm_mday = v; break;
            case 'H': case 'k': if (!readInt(maxDigits, v)) return std::nullopt; tm.tm_hour = v % 24; break;
            case 'h': case 'K': if (!readInt(maxDigits, v)) return std::nullopt; hour12 = v % 12; break;
            case 'm': if (!readInt(maxDigits, v)) return std::nullopt; tm.tm_min = v; break;
            case 's': if (!readInt(maxDigits, v)) return std::nullopt; tm.tm_sec = v; break;
            case 'S': readInt(9, v); break;
            case 'a':
                if (strncasecmp(s.c_str() + p, "PM", 2) == 0) { pm = true; p += 2; }
                else if (strncasecmp(s.c_str() + p, "AM", 2) == 0) p += 2;
                break;
            case 'E': case 'e': case 'c':
                while (p < s.size() && std::isalpha(uint8_t(s[p]))) ++p;
                break;
            case 'Z': case 'z': case 'x': case 'X': {
                if (s.compare(p, 3, "GMT") == 0 || s.compare(p, 3, "UTC") == 0) p += 3;
                if (p < s.size() && s[p] == 'Z') { ++p; haveTz = true; break; }
                if (p < s.size() && (s[p] == '+' || s[p] == '-')) {
                    const int sign = s[p] == '-' ? -1 : 1; ++p;
                    int hh = 0, mm = 0;
                    readInt(2, hh);
                    if (p < s.size() && s[p] == ':') ++p;
                    readInt(2, mm);
                    tzOffset = sign * (hh * 3600L + mm * 60L);
                }
                haveTz = true;
                break;
            }
            default: break;
        }
        i += n;
    }
    if (hour12 >= 0) tm.tm_hour = hour12 + (pm ? 12 : 0);
    std::time_t t;
    if (haveTz || d.utc) { t = timegm(&tm) - tzOffset; }
    else t = std::mktime(&tm);
    if (t == std::time_t(-1) && tm.tm_year != 69) return std::nullopt;
    return double(t) - kRefEpoch;
}

bool isUtcZone(Ctx& x, id tz) {
    if (!tz) return false;
    auto& r = ObjC::rt(x);
    if (!r.respondsTo(tz, r.sel("name"))) return false;
    const id name = id(r.send(x.cpu, tz, "name"));
    const std::string n = name ? utf8(x, name) : "";
    return n == "GMT" || n == "UTC" || n == "Etc/UTC" || n == "Etc/GMT";
}

// ----------------------------------------------------------- method glue ---

uint32_t arg(Ctx& x, int i) { return x.cpu.r[2 + i]; }
void ret(Ctx& x, uint32_t v) { x.cpu.r[0] = v; }
std::string strArg(Ctx& x, int i) { const id s = arg(x, i); return s ? utf8(x, s) : std::string(); }

} // namespace

void registerFormatters(Process& p, ObjC::Runtime& r) {
    r.defineClass("NSFormatter", "NSObject");
    ObjC::ClassBuilder n = r.defineClass("NSNumberFormatter", "NSFormatter");
    using Opt = std::optional<std::string> NumberFormatterData::*;
    auto strProp = [&](const char* setter, const char* getter, std::string NumberFormatterData::*field) {
        n.method(setter, [field](Ctx& x) { nf(x, x.cpu.r[0]).*field = strArg(x, 0); }, "v@:@");
        n.method(getter, [field](Ctx& x) { ret(x, makeString(x, nf(x, x.cpu.r[0]).*field)); });
    };
    auto optProp = [&](const char* setter, const char* getter, Opt field) {
        n.method(setter, [field](Ctx& x) { if (arg(x, 0)) nf(x, x.cpu.r[0]).*field = strArg(x, 0); else (nf(x, x.cpu.r[0]).*field).reset(); }, "v@:@");
        n.method(getter, [field](Ctx& x) { const auto& v = nf(x, x.cpu.r[0]).*field; ret(x, v ? makeString(x, *v) : makeString(x, std::string())); });
    };
    n.method("setNumberStyle:", [](Ctx& x) { nf(x, x.cpu.r[0]).style = int32_t(arg(x, 0)); }, "v@:i");
    n.method("numberStyle", [](Ctx& x) { ret(x, uint32_t(nf(x, x.cpu.r[0]).style)); }, "i@:");
    n.method("setFormatterBehavior:", [](Ctx& x) { nf(x, x.cpu.r[0]).behavior = arg(x, 0); }, "v@:I");
    n.method("formatterBehavior", [](Ctx& x) { ret(x, nf(x, x.cpu.r[0]).behavior); }, "I@:");
    n.method("setUsesGroupingSeparator:", [](Ctx& x) { nf(x, x.cpu.r[0]).grouping = (arg(x, 0) & 0xFF) != 0; }, "v@:c");
    n.method("usesGroupingSeparator", [](Ctx& x) { ret(x, usesGrouping(nf(x, x.cpu.r[0]))); }, "c@:");
    n.method("setGroupingSize:", [](Ctx& x) { nf(x, x.cpu.r[0]).groupingSize = arg(x, 0); }, "v@:I");
    n.method("groupingSize", [](Ctx& x) { ret(x, nf(x, x.cpu.r[0]).groupingSize); }, "I@:");
    strProp("setGroupingSeparator:", "groupingSeparator", &NumberFormatterData::groupingSeparator);
    strProp("setDecimalSeparator:", "decimalSeparator", &NumberFormatterData::decimalSeparator);
    strProp("setCurrencySymbol:", "currencySymbol", &NumberFormatterData::currencySymbol);
    strProp("setPercentSymbol:", "percentSymbol", &NumberFormatterData::percentSymbol);
    optProp("setPositivePrefix:", "positivePrefix", &NumberFormatterData::positivePrefix);
    optProp("setPositiveSuffix:", "positiveSuffix", &NumberFormatterData::positiveSuffix);
    optProp("setNegativePrefix:", "negativePrefix", &NumberFormatterData::negativePrefix);
    optProp("setNegativeSuffix:", "negativeSuffix", &NumberFormatterData::negativeSuffix);
    optProp("setZeroSymbol:", "zeroSymbol", &NumberFormatterData::zeroSymbol);
    optProp("setNilSymbol:", "nilSymbol", &NumberFormatterData::nilSymbol);
    n.method("setMaximumFractionDigits:", [](Ctx& x) { nf(x, x.cpu.r[0]).maxFraction = arg(x, 0); }, "v@:I");
    n.method("maximumFractionDigits", [](Ctx& x) { ret(x, maxFraction(nf(x, x.cpu.r[0]))); }, "I@:");
    n.method("setMinimumFractionDigits:", [](Ctx& x) {
        auto& d = nf(x, x.cpu.r[0]);
        d.minFraction = arg(x, 0);
        if (maxFraction(d) < arg(x, 0)) d.maxFraction = arg(x, 0);
    }, "v@:I");
    n.method("minimumFractionDigits", [](Ctx& x) { ret(x, minFraction(nf(x, x.cpu.r[0]))); }, "I@:");
    n.method("setMinimumIntegerDigits:", [](Ctx& x) { nf(x, x.cpu.r[0]).minInteger = arg(x, 0); }, "v@:I");
    n.method("minimumIntegerDigits", [](Ctx& x) { ret(x, nf(x, x.cpu.r[0]).minInteger); }, "I@:");
    n.method("setRoundingMode:", [](Ctx& x) { nf(x, x.cpu.r[0]).roundingMode = arg(x, 0); }, "v@:I");
    n.method("roundingMode", [](Ctx& x) { ret(x, nf(x, x.cpu.r[0]).roundingMode); }, "I@:");
    n.method("setAllowsFloats:", [](Ctx& x) { nf(x, x.cpu.r[0]).allowsFloats = (arg(x, 0) & 0xFF) != 0; }, "v@:c");
    n.method("allowsFloats", [](Ctx& x) { ret(x, nf(x, x.cpu.r[0]).allowsFloats); }, "c@:");
    n.method("setMultiplier:", [](Ctx& x) { const id m = arg(x, 0); nf(x, x.cpu.r[0]).multiplier = m ? numberArg(x, m) : 0; }, "v@:@");
    n.method("multiplier", [](Ctx& x) { const auto& d = nf(x, x.cpu.r[0]); ret(x, d.multiplier ? numberObject(x, d.multiplier) : 0); });
    n.method("setPositiveFormat:", [](Ctx& x) { applyPattern(nf(x, x.cpu.r[0]), strArg(x, 0), false); }, "v@:@");
    n.method("setNegativeFormat:", [](Ctx& x) { applyPattern(nf(x, x.cpu.r[0]), strArg(x, 0), true); }, "v@:@");
    n.method("setFormat:", [](Ctx& x) {
        const std::string f = strArg(x, 0);
        applyPattern(nf(x, x.cpu.r[0]), f, false);
        if (const size_t semi = f.find(';'); semi != std::string::npos) applyPattern(nf(x, x.cpu.r[0]), f.substr(semi + 1), true);
    }, "v@:@");
    // Accepted and ignored: everything is formatted as en_US.
    for (const char* s : {"setLocale:", "setCurrencyCode:", "setInternationalCurrencySymbol:", "setGeneratesDecimalNumbers:",
                          "setLenient:", "setPaddingCharacter:", "setFormatWidth:", "setPaddingPosition:", "setNotANumberSymbol:",
                          "setMaximumIntegerDigits:", "setMaximumSignificantDigits:", "setMinimumSignificantDigits:",
                          "setUsesSignificantDigits:", "setAlwaysShowsDecimalSeparator:", "setPerMillSymbol:", "setMinusSign:", "setPlusSign:"})
        n.method(s, [](Ctx&) {}, "v@:@");
    n.method("locale", [](Ctx& x) { ret(x, 0); });
    n.method("stringFromNumber:", [](Ctx& x) {
        const id num = arg(x, 0);
        const auto& d = nf(x, x.cpu.r[0]);
        if (!num) { ret(x, d.nilSymbol ? makeString(x, *d.nilSymbol) : 0); return; }
        ret(x, makeString(x, formatNumber(d, numberArg(x, num))));
    }, "@@:@");
    n.alias("stringForObjectValue:", "stringFromNumber:");
    n.method("numberFromString:", [](Ctx& x) {
        const id s = arg(x, 0);
        if (!s) { ret(x, 0); return; }
        const auto v = parseNumber(nf(x, x.cpu.r[0]), utf8(x, s));
        ret(x, v ? numberObject(x, *v) : 0);
    }, "@@:@");
    n.method("getObjectValue:forString:errorDescription:", [](Ctx& x) {
        const auto v = arg(x, 1) ? parseNumber(nf(x, x.cpu.r[0]), utf8(x, arg(x, 1))) : std::nullopt;
        if (arg(x, 0)) x.mem.write<uint32_t>(arg(x, 0), v ? numberObject(x, *v) : 0);
        ret(x, v ? 1 : 0);
    }, "c@:^@@^@");
    n.classMethod("localizedStringFromNumber:numberStyle:", [](Ctx& x) {
        NumberFormatterData d;
        d.style = int32_t(arg(x, 1));
        ret(x, makeString(x, formatNumber(d, numberArg(x, arg(x, 0)))));
    }, "@@:@i");

    ObjC::ClassBuilder dfb = r.defineClass("NSDateFormatter", "NSFormatter");
    dfb.method("setDateFormat:", [](Ctx& x) { df(x, x.cpu.r[0]).format = strArg(x, 0); }, "v@:@");
    dfb.method("dateFormat", [](Ctx& x) { ret(x, makeString(x, effectiveFormat(df(x, x.cpu.r[0])))); });
    dfb.method("setDateStyle:", [](Ctx& x) { auto& d = df(x, x.cpu.r[0]); d.dateStyle = int32_t(arg(x, 0)); d.format.clear(); }, "v@:i");
    dfb.method("dateStyle", [](Ctx& x) { ret(x, uint32_t(df(x, x.cpu.r[0]).dateStyle)); }, "i@:");
    dfb.method("setTimeStyle:", [](Ctx& x) { auto& d = df(x, x.cpu.r[0]); d.timeStyle = int32_t(arg(x, 0)); d.format.clear(); }, "v@:i");
    dfb.method("timeStyle", [](Ctx& x) { ret(x, uint32_t(df(x, x.cpu.r[0]).timeStyle)); }, "i@:");
    dfb.method("setTimeZone:", [](Ctx& x) { df(x, x.cpu.r[0]).utc = isUtcZone(x, arg(x, 0)); }, "v@:@");
    for (const char* s : {"setLocale:", "setFormatterBehavior:", "setLenient:", "setCalendar:", "setGeneratesCalendarDates:",
                          "setDoesRelativeDateFormatting:", "setDefaultDate:", "setTwoDigitStartDate:", "setAMSymbol:", "setPMSymbol:"})
        dfb.method(s, [](Ctx&) {}, "v@:@");
    dfb.method("locale", [](Ctx& x) { ret(x, 0); });
    dfb.method("timeZone", [](Ctx& x) { ret(x, 0); });
    dfb.method("stringFromDate:", [](Ctx& x) {
        const id d = arg(x, 0);
        ret(x, d ? makeString(x, formatDate(df(x, x.cpu.r[0]), dateInterval(x, d))) : 0);
    }, "@@:@");
    dfb.alias("stringForObjectValue:", "stringFromDate:");
    dfb.method("dateFromString:", [](Ctx& x) {
        const id s = arg(x, 0);
        const auto t = s ? parseDate(df(x, x.cpu.r[0]), utf8(x, s)) : std::nullopt;
        ret(x, t ? makeDate(x, *t) : 0);
    }, "@@:@");
    dfb.classMethod("localizedStringFromDate:dateStyle:timeStyle:", [](Ctx& x) {
        DateFormatterData d;
        d.dateStyle = int32_t(arg(x, 1));
        d.timeStyle = int32_t(arg(x, 2));
        ret(x, arg(x, 0) ? makeString(x, formatDate(d, dateInterval(x, arg(x, 0)))) : 0);
    }, "@@:@ii");
    dfb.classMethod("dateFormatFromTemplate:options:locale:", [](Ctx& x) { ret(x, arg(x, 0)); }, "@@:@I@");
    (void)p;
}

} // namespace Hydrogen::Runtime::Foundation
