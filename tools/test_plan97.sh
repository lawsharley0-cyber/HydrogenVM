# Reviewer feedback:
# In `f_snprintf_chk`, `___snprintf_chk` contract requires that if `n > os`, it must fail immediately!
# Yes, `___snprintf_chk(char *str, size_t maxlen, int flag, size_t os, const char *format, ...)`
# "If maxlen > os, the program is aborted."
# My current code does:
# `const size_t writeLen = std::min<size_t>(s.size(), n - 1);`
# `const uint32_t maxScan = uint32_t(writeLen + 1);`
# `if (maxScan > totalBound) throw ...` where `totalBound = min(bound, os)`.
# If `n = 10`, `os = 5`, `s.size() = 4`.
# `writeLen = min(4, 9) = 4`.
# `maxScan = 5`.
# `totalBound = min(bound, 5) = 5`.
# `maxScan > totalBound` => `5 > 5` is FALSE. So it succeeds!
# But the standard says if `n > os`, it must abort, regardless of `s.size()`!
# So we need to throw if `os != 0xFFFFFFFF && n > os`.
