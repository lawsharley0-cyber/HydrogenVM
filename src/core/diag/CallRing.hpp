// Fixed-capacity ring of the most recent calls one guest thread made into the
// host (HLE functions today; ObjC and GL entries are added by T2). It exists so a
// crash or a stuck quit can say what the thread was doing just before, instead
// of only where it stopped.
//
// One writer per ring (the guest thread that owns it); any number of readers on
// other threads (a crash bundle, a watchdog). A reader never blocks the writer
// and never returns a torn record: an entry that is overwritten while it is being
// read is dropped from that snapshot.
#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace Hydrogen::Diag {

// What kind of host entry a record names. `id` is interpreted per kind
// (HLE function index, and so on).
enum class CallKind : uint32_t { None = 0, Hle = 1, ObjC = 2, Gl = 3 };

struct CallRecord {
    uint64_t seq;     // 1-based position in this ring's history; strictly increasing
    CallKind kind;
    uint32_t id;
};

template <uint32_t Capacity>
class CallRing {
    static_assert(Capacity >= 2 && (Capacity & (Capacity - 1)) == 0, "capacity must be a power of two");

public:
    static constexpr uint32_t kCapacity = Capacity;

    // Writer only.
    void push(CallKind kind, uint32_t id) noexcept {
        const uint64_t n = head_.load(std::memory_order_relaxed) + 1;
        Slot& s = slots_[n & (Capacity - 1)];
        s.seq.store(0, std::memory_order_relaxed);               // mark the slot as being rewritten
        std::atomic_thread_fence(std::memory_order_release);
        s.data.store((uint64_t(id) << 32) | uint32_t(kind), std::memory_order_relaxed);
        s.seq.store(n, std::memory_order_release);               // publish
        head_.store(n, std::memory_order_release);
    }

    // Total calls ever pushed (not capped by Capacity).
    uint64_t total() const noexcept { return head_.load(std::memory_order_acquire); }

    // The last min(n, Capacity, total()) records, oldest first. Safe from any
    // thread. A record the writer overwrites mid-read is omitted, so under a
    // racing writer the result can be shorter than requested, never wrong.
    std::vector<CallRecord> last(size_t n) const {
        std::vector<CallRecord> out;
        const uint64_t head = head_.load(std::memory_order_acquire);
        if (n > Capacity) n = Capacity;
        if (n > head) n = size_t(head);
        out.reserve(n);
        for (uint64_t want = head - n + 1; want <= head; ++want) {
            const Slot& s = slots_[want & (Capacity - 1)];
            if (s.seq.load(std::memory_order_acquire) != want) continue;
            const uint64_t d = s.data.load(std::memory_order_relaxed);
            std::atomic_thread_fence(std::memory_order_acquire);
            if (s.seq.load(std::memory_order_relaxed) != want) continue;
            out.push_back({want, CallKind(uint32_t(d)), uint32_t(d >> 32)});
        }
        return out;
    }

private:
    struct Slot {
        std::atomic<uint64_t> seq{0};
        std::atomic<uint64_t> data{0};
    };
    Slot slots_[Capacity];
    std::atomic<uint64_t> head_{0};
};

// The ring each guest thread carries. 256 entries is 4 KB per thread.
using ThreadCallRing = CallRing<256>;

}  // namespace Hydrogen::Diag
