# The reviewer said:
# "Add/reuse a GuestArena helper that returns the contiguous WRITABLE mapped span from an arbitrary guest address... this is the universal backstop for heap, stack, and globals."
# "For a pointer inside a tracked Heap allocation, also calculate allocation_end - ptr, then cap by the smaller of the allocation remainder and mapped span."
# "on insufficient/unknown writable span produce a controlled guest failure"
# "even a small sprintf can incur O(1M) page checks, a major emulator performance regression. It also reads protection state without the map lock (potential concurrent map/unmap data race), and uint32_t(end - addr) can wrap for top-of-space spans."
# "Please implement an efficient, synchronization-safe writable-range query (or documented conservative bounded check)"
#
# If I implement a "documented conservative bounded check":
# ```cpp
# uint32_t GuestArena::mappedWritableSpan(uint32_t addr, uint32_t maxScan) const noexcept {
#     if (!maxScan) return 0;
#     std::lock_guard<std::mutex> lk(mu_);
#     uint64_t g = addr >> kGuestPageShift;
#     if (g >= kGuestPageCount || !(prot_[g] & kProtWrite)) return 0;
#     uint64_t end = (uint64_t(addr) & ~uint64_t(kGuestPageSize - 1)) + kGuestPageSize;
#     uint64_t scanLimit = uint64_t(addr) + maxScan;
#     for (++g; g < kGuestPageCount && end < scanLimit; ++g) {
#         if (!(prot_[g] & kProtWrite)) break;
#         end += kGuestPageSize;
#     }
#     uint64_t span = end - addr;
#     return span > maxScan ? maxScan : uint32_t(span);
# }
# ```
# Then in `storeFormatted`, `s.size() + 1` is the MAXIMUM bytes we will ever write. We don't need to scan past `s.size() + 1`.
# So `maxScan` can simply be `s.size() + 1` for `sprintf`, and `min(s.size() + 1, n)` for `snprintf`!
# Then `mappedWritableSpan` is extremely efficient! It only scans up to the string length (e.g. 1-2 pages)!
# This completely solves the O(1M) page checks, it uses the lock properly, and it avoids the wrap issue!
