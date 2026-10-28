# What if I change `GuestArena::mappedWritableSpan` to take `uint32_t maxScan`?
#
# ```cpp
# uint32_t GuestArena::mappedWritableSpan(uint32_t addr, uint32_t maxScan) const noexcept {
#     if (!maxScan) return 0;
#     std::lock_guard<std::mutex> lk(mu_);
#     uint64_t g = addr >> kGuestPageShift;
#     if (g >= kGuestPageCount || !(prot_[g] & kProtWrite)) return 0;
#     uint64_t end = (uint64_t(addr) & ~uint64_t(kGuestPageSize - 1)) + kGuestPageSize;
#     uint64_t scanLimit = uint64_t(addr) + maxScan;
#     if (scanLimit > kArenaSize) scanLimit = kArenaSize;
#     for (++g; g < kGuestPageCount && end < scanLimit; ++g) {
#         if (!(prot_[g] & kProtWrite)) break;
#         end += kGuestPageSize;
#     }
#     uint64_t span = end - addr;
#     return span > maxScan ? maxScan : uint32_t(span);
# }
# ```
#
# And then:
# ```cpp
# static uint32_t getWritableBound(Process& proc, GuestPtr buf, uint32_t maxScan) {
#     if (!buf || !maxScan) return 0;
#     const uint32_t mappedSpan = proc.mem().mappedWritableSpan(buf, maxScan);
#     if (!mappedSpan) return 0;
#     const uint32_t allocRem = proc.heap().allocationRemainder(buf);
#     return allocRem ? std::min(mappedSpan, allocRem) : mappedSpan;
# }
#
# int32_t storeFormatted(...) {
#     if (n && buf) {
#         const size_t writeLen = std::min<size_t>(s.size(), n - 1);
#         const uint32_t maxScan = uint32_t(writeLen + 1);
#         const uint32_t bound = getWritableBound(x.proc, buf, maxScan);
#         const uint32_t totalBound = std::min(bound, os);
#         if (writeLen + 1 > totalBound) { ... }
#     ...
# }
# ```
#
# For wide strings, `maxScan` is `(writeLen + 1) * 4`.
