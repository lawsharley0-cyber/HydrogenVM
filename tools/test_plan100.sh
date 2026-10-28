# Let's fix `storeWideFormatted` too.
# ```cpp
# int32_t storeWideFormatted(Ctx& x, GuestPtr buf, uint32_t n, uint32_t os, const std::string& s) {
#     if (os != 0xFFFFFFFF && n > os) throw std::runtime_error("swprintf_chk buffer overflow");
#     ...
# ```
