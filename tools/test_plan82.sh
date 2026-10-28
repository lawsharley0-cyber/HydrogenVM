# Let's fix the performance issue in `GuestArena::mappedWritableSpan`.
# The reviewer said:
# 1. "linearly scans prot_ over potentially the remaining entire 32-bit guest address space on every formatted write; even a small sprintf can incur O(1M) page checks"
# 2. "reads protection state without the map lock (potential concurrent map/unmap data race)"
# 3. "uint32_t(end - addr) can wrap for top-of-space spans."
# 4. "The new test's supposed stackRegion + 1024 unmapped boundary is only a suballocation inside a mapped page, not proof of an inaccessible mapping."
#
# How to implement an efficient, synchronization-safe writable-range query (or documented conservative bounded check)?
