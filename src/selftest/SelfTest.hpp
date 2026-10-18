// HydrogenVM — built-in smoke test, runnable on device from the app and on
// desktop from the test runner. Runs a tiny ARM program that calls a Thumb
// helper a million times (xorshift32), checks the result and measures speed.
#pragma once
#include <cstdint>
#include <string>

namespace Hydrogen {

struct SelfTestResult {
    bool        passed = false;
    double      mips = 0;
    uint64_t    instructions = 0;
    std::string detail;
};

SelfTestResult runSelfTest(uint32_t iterations = 1'000'000);

} // namespace Hydrogen
