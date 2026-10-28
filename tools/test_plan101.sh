# "FAIL snprintf_chk exact fit with os=5"
# What happened?
# Let's check `snprintf_chk` logic.
# `if (os != 0xFFFFFFFF && n > os) throw std::runtime_error("sprintf_chk buffer overflow");`
# In the test:
# `r = runFormat("___snprintf_chk", {buf, 10, 0, 5, fmtExact});`
# Here `n = 10`, `os = 5`.
# `n > os` is `10 > 5`, which is TRUE!
# So it throws "sprintf_chk buffer overflow"!
# Ah! "The PR's TestRunner.cpp even expects ___snprintf_chk(buf, n=10, os=5, short text) to succeed. Please correct the _chk object-size contract while preserving standard snprintf truncation and would-have-written return value."
#
# Wait, why did the reviewer say:
# "but the check applies min(bound, os) to the *actual short formatted output*; it does not reject n > os as ___snprintf_chk must. The PR's TestRunner.cpp even expects ___snprintf_chk(buf, n=10, os=5, short text) to succeed. Please correct the _chk object-size contract while preserving standard snprintf truncation"
#
# OH! The reviewer explicitly pointed out that the test EXPECTED IT TO SUCCEED, but `___snprintf_chk` MUST REJECT `n > os`!
# So the standard glibc `__snprintf_chk` contract aborts if `maxlen > os`!
# The reviewer is telling me to CHANGE THE TEST to expect a failure!
# And keep my fix which throws when `n > os`!
