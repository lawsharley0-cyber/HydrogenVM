# What about the map lock issue?
# "reads protection state without the map lock (potential concurrent map/unmap data race)"
# I must add `std::lock_guard<std::mutex> lk(mu_);` to `GuestArena::mappedWritableSpan`.
