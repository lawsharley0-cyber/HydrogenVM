# "conservative bounded check"
# If we change `mappedWritableSpan(uint32_t addr, uint32_t maxLimit)`:
# We only need to guarantee that there's ENOUGH writable space for what we ACTUALLY write!
# But wait, `sprintf` generates a string `s`.
# In `storeFormatted`, we already know `s.size()`!
# We just need to check if `addr` through `addr + writeLen` is writable!
# Wait!
# In `storeFormatted(Ctx& x, GuestPtr buf, uint32_t n, uint32_t os, const std::string& s)`
# We KNOW `s.size()`.
# We KNOW `writeLen = std::min(s.size(), n - 1)`.
# We only need to check if `writeLen + 1` bytes starting at `buf` are mapped and writable!
# That means we only scan the pages covering `buf` to `buf + writeLen`!
# This is at most `writeLen / 4096 + 2` pages!
# That takes O(1) time!
