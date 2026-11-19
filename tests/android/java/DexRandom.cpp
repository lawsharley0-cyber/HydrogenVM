// Host DEX verification of java.util.Random against OpenJDK 17.0.21 reference values.
#include "runtime/android/dex/CoreRandom.hpp"
#include "runtime/android/dex/Machine.hpp"

#include <array>
#include <bit>
#include <cmath>
#include <cstdint>
#include <fstream>
#include <iostream>
#include <iterator>
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
Value ref(uint32_t id) { return {ValueKind::Reference, id}; }

uint32_t refOf(const Execution& result) {
    const auto* v = std::get_if<Value>(&result);
    return v && v->kind == ValueKind::Reference ? uint32_t(v->bits) : 0;
}

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

bool isGaussianNear(const Execution& result, double expected) {
    const auto* v = std::get_if<Value>(&result);
    if (!v || v->kind != ValueKind::Wide) return false;
    const double actual = std::bit_cast<double>(v->bits);
    return std::isfinite(actual) && std::abs(actual - expected) <= 1e-15;
}

bool isException(const Execution& result, std::u16string_view expected) {
    const auto* thrown = std::get_if<JavaException>(&result);
    return thrown && thrown->type == expected;
}

} // namespace

int main(int argc, char** argv) {
    if (argc != 2) return 2;
    const std::filesystem::path fixtures = argv[1];
    const auto classes = std::make_shared<ClassPath>(std::vector<std::string>{"classes.dex"},
        [&](const auto&) {
            std::ifstream input(fixtures / "random.dex", std::ios::binary);
            if (!input) throw FormatError("missing random.dex fixture");
            return std::vector<uint8_t>{std::istreambuf_iterator<char>(input), {}};
        });

    CoreRandom random;
    Machine* vmPtr = nullptr;
    Machine::Host host;
    host.hasClass = [](std::u16string_view type) { return CoreRandom::hasClass(type); };
    host.call = [&](std::u16string_view type, std::u16string_view name, std::u16string_view sig,
                    bool isStatic, bool isNative, std::span<const Value> args) -> std::optional<Value> {
        if (isNative) return {};
        return random.invoke(vmPtr->heap(), type, name, sig, isStatic, args);
    };
    Machine vm(classes, {}, host);
    vmPtr = &vm;

    const auto cls = u"Ltest/RandomChecks;";

    // Reference values generated independently with OpenJDK 17.0.21 java.util.Random.
    const uint32_t r42 = refOf(vm.callStatic(cls, u"newSeeded", u"(J)Ljava/util/Random;", std::array{wide(42)}));
    check("new Random(42L) constructs an instance", r42 != 0);

    bool seq42 = true;
    seq42 &= isWord(vm.callStatic(cls, u"nextInt", u"(Ljava/util/Random;)I", std::array{ref(r42)}), -1170105035);
    seq42 &= isWord(vm.callStatic(cls, u"nextInt", u"(Ljava/util/Random;)I", std::array{ref(r42)}), 234785527);
    check("Random(42L).nextInt() matches OpenJDK 17", seq42);

    bool bounded42 = true;
    bounded42 &= isWord(vm.callStatic(cls, u"nextIntBound", u"(Ljava/util/Random;I)I", std::array{ref(r42), word(1)}), 0);
    bounded42 &= isWord(vm.callStatic(cls, u"nextIntBound", u"(Ljava/util/Random;I)I", std::array{ref(r42), word(2)}), 0);
    bounded42 &= isWord(vm.callStatic(cls, u"nextIntBound", u"(Ljava/util/Random;I)I", std::array{ref(r42), word(16)}), 4);
    bounded42 &= isWord(vm.callStatic(cls, u"nextIntBound", u"(Ljava/util/Random;I)I", std::array{ref(r42), word(1024)}), 964);
    bounded42 &= isWord(vm.callStatic(cls, u"nextIntBound", u"(Ljava/util/Random;I)I", std::array{ref(r42), word(10)}), 5);
    bounded42 &= isWord(vm.callStatic(cls, u"nextIntBound", u"(Ljava/util/Random;I)I", std::array{ref(r42), word(100)}), 18);
    bounded42 &= isWord(vm.callStatic(cls, u"nextIntBound", u"(Ljava/util/Random;I)I", std::array{ref(r42), word(7)}), 6);
    check("Random(42L).nextInt(bound) matches OpenJDK 17 for power-of-two and general bounds", bounded42);

    bool longs42 = true;
    longs42 &= isWide(vm.callStatic(cls, u"nextLong", u"(Ljava/util/Random;)J", std::array{ref(r42)}), 1684641590762760125LL);
    longs42 &= isWide(vm.callStatic(cls, u"nextLong", u"(Ljava/util/Random;)J", std::array{ref(r42)}), 8324226458996739737LL);
    check("Random(42L).nextLong() matches OpenJDK 17", longs42);

    bool scalars42 = true;
    scalars42 &= isWord(vm.callStatic(cls, u"nextBoolean", u"(Ljava/util/Random;)Z", std::array{ref(r42)}), 0);
    scalars42 &= isWord(vm.callStatic(cls, u"nextBoolean", u"(Ljava/util/Random;)Z", std::array{ref(r42)}), 0);
    scalars42 &= isWord(vm.callStatic(cls, u"nextBoolean", u"(Ljava/util/Random;)Z", std::array{ref(r42)}), 1);
    scalars42 &= isWordBits(vm.callStatic(cls, u"nextFloat", u"(Ljava/util/Random;)F", std::array{ref(r42)}), 0x3eed6400u);
    scalars42 &= isWideBits(vm.callStatic(cls, u"nextDouble", u"(Ljava/util/Random;)D", std::array{ref(r42)}), 0x3fe8630aee436200ULL);
    check("Random(42L) nextBoolean/nextFloat/nextDouble match OpenJDK 17 exact IEEE-754 bits", scalars42);

    const uint32_t bytesRef = refOf(vm.callStatic(cls, u"nextBytes", u"(Ljava/util/Random;I)[B", std::array{ref(r42), word(7)}));
    const std::array<int8_t, 7> expectedBytes{-29, -95, -120, -1, -15, 16, 89};
    bool bytesOk = bytesRef != 0 && vm.heap().length(bytesRef) == 7;
    for (int i = 0; bytesOk && i < 7; ++i) {
        const auto el = vm.heap().element(bytesRef, i);
        bytesOk &= el.kind == ValueKind::Word && std::bit_cast<int32_t>(uint32_t(el.bits)) == int32_t(expectedBytes[i]);
    }
    check("Random(42L).nextBytes(byte[7]) matches OpenJDK 17 signed bytes", bytesOk);

    // Seed reproducibility via setSeed(42L)
    vm.callStatic(cls, u"setSeed", u"(Ljava/util/Random;J)V", std::array{ref(r42), wide(42)});
    bool resetOk = isWord(vm.callStatic(cls, u"nextInt", u"(Ljava/util/Random;)I", std::array{ref(r42)}), -1170105035) &&
                   isWord(vm.callStatic(cls, u"nextInt", u"(Ljava/util/Random;)I", std::array{ref(r42)}), 234785527);
    check("setSeed(42L) reproduces the exact initial sequence", resetOk);

    // Seeds 0 and -1 (negative seed sign/mask handling & nextLong sign-extension)
    const uint32_t r0 = refOf(vm.callStatic(cls, u"newSeeded", u"(J)Ljava/util/Random;", std::array{wide(0)}));
    const uint32_t rm1 = refOf(vm.callStatic(cls, u"newSeeded", u"(J)Ljava/util/Random;", std::array{wide(-1)}));
    bool edgeSeeds = true;
    edgeSeeds &= isWord(vm.callStatic(cls, u"nextInt", u"(Ljava/util/Random;)I", std::array{ref(r0)}), -1155484576);
    edgeSeeds &= isWord(vm.callStatic(cls, u"nextInt", u"(Ljava/util/Random;)I", std::array{ref(r0)}), -723955400);
    edgeSeeds &= isWide(vm.callStatic(cls, u"nextLong", u"(Ljava/util/Random;)J", std::array{ref(r0)}), 4437113781045784766LL);
    edgeSeeds &= isWord(vm.callStatic(cls, u"nextInt", u"(Ljava/util/Random;)I", std::array{ref(rm1)}), 1155099827);
    edgeSeeds &= isWord(vm.callStatic(cls, u"nextInt", u"(Ljava/util/Random;)I", std::array{ref(rm1)}), 1887904451);
    edgeSeeds &= isWide(vm.callStatic(cls, u"nextLong", u"(Ljava/util/Random;)J", std::array{ref(rm1)}), 226341162490527646LL);
    check("Random(0L) and Random(-1L) match OpenJDK 17 including negative next(32) in nextLong", edgeSeeds);

    // Rejection-loop case: seed 0 with bound 1_500_000_000 rejects the first draw in OpenJDK
    const uint32_t rr = refOf(vm.callStatic(cls, u"newSeeded", u"(J)Ljava/util/Random;", std::array{wide(0)}));
    bool rejectOk =
        isWord(vm.callStatic(cls, u"nextIntBound", u"(Ljava/util/Random;I)I", std::array{ref(rr), word(1500000000)}), 516548029) &&
        isWord(vm.callStatic(cls, u"nextInt", u"(Ljava/util/Random;)I", std::array{ref(rr)}), -1690734402);
    check("nextInt(bound) rejection loop advances state identically to OpenJDK 17", rejectOk);

    // Non-positive bounds throw IllegalArgumentException; null nextBytes throws NullPointerException
    bool exceptionsOk =
        isException(vm.callStatic(cls, u"nextIntBound", u"(Ljava/util/Random;I)I", std::array{ref(r42), word(0)}), u"Ljava/lang/IllegalArgumentException;") &&
        isException(vm.callStatic(cls, u"nextIntBound", u"(Ljava/util/Random;I)I", std::array{ref(r42), word(-7)}), u"Ljava/lang/IllegalArgumentException;") &&
        isException(vm.callStatic(cls, u"nextBytesNull", u"(Ljava/util/Random;)V", std::array{ref(r42)}), u"Ljava/lang/NullPointerException;");
    check("nextInt(<=0) throws IllegalArgumentException and nextBytes(null) throws NullPointerException", exceptionsOk);

    // nextGaussian pair and setSeed clearing the cached second Gaussian
    const uint32_t rg = refOf(vm.callStatic(cls, u"newSeeded", u"(J)Ljava/util/Random;", std::array{wide(12345)}));
    const auto g0 = vm.callStatic(cls, u"nextGaussian", u"(Ljava/util/Random;)D", std::array{ref(rg)});
    const auto g1 = vm.callStatic(cls, u"nextGaussian", u"(Ljava/util/Random;)D", std::array{ref(rg)});
    vm.callStatic(cls, u"setSeed", u"(Ljava/util/Random;J)V", std::array{ref(rg), wide(12345)});
    const auto g0Again = vm.callStatic(cls, u"nextGaussian", u"(Ljava/util/Random;)D", std::array{ref(rg)});
    vm.callStatic(cls, u"setSeed", u"(Ljava/util/Random;J)V", std::array{ref(rg), wide(12345)});
    const auto g0Third = vm.callStatic(cls, u"nextGaussian", u"(Ljava/util/Random;)D", std::array{ref(rg)});
    const auto* g0Val = std::get_if<Value>(&g0);
    const auto* g0AgainVal = std::get_if<Value>(&g0Again);
    const auto* g0ThirdVal = std::get_if<Value>(&g0Third);
    bool gaussOk = isGaussianNear(g0, -0.18780898965891200) &&
                   isGaussianNear(g1, 0.58843630511547960) &&
                   g0Val && g0AgainVal && g0ThirdVal &&
                   g0Val->bits == g0AgainVal->bits &&
                   g0Val->bits == g0ThirdVal->bits;
    check("nextGaussian() matches OpenJDK 17 and setSeed clears the cached second sample", gaussOk);

    // Default constructor produces two valid, distinct seeded generators
    const uint32_t d1 = refOf(vm.callStatic(cls, u"newDefault", u"()Ljava/util/Random;"));
    const uint32_t d2 = refOf(vm.callStatic(cls, u"newDefault", u"()Ljava/util/Random;"));
    const auto v1 = vm.callStatic(cls, u"nextLong", u"(Ljava/util/Random;)J", std::array{ref(d1)});
    const auto v2 = vm.callStatic(cls, u"nextLong", u"(Ljava/util/Random;)J", std::array{ref(d2)});
    check("new Random() default constructor initializes distinct streams",
          d1 != 0 && d2 != 0 && d1 != d2 &&
          std::get_if<Value>(&v1) && std::get_if<Value>(&v2) &&
          std::get<Value>(v1).bits != std::get<Value>(v2).bits);

    std::cout << "done: " << failures << " failure(s)\n";
    return failures != 0;
}
