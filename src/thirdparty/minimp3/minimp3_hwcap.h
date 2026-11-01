#ifndef MINIMP3_HWCAP_H
#define MINIMP3_HWCAP_H
/*
    Host-testable NEON decision for the 32-bit ARM Linux path in minimp3.h.
    The decoder calls minimp3_linux_neon_probe(). Unit tests call
    minimp3_hwcap_has_neon() and minimp3_auxv_has_neon() with synthetic words
    so the decision does not depend on this machine's CPU.
*/
#include <stddef.h>
#include <string.h>

#if defined(__has_include)
#  if __has_include(<sys/auxv.h>)
#    include <sys/auxv.h>
#    define MINIMP3_HAVE_GETAUXVAL 1
#  endif
#endif
#if !defined(MINIMP3_HAVE_GETAUXVAL)
#include <stdio.h>
#endif

/* Linux ELF AT_HWCAP is 16 on every architecture (elf.h / linux/auxvec.h). */
#if defined(AT_HWCAP) && (AT_HWCAP + 0 == 16)
#  define MINIMP3_AT_HWCAP AT_HWCAP
#else
#  define MINIMP3_AT_HWCAP 16
#endif

/*
    32-bit ARM Linux puts NEON in bit 12 of AT_HWCAP (value 4096).
    glibc names that bit HWCAP_ARM_NEON. The kernel uapi names it HWCAP_NEON.
    aarch64 HWCAP_ASIMD is a different bit and is not consulted here.
    If the target header does not provide a matching macro, use the named
    constant. A missing auxv entry is scalar (0), never an assumed NEON CPU.
*/
#if defined(HWCAP_NEON) && (HWCAP_NEON + 0 == (1 << 12))
#  define MINIMP3_HWCAP_NEON HWCAP_NEON
#elif defined(HWCAP_ARM_NEON) && (HWCAP_ARM_NEON + 0 == (1 << 12))
#  define MINIMP3_HWCAP_NEON HWCAP_ARM_NEON
#else
#  define MINIMP3_HWCAP_NEON (1u << 12)
#endif

static inline int minimp3_hwcap_has_neon(unsigned long hwcap)
{
    return (hwcap & (unsigned long)MINIMP3_HWCAP_NEON) != 0;
}

static inline int minimp3_auxv_has_neon(const unsigned long *pairs, size_t nentries)
{
    size_t i;
    if (!pairs)
        return 0;
    for (i = 0; i < nentries; i++) {
        unsigned long tag = pairs[i * 2];
        if (tag == 0) /* AT_NULL ends the vector */
            return 0;
        if (tag == (unsigned long)MINIMP3_AT_HWCAP)
            return minimp3_hwcap_has_neon(pairs[i * 2 + 1]);
    }
    return 0;
}

static inline int minimp3_auxv_bytes_has_neon(const void *bytes, size_t nbytes)
{
    unsigned long pairs[64];
    size_t n = 0;
    const unsigned char *p = (const unsigned char *)bytes;
    if (!bytes)
        return 0;
    while (n < 32 && nbytes >= sizeof(unsigned long) * 2) {
        unsigned long tag, val;
        memcpy(&tag, p, sizeof tag);
        memcpy(&val, p + sizeof tag, sizeof val);
        pairs[n * 2] = tag;
        pairs[n * 2 + 1] = val;
        n++;
        p += sizeof(unsigned long) * 2;
        nbytes -= sizeof(unsigned long) * 2;
        if (tag == 0)
            break;
    }
    return minimp3_auxv_has_neon(pairs, n);
}

/* 1 = NEON, 0 = scalar. getauxval returns 0 when AT_HWCAP is absent. */
static inline int minimp3_linux_neon_probe(void)
{
#if defined(MINIMP3_HAVE_GETAUXVAL)
    return minimp3_hwcap_has_neon(getauxval((unsigned long)MINIMP3_AT_HWCAP));
#else
    unsigned long pairs[64];
    size_t n = 0;
    FILE *f = fopen("/proc/self/auxv", "rb");
    if (!f)
        return 0;
    while (n < 32) {
        unsigned long auxv[2];
        if (fread(auxv, sizeof auxv[0], 2, f) != 2)
            break;
        pairs[n * 2] = auxv[0];
        pairs[n * 2 + 1] = auxv[1];
        n++;
        if (auxv[0] == 0)
            break;
    }
    fclose(f);
    return minimp3_auxv_has_neon(pairs, n);
#endif
}

#endif /* MINIMP3_HWCAP_H */
