// Host-side checks for the 32-bit ARM Linux NEON decision used by minimp3.
// Synthetic hwcap and auxv words only: this file does not read the host CPU.
#include "thirdparty/minimp3/minimp3_hwcap.h"

#include <cstdio>

static_assert(MINIMP3_AT_HWCAP == 16, "Linux AT_HWCAP is 16");
static_assert(MINIMP3_HWCAP_NEON == (1u << 12), "32-bit ARM NEON is hwcap bit 12");

static int g_hwcap_fail = 0;

static void hwcap_expect(int ok, const char *what) {
    if (ok) {
        std::printf("  ok   %s\n", what);
    } else {
        std::printf("  FAIL %s\n", what);
        ++g_hwcap_fail;
    }
}

int runMinimp3HwcapChecks() {
    g_hwcap_fail = 0;
    const unsigned long neon = (unsigned long)MINIMP3_HWCAP_NEON;
    const unsigned long vfp = 1ul << 6; /* HWCAP_VFP, not NEON */

    hwcap_expect(minimp3_hwcap_has_neon(0) == 0,
                 "minimp3 treats a zero ARM hwcap as scalar");
    hwcap_expect(minimp3_hwcap_has_neon(neon) == 1,
                 "minimp3 enables NEON when only the NEON hwcap bit is set");
    hwcap_expect(minimp3_hwcap_has_neon(neon | vfp) == 1,
                 "minimp3 enables NEON when the NEON bit is set beside other hwcap bits");
    hwcap_expect(minimp3_hwcap_has_neon(vfp) == 0,
                 "minimp3 stays scalar when AT_HWCAP has VFP but not NEON");
    hwcap_expect(minimp3_hwcap_has_neon(neon - 1) == 0,
                 "minimp3 stays scalar when every bit below NEON is set");
    hwcap_expect(minimp3_hwcap_has_neon(1ul << 11) == 0,
                 "minimp3 does not treat ThumbEE (bit 11) as NEON");
    hwcap_expect(minimp3_hwcap_has_neon(1ul << 13) == 0,
                 "minimp3 does not treat VFPv3 (bit 13) as NEON");
    hwcap_expect(minimp3_hwcap_has_neon(~0ul) == 1,
                 "minimp3 enables NEON when every hwcap bit is set");

    const unsigned long just_neon[] = {(unsigned long)MINIMP3_AT_HWCAP, neon};
    hwcap_expect(minimp3_auxv_has_neon(just_neon, 1) == 1,
                 "minimp3 reads NEON from an auxv AT_HWCAP entry");
    const unsigned long just_vfp[] = {(unsigned long)MINIMP3_AT_HWCAP, vfp};
    hwcap_expect(minimp3_auxv_has_neon(just_vfp, 1) == 0,
                 "minimp3 stays scalar when auxv AT_HWCAP lacks NEON");
    const unsigned long pagesz[] = {6 /* AT_PAGESZ */, 4096};
    hwcap_expect(minimp3_auxv_has_neon(pagesz, 1) == 0,
                 "minimp3 ignores an auxv entry that is not AT_HWCAP");
    const unsigned long hwcap2[] = {26 /* AT_HWCAP2 */, neon};
    hwcap_expect(minimp3_auxv_has_neon(hwcap2, 1) == 0,
                 "minimp3 does not treat AT_HWCAP2 as the NEON bit");
    const unsigned long later[] = {6, 4096, (unsigned long)MINIMP3_AT_HWCAP, neon, 0, 0};
    hwcap_expect(minimp3_auxv_has_neon(later, 3) == 1,
                 "minimp3 finds AT_HWCAP after unrelated auxv entries");
    const unsigned long stopped[] = {0, 0, (unsigned long)MINIMP3_AT_HWCAP, neon};
    hwcap_expect(minimp3_auxv_has_neon(stopped, 2) == 0,
                 "minimp3 stops at AT_NULL and does not read a following AT_HWCAP");
    hwcap_expect(minimp3_auxv_has_neon(nullptr, 1) == 0,
                 "minimp3 stays scalar when the auxv vector is missing");
    hwcap_expect(minimp3_auxv_has_neon(just_neon, 0) == 0,
                 "minimp3 stays scalar when the auxv vector is empty");

    hwcap_expect(minimp3_auxv_bytes_has_neon(just_neon, sizeof just_neon) == 1,
                 "minimp3 parses a native-width auxv byte image that contains NEON");
    hwcap_expect(minimp3_auxv_bytes_has_neon(just_vfp, sizeof just_vfp) == 0,
                 "minimp3 parses a native-width auxv byte image without NEON as scalar");
    const unsigned long one_word = (unsigned long)MINIMP3_AT_HWCAP;
    hwcap_expect(minimp3_auxv_bytes_has_neon(&one_word, sizeof one_word) == 0,
                 "minimp3 stays scalar when the auxv image is truncated");
    hwcap_expect(minimp3_auxv_bytes_has_neon(nullptr, 16) == 0,
                 "minimp3 stays scalar when the auxv image pointer is null");

    return g_hwcap_fail;
}

#ifndef MINIMP3_HWCAP_NO_MAIN
int main() {
    const int fail = runMinimp3HwcapChecks();
    std::printf("minimp3 hwcap: %d failed\n", fail);
    return fail ? 1 : 0;
}
#endif
