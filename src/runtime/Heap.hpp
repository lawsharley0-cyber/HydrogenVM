// HydrogenVM — guest heap (malloc/free/realloc) managed from the host.
//
// Block bookkeeping lives on the host side, so guest heap corruption can't
// corrupt the allocator. Small sizes come from per-size-class free lists
// carved out of 64 KB runs; large blocks use a best-fit free map with
// coalescing. All blocks are 16-byte aligned. Thread-safe.
#pragma once
#include "core/memory/GuestArena.hpp"
#include <deque>
#include <map>
#include <mutex>
#include <unordered_map>
#include <vector>

namespace Hydrogen::Runtime {

class Heap {
public:
    Heap(Memory::GuestArena& mem, uint32_t base, uint32_t limit);

    uint32_t alloc(uint32_t size);                 // 0 on failure
    uint32_t allocZeroed(uint32_t size);
    uint32_t allocAligned(uint32_t size, uint32_t align);
    void     free(uint32_t addr);                  // ignores 0 and unknown pointers (logs via callback)
    uint32_t realloc(uint32_t addr, uint32_t size);
    uint32_t blockSize(uint32_t addr) const;       // malloc_size(); 0 if not a heap block
    bool copyCStringWithinAllocation(uint32_t addr, const char* src, size_t chars);
    // `count` narrow bytes stored as wchar_t, plus the 4-byte NUL. The live
    // allocation must contain the whole span, and that check stays held
    // through the mapping lock and the write.
    bool copyWideWithinAllocation(uint32_t addr, const char* chars, size_t count);
    bool     owns(uint32_t addr) const { return addr >= base_ && addr < limit_; }

    struct Stats { uint64_t liveBytes = 0, liveBlocks = 0, committed = 0, badFrees = 0; };
    Stats stats() const;

private:
    static constexpr uint32_t kAlign = 16;
    static constexpr uint32_t kSmallMax = 2048;
    static constexpr uint32_t kClasses = kSmallMax / kAlign;
    static constexpr uint32_t kRunShift = 16;
    static constexpr uint32_t kRun = 1u << kRunShift;   // small runs are kRun-aligned from base_

    uint32_t bigAlloc(uint32_t size);              // size already rounded
    void     bigFree(uint32_t addr, uint32_t size);
    void     release(uint32_t addr, uint32_t size); // back to the free lists (caller holds mu_)
    void     ensureCommitted(uint32_t end);
    static uint32_t iosRoundedSize(uint32_t size);   // libmalloc's block size for a request
    uint32_t carveRun(uint32_t rounded);           // a fresh 64 KB run for one class; 0 if full
    // Small blocks: the class of the run holding addr and whether addr starts a live block.
    uint32_t smallClassAt(uint32_t addr) const {
        if (addr < base_ || addr >= top_) return 0;
        const uint32_t idx = (addr - base_) >> kRunShift;
        return idx < runClass_.size() ? runClass_[idx] * kAlign : 0;
    }
    bool smallLive(uint32_t addr) const {
        const uint32_t g = (addr - base_) >> 4;
        return (g >> 6) < smallLive_.size() && (smallLive_[g >> 6] >> (g & 63) & 1);
    }
    void setSmallLive(uint32_t addr, bool on) {
        const uint32_t g = (addr - base_) >> 4;
        if (on) smallLive_[g >> 6] |= uint64_t(1) << (g & 63);
        else smallLive_[g >> 6] &= ~(uint64_t(1) << (g & 63));
    }
    // The live block containing addr (interior pointers too); false if none.
    bool liveSpan(uint32_t addr, uint32_t& start, uint32_t& size) const;

    Memory::GuestArena& mem_;
    const uint32_t base_, limit_;
    uint32_t top_;                                 // bump pointer for fresh space
    uint32_t committed_;                           // [base_, committed_) is mapped
    std::vector<std::vector<uint32_t>> small_;     // free lists per class
    // A freed small block waits behind the last kReuseDelay freed blocks of its size before it is
    // handed out again. iOS rarely reuses a block at once, and games read memory they just freed:
    // Dead Space walks an EASTL hash table's old bucket array after the table regrew under it, which
    // worked on iOS and crashed 4 runs in 5 here when the very next allocation took that block.
    static constexpr size_t kReuseDelay = 64;
    std::vector<std::deque<uint32_t>> smallDelayed_;
    std::map<uint32_t, uint32_t> freeByAddr_;      // big free blocks: addr -> size
    std::multimap<uint32_t, uint32_t> freeBySize_; // size -> addr
    // Small blocks are tracked without per-block maps, which made every malloc/free cost a few
    // hundred ns of host allocation: each 64 KB run records its class, and a bitmap (one bit per
    // 16-byte granule) marks which blocks are live.
    std::vector<uint16_t> runClass_;               // per run: class (size / kAlign), 0 = not small
    std::vector<uint64_t> smallLive_;              // grown as runs are carved
    std::unordered_map<uint32_t, uint32_t> live_;  // big blocks: addr -> usable size (rounded)
    std::map<uint32_t, uint32_t> liveIntervals_;    // big blocks: ordered, for interior-pointer bounds
    // HVM_HEAP_QUARANTINE=N (a diagnostic): the last N freed blocks wait before they can be
    // handed out again. A game that reads memory after freeing it works on iOS, whose allocator
    // rarely reuses a block at once; if a crash goes away with this set, that is the cause.
    std::deque<std::pair<uint32_t, uint32_t>> quarantine_;
    size_t quarantineMax_ = 0;
    mutable std::mutex mu_;
    Stats stats_;
};

} // namespace Hydrogen::Runtime
