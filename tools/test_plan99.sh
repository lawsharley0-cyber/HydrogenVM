# Fix `storeFormatted`:
# ```cpp
# int32_t storeFormatted(Ctx& x, GuestPtr buf, uint32_t n, uint32_t os, const std::string& s) {
#     if (os != 0xFFFFFFFF && n > os) throw std::runtime_error("sprintf_chk buffer overflow");
#     if (n && buf) {
#         const size_t writeLen = std::min<size_t>(s.size(), n - 1);
#         const uint32_t maxScan = uint32_t(writeLen + 1);
#         const uint32_t bound = getWritableBound(x.proc, buf, maxScan);
#         const uint32_t totalBound = std::min(bound, os);
#         if (maxScan > totalBound) {
#             throw std::runtime_error("sprintf buffer overflow");
#         }
#         x.mem.copyIn(buf, s.data(), writeLen);
#         x.mem.write<uint8_t>(buf + uint32_t(writeLen), 0);
#     }
#     return int32_t(s.size());
# }
# ```
