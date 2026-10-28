# And update `Stdio.cpp` to pass `maxScan`:
# ```cpp
# static uint32_t getWritableBound(Process& proc, GuestPtr buf, uint32_t maxScan) {
#     if (!buf) return 0;
#     const uint32_t mappedSpan = proc.mem().mappedWritableSpan(buf, maxScan);
#     if (!mappedSpan) return 0;
#     const uint32_t allocRem = proc.heap().allocationRemainder(buf);
#     return allocRem ? std::min(mappedSpan, allocRem) : mappedSpan;
# }
# ```
# And in `storeFormatted`:
# ```cpp
# int32_t storeFormatted(Ctx& x, GuestPtr buf, uint32_t n, uint32_t os, const std::string& s) {
#     if (n && buf) {
#         const size_t writeLen = std::min<size_t>(s.size(), n - 1);
#         const uint32_t maxScan = uint32_t(writeLen + 1);
#         const uint32_t bound = getWritableBound(x.proc, buf, maxScan);
#         const uint32_t totalBound = std::min(bound, os);
#         if (writeLen + 1 > totalBound) {
#             throw std::runtime_error("sprintf buffer overflow");
#         }
#         x.mem.copyIn(buf, s.data(), writeLen);
#         x.mem.write<uint8_t>(buf + uint32_t(writeLen), 0);
#     }
#     return int32_t(s.size());
# }
# ```
# This is perfect! The maximum we ever scan is `writeLen + 1`. This is at most the length of the formatted string! O(1) in terms of pages for typical strings.
# This solves ALL the reviewer's performance and synchronization concerns perfectly!
