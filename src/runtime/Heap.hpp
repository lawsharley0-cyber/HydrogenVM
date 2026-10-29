// HydrogenVM — guest heap (malloc/free/realloc) managed from the host.
//
// Block bookkeeping lives on the host side, so guest heap corruption can't
// corrupt the allocator. Small sizes come from per-size-class free lists
// carved out of 64 KB runs; large blocks use a best-fit free map with
// coalescing. All blocks are 16-byte aligned. Thread-safe.
#pragma once
#include "core/memory/GuestArena.hpp"
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
    enum class CopyResult { Success, BoundsError, NotOwned };
    CopyResult copyCStringWithinAllocation(uint32_t addr, const char* src, size_t chars);
    // `count` wide characters stored as 4-byte code units, plus the 4-byte NUL.
    // The live allocation must contain the whole span, and that check stays held
    // through the mapping lock and the write. Returns NotOwned if the pointer
    // is not within any live allocation.
    CopyResult copyWideWithinAllocation(uint32_t addr, const uint32_t* chars, size_t count);
    bool     owns(uint32_t addr) const { return addr >= base_ && addr < limit_; }

    struct Stats { uint64_t liveBytes = 0, liveBlocks = 0, committed = 0, badFrees = 0; };
    Stats stats() const;

private:
    static constexpr uint32_t kAlign = 16;
    static constexpr uint32_t kSmallMax = 2048;
    static constexpr uint32_t kClasses = kSmallMax / kAlign;
    static constexpr uint32_t kRun = 64 * 1024;

    uint32_t bigAlloc(uint32_t size);              // size already rounded
    void     bigFree(uint32_t addr, uint32_t size);
    void     ensureCommitted(uint32_t end);

    Memory::GuestArena& mem_;
    const uint32_t base_, limit_;
    uint32_t top_;                                 // bump pointer for fresh space
    uint32_t committed_;                           // [base_, committed_) is mapped
    std::vector<std::vector<uint32_t>> small_;     // free lists per class
    std::map<uint32_t, uint32_t> freeByAddr_;      // big free blocks: addr -> size
    std::multimap<uint32_t, uint32_t> freeBySize_; // size -> addr
    std::unordered_map<uint32_t, uint32_t> live_;  // addr -> usable size (rounded)
    std::map<uint32_t, uint32_t> liveIntervals_;   // ordered index for interior-pointer bounds
    mutable std::mutex mu_;
    Stats stats_;
};

} // namespace Hydrogen::Runtime
