#pragma once
#include <atomic>
#include <cstdint>

namespace Hydrogen::CPU {
struct ThreadProgressSnapshot {
    uint64_t instructions = 0;
    uint32_t pc = 0, lr = 0, blockPc = 0, hle = 0xFFFFFFFFu;
    uint32_t argument0 = 0, argument1 = 0;
    bool inGuest = false;
};

// One owning CPU thread publishes at dispatch/HLE boundaries. Readers never
// inspect the live register file and make at most four attempts. Atomic fields
// also make an interrupted/inconsistent read data-race-free.
struct ThreadProgress {
    bool enabled = false; // configured before the owning thread starts
    std::atomic<uint64_t> sequence{0}, instructions{0};
    std::atomic<uint32_t> pc{0}, lr{0}, blockPc{0}, hle{0xFFFFFFFFu}, argument0{0}, argument1{0};
    std::atomic<bool> inGuest{false};

    void publish(const ThreadProgressSnapshot& s) noexcept {
        if (!enabled) return;
        const auto seq = sequence.load(std::memory_order_relaxed);
        sequence.store(seq + 1, std::memory_order_relaxed);
        std::atomic_thread_fence(std::memory_order_release);
        instructions.store(s.instructions, std::memory_order_relaxed);
        pc.store(s.pc, std::memory_order_relaxed);
        lr.store(s.lr, std::memory_order_relaxed);
        blockPc.store(s.blockPc, std::memory_order_relaxed);
        hle.store(s.hle, std::memory_order_relaxed);
        argument0.store(s.argument0, std::memory_order_relaxed);
        argument1.store(s.argument1, std::memory_order_relaxed);
        inGuest.store(s.inGuest, std::memory_order_relaxed);
        sequence.store(seq + 2, std::memory_order_release);
    }

    bool snapshot(ThreadProgressSnapshot& s) const noexcept {
        for (unsigned attempt = 0; attempt < 4; ++attempt) {
            const auto seq = sequence.load(std::memory_order_acquire);
            if (!seq || (seq & 1)) continue;
            s.instructions = instructions.load(std::memory_order_relaxed);
            s.pc = pc.load(std::memory_order_relaxed);
            s.lr = lr.load(std::memory_order_relaxed);
            s.blockPc = blockPc.load(std::memory_order_relaxed);
            s.hle = hle.load(std::memory_order_relaxed);
            s.argument0 = argument0.load(std::memory_order_relaxed);
            s.argument1 = argument1.load(std::memory_order_relaxed);
            s.inGuest = inGuest.load(std::memory_order_relaxed);
            std::atomic_thread_fence(std::memory_order_acquire);
            if (seq == sequence.load(std::memory_order_relaxed)) return true;
        }
        return false;
    }
};
} // namespace Hydrogen::CPU
