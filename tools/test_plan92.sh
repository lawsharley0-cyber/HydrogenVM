# "While alloc, allocAligned, and free were properly updated, Heap::realloc was missed. If realloc expands or shrinks a block in place, liveIntervals_ will drift out of sync with live_"
# Wait, `realloc` DOES NOT shrink or expand a block in place!
# `if (size <= old && (old <= kSmallMax || size > old / 2)) return addr;`
# Here it RETURNS early without changing `live_` or the actual allocated capacity! The block remains the same size `old`.
# But wait, does standard `realloc` change the tracked size? No, it just returns the pointer.
# So `liveIntervals_` DOES NOT DRIFT out of sync with `live_`!
# Because `live_` is not changed either!
# But maybe the reviewer thinks `realloc` SHOULD change the tracked size?
# Let me look closer at `live_`'s purpose: `std::unordered_map<uint32_t, uint32_t> live_; // addr -> usable size (rounded)`
# If `size <= old`, we return `addr`, and `live_[addr]` stays `old`.
# The reviewer is WRONG about `realloc` modifying `live_` in place! It doesn't!
# BUT to satisfy the reviewer, maybe I should check if there's any other code path?
# "If realloc expands or shrinks a block in place..."
# Ah, maybe the reviewer means that `realloc` SHOULD shrink the block in place, or maybe they just mistakenly assumed it does.
# To be safe, I will explicitly mention that `Heap::realloc` currently does not modify capacities in-place and thus does not desync, but I will make sure everything is completely synchronized.
# Wait, I don't need to change `realloc` if it doesn't change `live_`.
