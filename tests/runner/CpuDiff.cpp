// hvm_cpudiff: runs short instruction sequences on HydrogenVM's CPU and prints the resulting
// state, so tools/cpudiff/cpudiff.py can compare it with an independent ARM emulator.
//
// Reads one case per line on stdin (all numbers hex):
//   <thumb 0|1> <code bytes, no spaces> r0..r12 <nzcv nibble> <q> <ge nibble> <fpscr> d0..d31
// Writes "halt <addr>" first (the LR every case starts with), then one line per case:
//   <status> r0..r12 sp <nzcv> <q> <ge> <fpscr> d0..d31 <window fnv1a64>
// status: ok | undef | fault:<addr> | other:<exit>
//
// Layout shared with the Python side: code at kCode (each case gets its own slot, so no block
// cache invalidation is needed), a 4 KB data window at kWindow filled with a fixed pattern,
// SP = kWindow + 0x800. The sequence ends with `bx lr`; LR points at a halt stub.
#include "core/cpu/Engine.hpp"

#include <cinttypes>
#include <cstdio>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

using namespace Hydrogen;

namespace {

constexpr uint32_t kCode = 0x00100000u, kCodeSize = 0x01000000u, kSlot = 64;
constexpr uint32_t kWindow = 0x10000000u, kWindowSize = 0x1000u;

uint8_t pattern(uint32_t i) { return uint8_t(i * 131u + 7u); }

uint64_t fnv(const uint8_t* p, size_t n) {
    uint64_t h = 1469598103934665603ull;
    for (size_t i = 0; i < n; ++i) { h ^= p[i]; h *= 1099511628211ull; }
    return h;
}

} // namespace

int main() {
    Memory::GuestArena mem;
    CPU::Engine engine{mem};
    mem.map(kCode, kCodeSize, Memory::kProtRX);
    mem.map(kWindow, kWindowSize, Memory::kProtRW);
    const uint32_t halt = engine.registerHle("halt", [](CPU::Cpu& c) { c.exit = CPU::Exit::Halt; });
    uint32_t slot = 0;
    std::printf("halt %x\n", halt);   // the LR value, so the other emulator starts identically

    std::string line;
    while (std::getline(std::cin, line)) {
        if (line.empty() || line[0] == '#') continue;
        std::istringstream in(line);
        uint32_t thumb = 0;
        std::string code;
        in >> std::hex >> thumb >> code;
        std::vector<uint8_t> bytes;
        for (size_t i = 0; i + 1 < code.size(); i += 2) bytes.push_back(uint8_t(std::stoul(code.substr(i, 2), nullptr, 16)));
        if (thumb) { bytes.push_back(0x70); bytes.push_back(0x47); }                              // bx lr
        else { for (uint8_t b : {0x1E, 0xFF, 0x2F, 0xE1}) bytes.push_back(b); }                  // bx lr
        const uint32_t at = kCode + slot++ * kSlot;   // slot i holds case i, as on the Python side
        if (bytes.size() > kSlot || at >= kCode + kCodeSize) { std::printf("other:too-long\n"); continue; }
        // An earlier case may have branched forward into this slot and decoded what was there
        // then: drop that before writing, as cacheflush does for a game.
        if (engine.hasBlocksIn(at, kSlot)) engine.retireRange(at, kSlot);
        for (size_t i = 0; i < bytes.size(); ++i) mem.write<uint8_t>(at + uint32_t(i), bytes[i]);
        for (uint32_t i = 0; i < kWindowSize; ++i) mem.write<uint8_t>(kWindow + i, pattern(i));

        CPU::Cpu c;
        for (int i = 0; i < 13; ++i) in >> c.r[i];
        uint32_t nzcv = 0, q = 0, ge = 0;
        in >> nzcv >> q >> ge >> c.fpscr;
        c.nzcv = (nzcv & 0xF) << 28;
        c.q = q & 1;
        c.ge = (ge & 0xF) << 16;
        for (int i = 0; i < 32; ++i) in >> c.d[i];
        c.r[CPU::kSP] = kWindow + 0x800;
        c.r[CPU::kLR] = halt;
        c.r[CPU::kPC] = at;
        c.thumb = thumb;

        const CPU::Exit e = engine.run(c, 10'000);
        char status[48];
        if (e == CPU::Exit::Halt) std::snprintf(status, sizeof status, "ok");
        else if (e == CPU::Exit::Undefined) std::snprintf(status, sizeof status, "undef");
        else if (e == CPU::Exit::MemFault) std::snprintf(status, sizeof status, "fault:%x", c.exit_info);
        else std::snprintf(status, sizeof status, "other:%u", unsigned(e));
        std::printf("%s", status);
        for (int i = 0; i < 14; ++i) std::printf(" %x", c.r[i]);
        std::printf(" %x %x %x %x", c.nzcv >> 28, c.q, (c.ge >> 16) & 0xF, c.fpscr);
        for (int i = 0; i < 32; ++i) std::printf(" %" PRIx64, c.d[i]);
        std::printf(" %" PRIx64 "\n", fnv(mem.host(kWindow), kWindowSize));
        std::fflush(stdout);
    }
    return 0;
}
