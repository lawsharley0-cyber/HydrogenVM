# To fix the lock issue:
# Add `std::lock_guard<std::mutex> lk(mu_);` to `mappedWritableSpan`.
# BUT wait! `mu_` is a `mutable std::mutex`. If `mappedWritableSpan` is called by `getWritableBound`, which is called by `storeFormatted`, it locks `mu_` on EVERY `sprintf`/`snprintf`.
# Is that okay? `GuestArena` only locks `mu_` during `map` and `unmap`. It doesn't lock during `read` or `write`.
# If `sprintf` runs concurrently, it will lock `mu_` and cause contention. But how often does the guest run `sprintf`? Quite often.
# Is there a way to do a "conservative bounded check"?
# "implement an efficient, synchronization-safe writable-range query (or documented conservative bounded check)"
# If we cap the check to e.g. 1 MB, we only scan 256 pages!
# Or we can just bound it by some max length for `sprintf`.
# Wait, `getWritableBound` is used for `sprintf` (which has `os = 0xFFFFFFFF`).
# What if we only scan up to `os`?
# For `snprintf_chk`, `os` is the compiler object size (e.g. 100 bytes).
# For `snprintf`, `n` is provided (e.g. 100 bytes).
# But for `sprintf`, `n` and `os` are both `0xFFFFFFFF`.
# So for `sprintf`, how much should we scan?
# If we scan up to `0xFFFFFFFF`, it's 1M pages.
# BUT wait. `snprintf` often passes small `n`.
# We should change `mappedWritableSpan(uint32_t addr, uint32_t maxScan = 0xFFFFFFFF)`!
# And `getWritableBound(proc, buf, os)`. Wait, we pass the minimum of `n` and `os` as `maxScan`!
# For `sprintf`, `maxScan` will still be `0xFFFFFFFF`.
# If `maxScan` is 0xFFFFFFFF, we can limit it to say 16 MB. No sensible `sprintf` generates > 16 MB strings. Or we just scan fast!
