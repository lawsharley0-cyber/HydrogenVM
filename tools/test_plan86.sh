# YES!!!
# "return true if the span is entirely mapped and writable"
# `bool GuestArena::isWritable(uint32_t addr, uint32_t size) const noexcept;`
# ```cpp
# bool GuestArena::isWritable(uint32_t addr, uint32_t size) const noexcept {
#     if (!size) return true;
#     std::lock_guard<std::mutex> lk(mu_);
#     uint64_t g = addr >> kGuestPageShift;
#     uint64_t end = (uint64_t(addr) + size + kGuestPageSize - 1) >> kGuestPageShift;
#     if (end > kGuestPageCount) return false;
#     for (; g < end; ++g) {
#         if (!(prot_[g] & kProtWrite)) return false;
#     }
#     return true;
# }
# ```
# Then `getWritableBound` doesn't need to return the ABSOLUTE maximum.
# But wait, `getWritableBound` needs to return a NUMBER (the bound) so we can do `min(bound, os)`.
# If `getWritableBound` returns the maximum, we do `min`.
# But instead of finding the MAXIMUM, what if `getWritableBound` takes the `requestedSize` (which is `writeLen + 1`) and just checks if it's safe?
# Actually, the compiler bound `os` must be respected.
# If `writeLen + 1 > os`, we throw immediately.
# If `writeLen + 1 <= os`, then we check if `writeLen + 1` fits in the heap allocation.
# If it's in the heap, we check `allocRem`. If `writeLen + 1 > allocRem`, we throw!
# If it's NOT in the heap, we just check `isWritable(buf, writeLen + 1)`. If not, we throw!
# This perfectly avoids scanning!
