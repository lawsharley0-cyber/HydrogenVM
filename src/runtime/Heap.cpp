#include "Heap.hpp"
#include <algorithm>
#include <cstdlib>
#include <cstring>

namespace Hydrogen::Runtime {

Heap::Heap(Memory::GuestArena& mem, uint32_t base, uint32_t limit)
    : mem_(mem), base_(base), limit_(limit), top_(base), committed_(base), small_(kClasses + 1),
      runClass_(((uint64_t(limit) - base) >> kRunShift) + 1, 0) {
    if (const char* q = std::getenv("HVM_HEAP_QUARANTINE")) quarantineMax_ = std::strtoul(q, nullptr, 10);
}

void Heap::ensureCommitted(uint32_t end) {
    if (end <= committed_) return;
    const uint32_t step = 4u << 20;   // commit 4 MB at a time
    uint32_t newEnd = std::min<uint64_t>(limit_, (uint64_t(end) + step - 1) / step * step);
    mem_.map(committed_, newEnd - committed_, Memory::kProtRW);
    stats_.committed += newEnd - committed_;
    committed_ = newEnd;
}

uint32_t Heap::bigAlloc(uint32_t size) {
    auto it = freeBySize_.lower_bound(size);
    if (it != freeBySize_.end()) {
        const uint32_t bsize = it->first, addr = it->second;
        freeBySize_.erase(it);
        freeByAddr_.erase(addr);
        if (bsize - size >= kAlign * 4) {   // split, keep the tail free
            const uint32_t rest = addr + size, rsize = bsize - size;
            freeByAddr_[rest] = rsize;
            freeBySize_.emplace(rsize, rest);
        } else {
            size = bsize;
        }
        live_[addr] = size;
        liveIntervals_[addr] = size;
        return addr;
    }
    if (uint64_t(top_) + size > limit_) return 0;
    const uint32_t addr = top_;
    top_ += size;
    ensureCommitted(top_);
    live_[addr] = size;
    liveIntervals_[addr] = size;
    return addr;
}

void Heap::bigFree(uint32_t addr, uint32_t size) {
    // Coalesce with neighbours.
    auto next = freeByAddr_.lower_bound(addr);
    if (next != freeByAddr_.end() && addr + size == next->first) {
        auto range = freeBySize_.equal_range(next->second);
        for (auto r = range.first; r != range.second; ++r) if (r->second == next->first) { freeBySize_.erase(r); break; }
        size += next->second;
        next = freeByAddr_.erase(next);
    }
    if (next != freeByAddr_.begin()) {
        auto prev = std::prev(next);
        if (prev->first + prev->second == addr) {
            auto range = freeBySize_.equal_range(prev->second);
            for (auto r = range.first; r != range.second; ++r) if (r->second == prev->first) { freeBySize_.erase(r); break; }
            addr = prev->first;
            size += prev->second;
            freeByAddr_.erase(prev);
        }
    }
    if (addr + size == top_) { top_ = addr; return; }   // give back to the bump region
    freeByAddr_[addr] = size;
    freeBySize_.emplace(size, addr);
}

uint32_t Heap::carveRun(uint32_t rounded) {
    const uint32_t start = base_ + ((top_ - base_ + kRun - 1) & ~(kRun - 1));
    if (uint64_t(start) + kRun > limit_) return 0;
    const uint32_t gap = start - top_, gapAt = top_;
    top_ = start + kRun;
    ensureCommitted(top_);
    if (gap) bigFree(gapAt, gap);                  // alignment slack stays usable by big blocks
    runClass_[(start - base_) >> kRunShift] = uint16_t(rounded / kAlign);
    const size_t words = (size_t((top_ - base_) >> 4) + 63) / 64;
    if (smallLive_.size() < words) smallLive_.resize(std::max(words, smallLive_.size() * 2));
    return start;
}

// Block sizes as iOS's allocator (libmalloc, 32-bit) hands them out: "tiny" blocks up to 496
// bytes in 16-byte steps, "small" ones up to 15 KB in 512-byte steps, larger ones in whole
// pages. Games write past the end of their allocations without noticing on iOS when the
// overrun stays inside that rounding: Minion Rush 1.0.0 allocates a 1852-byte object and
// uses 12 bytes past it (a ring buffer at +0x734..0x748). Rounded to 16 bytes the next block
// started at +0x740 and its owner overwrote the ring's write pointer, so the game crashed
// writing through it (at +0x2822ce, when a level loads); rounded to 512 (2048), as on iOS,
// the overrun lands in the block's own slack. malloc_size() reports these sizes too.
uint32_t Heap::iosRoundedSize(uint32_t size) {
    if (size <= 496) return (size + kAlign - 1) & ~(kAlign - 1);
    if (size <= 15 * 1024) return (size + 511) & ~511u;
    return (size + 4095) & ~4095u;
}

uint32_t Heap::alloc(uint32_t size) {
    if (size == 0) size = 1;
    if (size > 0x7FFFFFF0u) return 0;
    const uint32_t rounded = iosRoundedSize(size);
    std::lock_guard<std::mutex> lk(mu_);
    uint32_t addr;
    if (rounded <= kSmallMax) {
        auto& list = small_[rounded / kAlign];
        if (list.empty()) {
            const uint32_t run = carveRun(rounded);
            if (!run) {
                // Out of fresh space: fall back to a big block (tracked in the maps).
                const uint32_t b = bigAlloc(rounded);
                if (!b) return 0;
                stats_.liveBytes += rounded; ++stats_.liveBlocks;
                return b;
            }
            const uint32_t count = kRun / rounded;
            list.reserve(list.size() + count);
            for (uint32_t k = count; k-- > 0;) list.push_back(run + k * rounded);
        }
        addr = list.back();
        list.pop_back();
        setSmallLive(addr, true);
        stats_.liveBytes += rounded;
    } else {
        addr = bigAlloc(rounded);
        if (!addr) return 0;
        stats_.liveBytes += live_[addr];
    }
    ++stats_.liveBlocks;
    return addr;
}

bool Heap::liveSpan(uint32_t addr, uint32_t& start, uint32_t& size) const {
    if (const uint32_t cls = smallClassAt(addr)) {
        const uint32_t run = base_ + ((addr - base_) & ~(kRun - 1));
        start = run + (addr - run) / cls * cls;
        size = cls;
        return start + cls <= run + kRun && smallLive(start);
    }
    auto it = liveIntervals_.upper_bound(addr);
    if (it == liveIntervals_.begin()) return false;
    --it;
    start = it->first;
    size = it->second;
    return uint64_t(addr) < uint64_t(start) + size;
}

uint32_t Heap::allocZeroed(uint32_t size) {
    const uint32_t a = alloc(size);
    if (a) std::memset(mem_.host(a), 0, blockSize(a));
    return a;
}

uint32_t Heap::allocAligned(uint32_t size, uint32_t align) {
    // Validate before rounding or adding alignment slack. Guest size_t is
    // 32-bit: wrapping here would turn a large request into a small live block.
    if (!align || (align & (align - 1)) || size > 0x7FFFFFF0u) return 0;
    if (align <= kAlign) return alloc(size);
    const uint64_t rounded = (uint64_t(std::max(size, kSmallMax + 1)) + kAlign - 1) & ~uint64_t(kAlign - 1);
    const uint64_t requested = rounded + align;
    if (requested > UINT32_MAX) return 0;
    // Over-allocate and free the slack in front (big blocks only).
    std::lock_guard<std::mutex> lk(mu_);
    const uint32_t raw = bigAlloc(uint32_t(requested));
    if (!raw) return 0;
    const uint32_t aligned = (raw + align - 1) & ~(align - 1);
    const uint32_t total = live_[raw];
    live_.erase(raw);
    liveIntervals_.erase(raw);
    if (aligned > raw) bigFree(raw, aligned - raw);
    live_[aligned] = total - (aligned - raw);
    liveIntervals_[aligned] = total - (aligned - raw);
    stats_.liveBytes += live_[aligned];
    ++stats_.liveBlocks;
    return aligned;
}

void Heap::free(uint32_t addr) {
    if (!addr) return;
    std::lock_guard<std::mutex> lk(mu_);
    uint32_t size;
    if (const uint32_t cls = smallClassAt(addr)) {
        if ((addr - base_) % kRun % cls || !smallLive(addr)) { ++stats_.badFrees; return; }
        setSmallLive(addr, false);
        size = cls;
    } else {
        auto it = live_.find(addr);
        if (it == live_.end()) { ++stats_.badFrees; return; }
        size = it->second;
        live_.erase(it);
        liveIntervals_.erase(addr);
    }
    stats_.liveBytes -= size;
    --stats_.liveBlocks;
    if (quarantineMax_) {
        quarantine_.push_back({addr, size});
        if (quarantine_.size() <= quarantineMax_) return;
        addr = quarantine_.front().first;
        const uint32_t oldest = quarantine_.front().second;
        quarantine_.pop_front();
        release(addr, oldest);
        return;
    }
    release(addr, size);
}
void Heap::release(uint32_t addr, uint32_t size) {
    if (smallClassAt(addr)) small_[size / kAlign].push_back(addr);
    else bigFree(addr, size);
}

uint32_t Heap::realloc(uint32_t addr, uint32_t size) {
    if (!addr) return alloc(size);
    if (size == 0) { free(addr); return 0; }
    const uint32_t old = blockSize(addr);
    if (!old) return 0;
    if (size <= old && (old <= kSmallMax || size > old / 2)) return addr;
    const uint32_t n = alloc(size);
    if (!n) return 0;
    std::memcpy(mem_.host(n), mem_.host(addr), std::min(old, size));
    free(addr);
    return n;
}

uint32_t Heap::blockSize(uint32_t addr) const {
    std::lock_guard<std::mutex> lk(mu_);
    if (const uint32_t cls = smallClassAt(addr))
        return ((addr - base_) % kRun % cls == 0 && smallLive(addr)) ? cls : 0;
    auto it = live_.find(addr);
    return it == live_.end() ? 0 : it->second;
}

bool Heap::copyCStringWithinAllocation(uint32_t addr, const char* src, size_t chars) {
    std::lock_guard<std::mutex> lk(mu_);
    uint32_t start, size;
    if (!liveSpan(addr, start, size)) return false;
    const uint64_t end = uint64_t(start) + size;
    if (chars + uint64_t(1) > end - addr) return false;
    return mem_.copyCStringWritable(addr, src, chars);
}

bool Heap::copyWideWithinAllocation(uint32_t addr, const char* chars, size_t count) {
    if (count > (UINT64_MAX / 4) - 1) return false;
    const uint64_t bytes = (uint64_t(count) + 1) * 4;
    std::lock_guard<std::mutex> lk(mu_);
    uint32_t start, size;
    if (!liveSpan(addr, start, size)) return false;
    const uint64_t end = uint64_t(start) + size;
    if (bytes > end - addr) return false;
    return mem_.copyWideWritable(addr, chars, count);
}

Heap::Stats Heap::stats() const {
    std::lock_guard<std::mutex> lk(mu_);
    return stats_;
}

} // namespace Hydrogen::Runtime
