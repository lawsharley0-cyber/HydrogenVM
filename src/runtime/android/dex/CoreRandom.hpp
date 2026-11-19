// Java SE java.util.Random backed by DEX heap identities.
#pragma once
#include "runtime/android/dex/Heap.hpp"

#include <cstdint>
#include <map>
#include <mutex>
#include <optional>
#include <span>
#include <string_view>

namespace Hydrogen::Runtime::Android::Dex {

class CoreRandom {
public:
    static bool hasClass(std::u16string_view type);
    std::optional<Value> invoke(Heap& heap, std::u16string_view type,
                                std::u16string_view name, std::u16string_view signature,
                                bool isStatic, std::span<const Value> args);

private:
    struct State {
        uint64_t seed = 0;
        double nextNextGaussian = 0.0;
        bool haveNextNextGaussian = false;
    };

    static uint64_t initialScramble(uint64_t seed);
    static int32_t nextBits(State& state, int32_t bits);
    static int32_t nextBoundedInt(State& state, int32_t bound);
    static int64_t nextLong(State& state);
    static double nextDouble(State& state);
    static double nextGaussian(State& state);

    std::mutex mutex_;
    uint64_t seedUniquifier_ = 8682522807148012ULL;
    std::map<uint32_t, State> states_;
};

} // namespace Hydrogen::Runtime::Android::Dex
