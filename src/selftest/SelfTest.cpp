#include "SelfTest.hpp"
#include "core/cpu/Engine.hpp"
#include "loader/MachO.hpp"
#include <chrono>
#include <cstdio>

namespace Hydrogen {
namespace {

// Assembled from:
//   _start: push {r4-r6,lr}; mov r4,r0; ldr r5,=2463534242; mov r6,#0
//   1: mov r0,r5; blx xs; mov r5,r0; add r6,r6,r0; subs r4,r4,#1; bne 1b
//      mov r0,r6; pop {r4-r6,pc}
//   .thumb xs: eor r0,r0,r0,lsl #13; eor r0,r0,r0,lsr #17; eor r0,r0,r0,lsl #5; bx lr
constexpr uint8_t kProgram[] = {
    0x70, 0x40, 0x2D, 0xE9, 0x00, 0x40, 0xA0, 0xE1, 0x20, 0x50, 0x9F, 0xE5, 0x00, 0x60, 0xA0, 0xE3,
    0x05, 0x00, 0xA0, 0xE1, 0x06, 0x00, 0x00, 0xFA, 0x00, 0x50, 0xA0, 0xE1, 0x00, 0x60, 0x86, 0xE0,
    0x01, 0x40, 0x54, 0xE2, 0xF9, 0xFF, 0xFF, 0x1A, 0x06, 0x00, 0xA0, 0xE1, 0x70, 0x80, 0xBD, 0xE8,
    0xA2, 0x8C, 0xD6, 0x92, 0x80, 0xEA, 0x40, 0x30, 0x80, 0xEA, 0x50, 0x40, 0x80, 0xEA, 0x40, 0x10,
    0x70, 0x47,
};
constexpr uint32_t kLoad = 0x10000, kStackTop = 0x00100000;

uint32_t reference(uint32_t n) {
    uint32_t x = 2463534242u, s = 0;
    for (uint32_t i = 0; i < n; ++i) { x ^= x << 13; x ^= x >> 17; x ^= x << 5; s += x; }
    return s;
}

} // namespace

SelfTestResult runSelfTest(uint32_t iterations) {
    SelfTestResult res;
    try {
        Memory::GuestArena mem;
        CPU::Engine engine(mem);
        mem.map(kLoad, sizeof kProgram, Memory::kProtRX);
        mem.copyIn(kLoad, kProgram, sizeof kProgram);
        mem.map(kStackTop - 0x10000, 0x10000, Memory::kProtRW);
        const uint32_t halt = engine.registerHle("halt", [](CPU::Cpu& c) { c.exit = CPU::Exit::Halt; });

        CPU::Cpu cpu;
        cpu.r[0] = iterations;
        cpu.r[CPU::kSP] = kStackTop;
        cpu.r[CPU::kLR] = halt;
        cpu.r[CPU::kPC] = kLoad;
        const auto t0 = std::chrono::steady_clock::now();
        CPU::Exit e;
        while ((e = engine.run(cpu, 10'000'000)) == CPU::Exit::Budget) {}
        const double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        res.instructions = cpu.insn_count;
        res.mips = secs > 0 ? cpu.insn_count / 1e6 / secs : 0;
        const uint32_t want = reference(iterations);
        res.passed = e == CPU::Exit::Halt && cpu.r[0] == want;
        char buf[160];
        std::snprintf(buf, sizeof buf, "exit=%u result=0x%08X expected=0x%08X, %.1f M insns in %.3f s",
                      unsigned(e), cpu.r[0], want, cpu.insn_count / 1e6, secs);
        res.detail = buf;
    } catch (const std::exception& ex) {
        res.detail = ex.what();
    }
    return res;
}

} // namespace Hydrogen

