// HydrogenVM — Phase E4 quick-save & resume snapshot spike.
//
// Captures every mapped non-HLE guest page (sparse 4 KB pages + protections)
// and guest thread architectural CPU state (r0..r15, NZCV/Q/GE, Thumb, VFP/NEON
// d0..d35, FPSCR, TPIDRs) into an atomic, FNV-1a-checksummed `.hvqs` file so a
// guest paused at a safepoint/budget boundary can resume mid-execution after an
// app kill.
#pragma once
#include "Cpu.hpp"
#include "../memory/GuestArena.hpp"
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace Hydrogen::CPU {

inline constexpr size_t kQuickSnapshotMaxPages = 262144; // 1 GiB of guest pages

struct QuickSaveStats {
    size_t pagesSaved = 0;
    size_t threadsSaved = 0;
    size_t bytesWritten = 0;
};

struct QuickSaveThreadState {
    uint32_t r[16] = {};
    uint32_t nzcv = 0;
    uint32_t q = 0;
    uint32_t ge = 0;
    uint32_t thumb = 0;
    uint64_t d[36] = {};
    uint32_t fpscr = 0;
    uint32_t tpidrurw = 0;
    uint32_t tpidruro = 0;
    uint32_t reserved = 0;
};

// Save all mapped non-HLE guest pages from `mem` and the given paused `threads`
// atomically to `path` (`.tmp` + `rename`). Callers can lower the page limit.
bool saveQuickSnapshot(const std::string& path,
                       const Memory::GuestArena& mem,
                       const std::vector<const Cpu*>& threads,
                       QuickSaveStats* statsOut = nullptr,
                       size_t pageLimit = kQuickSnapshotMaxPages);

// Verify checksum and restore all saved pages into `mem` and architectural
// register state into `threadsOut`. Strictly refuses truncated/corrupt files.
bool restoreQuickSnapshot(const std::string& path,
                          Memory::GuestArena& mem,
                          std::vector<QuickSaveThreadState>& threadsOut);

// Apply a restored QuickSaveThreadState to a live Cpu (outside guest code).
void applyQuickSaveThread(Cpu& dst, const QuickSaveThreadState& src);

} // namespace Hydrogen::CPU
