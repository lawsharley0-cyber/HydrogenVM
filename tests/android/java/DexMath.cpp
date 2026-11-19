// Host DEX verification of java.lang.Math against Java SE / IEEE-754 specifications.
#include "runtime/android/dex/CoreMath.hpp"
#include "runtime/android/dex/Machine.hpp"

#include <array>
#include <bit>
#include <cmath>
#include <cstdint>
#include <fstream>
#include <iostream>
#include <iterator>
#include <limits>
#include <vector>

using namespace Hydrogen::Runtime::Android::Dex;
namespace {

int failures = 0;
void check(const char* test, bool passed) {
    std::cout << (passed ? "ok " : "FAIL ") << test << '\n';
    failures += !passed;
}

Value word(int32_t v) { return {ValueKind::Word, uint32_t(v)}; }
Value wide(int64_t v) { return {ValueKind::Wide, uint64_t(v)}; }
Value f32(float v) { return {ValueKind::Word, std::bit_cast<uint32_t>(v)}; }
Value f64(double v) { return {ValueKind::Wide, std::bit_cast<uint64_t>(v)}; }

bool isWord(const Execution& result, int32_t expected) {
    const auto* v = std::get_if<Value>(&result);
    return v && v->kind == ValueKind::Word && std::bit_cast<int32_t>(uint32_t(v->bits)) == expected;
}

bool isWordBits(const Execution& result, uint32_t expected) {
    const auto* v = std::get_if<Value>(&result);
    return v && v->kind == ValueKind::Word && uint32_t(v->bits) == expected;
}

bool isWide(const Execution& result, int64_t expected) {
    const auto* v = std::get_if<Value>(&result);
    return v && v->kind == ValueKind::Wide && std::bit_cast<int64_t>(v->bits) == expected;
}

bool isWideBits(const Execution& result, uint64_t expected) {
    const auto* v = std::get_if<Value>(&result);
    return v && v->kind == ValueKind::Wide && v->bits == expected;
}

bool isDoubleNear(const Execution& result, double expected, double tol = 1e-12) {
    const auto* v = std::get_if<Value>(&result);
    if (!v || v->kind != ValueKind::Wide) return false;
    const double actual = std::bit_cast<double>(v->bits);
    return std::isfinite(actual) && std::abs(actual - expected) <= tol;
}

bool isDoubleNaN(const Execution& result) {
    const auto* v = std::get_if<Value>(&result);
    return v && v->kind == ValueKind::Wide && std::isnan(std::bit_cast<double>(v->bits));
}

bool isFloatNaN(const Execution& result) {
    const auto* v = std::get_if<Value>(&result);
    return v && v->kind == ValueKind::Word && std::isnan(std::bit_cast<float>(uint32_t(v->bits)));
}

} // namespace

