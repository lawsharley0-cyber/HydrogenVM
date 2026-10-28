# Also, they mentioned: "The new test's supposed stackRegion + 1024 unmapped boundary is only a suballocation inside a mapped page, not proof of an inaccessible mapping."
# Ah! `p.heap().allocZeroed(1024)` allocates 1024 bytes. But the heap page is 64KB!
# So `unmappedEdge` is NOT unmapped! It's just the end of the allocation!
# So my stack edge test didn't test an unmapped boundary, it tested an allocation boundary.
# To test an unmapped boundary, we need to map exactly one page.
# `uint32_t mapStack = 0x20000000; p.mem().map(mapStack, 4096, Memory::kProtRW);`
# Then `mapStack + 4096` is truly unmapped!
# I will rewrite the test for the stack boundary to use a dedicated manual mapping.
