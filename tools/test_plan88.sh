# Let's adjust `GuestArena::mappedWritableSpan` to take `uint32_t maxScan`.
# ```cpp
# uint32_t GuestArena::mappedWritableSpan(uint32_t addr, uint32_t maxScan) const noexcept {
#     if (!maxScan) return 0;
#     std::lock_guard<std::mutex> lk(mu_);
#     uint64_t g = addr >> kGuestPageShift;
#     if (g >= kGuestPageCount || !(prot_[g] & kProtWrite)) return 0;
#
#     uint64_t end = (uint64_t(addr) & ~uint64_t(kGuestPageSize - 1)) + kGuestPageSize;
#     uint64_t scanLimit = uint64_t(addr) + maxScan;
#     // Cap scanLimit to 4GB boundary to prevent overflow logic issues
#     if (scanLimit > kArenaSize) scanLimit = kArenaSize;
#
#     for (++g; g < kGuestPageCount && end < scanLimit; ++g) {
#         if (!(prot_[g] & kProtWrite)) break;
#         end += kGuestPageSize;
#     }
#     uint64_t span = end - addr;
#     return span > maxScan ? maxScan : uint32_t(span);
# }
# ```
