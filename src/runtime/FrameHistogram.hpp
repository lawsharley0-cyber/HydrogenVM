// Fixed-memory log histogram of frame durations in nanoseconds.
//
// 40 magnitude classes (exponents 0..39) and 64 linear sub-buckets in each
// class: 2560 counters, about 10 KB, independent of how long the run is.
// Values below 64 ns occupy one bucket each. Values at or above 2^40 ns
// (about 18 minutes) clamp into the last bucket. A percentile is the
// inclusive lower edge of the bucket that contains that rank, so it is
// never above the samples in that bucket and the relative width inside a
// class is 1/64.
#pragma once
#include <array>
#include <atomic>
#include <bit>
#include <cstdint>

namespace Hydrogen::Runtime {

class FrameHistogram {
public:
    static constexpr int kSubBits = 6;
    static constexpr int kMaxExp = 40;
    static constexpr int kSubCount = 1 << kSubBits;
    static constexpr int kBucketCount = kMaxExp << kSubBits;
    static constexpr int kTopIndex = kBucketCount - 1;
    // First duration that is outside the tracked exponents and clamps.
    static constexpr uint64_t kClampNs = 1ull << kMaxExp;

    void add(uint64_t ns) noexcept {
        counts_[size_t(bucketIndex(ns))].fetch_add(1, std::memory_order_relaxed);
    }

    uint64_t count() const noexcept {
        uint64_t n = 0;
        for (const auto& c : counts_) n += c.load(std::memory_order_relaxed);
        return n;
    }

    // `hundredths` is 50, 95, or 99. Empty histograms report 0.
    // Rank is ceil(hundredths/100 * n), computed in integers.
    uint64_t percentile(int hundredths) const noexcept {
        const uint64_t n = count();
        if (n == 0) return 0;
        if (hundredths < 1) hundredths = 1;
        if (hundredths > 100) hundredths = 100;
        uint64_t rank = (uint64_t(hundredths) * n + 99) / 100;
        if (rank < 1) rank = 1;
        if (rank > n) rank = n;
        uint64_t seen = 0;
        for (int i = 0; i < kBucketCount; ++i) {
            seen += counts_[size_t(i)].load(std::memory_order_relaxed);
            if (seen >= rank) return bucketLower(i);
        }
        return bucketLower(kTopIndex);
    }

    static int bucketIndex(uint64_t ns) noexcept {
        if (ns < uint64_t(kSubCount)) return int(ns);
        const int exp = 63 - std::countl_zero(ns);
        if (exp >= kMaxExp) return kTopIndex;
        const int frac = int((ns >> (exp - kSubBits)) & (kSubCount - 1));
        return (exp << kSubBits) + frac;
    }

    static uint64_t bucketLower(int index) noexcept {
        if (index < 0 || index >= kBucketCount) return 0;
        if (index < kSubCount) return uint64_t(index);
        const int exp = index >> kSubBits;
        const int frac = index & (kSubCount - 1);
        if (exp < kSubBits) return 0; // unused index between the exact prefix and exponent kSubBits
        return (uint64_t(kSubCount | frac)) << (exp - kSubBits);
    }

    // Exclusive end of a non-top bucket. The top bucket has no end (clamped samples).
    static uint64_t bucketUpper(int index) noexcept {
        if (index < 0 || index >= kTopIndex) return 0;
        const uint64_t lower = bucketLower(index);
        for (int i = index + 1; i < kBucketCount; ++i) {
            const uint64_t next = bucketLower(i);
            if (next > lower) return next;
        }
        return 0;
    }

private:
    std::array<std::atomic<uint32_t>, kBucketCount> counts_{};
};

} // namespace Hydrogen::Runtime
