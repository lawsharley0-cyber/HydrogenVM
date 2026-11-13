#include "runtime/Process.hpp"
#include <cstdio>
#include <limits>
using namespace Hydrogen;
using namespace Hydrogen::Runtime;
struct Time { int64_t value; int32_t scale; uint32_t flags; int64_t epoch; };
int main() {
    ProcessConfig cfg; cfg.homePath = "/tmp/hvm-media-time-test";
    Process p(cfg); CPU::Cpu cpu; cpu.r[13] = p.heap().allocZeroed(65536) + 65536 - 16;
    const auto add = p.lookupSymbol("_CMTimeAdd"), sub = p.lookupSymbol("_CMTimeSubtract");
    if (!add || !sub) { std::puts("FAIL: CMTimeAdd/Subtract missing"); return 1; }
    const auto out = p.heap().allocZeroed(24);
    auto invoke = [&](bool subtract, Time a, Time b) {
        std::vector<uint32_t> args(13); args[0] = out;
        std::memcpy(args.data() + 1, &a, 24); std::memcpy(args.data() + 7, &b, 24);
        p.callGuest(cpu, subtract ? *sub : *add, args);
        Time t; std::memcpy(&t, p.mem().host(out), 24); return t;
    };
    const auto sum = invoke(false, {1,2,1,0}, {1,3,1,0}), diff = invoke(true, {1,2,1,0}, {1,3,1,0});
    if (sum.value != 5 || sum.scale != 6 || sum.flags != 1 || diff.value != 1 || diff.scale != 6) {
        std::puts("FAIL: exact rational sum/difference and ARMv7 24-byte struct ABI"); return 1;
    }
    if (invoke(false, {1,2,1,3}, {1,2,1,4}).flags != 0 || invoke(false, {1,2,1,3}, {1,2,1,3}).epoch != 0 ||
        invoke(false, {1,2,1,0}, {1,2,1,3}).epoch != 3) { std::puts("FAIL: media time epoch rules"); return 1; }
    if (invoke(false, {0,0,5,0}, {0,0,9,0}).flags != 0 || invoke(true, {0,0,5,0}, {0,0,5,0}).flags != 0 ||
        invoke(false, {0,0,17,0}, {0,0,5,0}).flags != 5 || invoke(false, {0,0,0,0}, {1,1,1,0}).flags != 0) {
        std::puts("FAIL: invalid, infinity and indefinite arithmetic"); return 1;
    }
    const auto max = std::numeric_limits<int64_t>::max();
    const auto reduced = invoke(false, {max,2,1,0}, {max,2,1,0});
    if (reduced.value != max || reduced.scale != 1 || invoke(false, {max,1,1,0}, {max,1,1,0}).flags != 5) {
        std::puts("FAIL: media time overflow reduces scale before becoming infinite"); return 1;
    }
    const auto rounded = invoke(false, {1,2147483647,1,0}, {1,2147483646,1,0});
    if (rounded.scale != 2147483647 || rounded.value != 2 || rounded.flags != 3) { std::puts("FAIL: LCM overflow clamps scale and records rounding"); return 1; }
    std::puts("media time arithmetic: passed");
}
