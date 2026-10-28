# Let's create the final patch!
# 1. Update `GuestArena::mappedWritableSpan` to take `maxScan`, lock `mu_`, and cap scanning to `maxScan`.
# 2. Update `Stdio.cpp` to pass `writeLen + 1` to `mappedWritableSpan` (or `getWritableBound`).
# 3. Update `TestRunner.cpp` to use a manual page mapping for the unmapped boundary test.