namespace {
bool runMachOProbeTests() {
    using Bytes = std::vector<uint8_t>;
    int passed = 0, failed = 0;
    auto check2 = [&](const std::string& name, const Bytes& file, bool isMachO, bool hasArm32, bool encrypted, uint32_t cpuSubtype, const std::string& errorSnippet = "") {
        auto pr = Hydrogen::Loader::probeMachO(file);
        bool ok = true;
        if (pr.isMachO != isMachO) ok = false;
        if (pr.hasArm32 != hasArm32) ok = false;
        if (pr.encrypted != encrypted) ok = false;
        if (pr.cpuSubtype != cpuSubtype) ok = false;
        if (!errorSnippet.empty() && pr.error.find(errorSnippet) == std::string::npos) ok = false;
        else if (errorSnippet.empty() && !pr.error.empty()) ok = false;
        if (ok) ++passed; else ++failed;
    };

    check2("empty", {}, false, false, false, 0, "Mach-O: truncated (offset 0)");
    check2("random_bytes", {1, 2, 3, 4}, false, false, false, 0, "not a Mach-O file");

    auto num32 = [](Bytes& b, uint32_t v) {
        b.push_back(v & 0xff); b.push_back((v >> 8) & 0xff);
        b.push_back((v >> 16) & 0xff); b.push_back((v >> 24) & 0xff);
    };
    auto num32_be = [](Bytes& b, uint32_t v) {
        b.push_back((v >> 24) & 0xff); b.push_back((v >> 16) & 0xff);
        b.push_back((v >> 8) & 0xff); b.push_back(v & 0xff);
    };

    Bytes simple_armv7;
    num32(simple_armv7, 0xfeedface); num32(simple_armv7, 12); num32(simple_armv7, 9);
    num32(simple_armv7, 2); num32(simple_armv7, 0); num32(simple_armv7, 0); num32(simple_armv7, 0);
    simple_armv7.resize(28, 0);
    check2("simple_armv7", simple_armv7, true, true, false, 9);

    Bytes simple_x86;
    num32(simple_x86, 0xfeedface); num32(simple_x86, 7); num32(simple_x86, 3);
    num32(simple_x86, 2); num32(simple_x86, 0); num32(simple_x86, 0); num32(simple_x86, 0);
    check2("simple_x86", simple_x86, false, false, false, 0, "not a 32-bit ARM binary");

    Bytes fat_binary;
    num32_be(fat_binary, 0xcafebabe); num32_be(fat_binary, 2);
    num32_be(fat_binary, 7); num32_be(fat_binary, 3); num32_be(fat_binary, 4096); num32_be(fat_binary, 28); num32_be(fat_binary, 12);
    num32_be(fat_binary, 12); num32_be(fat_binary, 9); num32_be(fat_binary, 8192); num32_be(fat_binary, 28); num32_be(fat_binary, 12);
    fat_binary.resize(8192, 0);
    Bytes fat_armv7;
    num32(fat_armv7, 0xfeedface); num32(fat_armv7, 12); num32(fat_armv7, 9);
    num32(fat_armv7, 2); num32(fat_armv7, 0); num32(fat_armv7, 0); num32(fat_armv7, 0);
    fat_armv7.resize(28, 0);
    fat_binary.insert(fat_binary.end(), fat_armv7.begin(), fat_armv7.end());
    check2("fat_binary_armv7", fat_binary, true, true, false, 9);

    Bytes encrypted_armv7;
    num32(encrypted_armv7, 0xfeedface); num32(encrypted_armv7, 12); num32(encrypted_armv7, 9);
    num32(encrypted_armv7, 2); num32(encrypted_armv7, 1); num32(encrypted_armv7, 20); num32(encrypted_armv7, 0);
    num32(encrypted_armv7, 0x21); num32(encrypted_armv7, 20); num32(encrypted_armv7, 4096); num32(encrypted_armv7, 8192); num32(encrypted_armv7, 1);
    check2("encrypted_armv7", encrypted_armv7, true, true, true, 9);

    return failed == 0;
}
}
