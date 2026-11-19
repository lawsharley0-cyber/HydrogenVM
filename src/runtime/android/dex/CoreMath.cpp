#include "runtime/android/dex/CoreMath.hpp"
#include "runtime/android/dex/DexName.hpp"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdint>
#include <limits>
#include <random>

namespace Hydrogen::Runtime::Android::Dex {
namespace {

constexpr double kDegToRad = 0.017453292519943295; // pi / 180
constexpr double kRadToDeg = 57.29577951308232;    // 180 / pi

int32_t asInt(Value v) { return std::bit_cast<int32_t>(uint32_t(v.bits)); }
int64_t asLong(Value v) { return std::bit_cast<int64_t>(v.bits); }
float asFloat(Value v) { return std::bit_cast<float>(uint32_t(v.bits)); }
double asDouble(Value v) { return std::bit_cast<double>(v.bits); }

Value word(int32_t v) { return {ValueKind::Word, uint32_t(v)}; }
Value wide(int64_t v) { return {ValueKind::Wide, uint64_t(v)}; }
Value f32(float v) { return {ValueKind::Word, std::bit_cast<uint32_t>(v)}; }
Value f64(double v) { return {ValueKind::Wide, std::bit_cast<uint64_t>(v)}; }

float javaMin(float a, float b) {
    if (std::isnan(a)) return a;
    if (std::isnan(b)) return b;
    if (a == 0.0f && b == 0.0f) {
        return (std::bit_cast<uint32_t>(a) | std::bit_cast<uint32_t>(b)) & 0x80000000u
                   ? std::bit_cast<float>(0x80000000u)
                   : 0.0f;
    }
    return a < b ? a : b;
}

float javaMax(float a, float b) {
    if (std::isnan(a)) return a;
    if (std::isnan(b)) return b;
    if (a == 0.0f && b == 0.0f) {
        return (std::bit_cast<uint32_t>(a) & std::bit_cast<uint32_t>(b)) & 0x80000000u
                   ? std::bit_cast<float>(0x80000000u)
                   : 0.0f;
    }
    return a > b ? a : b;
}

double javaMin(double a, double b) {
    if (std::isnan(a)) return a;
    if (std::isnan(b)) return b;
    if (a == 0.0 && b == 0.0) {
        return (std::bit_cast<uint64_t>(a) | std::bit_cast<uint64_t>(b)) & 0x8000000000000000ULL
                   ? std::bit_cast<double>(0x8000000000000000ULL)
                   : 0.0;
    }
    return a < b ? a : b;
}

double javaMax(double a, double b) {
    if (std::isnan(a)) return a;
    if (std::isnan(b)) return b;
    if (a == 0.0 && b == 0.0) {
        return (std::bit_cast<uint64_t>(a) & std::bit_cast<uint64_t>(b)) & 0x8000000000000000ULL
                   ? std::bit_cast<double>(0x8000000000000000ULL)
                   : 0.0;
    }
    return a > b ? a : b;
}

int32_t javaRound(float a) {
    if (std::isnan(a)) return 0;
    const double shifted = std::floor(double(a) + 0.5);
    if (shifted <= double(INT32_MIN)) return INT32_MIN;
    if (shifted >= double(INT32_MAX)) return INT32_MAX;
    return int32_t(shifted);
}

int64_t javaRound(double a) {
    if (std::isnan(a)) return 0;
    if (a <= -9223372036854775808.0) return INT64_MIN;
    if (a >= 9223372036854775808.0) return INT64_MAX;
    if (std::fabs(a) >= 4503599627370496.0) return int64_t(a);
    const double f = std::floor(a);
    return (a - f >= 0.5) ? (int64_t(f) + 1) : int64_t(f);
}

float javaSignum(float a) {
    if (std::isnan(a) || a == 0.0f) return a;
    return a > 0.0f ? 1.0f : -1.0f;
}

double javaSignum(double a) {
    if (std::isnan(a) || a == 0.0) return a;
    return a > 0.0 ? 1.0 : -1.0;
}

int32_t javaFloorDiv(int32_t x, int32_t y) {
    if (y == 0) throw JavaException{u"Ljava/lang/ArithmeticException;"};
    if (x == INT32_MIN && y == -1) return INT32_MIN;
    int32_t r = x / y;
    if ((x ^ y) < 0 && (r * y != x)) --r;
    return r;
}

int64_t javaFloorDiv(int64_t x, int64_t y) {
    if (y == 0) throw JavaException{u"Ljava/lang/ArithmeticException;"};
    if (x == INT64_MIN && y == -1) return INT64_MIN;
    int64_t r = x / y;
    if ((x ^ y) < 0 && (r * y != x)) --r;
    return r;
}

} // namespace

bool CoreMath::hasClass(std::u16string_view type) {
    return type == u"Ljava/lang/Math;" || type == u"Ljava/lang/StrictMath;";
}

std::optional<Value> CoreMath::invoke(std::u16string_view type,
                                      std::u16string_view name,
                                      std::u16string_view signature,
                                      bool isStatic,
                                      std::span<const Value> args) {
    if (!isStatic || !hasClass(type)) return std::nullopt;

    if (name == u"abs") {
        if (signature == u"(I)I" && args.size() == 1) {
            const uint32_t u = uint32_t(args[0].bits);
            return word(std::bit_cast<int32_t>((u & 0x80000000u) ? (0u - u) : u));
        }
        if (signature == u"(J)J" && args.size() == 1) {
            const uint64_t u = args[0].bits;
            return wide(std::bit_cast<int64_t>((u & 0x8000000000000000ULL) ? (0ULL - u) : u));
        }
        if (signature == u"(F)F" && args.size() == 1)
            return Value{ValueKind::Word, uint32_t(args[0].bits) & 0x7fffffffu};
        if (signature == u"(D)D" && args.size() == 1)
            return Value{ValueKind::Wide, args[0].bits & 0x7fffffffffffffffULL};
    }

    if (name == u"min" && args.size() == 2) {
        if (signature == u"(II)I") return word(std::min(asInt(args[0]), asInt(args[1])));
        if (signature == u"(JJ)J") return wide(std::min(asLong(args[0]), asLong(args[1])));
        if (signature == u"(FF)F") return f32(javaMin(asFloat(args[0]), asFloat(args[1])));
        if (signature == u"(DD)D") return f64(javaMin(asDouble(args[0]), asDouble(args[1])));
    }

    if (name == u"max" && args.size() == 2) {
        if (signature == u"(II)I") return word(std::max(asInt(args[0]), asInt(args[1])));
        if (signature == u"(JJ)J") return wide(std::max(asLong(args[0]), asLong(args[1])));
        if (signature == u"(FF)F") return f32(javaMax(asFloat(args[0]), asFloat(args[1])));
        if (signature == u"(DD)D") return f64(javaMax(asDouble(args[0]), asDouble(args[1])));
    }

    if (name == u"ceil" && signature == u"(D)D" && args.size() == 1)
        return f64(std::ceil(asDouble(args[0])));
    if (name == u"floor" && signature == u"(D)D" && args.size() == 1)
        return f64(std::floor(asDouble(args[0])));
    if (name == u"rint" && signature == u"(D)D" && args.size() == 1)
        return f64(std::nearbyint(asDouble(args[0])));

    if (name == u"round" && args.size() == 1) {
        if (signature == u"(F)I") return word(javaRound(asFloat(args[0])));
        if (signature == u"(D)J") return wide(javaRound(asDouble(args[0])));
    }

    if (name == u"signum" && args.size() == 1) {
        if (signature == u"(F)F") return f32(javaSignum(asFloat(args[0])));
        if (signature == u"(D)D") return f64(javaSignum(asDouble(args[0])));
    }

    if (args.size() == 1 && signature == u"(D)D") {
        const double x = asDouble(args[0]);
        if (name == u"sqrt") return f64(std::sqrt(x));
        if (name == u"cbrt") return f64(std::cbrt(x));
        if (name == u"sin") return f64(std::sin(x));
        if (name == u"cos") return f64(std::cos(x));
        if (name == u"tan") return f64(std::tan(x));
        if (name == u"asin") return f64(std::asin(x));
        if (name == u"acos") return f64(std::acos(x));
        if (name == u"atan") return f64(std::atan(x));
        if (name == u"sinh") return f64(std::sinh(x));
        if (name == u"cosh") return f64(std::cosh(x));
        if (name == u"tanh") return f64(std::tanh(x));
        if (name == u"exp") return f64(std::exp(x));
        if (name == u"expm1") return f64(std::expm1(x));
        if (name == u"log") return f64(std::log(x));
        if (name == u"log10") return f64(std::log10(x));
        if (name == u"log1p") return f64(std::log1p(x));
        if (name == u"toRadians") return f64(x * kDegToRad);
        if (name == u"toDegrees") return f64(x * kRadToDeg);
    }

    if (args.size() == 2 && signature == u"(DD)D") {
        const double a = asDouble(args[0]), b = asDouble(args[1]);
        if (name == u"pow") return f64(std::pow(a, b));
        if (name == u"atan2") return f64(std::atan2(a, b));
        if (name == u"hypot") return f64(std::hypot(a, b));
        if (name == u"copySign") return f64(std::copysign(a, b));
        if (name == u"IEEEremainder") return f64(std::remainder(a, b));
    }

    if (name == u"copySign" && signature == u"(FF)F" && args.size() == 2)
        return f32(std::copysign(asFloat(args[0]), asFloat(args[1])));

    if (name == u"floorDiv" && args.size() == 2) {
        if (signature == u"(II)I") return word(javaFloorDiv(asInt(args[0]), asInt(args[1])));
        if (signature == u"(JJ)J") return wide(javaFloorDiv(asLong(args[0]), asLong(args[1])));
    }

    if (name == u"floorMod" && args.size() == 2) {
        if (signature == u"(II)I") {
            const int32_t x = asInt(args[0]), y = asInt(args[1]);
            const int32_t q = javaFloorDiv(x, y);
            return word(x - q * y);
        }
        if (signature == u"(JJ)J") {
            const int64_t x = asLong(args[0]), y = asLong(args[1]);
            const int64_t q = javaFloorDiv(x, y);
            return wide(x - q * y);
        }
    }

    if (name == u"random" && signature == u"()D" && args.empty()) {
        thread_local std::mt19937_64 gen(std::random_device{}());
        std::uniform_real_distribution<double> dist(0.0, 1.0);
        return f64(dist(gen));
    }

    return std::nullopt;
}

} // namespace Hydrogen::Runtime::Android::Dex
