#include "runtime/android/dex/CoreLang.hpp"
#include "runtime/android/dex/DexFile.hpp"

#include <algorithm>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <thread>
#include <vector>

namespace Hydrogen::Runtime::Android::Dex {
namespace {

Value word(uint32_t bits) { return {ValueKind::Word, bits}; }
Value wide(uint64_t bits) { return {ValueKind::Wide, bits}; }
Value ref(uint32_t id) { return {ValueKind::Reference, id}; }
Value f32(float v) { return {ValueKind::Word, std::bit_cast<uint32_t>(v)}; }
Value f64(double v) { return {ValueKind::Wide, std::bit_cast<uint64_t>(v)}; }

int32_t asInt(Value v) { return std::bit_cast<int32_t>(uint32_t(v.bits)); }
int64_t asLong(Value v) { return std::bit_cast<int64_t>(v.bits); }
float asFloat(Value v) { return std::bit_cast<float>(uint32_t(v.bits)); }
double asDouble(Value v) { return std::bit_cast<double>(v.bits); }
uint32_t asRef(Value v) { return uint32_t(v.bits); }

std::u16string asciiToU16(std::string_view text) {
    std::u16string out;
    out.reserve(text.size());
    for (unsigned char c : text) out.push_back(char16_t(c));
    return out;
}

std::u16string utf8ToU16(std::string_view text) {
    std::u16string out;
    for (size_t pos = 0; pos < text.size();) {
        const auto first = uint8_t(text[pos++]);
        uint32_t ch = first;
        unsigned count = 0;
        if ((first & 0xe0) == 0xc0) { ch &= 31; count = 1; }
        else if ((first & 0xf0) == 0xe0) { ch &= 15; count = 2; }
        else if ((first & 0xf8) == 0xf0) { ch &= 7; count = 3; }
        else if (first > 127) { out.push_back(0xfffd); continue; }
        for (unsigned n = 0; n < count; ++n) {
            if (pos == text.size() || (uint8_t(text[pos]) & 0xc0) != 0x80) { ch = 0xfffd; break; }
            ch = (ch << 6) | (uint8_t(text[pos++]) & 63);
        }
        if (ch > 0x10ffff) ch = 0xfffd;
        if (ch > 0xffff) {
            ch -= 0x10000;
            out.push_back(char16_t(0xd800 + (ch >> 10)));
            ch = 0xdc00 + (ch & 1023);
        }
        out.push_back(char16_t(ch));
    }
    return out;
}

std::string u16ToUtf8(std::u16string_view text) {
    std::string out;
    for (size_t i = 0; i < text.size(); ++i) {
        uint32_t ch = text[i];
        if (ch >= 0xd800 && ch <= 0xdbff && i + 1 < text.size() &&
            text[i + 1] >= 0xdc00 && text[i + 1] <= 0xdfff) {
            ch = 0x10000 + (((ch - 0xd800) << 10) | (uint32_t(text[++i]) - 0xdc00));
        }
        if (ch < 0x80) out.push_back(char(ch));
        else if (ch < 0x800) {
            out.push_back(char(0xc0 | (ch >> 6)));
            out.push_back(char(0x80 | (ch & 0x3f)));
        } else if (ch < 0x10000) {
            out.push_back(char(0xe0 | (ch >> 12)));
            out.push_back(char(0x80 | ((ch >> 6) & 0x3f)));
            out.push_back(char(0x80 | (ch & 0x3f)));
        } else {
            out.push_back(char(0xf0 | (ch >> 18)));
            out.push_back(char(0x80 | ((ch >> 12) & 0x3f)));
            out.push_back(char(0x80 | ((ch >> 6) & 0x3f)));
            out.push_back(char(0x80 | (ch & 0x3f)));
        }
    }
    return out;
}

int digitValue(char16_t ch, int radix) {
    if (radix < 2 || radix > 36) return -1;
    int val = -1;
    if (ch >= u'0' && ch <= u'9') val = int(ch - u'0');
    else if (ch >= u'a' && ch <= u'z') val = 10 + int(ch - u'a');
    else if (ch >= u'A' && ch <= u'Z') val = 10 + int(ch - u'A');
    return (val >= 0 && val < radix) ? val : -1;
}

int64_t parseSignedLong(std::u16string_view text, int radix, int64_t minVal, int64_t maxVal) {
    if (text.empty() || radix < 2 || radix > 36)
        throw JavaException{u"Ljava/lang/NumberFormatException;"};
    size_t i = 0;
    bool negative = false;
    if (text[0] == u'-') {
        negative = true;
        i = 1;
    } else if (text[0] == u'+') {
        i = 1;
    }
    if (i == text.size()) throw JavaException{u"Ljava/lang/NumberFormatException;"};
    // Accumulate as negative to handle INT64_MIN cleanly without signed overflow.
    const int64_t limit = negative ? minVal : -maxVal;
    const int64_t multmin = limit / radix;
    int64_t result = 0;
    for (; i < text.size(); ++i) {
        const int d = digitValue(text[i], radix);
        if (d < 0) throw JavaException{u"Ljava/lang/NumberFormatException;"};
        if (result < multmin) throw JavaException{u"Ljava/lang/NumberFormatException;"};
        result *= radix;
        if (result < limit + d) throw JavaException{u"Ljava/lang/NumberFormatException;"};
        result -= d;
    }
    return negative ? result : -result;
}

std::u16string formatSignedLong(int64_t value, int radix) {
    if (radix < 2 || radix > 36) radix = 10;
    constexpr char16_t kDigits[] = u"0123456789abcdefghijklmnopqrstuvwxyz";
    const bool negative = value < 0;
    uint64_t mag = negative ? (0ULL - uint64_t(value)) : uint64_t(value);
    std::u16string out;
    do {
        out.push_back(kDigits[mag % uint64_t(radix)]);
        mag /= uint64_t(radix);
    } while (mag != 0);
    if (negative) out.push_back(u'-');
    std::reverse(out.begin(), out.end());
    return out;
}

std::u16string formatUnsignedHex(uint64_t value) {
    constexpr char16_t kDigits[] = u"0123456789abcdef";
    std::u16string out;
    do {
        out.push_back(kDigits[value & 0xf]);
        value >>= 4;
    } while (value != 0);
    std::reverse(out.begin(), out.end());
    return out;
}

std::u16string formatFloat(float v) {
    if (std::isnan(v)) return u"NaN";
    if (std::isinf(v)) return std::signbit(v) ? u"-Infinity" : u"Infinity";
    if (v == 0.0f) return std::signbit(v) ? u"-0.0" : u"0.0";
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%.7g", double(v));
    std::string s(buf);
    if (s.find('.') == std::string::npos && s.find('e') == std::string::npos && s.find('E') == std::string::npos)
        s += ".0";
    return asciiToU16(s);
}

std::u16string formatDouble(double v) {
    if (std::isnan(v)) return u"NaN";
    if (std::isinf(v)) return std::signbit(v) ? u"-Infinity" : u"Infinity";
    if (v == 0.0) return std::signbit(v) ? u"-0.0" : u"0.0";
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%.15g", v);
    std::string s(buf);
    if (s.find('.') == std::string::npos && s.find('e') == std::string::npos && s.find('E') == std::string::npos)
        s += ".0";
    return asciiToU16(s);
}

double parseJavaDouble(std::u16string_view text) {
    std::string s = u16ToUtf8(text);
    size_t start = 0, end = s.size();
    while (start < end && static_cast<unsigned char>(s[start]) <= 0x20) ++start;
    while (end > start && static_cast<unsigned char>(s[end - 1]) <= 0x20) --end;
    s = s.substr(start, end - start);
    if (s.empty()) throw JavaException{u"Ljava/lang/NumberFormatException;"};
    if (s == "NaN" || s == "+NaN" || s == "-NaN") return std::numeric_limits<double>::quiet_NaN();
    if (s == "Infinity" || s == "+Infinity") return std::numeric_limits<double>::infinity();
    if (s == "-Infinity") return -std::numeric_limits<double>::infinity();
    // Strip optional Java trailing type suffix f/F/d/D.
    if (s.size() > 1 && (s.back() == 'f' || s.back() == 'F' || s.back() == 'd' || s.back() == 'D'))
        s.pop_back();
    char* finish = nullptr;
    const double val = std::strtod(s.c_str(), &finish);
    if (finish != s.c_str() + s.size()) throw JavaException{u"Ljava/lang/NumberFormatException;"};
    return val;
}

template <typename T>
int32_t javaFloatCompare(T a, T b) {
    const bool aNan = std::isnan(a), bNan = std::isnan(b);
    if (aNan && bNan) return 0;
    if (aNan) return 1;
    if (bNan) return -1;
    if (a < b) return -1;
    if (a > b) return 1;
    const bool aNeg = std::signbit(a), bNeg = std::signbit(b);
    if (aNeg == bNeg) return 0;
    return aNeg ? -1 : 1;
}

uint32_t reverseBytes32(uint32_t x) {
    return ((x & 0x000000ffu) << 24) | ((x & 0x0000ff00u) << 8) |
           ((x & 0x00ff0000u) >> 8) | ((x & 0xff000000u) >> 24);
}

uint64_t reverseBytes64(uint64_t x) {
    return (uint64_t(reverseBytes32(uint32_t(x))) << 32) | uint64_t(reverseBytes32(uint32_t(x >> 32)));
}

char16_t asciiLower(char16_t c) { return (c >= u'A' && c <= u'Z') ? char16_t(c + 32) : c; }
char16_t asciiUpper(char16_t c) { return (c >= u'a' && c <= u'z') ? char16_t(c - 32) : c; }

bool isBoxedClass(std::u16string_view d) {
    return d == u"Ljava/lang/Integer;" || d == u"Ljava/lang/Long;" ||
           d == u"Ljava/lang/Float;" || d == u"Ljava/lang/Double;" ||
           d == u"Ljava/lang/Short;" || d == u"Ljava/lang/Byte;" ||
           d == u"Ljava/lang/Boolean;" || d == u"Ljava/lang/Character;" ||
           d == u"Ljava/lang/Number;";
}

bool isThrowableClass(std::u16string_view d) {
    return d == u"Ljava/lang/Throwable;" || d == u"Ljava/lang/Exception;" ||
           d == u"Ljava/lang/RuntimeException;" || d == u"Ljava/lang/Error;" ||
           d == u"Ljava/lang/IllegalArgumentException;" || d == u"Ljava/lang/IllegalStateException;" ||
           d == u"Ljava/lang/IndexOutOfBoundsException;" || d == u"Ljava/lang/NullPointerException;" ||
           d == u"Ljava/lang/UnsupportedOperationException;" || d == u"Ljava/lang/ClassCastException;" ||
           d == u"Ljava/lang/InterruptedException;" || d == u"Ljava/lang/NumberFormatException;" ||
           d == u"Ljava/lang/ArrayIndexOutOfBoundsException;" || d == u"Ljava/lang/StringIndexOutOfBoundsException;" ||
           d == u"Ljava/lang/ArithmeticException;" || d == u"Ljava/lang/AssertionError;";
}

} // namespace

bool CoreLang::hasClass(std::u16string_view d) {
    return isBoxedClass(d) || isThrowableClass(d) ||
           d == u"Ljava/lang/Object;" || d == u"Ljava/lang/String;" ||
           d == u"Ljava/lang/CharSequence;" || d == u"Ljava/lang/Comparable;" ||
           d == u"Ljava/lang/StringBuilder;" || d == u"Ljava/lang/StringBuffer;" ||
           d == u"Ljava/lang/System;" || d == u"Ljava/lang/Runtime;" ||
           d == u"Ljava/lang/Enum;" || d == u"Ljava/lang/Cloneable;" ||
           d == u"Ljava/io/Serializable;" || d == u"Ljava/lang/Iterable;" ||
           d == u"Ljava/lang/Runnable;";
}

std::optional<uint64_t> CoreLang::boxedValueLocked(uint32_t reference) const {
    if (const auto it = boxed_.find(reference); it != boxed_.end()) return it->second;
    return std::nullopt;
}

std::u16string CoreLang::charSequenceText(Heap& heap, uint32_t reference) {
    if (!reference) return u"null";
    const auto type = heap.typeOf(reference);
    if (type == u"Ljava/lang/String;") return heap.string(reference);
    if (type == u"Ljava/lang/StringBuilder;" || type == u"Ljava/lang/StringBuffer;") {
        std::lock_guard lock(mutex_);
        if (const auto it = builders_.find(reference); it != builders_.end()) return it->second;
        return {};
    }
    return formatObject(heap, reference);
}

std::u16string CoreLang::formatObject(Heap& heap, uint32_t reference) {
    if (!reference) return u"null";
    const auto type = heap.typeOf(reference);
    if (type == u"Ljava/lang/String;") return heap.string(reference);
    std::lock_guard lock(mutex_);
    if (type == u"Ljava/lang/StringBuilder;" || type == u"Ljava/lang/StringBuffer;") {
        if (const auto it = builders_.find(reference); it != builders_.end()) return it->second;
        return {};
    }
    if (const auto it = enums_.find(reference); it != enums_.end() && it->second.nameRef)
        return heap.string(it->second.nameRef);
    if (const auto val = boxedValueLocked(reference)) {
        if (type == u"Ljava/lang/Integer;") return formatSignedLong(std::bit_cast<int32_t>(uint32_t(*val)), 10);
        if (type == u"Ljava/lang/Long;") return formatSignedLong(std::bit_cast<int64_t>(*val), 10);
        if (type == u"Ljava/lang/Short;") return formatSignedLong(int16_t(uint16_t(*val)), 10);
        if (type == u"Ljava/lang/Byte;") return formatSignedLong(int8_t(uint8_t(*val)), 10);
        if (type == u"Ljava/lang/Boolean;") return (*val & 1u) ? u"true" : u"false";
        if (type == u"Ljava/lang/Character;") return std::u16string(1, char16_t(*val));
        if (type == u"Ljava/lang/Float;") return formatFloat(std::bit_cast<float>(uint32_t(*val)));
        if (type == u"Ljava/lang/Double;") return formatDouble(std::bit_cast<double>(*val));
    }
    std::u16string cls = type;
    if (cls.size() > 2 && cls.front() == u'L' && cls.back() == u';') {
        cls = cls.substr(1, cls.size() - 2);
        for (auto& c : cls) if (c == u'/') c = u'.';
    }
    return cls + u"@" + formatUnsignedHex(reference);
}

std::optional<Value> CoreLang::boxedMethod(Heap& heap, std::u16string_view owner,
                                           std::u16string_view name, std::u16string_view sig,
                                           bool isStatic, std::span<const Value> args) {
    // Static methods first.
    if (isStatic) {
        if (owner == u"Ljava/lang/Long;") {
            if (name == u"numberOfLeadingZeros" && sig == u"(J)I" && args.size() == 1)
                return word(uint32_t(std::countl_zero(args[0].bits)));
            if (name == u"numberOfTrailingZeros" && sig == u"(J)I" && args.size() == 1)
                return word(uint32_t(std::countr_zero(args[0].bits)));
            if (name == u"bitCount" && sig == u"(J)I" && args.size() == 1)
                return word(uint32_t(std::popcount(args[0].bits)));
            if (name == u"reverseBytes" && sig == u"(J)J" && args.size() == 1)
                return wide(reverseBytes64(args[0].bits));
            if (name == u"highestOneBit" && sig == u"(J)J" && args.size() == 1)
                return wide(args[0].bits ? (1ULL << (63 - std::countl_zero(args[0].bits))) : 0ULL);
            if (name == u"lowestOneBit" && sig == u"(J)J" && args.size() == 1)
                return wide(args[0].bits & (0ULL - args[0].bits));
            if (name == u"compare" && sig == u"(JJ)I" && args.size() == 2) {
                const int64_t a = asLong(args[0]), b = asLong(args[1]);
                return word(uint32_t(a < b ? -1 : (a > b ? 1 : 0)));
            }
            if (name == u"toString" && sig == u"(J)Ljava/lang/String;" && args.size() == 1)
                return ref(heap.newString(formatSignedLong(asLong(args[0]), 10)));
            if (name == u"toString" && sig == u"(JI)Ljava/lang/String;" && args.size() == 2)
                return ref(heap.newString(formatSignedLong(asLong(args[0]), asInt(args[1]))));
            if (name == u"toHexString" && sig == u"(J)Ljava/lang/String;" && args.size() == 1)
                return ref(heap.newString(formatUnsignedHex(args[0].bits)));
            if (name == u"parseLong" && sig == u"(Ljava/lang/String;)J" && args.size() == 1) {
                const uint32_t s = asRef(args[0]);
                if (!s) throw JavaException{u"Ljava/lang/NumberFormatException;"};
                return wide(uint64_t(parseSignedLong(heap.string(s), 10, INT64_MIN, INT64_MAX)));
            }
            if (name == u"parseLong" && sig == u"(Ljava/lang/String;I)J" && args.size() == 2) {
                const uint32_t s = asRef(args[0]);
                if (!s) throw JavaException{u"Ljava/lang/NumberFormatException;"};
                return wide(uint64_t(parseSignedLong(heap.string(s), asInt(args[1]), INT64_MIN, INT64_MAX)));
            }
            if (name == u"valueOf" && sig == u"(J)Ljava/lang/Long;" && args.size() == 1) {
                const uint32_t id = heap.object(u"Ljava/lang/Long;");
                std::lock_guard lock(mutex_);
                boxed_[id] = args[0].bits;
                return ref(id);
            }
            if (name == u"valueOf" && sig == u"(Ljava/lang/String;)Ljava/lang/Long;" && args.size() == 1) {
                const uint32_t s = asRef(args[0]);
                if (!s) throw JavaException{u"Ljava/lang/NumberFormatException;"};
                const int64_t v = parseSignedLong(heap.string(s), 10, INT64_MIN, INT64_MAX);
                const uint32_t id = heap.object(u"Ljava/lang/Long;");
                std::lock_guard lock(mutex_);
                boxed_[id] = uint64_t(v);
                return ref(id);
            }
        }
        if (owner == u"Ljava/lang/Integer;") {
            if (name == u"numberOfLeadingZeros" && sig == u"(I)I" && args.size() == 1)
                return word(uint32_t(std::countl_zero(uint32_t(args[0].bits))));
            if (name == u"numberOfTrailingZeros" && sig == u"(I)I" && args.size() == 1)
                return word(uint32_t(std::countr_zero(uint32_t(args[0].bits))));
            if (name == u"bitCount" && sig == u"(I)I" && args.size() == 1)
                return word(uint32_t(std::popcount(uint32_t(args[0].bits))));
            if (name == u"reverseBytes" && sig == u"(I)I" && args.size() == 1)
                return word(reverseBytes32(uint32_t(args[0].bits)));
            if (name == u"highestOneBit" && sig == u"(I)I" && args.size() == 1) {
                const uint32_t u = uint32_t(args[0].bits);
                return word(u ? (1u << (31 - std::countl_zero(u))) : 0u);
            }
            if (name == u"lowestOneBit" && sig == u"(I)I" && args.size() == 1) {
                const uint32_t u = uint32_t(args[0].bits);
                return word(u & (0u - u));
            }
            if (name == u"compare" && sig == u"(II)I" && args.size() == 2) {
                const int32_t a = asInt(args[0]), b = asInt(args[1]);
                return word(uint32_t(a < b ? -1 : (a > b ? 1 : 0)));
            }
            if (name == u"toString" && sig == u"(I)Ljava/lang/String;" && args.size() == 1)
                return ref(heap.newString(formatSignedLong(asInt(args[0]), 10)));
            if (name == u"toString" && sig == u"(II)Ljava/lang/String;" && args.size() == 2)
                return ref(heap.newString(formatSignedLong(asInt(args[0]), asInt(args[1]))));
            if (name == u"toHexString" && sig == u"(I)Ljava/lang/String;" && args.size() == 1)
                return ref(heap.newString(formatUnsignedHex(uint32_t(args[0].bits))));
            if (name == u"parseInt" && sig == u"(Ljava/lang/String;)I" && args.size() == 1) {
                const uint32_t s = asRef(args[0]);
                if (!s) throw JavaException{u"Ljava/lang/NumberFormatException;"};
                return word(uint32_t(int32_t(parseSignedLong(heap.string(s), 10, INT32_MIN, INT32_MAX))));
            }
            if (name == u"parseInt" && sig == u"(Ljava/lang/String;I)I" && args.size() == 2) {
                const uint32_t s = asRef(args[0]);
                if (!s) throw JavaException{u"Ljava/lang/NumberFormatException;"};
                return word(uint32_t(int32_t(parseSignedLong(heap.string(s), asInt(args[1]), INT32_MIN, INT32_MAX))));
            }
            if (name == u"valueOf" && sig == u"(I)Ljava/lang/Integer;" && args.size() == 1) {
                const uint32_t id = heap.object(u"Ljava/lang/Integer;");
                std::lock_guard lock(mutex_);
                boxed_[id] = uint32_t(args[0].bits);
                return ref(id);
            }
            if (name == u"valueOf" && sig == u"(Ljava/lang/String;)Ljava/lang/Integer;" && args.size() == 1) {
                const uint32_t s = asRef(args[0]);
                if (!s) throw JavaException{u"Ljava/lang/NumberFormatException;"};
                const int32_t v = int32_t(parseSignedLong(heap.string(s), 10, INT32_MIN, INT32_MAX));
                const uint32_t id = heap.object(u"Ljava/lang/Integer;");
                std::lock_guard lock(mutex_);
                boxed_[id] = uint32_t(v);
                return ref(id);
            }
        }
        if (owner == u"Ljava/lang/Float;") {
            if (name == u"floatToIntBits" && sig == u"(F)I" && args.size() == 1) {
                const uint32_t bits = uint32_t(args[0].bits);
                return word(std::isnan(std::bit_cast<float>(bits)) ? 0x7fc00000u : bits);
            }
            if (name == u"floatToRawIntBits" && sig == u"(F)I" && args.size() == 1)
                return word(uint32_t(args[0].bits));
            if (name == u"intBitsToFloat" && sig == u"(I)F" && args.size() == 1)
                return word(uint32_t(args[0].bits));
            if (name == u"isNaN" && sig == u"(F)Z" && args.size() == 1)
                return word(std::isnan(asFloat(args[0])) ? 1u : 0u);
            if (name == u"isInfinite" && sig == u"(F)Z" && args.size() == 1)
                return word(std::isinf(asFloat(args[0])) ? 1u : 0u);
            if (name == u"compare" && sig == u"(FF)I" && args.size() == 2)
                return word(uint32_t(javaFloatCompare(asFloat(args[0]), asFloat(args[1]))));
            if (name == u"parseFloat" && sig == u"(Ljava/lang/String;)F" && args.size() == 1) {
                const uint32_t s = asRef(args[0]);
                if (!s) throw JavaException{u"Ljava/lang/NullPointerException;"};
                return f32(float(parseJavaDouble(heap.string(s))));
            }
            if (name == u"toString" && sig == u"(F)Ljava/lang/String;" && args.size() == 1)
                return ref(heap.newString(formatFloat(asFloat(args[0]))));
            if (name == u"valueOf" && sig == u"(F)Ljava/lang/Float;" && args.size() == 1) {
                const uint32_t id = heap.object(u"Ljava/lang/Float;");
                std::lock_guard lock(mutex_);
                boxed_[id] = uint32_t(args[0].bits);
                return ref(id);
            }
            if (name == u"valueOf" && sig == u"(Ljava/lang/String;)Ljava/lang/Float;" && args.size() == 1) {
                const uint32_t s = asRef(args[0]);
                if (!s) throw JavaException{u"Ljava/lang/NullPointerException;"};
                const float v = float(parseJavaDouble(heap.string(s)));
                const uint32_t id = heap.object(u"Ljava/lang/Float;");
                std::lock_guard lock(mutex_);
                boxed_[id] = std::bit_cast<uint32_t>(v);
                return ref(id);
            }
        }
        if (owner == u"Ljava/lang/Double;") {
            if (name == u"doubleToLongBits" && sig == u"(D)J" && args.size() == 1) {
                const uint64_t bits = args[0].bits;
                return wide(std::isnan(std::bit_cast<double>(bits)) ? 0x7ff8000000000000ULL : bits);
            }
            if (name == u"doubleToRawLongBits" && sig == u"(D)J" && args.size() == 1)
                return wide(args[0].bits);
            if (name == u"longBitsToDouble" && sig == u"(J)D" && args.size() == 1)
                return wide(args[0].bits);
            if (name == u"isNaN" && sig == u"(D)Z" && args.size() == 1)
                return word(std::isnan(asDouble(args[0])) ? 1u : 0u);
            if (name == u"isInfinite" && sig == u"(D)Z" && args.size() == 1)
                return word(std::isinf(asDouble(args[0])) ? 1u : 0u);
            if (name == u"compare" && sig == u"(DD)I" && args.size() == 2)
                return word(uint32_t(javaFloatCompare(asDouble(args[0]), asDouble(args[1]))));
            if (name == u"parseDouble" && sig == u"(Ljava/lang/String;)D" && args.size() == 1) {
                const uint32_t s = asRef(args[0]);
                if (!s) throw JavaException{u"Ljava/lang/NullPointerException;"};
                return f64(parseJavaDouble(heap.string(s)));
            }
            if (name == u"toString" && sig == u"(D)Ljava/lang/String;" && args.size() == 1)
                return ref(heap.newString(formatDouble(asDouble(args[0]))));
            if (name == u"valueOf" && sig == u"(D)Ljava/lang/Double;" && args.size() == 1) {
                const uint32_t id = heap.object(u"Ljava/lang/Double;");
                std::lock_guard lock(mutex_);
                boxed_[id] = args[0].bits;
                return ref(id);
            }
            if (name == u"valueOf" && sig == u"(Ljava/lang/String;)Ljava/lang/Double;" && args.size() == 1) {
                const uint32_t s = asRef(args[0]);
                if (!s) throw JavaException{u"Ljava/lang/NullPointerException;"};
                const double v = parseJavaDouble(heap.string(s));
                const uint32_t id = heap.object(u"Ljava/lang/Double;");
                std::lock_guard lock(mutex_);
                boxed_[id] = std::bit_cast<uint64_t>(v);
                return ref(id);
            }
        }
        if (owner == u"Ljava/lang/Short;") {
            if (name == u"parseShort" && sig == u"(Ljava/lang/String;)S" && args.size() == 1) {
                const uint32_t s = asRef(args[0]);
                if (!s) throw JavaException{u"Ljava/lang/NumberFormatException;"};
                return word(uint32_t(int32_t(int16_t(parseSignedLong(heap.string(s), 10, INT16_MIN, INT16_MAX)))));
            }
            if (name == u"parseShort" && sig == u"(Ljava/lang/String;I)S" && args.size() == 2) {
                const uint32_t s = asRef(args[0]);
                if (!s) throw JavaException{u"Ljava/lang/NumberFormatException;"};
                return word(uint32_t(int32_t(int16_t(parseSignedLong(heap.string(s), asInt(args[1]), INT16_MIN, INT16_MAX)))));
            }
            if (name == u"toString" && sig == u"(S)Ljava/lang/String;" && args.size() == 1)
                return ref(heap.newString(formatSignedLong(int16_t(uint16_t(args[0].bits)), 10)));
            if (name == u"valueOf" && sig == u"(S)Ljava/lang/Short;" && args.size() == 1) {
                const uint32_t id = heap.object(u"Ljava/lang/Short;");
                std::lock_guard lock(mutex_);
                boxed_[id] = uint32_t(int32_t(int16_t(uint16_t(args[0].bits))));
                return ref(id);
            }
            if (name == u"valueOf" && sig == u"(Ljava/lang/String;)Ljava/lang/Short;" && args.size() == 1) {
                const uint32_t s = asRef(args[0]);
                if (!s) throw JavaException{u"Ljava/lang/NumberFormatException;"};
                const int16_t v = int16_t(parseSignedLong(heap.string(s), 10, INT16_MIN, INT16_MAX));
                const uint32_t id = heap.object(u"Ljava/lang/Short;");
                std::lock_guard lock(mutex_);
                boxed_[id] = uint32_t(int32_t(v));
                return ref(id);
            }
        }
        if (owner == u"Ljava/lang/Byte;") {
            if (name == u"parseByte" && sig == u"(Ljava/lang/String;)B" && args.size() == 1) {
                const uint32_t s = asRef(args[0]);
                if (!s) throw JavaException{u"Ljava/lang/NumberFormatException;"};
                return word(uint32_t(int32_t(int8_t(parseSignedLong(heap.string(s), 10, INT8_MIN, INT8_MAX)))));
            }
            if (name == u"toString" && sig == u"(B)Ljava/lang/String;" && args.size() == 1)
                return ref(heap.newString(formatSignedLong(int8_t(uint8_t(args[0].bits)), 10)));
            if (name == u"valueOf" && sig == u"(B)Ljava/lang/Byte;" && args.size() == 1) {
                const uint32_t id = heap.object(u"Ljava/lang/Byte;");
                std::lock_guard lock(mutex_);
                boxed_[id] = uint32_t(int32_t(int8_t(uint8_t(args[0].bits))));
                return ref(id);
            }
        }
        if (owner == u"Ljava/lang/Boolean;") {
            if (name == u"parseBoolean" && sig == u"(Ljava/lang/String;)Z" && args.size() == 1) {
                const uint32_t s = asRef(args[0]);
                if (!s) return word(0);
                std::u16string text = heap.string(s);
                for (auto& c : text) c = asciiLower(c);
                return word(text == u"true" ? 1u : 0u);
            }
            if (name == u"toString" && sig == u"(Z)Ljava/lang/String;" && args.size() == 1)
                return ref(heap.newString((args[0].bits & 1u) ? u"true" : u"false"));
            if (name == u"valueOf" && sig == u"(Z)Ljava/lang/Boolean;" && args.size() == 1) {
                const uint32_t id = heap.object(u"Ljava/lang/Boolean;");
                std::lock_guard lock(mutex_);
                boxed_[id] = args[0].bits & 1u;
                return ref(id);
            }
            if (name == u"valueOf" && sig == u"(Ljava/lang/String;)Ljava/lang/Boolean;" && args.size() == 1) {
                const uint32_t s = asRef(args[0]);
                bool truth = false;
                if (s) {
                    std::u16string text = heap.string(s);
                    for (auto& c : text) c = asciiLower(c);
                    truth = (text == u"true");
                }
                const uint32_t id = heap.object(u"Ljava/lang/Boolean;");
                std::lock_guard lock(mutex_);
                boxed_[id] = truth ? 1u : 0u;
                return ref(id);
            }
        }
        if (owner == u"Ljava/lang/Character;") {
            if (name == u"valueOf" && sig == u"(C)Ljava/lang/Character;" && args.size() == 1) {
                const uint32_t id = heap.object(u"Ljava/lang/Character;");
                std::lock_guard lock(mutex_);
                boxed_[id] = uint16_t(args[0].bits);
                return ref(id);
            }
            if (name == u"toString" && sig == u"(C)Ljava/lang/String;" && args.size() == 1)
                return ref(heap.newString(std::u16string(1, char16_t(args[0].bits))));
            if (name == u"isDigit" && sig == u"(C)Z" && args.size() == 1) {
                const char16_t c = char16_t(args[0].bits);
                return word((c >= u'0' && c <= u'9') ? 1u : 0u);
            }
            if (name == u"isLetter" && sig == u"(C)Z" && args.size() == 1) {
                const char16_t c = char16_t(args[0].bits);
                return word(((c >= u'a' && c <= u'z') || (c >= u'A' && c <= u'Z')) ? 1u : 0u);
            }
            if (name == u"isLetterOrDigit" && sig == u"(C)Z" && args.size() == 1) {
                const char16_t c = char16_t(args[0].bits);
                return word(((c >= u'a' && c <= u'z') || (c >= u'A' && c <= u'Z') || (c >= u'0' && c <= u'9')) ? 1u : 0u);
            }
            if (name == u"isWhitespace" && sig == u"(C)Z" && args.size() == 1) {
                const char16_t c = char16_t(args[0].bits);
                return word((c == u' ' || c == u'\t' || c == u'\n' || c == u'\r' || c == u'\f' || c == 0x0b) ? 1u : 0u);
            }
            if (name == u"isUpperCase" && sig == u"(C)Z" && args.size() == 1) {
                const char16_t c = char16_t(args[0].bits);
                return word((c >= u'A' && c <= u'Z') ? 1u : 0u);
            }
            if (name == u"isLowerCase" && sig == u"(C)Z" && args.size() == 1) {
                const char16_t c = char16_t(args[0].bits);
                return word((c >= u'a' && c <= u'z') ? 1u : 0u);
            }
            if (name == u"toLowerCase" && sig == u"(C)C" && args.size() == 1)
                return word(asciiLower(char16_t(args[0].bits)));
            if (name == u"toUpperCase" && sig == u"(C)C" && args.size() == 1)
                return word(asciiUpper(char16_t(args[0].bits)));
            if (name == u"digit" && sig == u"(CI)I" && args.size() == 2)
                return word(uint32_t(digitValue(char16_t(args[0].bits), asInt(args[1]))));
        }
        return std::nullopt;
    }

    // Instance methods on boxed wrappers and Number.
    if (args.empty()) return std::nullopt;
    const uint32_t self = asRef(args[0]);
    if (!self) throw JavaException{u"Ljava/lang/NullPointerException;"};
    const auto actual = heap.typeOf(self);

    if (name == u"<init>" && args.size() == 2) {
        uint64_t stored = 0;
        if (owner == u"Ljava/lang/Integer;" && sig == u"(I)V") stored = uint32_t(args[1].bits);
        else if (owner == u"Ljava/lang/Long;" && sig == u"(J)V") stored = args[1].bits;
        else if (owner == u"Ljava/lang/Long;" && sig == u"(Ljava/lang/String;)V") {
            const uint32_t s = asRef(args[1]);
            if (!s) throw JavaException{u"Ljava/lang/NumberFormatException;"};
            stored = uint64_t(parseSignedLong(heap.string(s), 10, INT64_MIN, INT64_MAX));
        } else if (owner == u"Ljava/lang/Integer;" && sig == u"(Ljava/lang/String;)V") {
            const uint32_t s = asRef(args[1]);
            if (!s) throw JavaException{u"Ljava/lang/NumberFormatException;"};
            stored = uint32_t(int32_t(parseSignedLong(heap.string(s), 10, INT32_MIN, INT32_MAX)));
        } else if (owner == u"Ljava/lang/Float;" && sig == u"(F)V") stored = uint32_t(args[1].bits);
        else if (owner == u"Ljava/lang/Double;" && sig == u"(D)V") stored = args[1].bits;
        else if (owner == u"Ljava/lang/Short;" && sig == u"(S)V") stored = uint32_t(int32_t(int16_t(uint16_t(args[1].bits))));
        else if (owner == u"Ljava/lang/Byte;" && sig == u"(B)V") stored = uint32_t(int32_t(int8_t(uint8_t(args[1].bits))));
        else if (owner == u"Ljava/lang/Boolean;" && sig == u"(Z)V") stored = args[1].bits & 1u;
        else if (owner == u"Ljava/lang/Character;" && sig == u"(C)V") stored = uint16_t(args[1].bits);
        else return std::nullopt;
        std::lock_guard lock(mutex_);
        boxed_[self] = stored;
        return Value{ValueKind::Void};
    }

    uint64_t raw = 0;
    {
        std::lock_guard lock(mutex_);
        if (const auto v = boxedValueLocked(self)) raw = *v;
    }
    auto asDoubleVal = [&]() -> double {
        if (actual == u"Ljava/lang/Integer;") return double(std::bit_cast<int32_t>(uint32_t(raw)));
        if (actual == u"Ljava/lang/Long;") return double(std::bit_cast<int64_t>(raw));
        if (actual == u"Ljava/lang/Short;") return double(int16_t(uint16_t(raw)));
        if (actual == u"Ljava/lang/Byte;") return double(int8_t(uint8_t(raw)));
        if (actual == u"Ljava/lang/Float;") return double(std::bit_cast<float>(uint32_t(raw)));
        if (actual == u"Ljava/lang/Double;") return std::bit_cast<double>(raw);
        return 0.0;
    };
    auto asLongVal = [&]() -> int64_t {
        if (actual == u"Ljava/lang/Integer;") return int64_t(std::bit_cast<int32_t>(uint32_t(raw)));
        if (actual == u"Ljava/lang/Long;") return std::bit_cast<int64_t>(raw);
        if (actual == u"Ljava/lang/Short;") return int64_t(int16_t(uint16_t(raw)));
        if (actual == u"Ljava/lang/Byte;") return int64_t(int8_t(uint8_t(raw)));
        if (actual == u"Ljava/lang/Float;") return int64_t(std::bit_cast<float>(uint32_t(raw)));
        if (actual == u"Ljava/lang/Double;") return int64_t(std::bit_cast<double>(raw));
        return 0;
    };

    if (name == u"intValue" && sig == u"()I") return word(uint32_t(int32_t(asLongVal())));
    if (name == u"longValue" && sig == u"()J") return wide(uint64_t(asLongVal()));
    if (name == u"shortValue" && sig == u"()S") return word(uint32_t(int32_t(int16_t(asLongVal()))));
    if (name == u"byteValue" && sig == u"()B") return word(uint32_t(int32_t(int8_t(asLongVal()))));
    if (name == u"floatValue" && sig == u"()F")
        return f32(actual == u"Ljava/lang/Float;" ? std::bit_cast<float>(uint32_t(raw)) : float(asDoubleVal()));
    if (name == u"doubleValue" && sig == u"()D") return f64(asDoubleVal());
    if (name == u"booleanValue" && sig == u"()Z") return word(uint32_t(raw & 1u));
    if (name == u"charValue" && sig == u"()C") return word(uint16_t(raw));
    if (name == u"isNaN" && sig == u"()Z") {
        if (actual == u"Ljava/lang/Float;") return word(std::isnan(std::bit_cast<float>(uint32_t(raw))) ? 1u : 0u);
        if (actual == u"Ljava/lang/Double;") return word(std::isnan(std::bit_cast<double>(raw)) ? 1u : 0u);
    }
    if (name == u"isInfinite" && sig == u"()Z") {
        if (actual == u"Ljava/lang/Float;") return word(std::isinf(std::bit_cast<float>(uint32_t(raw))) ? 1u : 0u);
        if (actual == u"Ljava/lang/Double;") return word(std::isinf(std::bit_cast<double>(raw)) ? 1u : 0u);
    }
    if (name == u"toString" && sig == u"()Ljava/lang/String;")
        return ref(heap.newString(formatObject(heap, self)));
    if (name == u"equals" && sig == u"(Ljava/lang/Object;)Z" && args.size() == 2) {
        const uint32_t other = asRef(args[1]);
        if (!other || heap.typeOf(other) != actual) return word(0);
        std::lock_guard lock(mutex_);
        const auto otherVal = boxedValueLocked(other).value_or(0);
        if (actual == u"Ljava/lang/Float;") {
            const uint32_t a = std::isnan(std::bit_cast<float>(uint32_t(raw))) ? 0x7fc00000u : uint32_t(raw);
            const uint32_t b = std::isnan(std::bit_cast<float>(uint32_t(otherVal))) ? 0x7fc00000u : uint32_t(otherVal);
            return word(a == b ? 1u : 0u);
        }
        if (actual == u"Ljava/lang/Double;") {
            const uint64_t a = std::isnan(std::bit_cast<double>(raw)) ? 0x7ff8000000000000ULL : raw;
            const uint64_t b = std::isnan(std::bit_cast<double>(otherVal)) ? 0x7ff8000000000000ULL : otherVal;
            return word(a == b ? 1u : 0u);
        }
        return word(raw == otherVal ? 1u : 0u);
    }
    if (name == u"hashCode" && sig == u"()I") {
        if (actual == u"Ljava/lang/Long;" || actual == u"Ljava/lang/Double;")
            return word(uint32_t(raw ^ (raw >> 32)));
        if (actual == u"Ljava/lang/Boolean;") return word((raw & 1u) ? 1231u : 1237u);
        return word(uint32_t(raw));
    }
    return std::nullopt;
}

std::optional<Value> CoreLang::stringMethod(Heap& heap, std::u16string_view name,
                                            std::u16string_view sig, bool isStatic,
                                            std::span<const Value> args) {
    if (isStatic) {
        if (name == u"valueOf" && args.size() == 1) {
            if (sig == u"(C)Ljava/lang/String;")
                return ref(heap.newString(std::u16string(1, char16_t(args[0].bits))));
            if (sig == u"(I)Ljava/lang/String;")
                return ref(heap.newString(formatSignedLong(asInt(args[0]), 10)));
            if (sig == u"(J)Ljava/lang/String;")
                return ref(heap.newString(formatSignedLong(asLong(args[0]), 10)));
            if (sig == u"(F)Ljava/lang/String;")
                return ref(heap.newString(formatFloat(asFloat(args[0]))));
            if (sig == u"(D)Ljava/lang/String;")
                return ref(heap.newString(formatDouble(asDouble(args[0]))));
            if (sig == u"(Z)Ljava/lang/String;")
                return ref(heap.newString((args[0].bits & 1u) ? u"true" : u"false"));
            if (sig == u"(Ljava/lang/Object;)Ljava/lang/String;")
                return ref(heap.newString(formatObject(heap, asRef(args[0]))));
            if (sig == u"([C)Ljava/lang/String;") {
                const uint32_t arr = asRef(args[0]);
                if (!arr) throw JavaException{u"Ljava/lang/NullPointerException;"};
                const uint32_t n = heap.length(arr);
                std::u16string out;
                out.reserve(n);
                for (uint32_t i = 0; i < n; ++i) out.push_back(char16_t(heap.element(arr, int32_t(i)).bits));
                return ref(heap.newString(std::move(out)));
            }
        }
        return std::nullopt;
    }

    if (args.empty()) return std::nullopt;
    const uint32_t self = asRef(args[0]);
    if (!self) throw JavaException{u"Ljava/lang/NullPointerException;"};

    if (name == u"<init>") {
        if (sig == u"()V") { heap.setString(self, {}); return Value{ValueKind::Void}; }
        if (sig == u"(Ljava/lang/String;)V" && args.size() == 2) {
            const uint32_t src = asRef(args[1]);
            if (!src) throw JavaException{u"Ljava/lang/NullPointerException;"};
            heap.setString(self, heap.string(src));
            return Value{ValueKind::Void};
        }
        if (sig == u"([C)V" && args.size() == 2) {
            const uint32_t arr = asRef(args[1]);
            if (!arr) throw JavaException{u"Ljava/lang/NullPointerException;"};
            const uint32_t n = heap.length(arr);
            std::u16string out;
            out.reserve(n);
            for (uint32_t i = 0; i < n; ++i) out.push_back(char16_t(heap.element(arr, int32_t(i)).bits));
            heap.setString(self, std::move(out));
            return Value{ValueKind::Void};
        }
        if (sig == u"([CII)V" && args.size() == 4) {
            const uint32_t arr = asRef(args[1]);
            if (!arr) throw JavaException{u"Ljava/lang/NullPointerException;"};
            const int32_t off = asInt(args[2]), cnt = asInt(args[3]);
            const int64_t len = int64_t(heap.length(arr));
            if (off < 0 || cnt < 0 || int64_t(off) + int64_t(cnt) > len)
                throw JavaException{u"Ljava/lang/StringIndexOutOfBoundsException;"};
            std::u16string out;
            out.reserve(size_t(cnt));
            for (int32_t i = 0; i < cnt; ++i) out.push_back(char16_t(heap.element(arr, off + i).bits));
            heap.setString(self, std::move(out));
            return Value{ValueKind::Void};
        }
        if ((sig == u"([B)V" || sig == u"([BLjava/nio/charset/Charset;)V" || sig == u"([BLjava/lang/String;)V") &&
            args.size() >= 2) {
            const uint32_t arr = asRef(args[1]);
            if (!arr) throw JavaException{u"Ljava/lang/NullPointerException;"};
            const uint32_t n = heap.length(arr);
            std::string bytes(n, '\0');
            for (uint32_t i = 0; i < n; ++i) bytes[i] = char(uint8_t(heap.element(arr, int32_t(i)).bits));
            heap.setString(self, utf8ToU16(bytes));
            return Value{ValueKind::Void};
        }
        if ((sig == u"([BII)V" || sig == u"([BIILjava/nio/charset/Charset;)V" || sig == u"([BIILjava/lang/String;)V") &&
            args.size() >= 4) {
            const uint32_t arr = asRef(args[1]);
            if (!arr) throw JavaException{u"Ljava/lang/NullPointerException;"};
            const int32_t off = asInt(args[2]), cnt = asInt(args[3]);
            const int64_t len = int64_t(heap.length(arr));
            if (off < 0 || cnt < 0 || int64_t(off) + int64_t(cnt) > len)
                throw JavaException{u"Ljava/lang/StringIndexOutOfBoundsException;"};
            std::string bytes(size_t(cnt), '\0');
            for (int32_t i = 0; i < cnt; ++i) bytes[size_t(i)] = char(uint8_t(heap.element(arr, off + i).bits));
            heap.setString(self, utf8ToU16(bytes));
            return Value{ValueKind::Void};
        }
        return std::nullopt;
    }

    const std::u16string text = heap.string(self);
    if (name == u"length" && sig == u"()I") return word(uint32_t(text.size()));
    if (name == u"isEmpty" && sig == u"()Z") return word(text.empty() ? 1u : 0u);
    if (name == u"toString" && sig == u"()Ljava/lang/String;") return ref(self);
    if (name == u"charAt" && sig == u"(I)C" && args.size() == 2) {
        const int32_t i = asInt(args[1]);
        if (i < 0 || size_t(i) >= text.size()) throw JavaException{u"Ljava/lang/StringIndexOutOfBoundsException;"};
        return word(text[size_t(i)]);
    }
    if (name == u"hashCode" && sig == u"()I") {
        uint32_t h = 0;
        for (char16_t c : text) h = h * 31u + uint32_t(c);
        return word(h);
    }
    if (name == u"equals" && sig == u"(Ljava/lang/Object;)Z" && args.size() == 2) {
        const uint32_t other = asRef(args[1]);
        if (!other || heap.typeOf(other) != u"Ljava/lang/String;") return word(0);
        return word(text == heap.string(other) ? 1u : 0u);
    }
    if (name == u"equalsIgnoreCase" && sig == u"(Ljava/lang/String;)Z" && args.size() == 2) {
        const uint32_t other = asRef(args[1]);
        if (!other) return word(0);
        const auto rhs = heap.string(other);
        if (text.size() != rhs.size()) return word(0);
        for (size_t i = 0; i < text.size(); ++i)
            if (asciiLower(text[i]) != asciiLower(rhs[i])) return word(0);
        return word(1);
    }
    if (name == u"compareTo" && sig == u"(Ljava/lang/String;)I" && args.size() == 2) {
        const uint32_t other = asRef(args[1]);
        if (!other) throw JavaException{u"Ljava/lang/NullPointerException;"};
        const auto rhs = heap.string(other);
        const size_t lim = std::min(text.size(), rhs.size());
        for (size_t i = 0; i < lim; ++i)
            if (text[i] != rhs[i]) return word(uint32_t(int32_t(text[i]) - int32_t(rhs[i])));
        return word(uint32_t(int32_t(text.size()) - int32_t(rhs.size())));
    }
    if (name == u"concat" && sig == u"(Ljava/lang/String;)Ljava/lang/String;" && args.size() == 2) {
        const uint32_t other = asRef(args[1]);
        if (!other) throw JavaException{u"Ljava/lang/NullPointerException;"};
        const auto rhs = heap.string(other);
        if (rhs.empty()) return ref(self);
        return ref(heap.newString(text + rhs));
    }
    if (name == u"contains" && sig == u"(Ljava/lang/CharSequence;)Z" && args.size() == 2) {
        const uint32_t other = asRef(args[1]);
        if (!other) throw JavaException{u"Ljava/lang/NullPointerException;"};
        return word(text.find(charSequenceText(heap, other)) != std::u16string::npos ? 1u : 0u);
    }
    if (name == u"startsWith" && sig == u"(Ljava/lang/String;)Z" && args.size() == 2) {
        const uint32_t other = asRef(args[1]);
        if (!other) throw JavaException{u"Ljava/lang/NullPointerException;"};
        return word(text.starts_with(heap.string(other)) ? 1u : 0u);
    }
    if (name == u"startsWith" && sig == u"(Ljava/lang/String;I)Z" && args.size() == 3) {
        const uint32_t other = asRef(args[1]);
        if (!other) throw JavaException{u"Ljava/lang/NullPointerException;"};
        const int32_t off = asInt(args[2]);
        const auto prefix = heap.string(other);
        if (off < 0 || size_t(off) > text.size()) return word(0);
        return word(std::u16string_view(text).substr(size_t(off)).starts_with(prefix) ? 1u : 0u);
    }
    if (name == u"endsWith" && sig == u"(Ljava/lang/String;)Z" && args.size() == 2) {
        const uint32_t other = asRef(args[1]);
        if (!other) throw JavaException{u"Ljava/lang/NullPointerException;"};
        return word(text.ends_with(heap.string(other)) ? 1u : 0u);
    }
    if (name == u"indexOf" && sig == u"(I)I" && args.size() == 2) {
        const int32_t ch = asInt(args[1]);
        if (ch < 0 || ch > 0xffff) return word(uint32_t(-1));
        const auto pos = text.find(char16_t(ch));
        return word(pos == std::u16string::npos ? uint32_t(-1) : uint32_t(pos));
    }
    if (name == u"indexOf" && sig == u"(II)I" && args.size() == 3) {
        const int32_t ch = asInt(args[1]);
        const int32_t from = std::max(0, asInt(args[2]));
        if (ch < 0 || ch > 0xffff || size_t(from) >= text.size()) return word(uint32_t(-1));
        const auto pos = text.find(char16_t(ch), size_t(from));
        return word(pos == std::u16string::npos ? uint32_t(-1) : uint32_t(pos));
    }
    if (name == u"indexOf" && sig == u"(Ljava/lang/String;)I" && args.size() == 2) {
        const uint32_t other = asRef(args[1]);
        if (!other) throw JavaException{u"Ljava/lang/NullPointerException;"};
        const auto pos = text.find(heap.string(other));
        return word(pos == std::u16string::npos ? uint32_t(-1) : uint32_t(pos));
    }
    if (name == u"indexOf" && sig == u"(Ljava/lang/String;I)I" && args.size() == 3) {
        const uint32_t other = asRef(args[1]);
        if (!other) throw JavaException{u"Ljava/lang/NullPointerException;"};
        const int32_t from = std::max(0, asInt(args[2]));
        if (size_t(from) > text.size()) return word(uint32_t(-1));
        const auto pos = text.find(heap.string(other), size_t(from));
        return word(pos == std::u16string::npos ? uint32_t(-1) : uint32_t(pos));
    }
    if (name == u"lastIndexOf" && sig == u"(I)I" && args.size() == 2) {
        const int32_t ch = asInt(args[1]);
        if (ch < 0 || ch > 0xffff) return word(uint32_t(-1));
        const auto pos = text.rfind(char16_t(ch));
        return word(pos == std::u16string::npos ? uint32_t(-1) : uint32_t(pos));
    }
    if (name == u"lastIndexOf" && sig == u"(II)I" && args.size() == 3) {
        const int32_t ch = asInt(args[1]);
        const int32_t from = asInt(args[2]);
        if (ch < 0 || ch > 0xffff || from < 0 || text.empty()) return word(uint32_t(-1));
        const auto pos = text.rfind(char16_t(ch), size_t(from));
        return word(pos == std::u16string::npos ? uint32_t(-1) : uint32_t(pos));
    }
    if (name == u"substring" && sig == u"(I)Ljava/lang/String;" && args.size() == 2) {
        const int32_t begin = asInt(args[1]);
        if (begin < 0 || size_t(begin) > text.size())
            throw JavaException{u"Ljava/lang/StringIndexOutOfBoundsException;"};
        return ref(begin == 0 ? self : heap.newString(text.substr(size_t(begin))));
    }
    if ((name == u"substring" && sig == u"(II)Ljava/lang/String;") ||
        (name == u"subSequence" && sig == u"(II)Ljava/lang/CharSequence;")) {
        const int32_t begin = asInt(args[1]), end = asInt(args[2]);
        if (begin < 0 || end < begin || size_t(end) > text.size())
            throw JavaException{u"Ljava/lang/StringIndexOutOfBoundsException;"};
        return ref(heap.newString(text.substr(size_t(begin), size_t(end - begin))));
    }
    if (name == u"toCharArray" && sig == u"()[C") {
        const uint32_t arr = heap.array(u"[C", int32_t(text.size()));
        for (size_t i = 0; i < text.size(); ++i) heap.setElement(arr, int32_t(i), word(text[i]));
        return ref(arr);
    }
    if (name == u"trim" && sig == u"()Ljava/lang/String;") {
        size_t s = 0, e = text.size();
        while (s < e && text[s] <= 0x20) ++s;
        while (e > s && text[e - 1] <= 0x20) --e;
        return ref((s == 0 && e == text.size()) ? self : heap.newString(text.substr(s, e - s)));
    }
    if (name == u"toLowerCase" && (sig == u"()Ljava/lang/String;" || sig == u"(Ljava/util/Locale;)Ljava/lang/String;")) {
        std::u16string out = text;
        for (auto& c : out) c = asciiLower(c);
        return ref(heap.newString(std::move(out)));
    }
    if (name == u"toUpperCase" && (sig == u"()Ljava/lang/String;" || sig == u"(Ljava/util/Locale;)Ljava/lang/String;")) {
        std::u16string out = text;
        for (auto& c : out) c = asciiUpper(c);
        return ref(heap.newString(std::move(out)));
    }
    if (name == u"replace" && sig == u"(CC)Ljava/lang/String;" && args.size() == 3) {
        const char16_t from = char16_t(args[1].bits), to = char16_t(args[2].bits);
        std::u16string out = text;
        for (auto& c : out) if (c == from) c = to;
        return ref(heap.newString(std::move(out)));
    }
    if (name == u"replace" && sig == u"(Ljava/lang/CharSequence;Ljava/lang/CharSequence;)Ljava/lang/String;" &&
        args.size() == 3) {
        const uint32_t targetRef = asRef(args[1]), replRef = asRef(args[2]);
        if (!targetRef || !replRef) throw JavaException{u"Ljava/lang/NullPointerException;"};
        const auto target = charSequenceText(heap, targetRef);
        const auto repl = charSequenceText(heap, replRef);
        std::u16string out;
        if (target.empty()) {
            out += repl;
            for (char16_t c : text) { out.push_back(c); out += repl; }
        } else {
            size_t pos = 0;
            while (true) {
                const auto found = text.find(target, pos);
                if (found == std::u16string::npos) { out.append(text, pos, std::u16string::npos); break; }
                out.append(text, pos, found - pos);
                out += repl;
                pos = found + target.size();
            }
        }
        return ref(heap.newString(std::move(out)));
    }
    if (name == u"getBytes" && (sig == u"()[B" || sig == u"(Ljava/nio/charset/Charset;)[B" || sig == u"(Ljava/lang/String;)[B")) {
        const std::string bytes = u16ToUtf8(text);
        const uint32_t arr = heap.array(u"[B", int32_t(bytes.size()));
        for (size_t i = 0; i < bytes.size(); ++i)
            heap.setElement(arr, int32_t(i), word(uint32_t(int32_t(int8_t(bytes[i])))));
        return ref(arr);
    }
    return std::nullopt;
}

std::optional<Value> CoreLang::builderMethod(Heap& heap, std::u16string_view name,
                                             std::u16string_view sig, std::span<const Value> args) {
    if (args.empty()) return std::nullopt;
    const uint32_t self = asRef(args[0]);
    if (!self) throw JavaException{u"Ljava/lang/NullPointerException;"};

    if (name == u"<init>") {
        std::u16string initial;
        if (sig == u"(I)V" && args.size() == 2) {
            if (asInt(args[1]) < 0) throw JavaException{u"Ljava/lang/NegativeArraySizeException;"};
        } else if (sig == u"(Ljava/lang/String;)V" && args.size() == 2) {
            const uint32_t s = asRef(args[1]);
            if (!s) throw JavaException{u"Ljava/lang/NullPointerException;"};
            initial = heap.string(s);
        } else if (sig == u"(Ljava/lang/CharSequence;)V" && args.size() == 2) {
            const uint32_t s = asRef(args[1]);
            if (!s) throw JavaException{u"Ljava/lang/NullPointerException;"};
            initial = charSequenceText(heap, s);
        } else if (sig != u"()V") {
            return std::nullopt;
        }
        std::lock_guard lock(mutex_);
        builders_[self] = std::move(initial);
        return Value{ValueKind::Void};
    }

    if (name == u"append") {
        std::u16string piece;
        if (sig.starts_with(u"(Ljava/lang/String;)") && args.size() == 2) {
            const uint32_t s = asRef(args[1]);
            piece = s ? heap.string(s) : u"null";
        } else if (sig.starts_with(u"(Ljava/lang/CharSequence;)") && args.size() == 2) {
            piece = charSequenceText(heap, asRef(args[1]));
        } else if (sig.starts_with(u"(Ljava/lang/CharSequence;II)") && args.size() == 4) {
            const auto seq = charSequenceText(heap, asRef(args[1]));
            const int32_t start = asInt(args[2]), end = asInt(args[3]);
            if (start < 0 || end < start || size_t(end) > seq.size())
                throw JavaException{u"Ljava/lang/IndexOutOfBoundsException;"};
            piece = seq.substr(size_t(start), size_t(end - start));
        } else if (sig.starts_with(u"(Ljava/lang/Object;)") && args.size() == 2) {
            piece = formatObject(heap, asRef(args[1]));
        } else if (sig.starts_with(u"(C)") && args.size() == 2) {
            piece.push_back(char16_t(args[1].bits));
        } else if (sig.starts_with(u"(I)") && args.size() == 2) {
            piece = formatSignedLong(asInt(args[1]), 10);
        } else if (sig.starts_with(u"(J)") && args.size() == 2) {
            piece = formatSignedLong(asLong(args[1]), 10);
        } else if (sig.starts_with(u"(F)") && args.size() == 2) {
            piece = formatFloat(asFloat(args[1]));
        } else if (sig.starts_with(u"(D)") && args.size() == 2) {
            piece = formatDouble(asDouble(args[1]));
        } else if (sig.starts_with(u"(Z)") && args.size() == 2) {
            piece = (args[1].bits & 1u) ? u"true" : u"false";
        } else if (sig.starts_with(u"([C)") && args.size() == 2) {
            const uint32_t arr = asRef(args[1]);
            if (!arr) throw JavaException{u"Ljava/lang/NullPointerException;"};
            const uint32_t n = heap.length(arr);
            piece.reserve(n);
            for (uint32_t i = 0; i < n; ++i) piece.push_back(char16_t(heap.element(arr, int32_t(i)).bits));
        } else if (sig.starts_with(u"([CII)") && args.size() == 4) {
            const uint32_t arr = asRef(args[1]);
            if (!arr) throw JavaException{u"Ljava/lang/NullPointerException;"};
            const int32_t off = asInt(args[2]), cnt = asInt(args[3]);
            if (off < 0 || cnt < 0 || int64_t(off) + int64_t(cnt) > int64_t(heap.length(arr)))
                throw JavaException{u"Ljava/lang/IndexOutOfBoundsException;"};
            piece.reserve(size_t(cnt));
            for (int32_t i = 0; i < cnt; ++i) piece.push_back(char16_t(heap.element(arr, off + i).bits));
        } else {
            return std::nullopt;
        }
        std::lock_guard lock(mutex_);
        builders_[self] += piece;
        return ref(self);
    }

    if (name == u"insert") {
        std::u16string piece;
        int32_t offset = 0;
        if (sig.starts_with(u"(IC)") && args.size() == 3) {
            offset = asInt(args[1]);
            piece.push_back(char16_t(args[2].bits));
        } else if (sig.starts_with(u"(ILjava/lang/String;)") && args.size() == 3) {
            offset = asInt(args[1]);
            const uint32_t s = asRef(args[2]);
            piece = s ? heap.string(s) : u"null";
        } else {
            return std::nullopt;
        }
        std::lock_guard lock(mutex_);
        auto& buf = builders_[self];
        if (offset < 0 || size_t(offset) > buf.size())
            throw JavaException{u"Ljava/lang/StringIndexOutOfBoundsException;"};
        buf.insert(size_t(offset), piece);
        return ref(self);
    }

    std::lock_guard lock(mutex_);
    auto& buf = builders_[self];
    if (name == u"length" && sig == u"()I") return word(uint32_t(buf.size()));
    if (name == u"setLength" && sig == u"(I)V" && args.size() == 2) {
        const int32_t n = asInt(args[1]);
        if (n < 0) throw JavaException{u"Ljava/lang/IndexOutOfBoundsException;"};
        buf.resize(size_t(n), u'\0');
        return Value{ValueKind::Void};
    }
    if (name == u"charAt" && sig == u"(I)C" && args.size() == 2) {
        const int32_t i = asInt(args[1]);
        if (i < 0 || size_t(i) >= buf.size()) throw JavaException{u"Ljava/lang/IndexOutOfBoundsException;"};
        return word(buf[size_t(i)]);
    }
    if (name == u"setCharAt" && sig == u"(IC)V" && args.size() == 3) {
        const int32_t i = asInt(args[1]);
        if (i < 0 || size_t(i) >= buf.size()) throw JavaException{u"Ljava/lang/IndexOutOfBoundsException;"};
        buf[size_t(i)] = char16_t(args[2].bits);
        return Value{ValueKind::Void};
    }
    if (name == u"delete" && sig.starts_with(u"(II)") && args.size() == 3) {
        const int32_t start = asInt(args[1]);
        const int32_t end = std::min(asInt(args[2]), int32_t(buf.size()));
        if (start < 0 || size_t(start) > buf.size() || start > end)
            throw JavaException{u"Ljava/lang/StringIndexOutOfBoundsException;"};
        buf.erase(size_t(start), size_t(end - start));
        return ref(self);
    }
    if (name == u"deleteCharAt" && sig.starts_with(u"(I)") && args.size() == 2) {
        const int32_t i = asInt(args[1]);
        if (i < 0 || size_t(i) >= buf.size()) throw JavaException{u"Ljava/lang/StringIndexOutOfBoundsException;"};
        buf.erase(size_t(i), 1);
        return ref(self);
    }
    if (name == u"substring" && sig == u"(I)Ljava/lang/String;" && args.size() == 2) {
        const int32_t start = asInt(args[1]);
        if (start < 0 || size_t(start) > buf.size()) throw JavaException{u"Ljava/lang/StringIndexOutOfBoundsException;"};
        return ref(heap.newString(buf.substr(size_t(start))));
    }
    if (name == u"substring" && sig == u"(II)Ljava/lang/String;" && args.size() == 3) {
        const int32_t start = asInt(args[1]), end = asInt(args[2]);
        if (start < 0 || end < start || size_t(end) > buf.size())
            throw JavaException{u"Ljava/lang/StringIndexOutOfBoundsException;"};
        return ref(heap.newString(buf.substr(size_t(start), size_t(end - start))));
    }
    if (name == u"toString" && sig == u"()Ljava/lang/String;")
        return ref(heap.newString(buf));
    return std::nullopt;
}

std::optional<Value> CoreLang::systemMethod(Heap& heap, std::u16string_view name,
                                            std::u16string_view sig, bool isStatic,
                                            std::span<const Value> args) {
    if (!isStatic) return std::nullopt;
    if (name == u"arraycopy" && sig == u"(Ljava/lang/Object;ILjava/lang/Object;II)V" && args.size() == 5) {
        const uint32_t src = asRef(args[0]), dst = asRef(args[2]);
        if (!src || !dst) throw JavaException{u"Ljava/lang/NullPointerException;"};
        const auto srcType = heap.typeOf(src), dstType = heap.typeOf(dst);
        if (!srcType.starts_with(u'[') || !dstType.starts_with(u'['))
            throw JavaException{u"Ljava/lang/ArrayStoreException;"};
        const bool srcPrim = srcType.size() == 2 && srcType[1] != u'L' && srcType[1] != u'[';
        const bool dstPrim = dstType.size() == 2 && dstType[1] != u'L' && dstType[1] != u'[';
        if (srcPrim != dstPrim || (srcPrim && srcType != dstType))
            throw JavaException{u"Ljava/lang/ArrayStoreException;"};
        const int32_t srcPos = asInt(args[1]), dstPos = asInt(args[3]), count = asInt(args[4]);
        const int64_t srcLen = int64_t(heap.length(src)), dstLen = int64_t(heap.length(dst));
        if (srcPos < 0 || dstPos < 0 || count < 0 ||
            int64_t(srcPos) + int64_t(count) > srcLen || int64_t(dstPos) + int64_t(count) > dstLen)
            throw JavaException{u"Ljava/lang/ArrayIndexOutOfBoundsException;"};
        if (src == dst && dstPos > srcPos && dstPos < srcPos + count) {
            for (int32_t i = count - 1; i >= 0; --i)
                heap.setElement(dst, dstPos + i, heap.element(src, srcPos + i));
        } else {
            for (int32_t i = 0; i < count; ++i)
                heap.setElement(dst, dstPos + i, heap.element(src, srcPos + i));
        }
        return Value{ValueKind::Void};
    }
    if (name == u"currentTimeMillis" && sig == u"()J") {
        const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
        return wide(uint64_t(ms));
    }
    if (name == u"nanoTime" && sig == u"()J") {
        const auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count();
        return wide(uint64_t(ns));
    }
    if (name == u"identityHashCode" && sig == u"(Ljava/lang/Object;)I" && args.size() == 1)
        return word(asRef(args[0]));
    if (name == u"getProperty" && (sig == u"(Ljava/lang/String;)Ljava/lang/String;" ||
                                   sig == u"(Ljava/lang/String;Ljava/lang/String;)Ljava/lang/String;") &&
        !args.empty()) {
        const uint32_t keyRef = asRef(args[0]);
        if (!keyRef) throw JavaException{u"Ljava/lang/NullPointerException;"};
        const auto key = heap.string(keyRef);
        if (key.empty()) throw JavaException{u"Ljava/lang/IllegalArgumentException;"};
        constexpr std::pair<std::u16string_view, std::u16string_view> kProps[] = {
            {u"java.runtime.name", u"Android Runtime"},
            {u"java.vm.name", u"Dalvik"},
            {u"java.vendor", u"The Android Project"},
            {u"os.name", u"Linux"},
            {u"os.arch", u"armv7l"},
            {u"file.separator", u"/"},
            {u"path.separator", u":"},
            {u"line.separator", u"\n"},
        };
        for (const auto& [k, v] : kProps)
            if (key == k) return ref(heap.newString(std::u16string(v)));
        return ref(args.size() == 2 ? asRef(args[1]) : 0u);
    }
    return std::nullopt;
}

std::optional<Value> CoreLang::enumMethod(Heap& heap, std::u16string_view name,
                                          std::u16string_view sig, std::span<const Value> args) {
    if (args.empty()) return std::nullopt;
    const uint32_t self = asRef(args[0]);
    if (!self) throw JavaException{u"Ljava/lang/NullPointerException;"};
    if (name == u"<init>" && sig == u"(Ljava/lang/String;I)V" && args.size() == 3) {
        std::lock_guard lock(mutex_);
        enums_[self] = {asRef(args[1]), asInt(args[2])};
        return Value{ValueKind::Void};
    }
    std::lock_guard lock(mutex_);
    const auto it = enums_.find(self);
    const EnumData data = it != enums_.end() ? it->second : EnumData{};
    if (name == u"name" && sig == u"()Ljava/lang/String;") return ref(data.nameRef);
    if (name == u"toString" && sig == u"()Ljava/lang/String;") return ref(data.nameRef);
    if (name == u"ordinal" && sig == u"()I") return word(uint32_t(data.ordinal));
    if (name == u"compareTo" && (sig == u"(Ljava/lang/Enum;)I" || sig == u"(Ljava/lang/Object;)I") && args.size() == 2) {
        const uint32_t other = asRef(args[1]);
        if (!other) throw JavaException{u"Ljava/lang/NullPointerException;"};
        const auto oit = enums_.find(other);
        const int32_t otherOrd = oit != enums_.end() ? oit->second.ordinal : 0;
        return word(uint32_t(data.ordinal - otherOrd));
    }
    if (name == u"equals" && sig == u"(Ljava/lang/Object;)Z" && args.size() == 2)
        return word(self == asRef(args[1]) ? 1u : 0u);
    if (name == u"hashCode" && sig == u"()I") return word(self);
    return std::nullopt;
}

std::optional<Value> CoreLang::throwableMethod(Heap&, std::u16string_view name,
                                               std::u16string_view sig, std::span<const Value> args) {
    if (args.empty()) return std::nullopt;
    const uint32_t self = asRef(args[0]);
    if (!self) throw JavaException{u"Ljava/lang/NullPointerException;"};
    std::lock_guard lock(mutex_);
    auto& data = throwables_[self];
    if (name == u"<init>") {
        if (sig == u"()V") return Value{ValueKind::Void};
        if (sig == u"(Ljava/lang/String;)V" && args.size() == 2) {
            data.messageRef = asRef(args[1]);
            return Value{ValueKind::Void};
        }
        if (sig == u"(Ljava/lang/Throwable;)V" && args.size() == 2) {
            data.causeRef = asRef(args[1]);
            data.causeSet = true;
            return Value{ValueKind::Void};
        }
        if (sig == u"(Ljava/lang/String;Ljava/lang/Throwable;)V" && args.size() == 3) {
            data.messageRef = asRef(args[1]);
            data.causeRef = asRef(args[2]);
            data.causeSet = true;
            return Value{ValueKind::Void};
        }
        return std::nullopt;
    }
    if (name == u"getMessage" && sig == u"()Ljava/lang/String;") return ref(data.messageRef);
    if (name == u"getCause" && sig == u"()Ljava/lang/Throwable;") return ref(data.causeRef);
    if (name == u"initCause" && sig == u"(Ljava/lang/Throwable;)Ljava/lang/Throwable;" && args.size() == 2) {
        if (data.causeSet) throw JavaException{u"Ljava/lang/IllegalStateException;"};
        const uint32_t cause = asRef(args[1]);
        if (cause == self) throw JavaException{u"Ljava/lang/IllegalArgumentException;"};
        data.causeRef = cause;
        data.causeSet = true;
        return ref(self);
    }
    if (name == u"addSuppressed" && sig == u"(Ljava/lang/Throwable;)V") return Value{ValueKind::Void};
    return std::nullopt;
}

std::optional<Value> CoreLang::invoke(Heap& heap, std::u16string_view owner, std::u16string_view name,
                                      std::u16string_view signature, bool isStatic,
                                      std::span<const Value> args) {
    if (isBoxedClass(owner)) return boxedMethod(heap, owner, name, signature, isStatic, args);
    if (owner == u"Ljava/lang/String;") return stringMethod(heap, name, signature, isStatic, args);
    if (!isStatic && (owner == u"Ljava/lang/StringBuilder;" || owner == u"Ljava/lang/StringBuffer;"))
        return builderMethod(heap, name, signature, args);
    if (owner == u"Ljava/lang/System;") return systemMethod(heap, name, signature, isStatic, args);
    if (owner == u"Ljava/lang/Runtime;") {
        if (isStatic && name == u"getRuntime" && signature == u"()Ljava/lang/Runtime;") {
            std::lock_guard lock(mutex_);
            if (!runtimeSingleton_) runtimeSingleton_ = heap.object(u"Ljava/lang/Runtime;");
            return ref(runtimeSingleton_);
        }
        if (!isStatic && name == u"availableProcessors" && signature == u"()I") {
            const unsigned n = std::thread::hardware_concurrency();
            return word(n ? n : 1u);
        }
        return std::nullopt;
    }
    if (!isStatic && owner == u"Ljava/lang/Enum;") return enumMethod(heap, name, signature, args);
    if (!isStatic && isThrowableClass(owner)) return throwableMethod(heap, name, signature, args);
    if (!isStatic && owner == u"Ljava/lang/Object;" && !args.empty()) {
        const uint32_t self = asRef(args[0]);
        if (!self) throw JavaException{u"Ljava/lang/NullPointerException;"};
        if (name == u"equals" && signature == u"(Ljava/lang/Object;)Z" && args.size() == 2)
            return word(self == asRef(args[1]) ? 1u : 0u);
        if (name == u"hashCode" && signature == u"()I") return word(self);
        if (name == u"toString" && signature == u"()Ljava/lang/String;")
            return ref(heap.newString(formatObject(heap, self)));
    }
    return std::nullopt;
}

} // namespace Hydrogen::Runtime::Android::Dex
