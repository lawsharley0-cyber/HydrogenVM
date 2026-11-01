minimp3 (https://github.com/lieff/minimp3), commit ea99364f61c14656440e8d77e9c233ccf3124633.
CC0 1.0 Universal (public domain). Only minimp3.h is used.
minimp3.h has one local change, which is not in that upstream commit: on 32-bit
ARM Linux, have_simd() asks getauxval(AT_HWCAP) (or /proc/self/auxv when
<sys/auxv.h> is absent) and enables NEON only when the HWCAP_NEON bit is set.
Upstream still returns 1. minimp3_hwcap.h is the host-testable decision.
