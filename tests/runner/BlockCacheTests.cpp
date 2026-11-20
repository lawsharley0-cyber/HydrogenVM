// Persistent decoded-block cache tests (Phase E1): verifies ARM/Thumb block
// serialization with zero host pointers, lazy hash-checked hits on a fresh
// Engine, automatic invalidation when guest code bytes change, and clean
// rejection of corrupt/truncated cache files.
#include "core/cpu/Engine.hpp"
#include "core/memory/GuestArena.hpp"
#include <cstdio>
#include <cstdint>
#include <fstream>
#include <functional>
#include <string>
#include <vector>

using namespace Hydrogen::CPU;
using namespace Hydrogen::Memory;

int runBlockCacheTests(const std::function<void(bool, const char*)>& check) {
    int failures = 0;
    auto ck = [&](bool ok, const char* what) { check(ok, what); failures += !ok; };

    const std::string cachePath = "/tmp/hvm-block-cache-test.hvbc";
    std::remove(cachePath.c_str());

    // Tiny ARMv7 loop: sum 1..r0 into r1, return r1 in r0.
    //   0: mov r1, #0          (0xE3A01000)
    //   4: cmp r0, #0          (0xE3500000)
    //   8: beq +12 (0x14)      (0x0A000002)
    //  12: add r1, r1, r0      (0xE0811000)
    //  16: sub r0, r0, #1      (0xE2400001)
    //  20: b   -16 (0x04)      (0xEAFFFFFA)
    //  24: mov r0, r1          (0xE1A00001)
    //  28: bx  lr              (0xE12FFF1E)
    const uint32_t kCode = 0x00100000u;
    const uint32_t kStack = 0x00200000u;
    const std::vector<uint32_t> armSum = {
        0xE3A01000u,
        0xE3500000u,
        0x0A000002u,
        0xE0811000u,
        0xE2400001u,
        0xEAFFFFFAu,
        0xE1A00001u,
        0xE12FFF1Eu,
    };

    // Tiny Thumb function at kCode + 0x100: r0 = (r0 + 7) * 2; bx lr
    //   adds r0, #7   (0x3007)
    //   lsls r0, r0,#1(0x0040)
    //   bx   lr       (0x4770)
    const uint32_t kThumb = kCode + 0x100u;
    const std::vector<uint16_t> thumbFn = { 0x3007u, 0x0040u, 0x4770u };

    auto loadCode = [&](GuestArena& arena) {
        arena.map(kCode, 4096);
        arena.map(kStack - 4096, 4096);
        for (size_t i = 0; i < armSum.size(); ++i)
            arena.write<uint32_t>(kCode + uint32_t(i * 4), armSum[i]);
        for (size_t i = 0; i < thumbFn.size(); ++i)
            arena.write<uint16_t>(kThumb + uint32_t(i * 2), thumbFn[i]);
    };

    // 1. Cold run: decode and save cache.
    {
        GuestArena arena;
        loadCode(arena);
        Engine eng(arena);
        Cpu cpu;
        cpu.r[kSP] = kStack - 16;
        const uint32_t rArm = uint32_t(eng.callGuest(cpu, kCode, {10}));
        const uint32_t rThumb = uint32_t(eng.callGuest(cpu, kThumb | 1u, {5}));
        ck(rArm == 55 && rThumb == 24, "block cache cold run produces expected ARM and Thumb results");
        const size_t saved = eng.saveBlockCache(cachePath);
        ck(saved >= 3, "block cache saves decoded ARM and Thumb blocks to disk");
    }

    // 2. Warm run on a fresh Engine: should serve blocks from disk with zero fresh decodes of guest blocks.
    {
        GuestArena arena;
        loadCode(arena);
        Engine eng(arena);
        const size_t loaded = eng.loadBlockCache(cachePath);
        ck(loaded >= 3, "block cache loads persisted blocks lazily into fresh Engine");
        Cpu cpu;
        cpu.r[kSP] = kStack - 16;
        const uint32_t rArm = uint32_t(eng.callGuest(cpu, kCode, {10}));
        const uint32_t rThumb = uint32_t(eng.callGuest(cpu, kThumb | 1u, {5}));
        const auto bcStats = eng.blockCacheStats();
        ck(rArm == 55 && rThumb == 24, "block cache warm run produces identical ARM and Thumb results");
        ck(bcStats.diskHits >= 3 && bcStats.diskHashMismatches == 0,
           "block cache warm run hits persisted Op records with zero hash mismatches");
    }

    // 3. Modified guest code byte: hash mismatch must reject stale cached block and decode new code.
    {
        GuestArena arena;
        loadCode(arena);
        // Patch Thumb function from adds r0, #7 -> adds r0, #10 (0x300A)
        arena.write<uint16_t>(kThumb, 0x300Au);
        Engine eng(arena);
        eng.loadBlockCache(cachePath);
        Cpu cpu;
        cpu.r[kSP] = kStack - 16;
        const uint32_t rThumb = uint32_t(eng.callGuest(cpu, kThumb | 1u, {5}));
        const auto bcStats = eng.blockCacheStats();
        ck(rThumb == 30 && bcStats.diskHashMismatches >= 1,
           "block cache detects modified guest code via hash and re-decodes safely");
    }

    // 4. Corrupt cache file must be rejected cleanly.
    {
        {
            std::ofstream f(cachePath, std::ios::binary | std::ios::trunc);
            const char junk[] = "HVBC\x01\x00\x00\x00" "bad-corrupt-payload";
            f.write(junk, sizeof(junk));
        }
        GuestArena arena;
        Engine eng(arena);
        ck(eng.loadBlockCache(cachePath) == 0, "block cache rejects corrupt or truncated cache files");
    }

    std::remove(cachePath.c_str());
    return failures;
}
