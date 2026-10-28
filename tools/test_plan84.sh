# What if we use `std::atomic<uint8_t>` for `prot_`?
# "reads protection state without the map lock (potential concurrent map/unmap data race)"
# If we read `prot_[g]` without the lock, it could race with `unmap` setting it to `kProtNone`.
# The reviewer asked for an "efficient, synchronization-safe writable-range query (or documented conservative bounded check)"
# Let's add the lock. Is it a major regression to lock `GuestArena::mu_` on every `sprintf`?
# `GuestArena::map` and `unmap` are rare (mmap / thread creation).
# So `GuestArena::mu_` is almost always uncontended!
# Locking an uncontended `std::mutex` takes ~10-20ns.
# A 1M loop takes maybe 1 million iterations * 1ns = 1ms.
# 1ms for every `sprintf` IS a major regression.
# So we need to bound the scan!
