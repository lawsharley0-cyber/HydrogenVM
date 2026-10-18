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

#pragma clang fp contract(off)

namespace Hydrogen::CPU {

enum : uint32_t {
    kFpscrN = 1u << 31, kFpscrQC = 1u << 27, kFpscrDN = 1u << 25, kFpscrFZ = 1u << 24,
    kFpscrRModeShift = 22,
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

// Flush a denormal to a same-signed zero.
template <typename T> inline T flushDenorm(T v) {
    return std::fpclassify(v) == FP_SUBNORMAL ? std::copysign(T(0), v) : v;
}
template <typename T> inline T defaultNaN() {
    if constexpr (sizeof(T) == 4) { uint32_t b = 0x7FC00000u; T f; std::memcpy(&f, &b, 4); return f; }
    else { uint64_t b = 0x7FF8000000000000ull; T f; std::memcpy(&f, &b, 8); return f; }
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

// a = d, b = n, c = m
template <int OP, bool DP>
HVM_H(H_Vfp3) {
    using T = FpT<DP>;
    T n = rdF<DP>(c, op->b), mm = rdF<DP>(c, op->c), d = rdF<DP>(c, op->a);
    if (__builtin_expect((c->fpscr & (kFpscrFZ | kFpscrDN)) != 0, 0)) {
        if (c->fpscr & kFpscrFZ) { n = flushDenorm(n); mm = flushDenorm(mm); d = flushDenorm(d); }
        T r = vfp3<OP>(n, mm, d);
        if (c->fpscr & kFpscrFZ) r = flushDenorm(r);
        if ((c->fpscr & kFpscrDN) && std::isnan(r)) r = defaultNaN<T>();
        wrF<DP>(c, op->a, r);
        HVM_NEXT();
    }
    wrF<DP>(c, op->a, vfp3<OP>(n, mm, d));
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
        T m = rdF<DP>(c, op->c);
        if (c->fpscr & kFpscrFZ) m = flushDenorm(m);
        T r = std::sqrt(m);
        if ((c->fpscr & kFpscrDN) && std::isnan(r)) r = defaultNaN<T>();
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
    if constexpr (LOAD) std::memcpy(vregs(c) + 4 * op->a, m + uint64_t(addr), 4 * op->d);
    else std::memcpy(m + uint64_t(addr), vregs(c) + 4 * op->a, 4 * op->d);
    HVM_NEXT();
}

// VLDM/VSTM: a = first word, imm = word count, b = rn, d: bit0 inc, bit2 wb (DB always writes back)
template <bool LOAD>
HVM_H(H_VLdmStm) {
    const uint32_t base = c->r[op->b], bytes = 4 * op->imm;
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