int main(int argc, char** argv) {
    if (argc != 2) return 2;
    const std::filesystem::path fixtures = argv[1];
    const auto classes = std::make_shared<ClassPath>(std::vector<std::string>{"classes.dex"},
        [&](const auto&) {
            std::ifstream input(fixtures / "math.dex", std::ios::binary);
            if (!input) throw FormatError("missing math.dex fixture");
            return std::vector<uint8_t>{std::istreambuf_iterator<char>(input), {}};
        });

    Machine::Host host;
    host.hasClass = [](std::u16string_view type) { return CoreMath::hasClass(type); };
    host.call = [&](std::u16string_view type, std::u16string_view name, std::u16string_view sig,
                    bool isStatic, bool isNative, std::span<const Value> args) -> std::optional<Value> {
        if (isNative) return {};
        return CoreMath::invoke(type, name, sig, isStatic, args);
    };
    Machine vm(classes, {}, host);
    const auto cls = u"Ltest/MathChecks;";

    // abs integer/long wrap-around & floating-point -0.0 -> +0.0 per Java SE spec.
    bool absOk = true;
    absOk &= isWord(vm.callStatic(cls, u"absInt", u"(I)I", std::array{word(-42)}), 42);
    absOk &= isWord(vm.callStatic(cls, u"absInt", u"(I)I", std::array{word(INT32_MIN)}), INT32_MIN);
    absOk &= isWide(vm.callStatic(cls, u"absLong", u"(J)J", std::array{wide(INT64_MIN)}), INT64_MIN);
    absOk &= isWordBits(vm.callStatic(cls, u"absFloat", u"(F)F", std::array{f32(-0.0f)}), 0x00000000u);
    absOk &= isWideBits(vm.callStatic(cls, u"absDouble", u"(D)D", std::array{f64(-0.0)}), 0x0000000000000000ULL);
    check("Math.abs preserves MIN_VALUE wrap and clears -0.0 sign bit", absOk);

    // min/max signed zero and NaN rules per Java SE spec.
    const double dNaN = std::numeric_limits<double>::quiet_NaN();
    const float fNaN = std::numeric_limits<float>::quiet_NaN();
    bool minMaxOk = true;
    minMaxOk &= isWideBits(vm.callStatic(cls, u"minDouble", u"(DD)D", std::array{f64(0.0), f64(-0.0)}), 0x8000000000000000ULL);
    minMaxOk &= isWideBits(vm.callStatic(cls, u"maxDouble", u"(DD)D", std::array{f64(-0.0), f64(0.0)}), 0x0000000000000000ULL);
    minMaxOk &= isDoubleNaN(vm.callStatic(cls, u"minDouble", u"(DD)D", std::array{f64(1.0), f64(dNaN)}));
    minMaxOk &= isDoubleNaN(vm.callStatic(cls, u"maxDouble", u"(DD)D", std::array{f64(dNaN), f64(1.0)}));
    minMaxOk &= isWordBits(vm.callStatic(cls, u"minFloat", u"(FF)F", std::array{f32(0.0f), f32(-0.0f)}), 0x80000000u);
    minMaxOk &= isWordBits(vm.callStatic(cls, u"maxFloat", u"(FF)F", std::array{f32(-0.0f), f32(0.0f)}), 0x00000000u);
    minMaxOk &= isFloatNaN(vm.callStatic(cls, u"minFloat", u"(FF)F", std::array{f32(fNaN), f32(2.0f)}));
    minMaxOk &= isWord(vm.callStatic(cls, u"minInt", u"(II)I", std::array{word(5), word(-9)}), -9);
    minMaxOk &= isWide(vm.callStatic(cls, u"maxLong", u"(JJ)J", std::array{wide(-100), wide(9000000000LL)}), 9000000000LL);
    check("Math.min and Math.max enforce signed-zero and NaN semantics", minMaxOk);

    // ceil / floor including ceil(-0.5) == -0.0.
    bool ceilFloorOk = true;
    ceilFloorOk &= isWideBits(vm.callStatic(cls, u"ceilDouble", u"(D)D", std::array{f64(2.1)}), std::bit_cast<uint64_t>(3.0));
    ceilFloorOk &= isWideBits(vm.callStatic(cls, u"ceilDouble", u"(D)D", std::array{f64(-0.5)}), 0x8000000000000000ULL);
    ceilFloorOk &= isWideBits(vm.callStatic(cls, u"floorDouble", u"(D)D", std::array{f64(-2.1)}), std::bit_cast<uint64_t>(-3.0));
    ceilFloorOk &= isWideBits(vm.callStatic(cls, u"floorDouble", u"(D)D", std::array{f64(-0.0)}), 0x8000000000000000ULL);
    check("Math.ceil and Math.floor match IEEE-754 / Java SE spec", ceilFloorOk);

    // round(float) and round(double): tie toward +inf, 0x1.fffffffffffffp-2 -> 0, NaN -> 0, saturation.
    const double belowHalf = std::bit_cast<double>(0x3fdfffffffffffffULL); // 0.49999999999999994
    bool roundOk = true;
    roundOk &= isWord(vm.callStatic(cls, u"roundFloat", u"(F)I", std::array{f32(-0.5f)}), 0);
    roundOk &= isWord(vm.callStatic(cls, u"roundFloat", u"(F)I", std::array{f32(-1.5f)}), -1);
    roundOk &= isWord(vm.callStatic(cls, u"roundFloat", u"(F)I", std::array{f32(fNaN)}), 0);
    roundOk &= isWord(vm.callStatic(cls, u"roundFloat", u"(F)I", std::array{f32(1e20f)}), INT32_MAX);
    roundOk &= isWord(vm.callStatic(cls, u"roundFloat", u"(F)I", std::array{f32(-1e20f)}), INT32_MIN);
    roundOk &= isWide(vm.callStatic(cls, u"roundDouble", u"(D)J", std::array{f64(belowHalf)}), 0);
    roundOk &= isWide(vm.callStatic(cls, u"roundDouble", u"(D)J", std::array{f64(0.5)}), 1);
    roundOk &= isWide(vm.callStatic(cls, u"roundDouble", u"(D)J", std::array{f64(-0.5)}), 0);
    roundOk &= isWide(vm.callStatic(cls, u"roundDouble", u"(D)J", std::array{f64(-1.5)}), -1);
    roundOk &= isWide(vm.callStatic(cls, u"roundDouble", u"(D)J", std::array{f64(dNaN)}), 0);
    roundOk &= isWide(vm.callStatic(cls, u"roundDouble", u"(D)J", std::array{f64(1e30)}), INT64_MAX);
    roundOk &= isWide(vm.callStatic(cls, u"roundDouble", u"(D)J", std::array{f64(-1e30)}), INT64_MIN);
    check("Math.round ties toward +infinity, avoids 0.49999999999999994 double-rounding, and saturates", roundOk);

    // signum, sqrt, pow, hypot, atan2, sin^2 + cos^2 == 1.
    bool elemOk = true;
    elemOk &= isWordBits(vm.callStatic(cls, u"signumFloat", u"(F)F", std::array{f32(-0.0f)}), 0x80000000u);
    elemOk &= isWordBits(vm.callStatic(cls, u"signumFloat", u"(F)F", std::array{f32(-3.5f)}), std::bit_cast<uint32_t>(-1.0f));
    elemOk &= isFloatNaN(vm.callStatic(cls, u"signumFloat", u"(F)F", std::array{f32(fNaN)}));
    elemOk &= isDoubleNear(vm.callStatic(cls, u"sqrtDouble", u"(D)D", std::array{f64(2.0)}), 1.4142135623730951);
    elemOk &= isDoubleNear(vm.callStatic(cls, u"powDouble", u"(DD)D", std::array{f64(2.0), f64(10.0)}), 1024.0);
    elemOk &= isDoubleNear(vm.callStatic(cls, u"hypotDouble", u"(DD)D", std::array{f64(3.0), f64(4.0)}), 5.0);
    elemOk &= isDoubleNear(vm.callStatic(cls, u"atan2Double", u"(DD)D", std::array{f64(1.0), f64(1.0)}), 0.7853981633974483);
    elemOk &= isDoubleNear(vm.callStatic(cls, u"sinDouble", u"(D)D", std::array{f64(1.0)}), 0.8414709848078965);
    elemOk &= isDoubleNear(vm.callStatic(cls, u"cosDouble", u"(D)D", std::array{f64(1.0)}), 0.5403023058681398);
    check("Math elementary functions (signum, sqrt, pow, hypot, atan2, sin/cos)", elemOk);

    std::cout << "done: " << failures << " failures\n";
    return failures ? 1 : 0;
}
