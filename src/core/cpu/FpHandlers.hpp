// HydrogenVM — VFPv3/VFPv4 scalar floating-point handlers.
//
// Register file: Cpu::d[32] holds D0-D31; S(2n) / S(2n+1) are the low / high
// halves of D(n). Indices in Ops are S-numbers for single precision and
// D-numbers for double precision.
//
// Accuracy notes:
//  * Contraction is disabled: ARM VMLA/VMLS/VNMLA/VNMLS round the product and
//    the sum separately, while an arm64 host compiler would happily fuse
//    `a * b + c` into one FMADD and change the result.
//  * FPSCR.FZ (flush-to-zero) and FPSCR.DN (default NaN) are honoured on a
//    slow path that's only taken when the guest enables them.
//  * Arithmetic always rounds to nearest; FPSCR.RMode is honoured only by
//    VCVTR (float→int with FPSCR rounding). Exception flags aren't tracked.
#pragma once
#include "Handlers.hpp"
#include <cmath>
#include <limits>
#include <type_traits>

#include <cfenv>

#pragma clang fp contract(off)

namespace Hydrogen::CPU {

enum : uint32_t {
    kFpscrN = 1u << 31, kFpscrQC = 1u << 27, kFpscrDN = 1u << 25, kFpscrFZ = 1u << 24,
    kFpscrRModeShift = 22, kFpscrRMode = 3u << 22,
};

inline uint8_t* vregs(Cpu* c) { return reinterpret_cast<uint8_t*>(c->d); }
inline const uint8_t* vregs(const Cpu* c) { return reinterpret_cast<const uint8_t*>(c->d); }

inline float    rdS(const Cpu* c, uint32_t i) { float f; std::memcpy(&f, vregs(c) + 4 * i, 4); return f; }
inline void     wrS(Cpu* c, uint32_t i, float f) { std::memcpy(vregs(c) + 4 * i, &f, 4); }
inline double   rdD(const Cpu* c, uint32_t i) { double f; std::memcpy(&f, vregs(c) + 8 * i, 8); return f; }
inline void     wrD(Cpu* c, uint32_t i, double f) { std::memcpy(vregs(c) + 8 * i, &f, 8); }
inline uint32_t rdSu(const Cpu* c, uint32_t i) { uint32_t v; std::memcpy(&v, vregs(c) + 4 * i, 4); return v; }
inline void     wrSu(Cpu* c, uint32_t i, uint32_t v) { std::memcpy(vregs(c) + 4 * i, &v, 4); }

template <bool DP> using FpT = std::conditional_t<DP, double, float>;
template <bool DP> inline FpT<DP> rdF(const Cpu* c, uint32_t i) { if constexpr (DP) return rdD(c, i); else return rdS(c, i); }
template <bool DP> inline void wrF(Cpu* c, uint32_t i, FpT<DP> v) { if constexpr (DP) wrD(c, i, v); else wrS(c, i, v); }

// Flush a denormal to a same-signed zero. A zero exponent field means zero or denormal; both keep
// only their sign. Branchless: RunFast (flush-to-zero) is how every iOS thread starts, so this runs
// on nearly every VFP/NEON operation.
template <typename T> inline T flushDenorm(T v) {
    if constexpr (sizeof(T) == 4) {
        uint32_t b; std::memcpy(&b, &v, 4);
        const uint32_t keep = (b & 0x7F800000u) ? ~0u : 0x80000000u;
        b &= keep; std::memcpy(&v, &b, 4);
    } else {
        uint64_t b; std::memcpy(&b, &v, 8);
        const uint64_t keep = (b & 0x7FF0000000000000ull) ? ~0ull : 0x8000000000000000ull;
        b &= keep; std::memcpy(&v, &b, 8);
    }
    return v;
}
template <typename T> inline T defaultNaN() {
    if constexpr (sizeof(T) == 4) { uint32_t b = 0x7FC00000u; T f; std::memcpy(&f, &b, 4); return f; }
    else { uint64_t b = 0x7FF8000000000000ull; T f; std::memcpy(&f, &b, 8); return f; }
}

// VFP arithmetic in a rounding mode other than round-to-nearest (FPSCR.RMode: 0 RN, 1 RP, 2 RM,
// 3 RZ). Games almost never change it, so the host's rounding mode is switched only for those
// operations. The operands are read and the result written through volatiles so the compiler
// can't move the arithmetic (it has no side effects) outside the switched window.
inline int hostRounding(uint32_t rmode) {
    static const int kModes[4] = {FE_TONEAREST, FE_UPWARD, FE_DOWNWARD, FE_TOWARDZERO};
    return kModes[rmode & 3];
}
template <typename F>
[[gnu::noinline]] auto withRounding(uint32_t rmode, F f) {
#pragma STDC FENV_ACCESS ON
    const int old = std::fegetround();
    std::fesetround(hostRounding(rmode));
    const auto r = f();
    std::fesetround(old);
    return r;
}

enum VfpOp3 : uint8_t { kVAdd, kVSub, kVMul, kVNMul, kVDiv, kVMla, kVMls, kVNMla, kVNMls, kVFma, kVFms, kVFNma, kVFNms };

template <int OP, typename T>
inline T vfp3(T n, T m, T d) {
    if constexpr (OP == kVAdd) return n + m;
    else if constexpr (OP == kVSub) return n - m;
    else if constexpr (OP == kVMul) return n * m;
    else if constexpr (OP == kVNMul) return -(n * m);
    else if constexpr (OP == kVDiv) return n / m;
    else if constexpr (OP == kVMla) { T p = n * m; return d + p; }
    else if constexpr (OP == kVMls) { T p = n * m; return d - p; }
    else if constexpr (OP == kVNMla) { T p = n * m; return -d - p; }
    else if constexpr (OP == kVNMls) { T p = n * m; return -d + p; }
    else if constexpr (OP == kVFma) return std::fma(n, m, d);
    else if constexpr (OP == kVFms) return std::fma(-n, m, d);
    else if constexpr (OP == kVFNma) return std::fma(-n, m, -d);
    else return std::fma(n, m, -d);   // VFNMS
}

// True when a NaN result came from a NaN operand (which ARM propagates) rather than from an invalid
// operation on numbers. The multiply-accumulate forms also read d.
template <int OP, typename T>
inline bool invalidFromNaN(T n, T m, T d) {
    return n != n || m != m || (OP >= kVMla && d != d);
}

// Anything but plain IEEE or exactly RunFast: another rounding mode, or FZ without DN and the
// like. A separate handler reached by tail call, so the common handler stays a leaf (a call would
// make every path set up a stack frame).
template <int OP, bool DP>
[[gnu::noinline]] HVM_H(H_Vfp3General) {
    using T = FpT<DP>;
    T n = rdF<DP>(c, op->b), mm = rdF<DP>(c, op->c), d = rdF<DP>(c, op->a);
    const uint32_t fpscr = c->fpscr;
    if (fpscr & kFpscrFZ) { n = flushDenorm(n); mm = flushDenorm(mm); d = flushDenorm(d); }
    const uint32_t rmode = (fpscr >> kFpscrRModeShift) & 3;
    T r = rmode ? withRounding(rmode, [=] { volatile T vn = n, vm = mm, vd = d; volatile T vr = vfp3<OP>(T(vn), T(vm), T(vd)); return T(vr); })
                : vfp3<OP>(n, mm, d);
    if (fpscr & kFpscrFZ) r = flushDenorm(r);
    if (r != r && ((fpscr & kFpscrDN) || !invalidFromNaN<OP>(n, mm, d))) r = defaultNaN<T>();
    wrF<DP>(c, op->a, r);
    HVM_NEXT();
}

// a = d, b = n, c = m. Two inline paths: IEEE (Android's default) and RunFast (FZ + DN, round to
// nearest: how every iOS thread starts).
template <int OP, bool DP>
HVM_H(H_Vfp3) {
    using T = FpT<DP>;
    const uint32_t mode = c->fpscr & (kFpscrFZ | kFpscrDN | kFpscrRMode);
    if (__builtin_expect(mode == 0, 1)) {
        const T n = rdF<DP>(c, op->b), mm = rdF<DP>(c, op->c), d = rdF<DP>(c, op->a);
        T r = vfp3<OP>(n, mm, d);
        // An invalid operation (inf - inf, 0 * inf) gives ARM's positive default NaN; x86 makes a
        // negative one. A NaN operand is propagated instead, so only fix NaNs made from numbers.
        if (__builtin_expect(r != r, 0) && !invalidFromNaN<OP>(n, mm, d)) r = defaultNaN<T>();
        wrF<DP>(c, op->a, r);
        HVM_NEXT();
    }
    if (mode == (kFpscrFZ | kFpscrDN)) {
        const T n = rdF<DP>(c, op->b), mm = rdF<DP>(c, op->c), d = rdF<DP>(c, op->a);
        T r = flushDenorm(vfp3<OP>(flushDenorm(n), flushDenorm(mm), flushDenorm(d)));
        if (r != r) r = defaultNaN<T>();
        wrF<DP>(c, op->a, r);
        HVM_NEXT();
    }
    HVM_MUSTTAIL return H_Vfp3General<OP, DP>(c, op, m);
}

// VSQRT in a rounding mode other than round-to-nearest (see withRounding).
template <bool DP>
[[gnu::noinline]] HVM_H(H_VsqrtRounded) {
    using T = FpT<DP>;
    T v = rdF<DP>(c, op->c);
    if (c->fpscr & kFpscrFZ) v = flushDenorm(v);
    const uint32_t rmode = (c->fpscr >> kFpscrRModeShift) & 3;
    T r = withRounding(rmode, [=] { volatile T vv = v; volatile T vr = std::sqrt(T(vv)); return T(vr); });
    if (r != r && ((c->fpscr & kFpscrDN) || v == v)) r = defaultNaN<T>();   // sqrt(negative): ARM's positive default NaN
    wrF<DP>(c, op->a, r);
    HVM_NEXT();
}

enum VfpOp2 : uint8_t { kVMov, kVAbs, kVNeg, kVSqrt };

// a = d, c = m
template <int OP, bool DP>
HVM_H(H_Vfp2) {
    using T = FpT<DP>;
    if constexpr (OP == kVMov) {
        if constexpr (DP) { uint64_t v; std::memcpy(&v, vregs(c) + 8 * op->c, 8); std::memcpy(vregs(c) + 8 * op->a, &v, 8); }
        else wrSu(c, op->a, rdSu(c, op->c));
    } else if constexpr (OP == kVAbs || OP == kVNeg) {
        // Pure sign-bit operations (never canonicalise NaNs).
        if constexpr (DP) {
            uint64_t v; std::memcpy(&v, vregs(c) + 8 * op->c, 8);
            v = OP == kVAbs ? (v & ~(1ull << 63)) : (v ^ (1ull << 63));
            std::memcpy(vregs(c) + 8 * op->a, &v, 8);
        } else {
            uint32_t v = rdSu(c, op->c);
            wrSu(c, op->a, OP == kVAbs ? (v & 0x7FFFFFFFu) : (v ^ 0x80000000u));
        }
    } else {
        if (__builtin_expect((c->fpscr & kFpscrRMode) != 0, 0)) { HVM_MUSTTAIL return H_VsqrtRounded<DP>(c, op, m); }
        T m = rdF<DP>(c, op->c);
        if (c->fpscr & kFpscrFZ) m = flushDenorm(m);
        T r = std::sqrt(m);
        // sqrt of a negative number is ARM's positive default NaN (x86 makes a negative one); a NaN
        // operand propagates unless DN is set.
        if (std::isnan(r) && ((c->fpscr & kFpscrDN) || !std::isnan(m))) r = defaultNaN<T>();
        wrF<DP>(c, op->a, r);
    }
    HVM_NEXT();
}

// VMOV (immediate): a = d, imm = low 32 bits, imm2 = high 32 bits (double)
template <bool DP>
HVM_H(H_VfpMovImm) {
    if constexpr (DP) { uint64_t v = (uint64_t(op->imm2) << 32) | op->imm; std::memcpy(vregs(c) + 8 * op->a, &v, 8); }
    else wrSu(c, op->a, op->imm);
    HVM_NEXT();
}

// VCMP{E}: a = d, c = m (ignored when ZERO). Writes FPSCR.NZCV.
template <bool DP, bool ZERO>
HVM_H(H_VfpCmp) {
    using T = FpT<DP>;
    T x = rdF<DP>(c, op->a), y = ZERO ? T(0) : rdF<DP>(c, op->c);
    if (c->fpscr & kFpscrFZ) { x = flushDenorm(x); y = flushDenorm(y); }
    uint32_t f;
    if (std::isnan(x) || std::isnan(y)) f = 0x3;        // unordered: C V
    else if (x == y) f = 0x6;                            // Z C
    else if (x < y) f = 0x8;                             // N
    else f = 0x2;                                        // C
    c->fpscr = (c->fpscr & 0x0FFFFFFFu) | (f << 28);
    HVM_NEXT();
}

// VCMP{E} + VMRS APSR_nzcv, FPSCR fused (BlockBuilder::fuseOps): the compare's
// flags go to FPSCR and straight to the integer flags.
template <bool DP, bool ZERO>
HVM_H(H_VfpCmpToApsr) {
    using T = FpT<DP>;
    T x = rdF<DP>(c, op->a), y = ZERO ? T(0) : rdF<DP>(c, op->c);
    if (c->fpscr & kFpscrFZ) { x = flushDenorm(x); y = flushDenorm(y); }
    uint32_t f;
    if (std::isnan(x) || std::isnan(y)) f = 0x3;
    else if (x == y) f = 0x6;
    else if (x < y) f = 0x8;
    else f = 0x2;
    c->fpscr = (c->fpscr & 0x0FFFFFFFu) | (f << 28);
    c->nzcv = (f << 28) & kFlagMaskNZCV;
    HVM_NEXT();
}

// VCVT between single and double: TO_DOUBLE: a = Dd, c = Sm; else a = Sd, c = Dm
template <bool TO_DOUBLE>
HVM_H(H_VfpCvtSD) {
    if constexpr (TO_DOUBLE) {
        float m = rdS(c, op->c);
        if (c->fpscr & kFpscrFZ) m = flushDenorm(m);
        double r = double(m);
        if ((c->fpscr & kFpscrDN) && std::isnan(r)) r = defaultNaN<double>();
        wrD(c, op->a, r);
    } else {
        double m = rdD(c, op->c);
        if (c->fpscr & kFpscrFZ) m = flushDenorm(m);
        float r = float(m);
        if (c->fpscr & kFpscrFZ) r = flushDenorm(r);
        if ((c->fpscr & kFpscrDN) && std::isnan(r)) r = defaultNaN<float>();
        wrS(c, op->a, r);
    }
    HVM_NEXT();
}

// Round a double according to an ARM rounding mode (0 RN, 1 RP, 2 RM, 3 RZ).
inline double roundMode(double v, uint32_t mode) {
    switch (mode) {
        case 0: return std::nearbyint(v);   // host default is round-to-nearest-even
        case 1: return std::ceil(v);
        case 2: return std::floor(v);
        default: return std::trunc(v);
    }
}

// FPToFixed saturation for 32-bit (or 16-bit) integers.
inline uint32_t fpToInt(double v, bool sgn, uint32_t bits, uint32_t& sat) {
    if (std::isnan(v)) { sat = 1; return 0; }
    if (sgn) {
        const double hi = std::ldexp(1.0, int(bits) - 1) - 1, lo = -std::ldexp(1.0, int(bits) - 1);
        if (v > hi) { sat = 1; return uint32_t(int64_t(hi)); }
        if (v < lo) { sat = 1; return uint32_t(int64_t(lo)); }
        return uint32_t(int64_t(v));
    }
    const double hi = std::ldexp(1.0, int(bits)) - 1;
    if (v > hi) { sat = 1; return uint32_t(uint64_t(hi)); }
    if (v < 0) { sat = 1; return 0; }
    return uint32_t(uint64_t(v));
}

// VCVT{R} float → int. a = Sd, c = Sm or Dm. RZ: round toward zero; else FPSCR mode.
template <bool DP, bool SIGNED, bool RZ>
HVM_H(H_VfpCvtToInt) {
    double v = DP ? rdD(c, op->c) : double(rdS(c, op->c));
    if (c->fpscr & kFpscrFZ) v = DP ? flushDenorm(v) : double(flushDenorm(float(v)));
    v = roundMode(v, RZ ? 3 : (c->fpscr >> kFpscrRModeShift) & 3);
    uint32_t sat = 0;
    wrSu(c, op->a, fpToInt(v, SIGNED, 32, sat));
    HVM_NEXT();
}

// VCVT int → float. a = Sd or Dd, c = Sm.
template <bool DP, bool SIGNED>
HVM_H(H_VfpCvtFromInt) {
    const uint32_t v = rdSu(c, op->c);
    if constexpr (DP) wrD(c, op->a, SIGNED ? double(int32_t(v)) : double(v));
    else wrS(c, op->a, SIGNED ? float(int32_t(v)) : float(v));
    HVM_NEXT();
}

// VCVT between float and fixed point (in place). a = Sd/Dd, imm = fraction bits.
template <bool DP, bool TO_FIXED, bool UNSIGNED, bool SIZE32>
HVM_H(H_VfpCvtFixed) {
    constexpr uint32_t bits = SIZE32 ? 32 : 16;
    if constexpr (TO_FIXED) {
        double v = DP ? rdD(c, op->a) : double(rdS(c, op->a));
        v = std::trunc(std::ldexp(v, int(op->imm)));
        uint32_t sat = 0;
        uint32_t r = fpToInt(v, !UNSIGNED, bits, sat);
        if constexpr (!SIZE32) r = UNSIGNED ? (r & 0xFFFF) : uint32_t(int32_t(int16_t(r)));
        if constexpr (DP) {
            uint64_t w = UNSIGNED ? uint64_t(r) : uint64_t(int64_t(int32_t(r)));
            std::memcpy(vregs(c) + 8 * op->a, &w, 8);
        } else wrSu(c, op->a, r);
    } else {
        uint32_t raw = DP ? uint32_t(c->d[op->a]) : rdSu(c, op->a);
        double v;
        if constexpr (SIZE32) v = UNSIGNED ? double(raw) : double(int32_t(raw));
        else v = UNSIGNED ? double(raw & 0xFFFF) : double(int16_t(raw));
        v = std::ldexp(v, -int(op->imm));
        if constexpr (DP) wrD(c, op->a, v); else wrS(c, op->a, float(v));
    }
    HVM_NEXT();
}

// --- half precision ---------------------------------------------------------

// IEEE binary16 <-> binary32, round to nearest even (FPSCR.AHP not modelled).
inline uint16_t f32ToF16(uint32_t f) {
    const uint32_t sign = (f >> 16) & 0x8000u;
    const int32_t exp = int32_t((f >> 23) & 0xFF);
    uint32_t man = f & 0x7FFFFFu;
    if (exp == 0xFF) return uint16_t(sign | 0x7C00u | (man ? (0x200u | (man >> 13)) : 0));   // inf / NaN (quiet)
    int32_t e = exp - 127 + 15;
    if (e >= 0x1F) return uint16_t(sign | 0x7C00u);                                        // overflow -> inf
    if (e <= 0) {                                                                          // subnormal / zero
        if (e < -10) return uint16_t(sign);
        man |= 0x800000u;
        const uint32_t shift = uint32_t(14 - e);
        uint32_t h = man >> shift;
        const uint32_t rem = man & ((1u << shift) - 1), half = 1u << (shift - 1);
        if (rem > half || (rem == half && (h & 1))) ++h;
        return uint16_t(sign | h);
    }
    uint32_t h = (uint32_t(e) << 10) | (man >> 13);
    const uint32_t rem = man & 0x1FFFu;
    if (rem > 0x1000u || (rem == 0x1000u && (h & 1))) ++h;                                 // may carry into exponent: correct
    return uint16_t(sign | h);
}
inline uint32_t f16ToF32(uint16_t h) {
    const uint32_t sign = uint32_t(h & 0x8000u) << 16;
    const uint32_t exp = (h >> 10) & 0x1F, man = h & 0x3FFu;
    if (exp == 0x1F) return sign | 0x7F800000u | (man << 13);
    if (exp == 0) {
        if (!man) return sign;
        int e = -1;
        uint32_t m = man;
        do { m <<= 1; ++e; } while (!(m & 0x400u));
        return sign | (uint32_t(127 - 15 - e) << 23) | ((m & 0x3FFu) << 13);
    }
    return sign | ((exp + 127 - 15) << 23) | (man << 13);
}

// VCVTB/VCVTT. TO_HALF: Sd[top?] = half(Sm); else Sd = float(Sm[top?]). a = d, c = m, d = top
template <bool TO_HALF>
HVM_H(H_VfpCvtHalf) {
    const uint32_t src = rdSu(c, op->c);
    if constexpr (TO_HALF) {
        const uint32_t h = f32ToF16(src), old = rdSu(c, op->a);
        wrSu(c, op->a, op->d ? ((old & 0xFFFFu) | (h << 16)) : ((old & 0xFFFF0000u) | h));
    } else {
        wrSu(c, op->a, f16ToF32(uint16_t(op->d ? (src >> 16) : src)));
    }
    HVM_NEXT();
}

// --- transfers between core and extension registers -------------------------

// VMOV Sn, Rt (a = s, b = rt) / VMOV Rt, Sn (a = rt, b = s)
HVM_H(H_VmovToS)   { wrSu(c, op->a, c->r[op->b]); HVM_NEXT(); }
HVM_H(H_VmovFromS) { c->r[op->a] = rdSu(c, op->b); HVM_NEXT(); }

// 64-bit: words w = a (S index of the low word), rt = b, rt2 = c
HVM_H(H_Vmov2ToS)   { wrSu(c, op->a, c->r[op->b]); wrSu(c, op->a + 1, c->r[op->c]); HVM_NEXT(); }
HVM_H(H_Vmov2FromS) {
    const uint32_t lo = rdSu(c, op->a), hi = rdSu(c, op->a + 1);
    c->r[op->b] = lo; c->r[op->c] = hi;
    HVM_NEXT();
}

// VMRS Rt, <reg>: a = rt (15 → APSR_nzcv), imm = system register number
HVM_H(H_Vmrs) {
    uint32_t v;
    switch (op->imm) {
        case 1: v = c->fpscr; break;
        case 0: v = 0x410330C3u; break;     // FPSID: ARM, VFPv3, Cortex-A8-class
        case 6: v = 0x01111111u; break;     // MVFR1: NEON (int, float, ld/st), half, FZ, DN
        case 7: v = 0x11110222u; break;     // MVFR0: VFPv3 SP+DP, sqrt, div, 32 D regs
        default: v = 0; break;
    }
    if (op->a == 15) c->nzcv = v & kFlagMaskNZCV;
    else c->r[op->a] = v;
    HVM_NEXT();
}
// VMSR FPSCR, Rt: a = rt
HVM_H(H_Vmsr) { c->fpscr = c->r[op->a] & 0xFFC09F9Fu; HVM_NEXT(); }

// --- loads and stores -------------------------------------------------------

// VLDR/VSTR: a = first word (S index; D n → 2n), b = rn, imm = signed offset, d = words (1 or 2)
template <bool LOAD>
HVM_H(H_VLdrStr) {
    const uint32_t addr = c->r[op->b] + op->imm;
    // Fixed sizes so the copies compile to single loads/stores (no memcpy call).
    uint8_t* reg = vregs(c) + 4 * op->a;
    uint8_t* mem = m + uint64_t(addr);
    if (op->d == 1) {
        if constexpr (LOAD) std::memcpy(reg, mem, 4); else std::memcpy(mem, reg, 4);
    } else {
        if constexpr (LOAD) std::memcpy(reg, mem, 8); else std::memcpy(mem, reg, 8);
    }
    HVM_NEXT();
}

// VLDM/VSTM: a = first word, imm = word count, b = rn, d: bit0 inc, bit2 wb (DB always writes back)
template <bool LOAD>
HVM_H(H_VLdmStm) {
    const uint32_t base = c->r[op->b], bytes = 4 * (op->imm + op->c);   // c: FLDMX/FSTMX format word
    const bool inc = op->d & 1;
    const uint32_t addr = inc ? base : base - bytes;
    if constexpr (LOAD) {
        // Copy word by word: a guest range may straddle the register file end.
        for (uint32_t i = 0; i < op->imm && op->a + i < 64; ++i)
            std::memcpy(vregs(c) + 4 * (op->a + i), m + uint64_t(addr + 4 * i), 4);
    } else {
        for (uint32_t i = 0; i < op->imm && op->a + i < 64; ++i)
            std::memcpy(m + uint64_t(addr + 4 * i), vregs(c) + 4 * (op->a + i), 4);
    }
    if (op->d & 4) c->r[op->b] = inc ? base + bytes : base - bytes;
    HVM_NEXT();
}

} // namespace Hydrogen::CPU
