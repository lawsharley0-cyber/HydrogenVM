// The batch runner creates and destroys one GuestArena per game. Each arena is a
// 4 GB reservation; if every destruction munmaps it and the next creation asks
// the kernel for another 4 GB, the host can transiently refuse the reservation
// even though no arena is live (issue #307: errno 12, live arenas 0, later games
// reserve fine). A destroyed arena must therefore keep its range reserved,
// PROT_NONE, and hand it to the next GuestArena.
#include "core/memory/GuestArena.hpp"
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

using namespace Hydrogen;

namespace {

#if defined(__linux__)
struct Mapping {
    uint64_t start = 0, end = 0;
    std::string perms;
};

std::vector<Mapping> readMappings() {
    std::vector<Mapping> out;
    std::ifstream maps("/proc/self/maps");
    std::string line;
    while (std::getline(maps, line)) {
        unsigned long long start = 0, end = 0;
        char perms[5] = {0};
        if (std::sscanf(line.c_str(), "%llx-%llx %4s", &start, &end, perms) != 3) continue;
        out.push_back({uint64_t(start), uint64_t(end), std::string(perms)});
    }
    return out;
}

uint64_t totalMappedBytes() {
    uint64_t total = 0;
    for (const auto& m : readMappings()) total += m.end - m.start;
    return total;
}
#endif

} // namespace

int runArenaPoolTests() {
    int failed = 0;
    auto check = [&](bool ok, const char* name) {
        std::printf("%s %s\n", ok ? "ok" : "FAIL", name);
        if (!ok) ++failed;
    };

    const uint32_t guestPage = 0x10000000;
    const uint32_t span = 4 * Memory::GuestArena::kGuestPageSize;   // one host page on iOS, four on Linux
    const uint32_t baselineLive = Memory::GuestArena::liveCount();

    // Warm up once so the baseline includes the pooled range and the allocator
    // metadata vectors are already at their steady size.
    { Memory::GuestArena warm; }
    const uint32_t firstLive = Memory::GuestArena::liveCount();
    uint8_t* firstBase = nullptr;

#if defined(__linux__)
    // Small allocator mappings come and go; a 4 GB reservation does not. Allow
    // 64 MiB of unrelated address-space churn, never a whole arena.
    const uint64_t tolerance = 64ull << 20;
    const uint64_t baselineBytes = totalMappedBytes();
#endif

    bool reused = true, zeroed = true, counted = true, retained = true, noAccess = true;

    for (int i = 0; i < 50; ++i) {
        uint8_t* base = nullptr;
        {
            Memory::GuestArena arena;
            base = arena.base();
            if (Memory::GuestArena::liveCount() != firstLive + 1) counted = false;
            arena.map(guestPage, span, Memory::kProtRW);
            for (uint32_t off = 0; off < span; off += 4096)
                arena.write<uint32_t>(guestPage + off, 0xA5A5A5A5u ^ off);
        }
        if (Memory::GuestArena::liveCount() != firstLive) counted = false;
        if (!firstBase) firstBase = base;
        if (base != firstBase) reused = false;

#if defined(__linux__)
        // Address space must not shrink back to the kernel, and the retained
        // range must be inaccessible so a stale guest pointer still faults.
        const uint64_t now = totalMappedBytes();
        if (now + tolerance < baselineBytes || now > baselineBytes + tolerance) retained = false;
        bool found = false;
        for (const auto& m : readMappings()) {
            if (m.start != uint64_t(base)) continue;
            found = true;
            if (m.end - m.start < Memory::GuestArena::kArenaSize) retained = false;
            if (m.perms != "---p") noAccess = false;
        }
        if (!found) { retained = false; noAccess = false; }
#endif

        {
            Memory::GuestArena next;
            if (next.base() != base) reused = false;
            next.map(guestPage, span, Memory::kProtRW);
            for (uint32_t off = 0; off < span; off += 4096)
                if (next.read<uint32_t>(guestPage + off) != 0) zeroed = false;
        }
    }

    check(reused, "guest arena pool reuses one 4 GB reservation across 50 destroy/create cycles");
    check(zeroed, "a reused guest arena hands out zeroed memory");
    check(counted, "liveCount still counts live arenas, not pooled reservations");
#if defined(__linux__)
    check(retained, "a destroyed arena keeps its address-space reservation (/proc/self/maps size flat)");
    check(noAccess, "a pooled reservation is PROT_NONE, so stale guest pointers fault");
#endif
    check(Memory::GuestArena::liveCount() == baselineLive, "the test leaves no live arena behind");
    std::printf("arena pool: %d failed\n", failed);
    return failed ? 1 : 0;
}

#ifndef HVM_ARENA_POOL_NO_MAIN
int main() { return runArenaPoolTests(); }
#endif
