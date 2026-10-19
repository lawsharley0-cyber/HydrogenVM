#include "Heap.hpp"
#include <algorithm>
#include <cstring>

namespace Hydrogen::Runtime {

Heap::Heap(Memory::GuestArena& mem, uint32_t base, uint32_t limit)
    : mem_(mem), base_(base), limit_(limit), top_(base), committed_(base), small_(kClasses + 1) {}

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
        return addr;
    }
    if (uint64_t(top_) + size > limit_) return 0;
    const uint32_t addr = top_;
    top_ += size;
    ensureCommitted(top_);
    live_[addr] = size;
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

uint32_t Heap::alloc(uint32_t size) {
    if (size == 0) size = 1;
    if (size > 0x7FFFFFF0u) return 0;
    const uint32_t rounded = (size + kAlign - 1) & ~(kAlign - 1);
    std::lock_guard<std::mutex> lk(mu_);
    uint32_t addr;
    if (rounded <= kSmallMax) {
        auto& list = small_[rounded / kAlign];
        if (list.empty()) {
            // Carve a run into blocks of this class. The run itself isn't a
            // live block; individual blocks are.
            const uint32_t runSize = std::max(kRun, rounded * 8);
            if (uint64_t(top_) + runSize > limit_) {
                const uint32_t b = bigAlloc(rounded);
                if (!b) return 0;
                stats_.liveBytes += rounded; ++stats_.liveBlocks;
                return b;
            }
            const uint32_t run = top_;
            top_ += runSize;
            ensureCommitted(top_);
            const uint32_t count = runSize / rounded;
            list.reserve(list.size() + count);
            for (uint32_t k = count; k-- > 0;) list.push_back(run + k * rounded);
        }
        addr = list.back();
        list.pop_back();
        live_[addr] = rounded;
    } else {
        addr = bigAlloc(rounded);
        if (!addr) return 0;
    }
    stats_.liveBytes += live_[addr];
    ++stats_.liveBlocks;
    return addr;
}

uint32_t Heap::allocZeroed(uint32_t size) {
    const uint32_t a = alloc(size);
    if (a) std::memset(mem_.host(a), 0, blockSize(a));
    return a;
}

uint32_t Heap::allocAligned(uint32_t size, uint32_t align) {
    if (align <= kAlign) return alloc(size);
    // Over-allocate and free the slack in front (big blocks only).
    std::lock_guard<std::mutex> lk(mu_);
    const uint32_t rounded = (std::max(size, kSmallMax + 1) + kAlign - 1) & ~(kAlign - 1);
    const uint32_t raw = bigAlloc(rounded + align);
    if (!raw) return 0;
    const uint32_t aligned = (raw + align - 1) & ~(align - 1);
    const uint32_t total = live_[raw];
    live_.erase(raw);
    if (aligned > raw) bigFree(raw, aligned - raw);
    live_[aligned] = total - (aligned - raw);
    stats_.liveBytes += live_[aligned];
    ++stats_.liveBlocks;
    return aligned;
}

void Heap::free(uint32_t addr) {
    if (!addr) return;
    std::lock_guard<std::mutex> lk(mu_);
    auto it = live_.find(addr);
    if (it == live_.end()) { ++stats_.badFrees; return; }
    const uint32_t size = it->second;
    live_.erase(it);
    stats_.liveBytes -= size;
    --stats_.liveBlocks;
    if (size <= kSmallMax) small_[size / kAlign].push_back(addr);
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
    auto it = live_.find(addr);
    return it == live_.end() ? 0 : it->second;
}

Heap::Stats Heap::stats() const {
    std::lock_guard<std::mutex> lk(mu_);
    return stats_;
}

} // namespace Hydrogen::Runtime
