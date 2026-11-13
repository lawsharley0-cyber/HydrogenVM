#include "runtime/Process.hpp"
#include <algorithm>
#include <limits>
#include <numeric>

namespace Hydrogen::Runtime::UIKit {
namespace {
struct Time {
    int64_t value; int32_t scale; uint32_t flags; int64_t epoch;
    static constexpr bool kGuestStruct = true;
};
static_assert(sizeof(Time) == 24);
constexpr uint32_t valid = 1, rounded = 2, positiveInfinity = 4, negativeInfinity = 8, indefinite = 16;
Time special(uint32_t flag) { return {0, 0, flag ? valid | flag : 0, 0}; }
__int128 roundDefault(__int128 numerator, __int128 denominator, bool& wasRounded) {
    const __int128 quotient = numerator / denominator, remainder = numerator % denominator;
    wasRounded |= remainder != 0;
    const auto magnitude = remainder < 0 ? -remainder : remainder;
    return quotient + (magnitude * 2 >= denominator ? (numerator < 0 ? -1 : 1) : 0);
}
// CoreMedia's documented rational/overflow/epoch rules, using widened integer
// intermediates so neither large values nor INT64_MIN subtraction invoke UB.
// https://developer.apple.com/documentation/coremedia/cmtimeadd(_:_:).md
Time calculate(Time a, Time b, bool subtract) {
    if (!(a.flags & valid) || !(b.flags & valid)) return special(0);
    auto infinity = [](Time t) { return t.flags & positiveInfinity ? 1 : t.flags & negativeInfinity ? -1 : 0; };
    const int ai = infinity(a), bi = infinity(b) * (subtract ? -1 : 1);
    if (ai || bi) {
        if (ai && bi && ai != bi) return special(0);
        return special((ai ? ai : bi) > 0 ? positiveInfinity : negativeInfinity);
    }
    if ((a.flags | b.flags) & indefinite) return special(indefinite);
    if (a.scale <= 0 || b.scale <= 0 || (a.epoch && b.epoch && a.epoch != b.epoch)) return special(0);
    const int64_t common = int64_t(a.scale / std::gcd(a.scale, b.scale)) * b.scale;
    int32_t scale = int32_t(std::min<int64_t>(common, std::numeric_limits<int32_t>::max()));
    const __int128 numerator = __int128(a.value) * b.scale + (subtract ? -1 : 1) * __int128(b.value) * a.scale;
    const __int128 denominator = __int128(a.scale) * b.scale;
    const bool inputRounded = (a.flags | b.flags) & rounded;
    for (;;) {
        bool wasRounded = inputRounded;
        const auto value = roundDefault(numerator * scale, denominator, wasRounded);
        if (value >= std::numeric_limits<int64_t>::min() && value <= std::numeric_limits<int64_t>::max())
            return {int64_t(value), scale, valid | (wasRounded ? rounded : 0), a.epoch == b.epoch ? 0 : a.epoch ? a.epoch : b.epoch};
        if (scale == 1) return special(value < 0 ? negativeInfinity : positiveInfinity);
        scale = std::max(1, scale / 2);
    }
}
void add(Ctx& x, GuestPtr out, Time a, Time b) { const auto t = calculate(a, b, false); x.mem.copyIn(out, &t, sizeof(t)); }
void subtract(Ctx& x, GuestPtr out, Time a, Time b) { const auto t = calculate(a, b, true); x.mem.copyIn(out, &t, sizeof(t)); }
}
void registerCoreMediaTime(Process& p) {
    p.def<&add>("_CMTimeAdd");
    p.def<&subtract>("_CMTimeSubtract");
}
}
