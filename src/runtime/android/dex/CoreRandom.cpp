#include "runtime/android/dex/CoreRandom.hpp"
#include "runtime/android/dex/DexFile.hpp"

#include <algorithm>
#include <bit>
#include <chrono>
#include <cmath>

namespace Hydrogen::Runtime::Android::Dex {
namespace {

constexpr uint64_t kMultiplier = 0x5DEECE66DULL;
constexpr uint64_t kAddend = 0xBULL;
constexpr uint64_t kMask = (1ULL << 48) - 1;
constexpr uint64_t kUniquifierMultiplier = 1181783497276652981ULL;

int32_t integer(Value value) {
    if (value.kind != ValueKind::Word) throw FormatError("Random argument is not an int");
    return std::bit_cast<int32_t>(uint32_t(value.bits));
}

int64_t wideInt(Value value) {
    if (value.kind != ValueKind::Wide) throw FormatError("Random argument is not a long");
    return std::bit_cast<int64_t>(value.bits);
}

uint32_t reference(Value value) {
    if (value.kind != ValueKind::Reference) throw FormatError("Random argument is not a reference");
    return uint32_t(value.bits);
}

Value word(uint32_t bits) { return {ValueKind::Word, bits}; }
Value wide(uint64_t bits) { return {ValueKind::Wide, bits}; }

} // namespace

bool CoreRandom::hasClass(std::u16string_view type) {
    return type == u"Ljava/util/Random;" || type == u"Ljava/security/SecureRandom;";
}

uint64_t CoreRandom::initialScramble(uint64_t seed) {
    return (seed ^ kMultiplier) & kMask;
}

int32_t CoreRandom::nextBits(State& state, int32_t bits) {
    state.seed = (state.seed * kMultiplier + kAddend) & kMask;
    const unsigned shift = static_cast<unsigned>(48 - bits) & 63u;
    return std::bit_cast<int32_t>(static_cast<uint32_t>(state.seed >> shift));
}

int32_t CoreRandom::nextBoundedInt(State& state, int32_t bound) {
    if (bound <= 0) throw JavaException{u"Ljava/lang/IllegalArgumentException;"};
    const int32_t r = nextBits(state, 31);
    const int32_t m = bound - 1;
    if ((bound & m) == 0) {
        return static_cast<int32_t>((static_cast<uint64_t>(bound) * static_cast<uint64_t>(r)) >> 31);
    }
    int32_t u = r;
    int32_t val = u % bound;
    while (static_cast<int32_t>(static_cast<uint32_t>(u) - static_cast<uint32_t>(val) +
                                static_cast<uint32_t>(m)) < 0) {
        u = nextBits(state, 31);
        val = u % bound;
    }
    return val;
}

int64_t CoreRandom::nextLong(State& state) {
    const uint64_t hi = static_cast<uint64_t>(static_cast<int64_t>(nextBits(state, 32)));
    const uint64_t lo = static_cast<uint64_t>(static_cast<int64_t>(nextBits(state, 32)));
    return std::bit_cast<int64_t>((hi << 32) + lo);
}

double CoreRandom::nextDouble(State& state) {
    const int64_t hi = static_cast<int64_t>(nextBits(state, 26));
    const int64_t lo = static_cast<int64_t>(nextBits(state, 27));
    return static_cast<double>((hi << 27) + lo) * (1.0 / static_cast<double>(1LL << 53));
}

double CoreRandom::nextGaussian(State& state) {
    if (state.haveNextNextGaussian) {
        state.haveNextNextGaussian = false;
        return state.nextNextGaussian;
    }
    double v1 = 0.0;
    double v2 = 0.0;
    double s = 0.0;
    do {
        v1 = 2.0 * nextDouble(state) - 1.0;
        v2 = 2.0 * nextDouble(state) - 1.0;
        s = v1 * v1 + v2 * v2;
    } while (s >= 1.0 || s == 0.0);
    const double multiplier = std::sqrt(-2.0 * std::log(s) / s);
    state.nextNextGaussian = v2 * multiplier;
    state.haveNextNextGaussian = true;
    return v1 * multiplier;
}

std::optional<Value> CoreRandom::invoke(Heap& heap, std::u16string_view type,
                                        std::u16string_view name, std::u16string_view sig,
                                        bool isStatic, std::span<const Value> args) {
    if (isStatic || args.empty()) return {};
    const uint32_t id = reference(args.front());
    if (!id) return {};
    const auto actual = heap.typeOf(id);
    if (type != u"Ljava/util/Random;" && type != u"Ljava/security/SecureRandom;" &&
        actual != u"Ljava/util/Random;" && actual != u"Ljava/security/SecureRandom;") return {};

    std::lock_guard lock(mutex_);
    if (name == u"<init>" && sig == u"()V") {
        if (args.size() != 1) throw FormatError("invalid Random() arguments");
        seedUniquifier_ *= kUniquifierMultiplier;
        const uint64_t now = static_cast<uint64_t>(
            std::chrono::steady_clock::now().time_since_epoch().count());
        states_[id] = State{initialScramble(seedUniquifier_ ^ now), 0.0, false};
        return Value{ValueKind::Void};
    }
    if (name == u"<init>" && sig == u"(J)V") {
        if (args.size() != 2) throw FormatError("invalid Random(long) arguments");
        states_[id] = State{initialScramble(static_cast<uint64_t>(wideInt(args[1]))), 0.0, false};
        return Value{ValueKind::Void};
    }
    const auto found = states_.find(id);
    if (found == states_.end()) throw FormatError("Random used before its constructor");
    auto& state = found->second;

    if (name == u"setSeed" && sig == u"(J)V") {
        if (args.size() != 2) throw FormatError("invalid Random.setSeed arguments");
        state.seed = initialScramble(static_cast<uint64_t>(wideInt(args[1])));
        state.haveNextNextGaussian = false;
        return Value{ValueKind::Void};
    }
    if (name == u"next" && sig == u"(I)I") {
        if (args.size() != 2) throw FormatError("invalid Random.next arguments");
        return word(std::bit_cast<uint32_t>(nextBits(state, integer(args[1]))));
    }
    if (name == u"nextInt" && sig == u"()I") {
        if (args.size() != 1) throw FormatError("invalid Random.nextInt() arguments");
        return word(std::bit_cast<uint32_t>(nextBits(state, 32)));
    }
    if (name == u"nextInt" && sig == u"(I)I") {
        if (args.size() != 2) throw FormatError("invalid Random.nextInt(int) arguments");
        return word(std::bit_cast<uint32_t>(nextBoundedInt(state, integer(args[1]))));
    }
    if (name == u"nextLong" && sig == u"()J") {
        if (args.size() != 1) throw FormatError("invalid Random.nextLong() arguments");
        return wide(std::bit_cast<uint64_t>(nextLong(state)));
    }
    if (name == u"nextBoolean" && sig == u"()Z") {
        if (args.size() != 1) throw FormatError("invalid Random.nextBoolean() arguments");
        return word(nextBits(state, 1) != 0 ? 1u : 0u);
    }
    if (name == u"nextFloat" && sig == u"()F") {
        if (args.size() != 1) throw FormatError("invalid Random.nextFloat() arguments");
        const float value = static_cast<float>(nextBits(state, 24)) / static_cast<float>(1u << 24);
        return word(std::bit_cast<uint32_t>(value));
    }
    if (name == u"nextDouble" && sig == u"()D") {
        if (args.size() != 1) throw FormatError("invalid Random.nextDouble() arguments");
        return wide(std::bit_cast<uint64_t>(nextDouble(state)));
    }
    if (name == u"nextGaussian" && sig == u"()D") {
        if (args.size() != 1) throw FormatError("invalid Random.nextGaussian() arguments");
        return wide(std::bit_cast<uint64_t>(nextGaussian(state)));
    }
    if (name == u"nextBytes" && sig == u"([B)V") {
        if (args.size() != 2) throw FormatError("invalid Random.nextBytes arguments");
        const uint32_t arr = reference(args[1]);
        if (!arr) throw JavaException{u"Ljava/lang/NullPointerException;"};
        const uint32_t len = heap.length(arr);
        for (uint32_t i = 0; i < len;) {
            uint32_t rnd = std::bit_cast<uint32_t>(nextBits(state, 32));
            for (uint32_t n = std::min<uint32_t>(len - i, 4); n-- > 0; rnd >>= 8) {
                heap.setElement(arr, static_cast<int32_t>(i++), word(rnd & 0xffu));
            }
        }
        return Value{ValueKind::Void};
    }
    return {};
}

} // namespace Hydrogen::Runtime::Android::Dex
