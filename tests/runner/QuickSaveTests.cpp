// Phase E4 quick-save & resume tests: runs an ARMv7 guest function halfway
// through execution, saves a snapshot, tears down the entire GuestArena and
// Engine (simulating an app kill), restores into a brand-new GuestArena and
// Engine, resumes to completion, and checks exact register + memory parity and
// corrupt-snapshot rejection.
#include "core/cpu/Engine.hpp"
#include "core/cpu/QuickSave.hpp"
#include "core/memory/GuestArena.hpp"
#include <cstdio>
#include <cstdint>
#include <fstream>
#include <functional>
#include <string>
#include <vector>

using namespace Hydrogen::CPU;
using namespace Hydrogen::Memory;

int runQuickSaveTests(const std::function<void(bool, const char*)>& check) {
    int failures = 0;
    auto ck = [&](bool ok, const char* what) { check(ok, what); failures += !ok; };

    const std::string snapPath = "/tmp/hvm-quicksave-test.hvqs";
    std::remove(snapPath.c_str());

    // ARMv7 loop at 0x00100000:
    //   0: cmp r0, #0          (0xE3500000)
    //   4: beq +12 (0x14)      (0x0A000002)
    //   8: add r1, r1, r0      (0xE0811000)
    //  12: sub r0, r0, #1      (0xE2400001)
    //  16: b   -16 (0x00)      (0xEAFFFFFA)
    //  20: str r1, [r2, #0]    (0xE5821000)
    //  24: mov r0, r1          (0xE1A00001)
    //  28: svc #0              (0xEF000000)
    const uint32_t kCode = 0x00100000u;
    const uint32_t kData = 0x00200000u;
    const std::vector<uint32_t> loopCode = {
        0xE3500000u,
        0x0A000002u,
        0xE0811000u,
        0xE2400001u,
        0xEAFFFFFAu,
        0xE5821000u,
        0xE1A00001u,
        0xEF000000u,
    };

    // 1. Run halfway (budget = 25 instructions out of sum 1..20), then snapshot!
    QuickSaveStats stats{};
    {
        GuestArena arena;
        arena.map(kCode, 4096, kProtRWX);
        arena.map(kData, 4096, kProtRW);
        for (size_t i = 0; i < loopCode.size(); ++i)
            arena.write<uint32_t>(kCode + uint32_t(i * 4), loopCode[i]);

        Engine eng(arena);
        Cpu cpu;
        cpu.r[0] = 20;      // sum 1..20 = 210
        cpu.r[1] = 0;
        cpu.r[2] = kData + 64;
        cpu.r[kPC] = kCode;
        cpu.d[3] = 0x400921FB54442D18ULL; // pi in D3 to verify VFP state survives

        const Exit mid = eng.run(cpu, 25);
        ck(mid == Exit::Budget && cpu.r[0] > 0 && cpu.r[0] < 20,
           "quick-save pauses guest cleanly mid-loop at budget boundary");

        const bool okSave = saveQuickSnapshot(snapPath, arena, {&cpu}, &stats, 2);
        ck(okSave && stats.pagesSaved == 2 && stats.threadsSaved == 1,
           "quick-save writes sparse guest pages and CPU state atomically");
    }

    // An oversized snapshot must fail before reading pages or replacing the prior save.
    {
        GuestArena arena;
        arena.map(kData, 2 * GuestArena::kGuestPageSize, kProtRead);
        ck(!saveQuickSnapshot(snapPath, arena, {}, nullptr, 1),
           "quick-save rejects captures above the page limit and retains the prior snapshot");
    }

    // 2. Restore into a completely fresh GuestArena + Engine + Cpu and finish execution!
    {
        GuestArena arena;
        std::vector<QuickSaveThreadState> restored;
        const bool okLoad = restoreQuickSnapshot(snapPath, arena, restored);
        const bool hasThread = okLoad && restored.size() == 1;
        ck(hasThread, "quick-save restores verified snapshot into fresh arena");
        if (!hasThread) {
            std::remove(snapPath.c_str());
            return failures;
        }

        Engine eng(arena);
        Cpu cpu;
        applyQuickSaveThread(cpu, restored[0]);
        ck(cpu.d[3] == 0x400921FB54442D18ULL, "quick-save preserves VFP/NEON register state");

        const Exit fin = eng.run(cpu, 10000);
        ck(fin == Exit::Svc && cpu.r[0] == 210 && arena.read<uint32_t>(kData + 64) == 210,
           "quick-save resumes mid-loop after kill and computes exact final result");
    }

    // 3. Flip 1 byte in the snapshot file -> checksum verification must refuse it!
    {
        std::fstream f(snapPath, std::ios::binary | std::ios::in | std::ios::out);
        f.seekg(0, std::ios::end);
        const std::streamoff sz = f.tellg();
        if (sz > 40) {
            f.seekg(sz - 5);
            char b = 0;
            f.read(&b, 1);
            b ^= 0x55;
            f.seekp(sz - 5);
            f.write(&b, 1);
        }
        f.close();

        GuestArena arena;
        std::vector<QuickSaveThreadState> restored;
        ck(!restoreQuickSnapshot(snapPath, arena, restored),
           "quick-save refuses tampered snapshot with checksum mismatch");
    }

    std::remove(snapPath.c_str());
    return failures;
}
