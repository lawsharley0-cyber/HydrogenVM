// HydrogenVM — Advanced SIMD (NEON) handlers.
//
// Vector operands are read out of the register file into small local arrays,
// processed lane by lane, and written back. The lane loops are simple enough
// that the host compiler vectorises them, so on an arm64 host most of these
// end up as a handful of native NEON instructions.
//
// ARMv7 NEON floating point always flushes denormals to zero and returns the
// default NaN (it ignores FPSCR.FZ/DN); that's modelled here.
#pragma once
#include "FpHandlers.hpp"
#include <type_traits>

#pragma clang fp contract(off)

namespace Hydrogen::CPU {

// ------------------------------------------------------------ utilities ---

template <typename T> inline void vld(const Cpu* c, uint32_t dreg, T* out, uint32_t bytes) {
    std::memcpy(out, vregs(c) + 8 * dreg, bytes);
}
template <typename T> inline void vst(Cpu* c, uint32_t dreg, const T* in, uint32_t bytes) {
    std::memcpy(vregs(c) + 8 * dreg, in, bytes);
}
inline void setQC(Cpu* c) { c->fpscr |= kFpscrQC; }

using i128 = __int128;

template <typename T> constexpr int kBits = int(sizeof(T) * 8);
template <typename T> constexpr T allOnes() { return T(~std::make_unsigned_t<T>(0)); }
template <typename T> constexpr i128 tMax() { return i128(std::numeric_limits<T>::max()); }
template <typename T> constexpr i128 tMin() { return i128(std::numeric_limits<T>::min()); }

// Saturate a wide value into T, setting FPSCR.QC on saturation.
template <typename T> inline T sat(Cpu* c, i128 v) {
    if (v > tMax<T>()) { setQC(c); return std::numeric_limits<T>::max(); }
    if (v < tMin<T>()) { setQC(c); return std::numeric_limits<T>::min(); }
    return T(v);
}
// Wrapping multiply without signed-overflow UB.
template <typename T> inline T wmul(T a, T b) {
    using U = std::make_unsigned_t<T>;
    return T(U((unsigned long long)U(a) * (unsigned long long)U(b)));
}
template <typename T> inline T wadd(T a, T b) { using U = std::make_unsigned_t<T>; return T(U(U(a) + U(b))); }
template <typename T> inline T wsub(T a, T b) { using U = std::make_unsigned_t<T>; return T(U(U(a) - U(b))); }

// NEON float input/output conditioning: flush denormals, default NaN.
// Bit-level so the lane loops vectorise: a zero exponent with a nonzero
// mantissa is a denormal (keep only the sign); an all-ones exponent with a
// nonzero mantissa is a NaN (becomes the default NaN 0x7FC00000).
inline uint32_t fbits(float f) { uint32_t b; std::memcpy(&b, &f, 4); return b; }
inline float bitsf(uint32_t b) { float f; std::memcpy(&f, &b, 4); return f; }
inline float nIn(float x) {
    const uint32_t b = fbits(x);
    return bitsf((b & 0x7F800000u) == 0 ? (b & 0x80000000u) : b);
}
inline float nOut(float r) {
    uint32_t b = fbits(r);
    const uint32_t e = b & 0x7F800000u;
    b = e == 0 ? (b & 0x80000000u) : b;
    b = (e == 0x7F800000u && (b & 0x007FFFFFu)) ? 0x7FC00000u : b;
    return bitsf(b);
}

// Shift by a signed register amount (VSHL/VRSHL/VQSHL/VQRSHL semantics).
template <typename T> inline T shlReg(Cpu* c, T n, int sh, bool rnd, bool saturate) {
    constexpr int esz = kBits<T>;
    const i128 v = i128(n);
    i128 r;
    if (sh >= 0) {
        if (v == 0) r = 0;
        else if (sh >= esz) {
            if (!saturate) return T(0);
            r = v < 0 ? -(i128(1) << 100) : (i128(1) << 100);
        } else r = v << sh;
    } else {
        int s = -sh;
        if (s > esz + 1) s = esz + 1;
        r = rnd ? ((v + (i128(1) << (s - 1))) >> s) : (v >> s);
    }
    return saturate ? sat<T>(c, r) : T(r);
}

// ---------------------------------------------------- 3 registers, same ---

#define HVM_NF(name, readsD, ...) \
    struct name { static constexpr bool kReadsD = readsD; \
                  template <typename T> static T f([[maybe_unused]] Cpu* c, [[maybe_unused]] T n, [[maybe_unused]] T m, [[maybe_unused]] T d) { __VA_ARGS__ } };

HVM_NF(NAdd,   false, return wadd(n, m);)
HVM_NF(NSub,   false, return wsub(n, m);)
HVM_NF(NMul,   false, return wmul(n, m);)
HVM_NF(NMla,   true,  return wadd(d, wmul(n, m));)
HVM_NF(NMls,   true,  return wsub(d, wmul(n, m));)
HVM_NF(NAnd,   false, return n & m;)
HVM_NF(NBic,   false, return n & ~m;)
HVM_NF(NOrr,   false, return n | m;)
HVM_NF(NOrn,   false, return n | ~m;)
HVM_NF(NEor,   false, return n ^ m;)
HVM_NF(NBsl,   true,  return (d & n) | (~d & m);)
HVM_NF(NBit,   true,  return (d & ~m) | (n & m);)
HVM_NF(NBif,   true,  return (d & m) | (n & ~m);)
HVM_NF(NHAdd,  false, return T((i128(n) + i128(m)) >> 1);)
HVM_NF(NRHAdd, false, return T((i128(n) + i128(m) + 1) >> 1);)
HVM_NF(NHSub,  false, return T((i128(n) - i128(m)) >> 1);)
HVM_NF(NQAdd,  false, return sat<T>(c, i128(n) + i128(m));)
HVM_NF(NQSub,  false, return sat<T>(c, i128(n) - i128(m));)
HVM_NF(NCgt,   false, return n > m ? allOnes<T>() : T(0);)
HVM_NF(NCge,   false, return n >= m ? allOnes<T>() : T(0);)
HVM_NF(NCeq,   false, return n == m ? allOnes<T>() : T(0);)
HVM_NF(NTst,   false, return (n & m) ? allOnes<T>() : T(0);)
HVM_NF(NMax,   false, return n > m ? n : m;)
HVM_NF(NMin,   false, return n < m ? n : m;)
HVM_NF(NAbd,   false, return T(n > m ? i128(n) - i128(m) : i128(m) - i128(n));)
HVM_NF(NAba,   true,  return wadd(d, T(n > m ? i128(n) - i128(m) : i128(m) - i128(n)));)
HVM_NF(NShl,   false, return shlReg<T>(c, n, int(int8_t(m)), false, false);)
HVM_NF(NRShl,  false, return shlReg<T>(c, n, int(int8_t(m)), true, false);)
HVM_NF(NQShl,  false, return shlReg<T>(c, n, int(int8_t(m)), false, true);)
HVM_NF(NQRShl, false, return shlReg<T>(c, n, int(int8_t(m)), true, true);)
HVM_NF(NQDMulh,  false, return sat<T>(c, (2 * i128(n) * i128(m)) >> kBits<T>);)
HVM_NF(NQRDMulh, false, return sat<T>(c, (2 * i128(n) * i128(m) + (i128(1) << (kBits<T> - 1))) >> kBits<T>);)
HVM_NF(NPMul,  false, { unsigned r = 0; for (int i = 0; i < 8; ++i) if ((m >> i) & 1) r ^= unsigned(n) << i; return T(r); })

// Float (lane type float). Results go through nOut().
HVM_NF(FAdd,   false, return nOut(nIn(n) + nIn(m));)
HVM_NF(FSub,   false, return nOut(nIn(n) - nIn(m));)
HVM_NF(FMul,   false, return nOut(nIn(n) * nIn(m));)
HVM_NF(FMla,   true,  { T p = nOut(nIn(n) * nIn(m)); return nOut(nIn(d) + p); })
HVM_NF(FMls,   true,  { T p = nOut(nIn(n) * nIn(m)); return nOut(nIn(d) - p); })
HVM_NF(FFma,   true,  return nOut(std::fma(nIn(n), nIn(m), nIn(d)));)
HVM_NF(FFms,   true,  return nOut(std::fma(-nIn(n), nIn(m), nIn(d)));)
HVM_NF(FAbd,   false, return nOut(std::fabs(nIn(n) - nIn(m)));)
HVM_NF(FMax,   false, { T a = nIn(n), b = nIn(m); if (std::isnan(a) || std::isnan(b)) return defaultNaN<T>();
                        if (a == b) return std::signbit(a) ? b : a; return a > b ? a : b; })
HVM_NF(FMin,   false, { T a = nIn(n), b = nIn(m); if (std::isnan(a) || std::isnan(b)) return defaultNaN<T>();
                        if (a == b) return std::signbit(a) ? a : b; return a < b ? a : b; })
HVM_NF(FRecps, false, { T a = nIn(n), b = nIn(m);
                        if ((std::isinf(a) && b == 0) || (a == 0 && std::isinf(b))) return T(2);
                        T p = nOut(a * b); return nOut(T(2) - p); })
HVM_NF(FRsqrts, false, { T a = nIn(n), b = nIn(m);
                        if ((std::isinf(a) && b == 0) || (a == 0 && std::isinf(b))) return T(1.5);
                        T p = nOut(a * b); return nOut((T(3) - p) / T(2)); })

// a = d, b = n, c = m, imm = number of D registers (1 or 2)
// The D (8-byte) and Q (16-byte) forms get separate fixed-size bodies so the
// compiler unrolls and vectorises the lane loop. HVM_DQ(body) runs `body` with
// a constexpr BYTES of 8 or 16 chosen by op->imm (1 or 2 D registers).
#define HVM_DQ(...)                                                              \
    do {                                                                         \
        auto hvmBody = [&]<uint32_t BYTES>() { __VA_ARGS__ };                    \
        if (op->imm == 2) hvmBody.template operator()<16>();                     \
        else hvmBody.template operator()<8>();                                   \
    } while (0)
template <class F, typename T, uint32_t BYTES>
inline void n3Body(Cpu* c, const Op* op) {
    constexpr int cnt = int(BYTES / sizeof(T));
    T n[cnt], mm[cnt], d[cnt] = {};
    vld(c, op->b, n, BYTES);
    vld(c, op->c, mm, BYTES);
    if constexpr (F::kReadsD) vld(c, op->a, d, BYTES);
    for (int i = 0; i < cnt; ++i) d[i] = F::template f<T>(c, n[i], mm[i], d[i]);
    vst(c, op->a, d, BYTES);
}
template <class F, typename T>
HVM_H(H_N3) {
    if (op->imm == 2) n3Body<F, T, 16>(c, op);
    else n3Body<F, T, 8>(c, op);
    HVM_NEXT();
}

// Float compares producing lane masks. KIND: 0 EQ, 1 GE, 2 GT, 3 ACGE, 4 ACGT; ZERO compares n with 0
// (KIND 5 LE0, 6 LT0 only with ZERO).
template <int KIND, bool ZERO>
HVM_H(H_NFCmp) {
    HVM_DQ(
        constexpr int cnt = int(BYTES / 4);
        float n[cnt], mm[cnt] = {};
        uint32_t out[cnt];
        vld(c, op->b, n, BYTES);
        if constexpr (!ZERO) vld(c, op->c, mm, BYTES);
        for (int i = 0; i < cnt; ++i) {
            float a = nIn(n[i]), b = nIn(mm[i]);
            bool r;
            if constexpr (KIND == 0) r = a == b;
            else if constexpr (KIND == 1) r = a >= b;
            else if constexpr (KIND == 2) r = a > b;
            else if constexpr (KIND == 3) r = std::fabs(a) >= std::fabs(b);
            else if constexpr (KIND == 4) r = std::fabs(a) > std::fabs(b);
            else if constexpr (KIND == 5) r = a <= b;
            else r = a < b;
            out[i] = r ? 0xFFFFFFFFu : 0;
        }
        vst(c, op->a, out, BYTES);
    );
    HVM_NEXT();
}

// Pairwise (D registers only): a = d, b = n, c = m
template <class F, typename T>
HVM_H(H_NPair) {
    constexpr int L = 8 / int(sizeof(T));
    T n[L], mm[L], d[L];
    vld(c, op->b, n, 8);
    vld(c, op->c, mm, 8);
    for (int i = 0; i < L / 2; ++i) {
        d[i] = F::template f<T>(c, n[2 * i], n[2 * i + 1], T(0));
        d[L / 2 + i] = F::template f<T>(c, mm[2 * i], mm[2 * i + 1], T(0));
    }
    vst(c, op->a, d, 8);
    HVM_NEXT();
}

// ------------------------------------------------ 1 register + immediate ---

// a = d, imm/imm2 = 64-bit value, c = registers, d: 0 MOV, 1 MVN, 2 ORR, 3 BIC
HVM_H(H_NImm) {
    const uint64_t v = (uint64_t(op->imm2) << 32) | op->imm;
    for (uint32_t r = 0; r < op->c; ++r) {
        uint64_t& dst = c->d[op->a + r];
        switch (op->d) {
            case 0: dst = v; break;
            case 1: dst = ~v; break;
            case 2: dst |= v; break;
            default: dst &= ~v; break;
        }
    }
    HVM_NEXT();
}

// --------------------------------------------- 2 registers + shift amount ---

enum NShOp : uint8_t { kNShr, kNSra, kNRShr, kNRSra, kNSri, kNShl, kNSli, kNQShl, kNQShlU };

// a = d, c = m, d = shift, imm = registers
template <int OP, typename T>
HVM_H(H_NShImm) {
    constexpr int L = 16 / int(sizeof(T));
    constexpr int esz = kBits<T>;
    using U = std::make_unsigned_t<T>;
    (void)L;
    HVM_DQ(
    constexpr int cnt = int(BYTES / sizeof(T));
    T mm[cnt], d[cnt] = {};
    const int s = op->d;
    vld(c, op->c, mm, BYTES);
    if constexpr (OP == kNSra || OP == kNRSra || OP == kNSri || OP == kNSli) vld(c, op->a, d, BYTES);
    for (int i = 0; i < cnt; ++i) {
        const i128 v = i128(mm[i]);
        if constexpr (OP == kNShr || OP == kNSra) {
            T r = T(v >> s);                          // s in 1..esz; arithmetic for signed, logical for unsigned
            d[i] = OP == kNShr ? r : wadd(d[i], r);
        } else if constexpr (OP == kNRShr || OP == kNRSra) {
            T r = T((v + (i128(1) << (s - 1))) >> s);
            d[i] = OP == kNRShr ? r : wadd(d[i], r);
        } else if constexpr (OP == kNSri) {
            if (s < esz) {
                const U mask = U(allOnes<U>() >> s);
                d[i] = T((U(d[i]) & ~mask) | (U(mm[i]) >> s));
            }
        } else if constexpr (OP == kNShl) {
            d[i] = T(U(mm[i]) << s);
        } else if constexpr (OP == kNSli) {
            const U mask = U(allOnes<U>() << s);
            d[i] = T((U(d[i]) & ~mask) | U(U(mm[i]) << s));
        } else if constexpr (OP == kNQShl) {
            d[i] = sat<T>(c, v << s);
        } else {                                       // VQSHLU: signed in, unsigned out
            d[i] = T(sat<U>(c, v << s));
        }
    }
    vst(c, op->a, d, BYTES);
    );
    HVM_NEXT();
}

// Narrowing right shifts (and VMOVN/VQMOVN/VQMOVUN with shift 0).
// TW = wide input lane type; MODE: 0 truncate, 1 round, 2 sat, 3 round+sat, 4 sat→unsigned, 5 round+sat→unsigned
// a = d (D), c = m (Q), d = shift
template <typename TW, int MODE>
HVM_H(H_NShNarrow) {
    using TNs = std::conditional_t<sizeof(TW) == 2, int8_t, std::conditional_t<sizeof(TW) == 4, int16_t, int32_t>>;
    using TN = std::conditional_t<std::is_signed_v<TW>, TNs, std::make_unsigned_t<TNs>>;
    using TNU = std::make_unsigned_t<TNs>;
    constexpr int L = 16 / int(sizeof(TW));
    TW mm[L];
    TN out[L];
    vld(c, op->c, mm, 16);
    const int s = op->d;
    for (int i = 0; i < L; ++i) {
        i128 v = i128(mm[i]);
        if (MODE == 1 || MODE == 3 || MODE == 5) { if (s) v = (v + (i128(1) << (s - 1))) >> s; }
        else v >>= s;
        if constexpr (MODE <= 1) out[i] = TN(v);
        else if constexpr (MODE <= 3) out[i] = sat<TN>(c, v);
        else out[i] = TN(sat<TNU>(c, v));
    }
    vst(c, op->a, out, 8);
    HVM_NEXT();
}

// Widening: VMOVL / VSHLL. TN = narrow lane type. a = d (Q), c = m (D), d = shift
template <typename TN>
HVM_H(H_NWiden) {
    using TW = std::conditional_t<sizeof(TN) == 1, int16_t, std::conditional_t<sizeof(TN) == 2, int32_t, int64_t>>;
    using TWx = std::conditional_t<std::is_signed_v<TN>, TW, std::make_unsigned_t<TW>>;
    using U = std::make_unsigned_t<TW>;
    constexpr int L = 8 / int(sizeof(TN));
    TN mm[L];
    TWx out[L];
    vld(c, op->c, mm, 8);
    for (int i = 0; i < L; ++i) out[i] = TWx(U(TWx(mm[i])) << op->d);
    vst(c, op->a, out, 16);
    HVM_NEXT();
}

// VCVT between f32 and 32-bit fixed point / integer (fbits may be 0).
// a = d, c = m, imm = registers, d = fraction bits
template <bool TO_FIXED, bool UNSIGNED>
HVM_H(H_NCvtFixed) {
    // Scaling by 2^fbits is exact in double, so a multiply replaces ldexp.
    const double up = std::ldexp(1.0, int(op->d)), down = 1.0 / up;
    HVM_DQ(
        constexpr int cnt = int(BYTES / 4);
        uint32_t v[cnt];
        vld(c, op->c, v, BYTES);
        for (int i = 0; i < cnt; ++i) {
            if constexpr (TO_FIXED) {
                double x = std::trunc(double(nIn(bitsf(v[i]))) * up);
                uint32_t s = 0;
                v[i] = fpToInt(x, !UNSIGNED, 32, s);
                if (s) setQC(c);
            } else {
                double x = UNSIGNED ? double(v[i]) : double(int32_t(v[i]));
                v[i] = fbits(nOut(float(x * down)));
            }
        }
        vst(c, op->a, v, BYTES);
    );
    HVM_NEXT();
}

// VCVT.F16.F32 Dd, Qm (TO_HALF) / VCVT.F32.F16 Qd, Dm. a = d, c = m
template <bool TO_HALF>
HVM_H(H_NCvtHalf) {
    if constexpr (TO_HALF) {
        uint32_t in[4]; uint16_t out[4];
        vld(c, op->c, in, 16);
        for (int i = 0; i < 4; ++i) out[i] = f32ToF16(fbits(nIn(bitsf(in[i]))));
        vst(c, op->a, out, 8);
    } else {
        uint16_t in[4]; uint32_t out[4];
        vld(c, op->c, in, 8);
        for (int i = 0; i < 4; ++i) out[i] = fbits(nOut(bitsf(f16ToF32(in[i]))));
        vst(c, op->a, out, 16);
    }
    HVM_NEXT();
}

// --------------------------------------------- 3 registers, different len ---

enum NLongOp : uint8_t { kNAddL, kNSubL, kNAbdL, kNAbaL, kNMlaL, kNMlsL, kNMulL, kNQDMulL, kNQDMlaL, kNQDMlsL, kNPMulL };

// Long: n, m narrow (D) → d wide (Q). TN = narrow lane type.
template <int OP, typename TN>
HVM_H(H_N3Long) {
    using TWs = std::conditional_t<sizeof(TN) == 1, int16_t, std::conditional_t<sizeof(TN) == 2, int32_t, int64_t>>;
    using TW = std::conditional_t<std::is_signed_v<TN>, TWs, std::make_unsigned_t<TWs>>;
    constexpr int L = 8 / int(sizeof(TN));
    TN n[L], mm[L];
    TW d[L];
    vld(c, op->b, n, 8);
    vld(c, op->c, mm, 8);
    vld(c, op->a, d, 16);
    for (int i = 0; i < L; ++i) {
        const i128 a = i128(n[i]), b = i128(mm[i]);
        switch (OP) {
            case kNAddL: d[i] = TW(a + b); break;
            case kNSubL: d[i] = TW(a - b); break;
            case kNAbdL: d[i] = TW(a > b ? a - b : b - a); break;
            case kNAbaL: d[i] = wadd(d[i], TW(a > b ? a - b : b - a)); break;
            case kNMlaL: d[i] = wadd(d[i], TW(a * b)); break;
            case kNMlsL: d[i] = wsub(d[i], TW(a * b)); break;
            case kNMulL: d[i] = TW(a * b); break;
            case kNQDMulL: d[i] = sat<TW>(c, 2 * a * b); break;
            case kNQDMlaL: d[i] = sat<TW>(c, i128(d[i]) + i128(sat<TW>(c, 2 * a * b))); break;
            case kNQDMlsL: d[i] = sat<TW>(c, i128(d[i]) - i128(sat<TW>(c, 2 * a * b))); break;
            default: {                                         // VMULL.P8
                unsigned r = 0;
                for (int k = 0; k < 8; ++k) if ((uint8_t(mm[i]) >> k) & 1) r ^= unsigned(uint8_t(n[i])) << k;
                d[i] = TW(r);
            }
        }
    }
    vst(c, op->a, d, 16);
    HVM_NEXT();
}

// Wide: n wide (Q), m narrow (D) → d wide (Q). SUB selects VSUBW.
template <bool SUB, typename TN>
HVM_H(H_N3Wide) {
    using TWs = std::conditional_t<sizeof(TN) == 1, int16_t, std::conditional_t<sizeof(TN) == 2, int32_t, int64_t>>;
    using TW = std::conditional_t<std::is_signed_v<TN>, TWs, std::make_unsigned_t<TWs>>;
    constexpr int L = 8 / int(sizeof(TN));
    TW n[L], d[L];
    TN mm[L];
    vld(c, op->b, n, 16);
    vld(c, op->c, mm, 8);
    for (int i = 0; i < L; ++i) d[i] = SUB ? wsub(n[i], TW(mm[i])) : wadd(n[i], TW(mm[i]));
    vst(c, op->a, d, 16);
    HVM_NEXT();
}

// Narrow high half: n, m wide (Q) → d narrow (D). TW unsigned wide type. ROUND, SUB.
template <typename TW, bool SUB, bool ROUND>
HVM_H(H_N3NarrowHigh) {
    using TN = std::conditional_t<sizeof(TW) == 2, uint8_t, std::conditional_t<sizeof(TW) == 4, uint16_t, uint32_t>>;
    constexpr int L = 16 / int(sizeof(TW));
    constexpr int h = kBits<TN>;
    TW n[L], mm[L];
    TN d[L];
    vld(c, op->b, n, 16);
    vld(c, op->c, mm, 16);
    for (int i = 0; i < L; ++i) {
        TW r = SUB ? wsub(n[i], mm[i]) : wadd(n[i], mm[i]);
        if (ROUND) r = wadd(r, TW(TW(1) << (h - 1)));
        d[i] = TN(r >> h);
    }
    vst(c, op->a, d, 8);
    HVM_NEXT();
}

// ------------------------------------------------------ 2 registers, misc ---

#define HVM_NU(name, ...) \
    struct name { template <typename T> static T f([[maybe_unused]] Cpu* c, [[maybe_unused]] T x) { __VA_ARGS__ } };

HVM_NU(UClz,  { using U = std::make_unsigned_t<T>; U u = U(x); if (!u) return T(kBits<T>);
                return T(__builtin_clzll((unsigned long long)u) - (64 - kBits<T>)); })
HVM_NU(UCls,  { using U = std::make_unsigned_t<T>; U u = U(x); U s = U(u ^ U(u << 1)); s = U(s | 1u);
                return T(__builtin_clzll((unsigned long long)s) - (64 - kBits<T>)); })
HVM_NU(UCnt,  return T(__builtin_popcount(unsigned(x)));)
HVM_NU(UMvn,  return T(~x);)
HVM_NU(UAbs,  return x < 0 ? wsub(T(0), x) : x;)
HVM_NU(UNeg,  return wsub(T(0), x);)
HVM_NU(UQAbs, return sat<T>(c, x < 0 ? -i128(x) : i128(x));)
HVM_NU(UQNeg, return sat<T>(c, -i128(x));)
HVM_NU(UCgt0, return x > 0 ? allOnes<T>() : T(0);)
HVM_NU(UCge0, return x >= 0 ? allOnes<T>() : T(0);)
HVM_NU(UCeq0, return x == 0 ? allOnes<T>() : T(0);)
HVM_NU(UCle0, return x <= 0 ? allOnes<T>() : T(0);)
HVM_NU(UClt0, return x < 0 ? allOnes<T>() : T(0);)
HVM_NU(UFAbs, return bitsf(fbits(x) & 0x7FFFFFFFu);)
HVM_NU(UFNeg, return bitsf(fbits(x) ^ 0x80000000u);)

// a = d, c = m, imm = registers
template <class F, typename T>
HVM_H(H_NUnary) {
    constexpr int L = 16 / int(sizeof(T));
    (void)L;
    HVM_DQ(
        constexpr int cnt = int(BYTES / sizeof(T));
        T v[cnt];
        vld(c, op->c, v, BYTES);
        for (int i = 0; i < cnt; ++i) v[i] = F::template f<T>(c, v[i]);
        vst(c, op->a, v, BYTES);
    );
    HVM_NEXT();
}

// ---- reciprocal / reciprocal square root estimates (ARM ARM pseudocode) ----

inline double recipEstimate(double a) {           // a in [0.5, 1)
    int q = int(a * 512.0);
    double r = 1.0 / ((double(q) + 0.5) / 512.0);
    int s = int(256.0 * r + 0.5);
    return double(s) / 256.0;
}
inline double recipSqrtEstimate(double a) {       // a in [0.25, 1)
    double r;
    if (a < 0.5) { int q0 = int(a * 512.0); r = 1.0 / std::sqrt((double(q0) + 0.5) / 512.0); }
    else         { int q1 = int(a * 256.0); r = 1.0 / std::sqrt((double(q1) + 0.5) / 256.0); }
    int s = int(256.0 * r + 0.5);
    return double(s) / 256.0;
}
inline uint64_t dbits(double d) { uint64_t b; std::memcpy(&b, &d, 8); return b; }
inline double bitsd(uint64_t b) { double d; std::memcpy(&d, &b, 8); return d; }

inline uint32_t neonRecpeF32(uint32_t x) {
    const float f = nIn(bitsf(x));
    const uint32_t sign = fbits(f) & 0x80000000u;
    if (std::isnan(f)) return 0x7FC00000u;
    if (std::isinf(f)) return sign;
    if (f == 0) return sign | 0x7F800000u;
    const uint32_t exp = (fbits(f) >> 23) & 0xFF;
    if (exp >= 253) return sign;                  // |x| >= 2^126: result underflows (flushed)
    const double scaled = bitsd((uint64_t(0x3FE) << 52) | (uint64_t(fbits(f) & 0x7FFFFF) << 29));
    const uint64_t est = dbits(recipEstimate(scaled));
    const uint32_t rexp = (253 - exp) & 0xFF;
    return sign | (rexp << 23) | uint32_t((est >> 29) & 0x7FFFFF);
}
inline uint32_t neonRsqrteF32(uint32_t x) {
    const float f = nIn(bitsf(x));
    const uint32_t sign = fbits(f) & 0x80000000u;
    if (std::isnan(f)) return 0x7FC00000u;
    if (f == 0) return sign | 0x7F800000u;
    if (sign) return 0x7FC00000u;
    if (std::isinf(f)) return 0;
    const uint32_t exp = (fbits(f) >> 23) & 0xFF;
    const uint64_t frac = uint64_t(fbits(f) & 0x7FFFFF) << 29;
    // Even exponent: scale into [0.5, 1); odd exponent: into [0.25, 0.5).
    const double scaled = bitsd(((exp & 1) == 0 ? (uint64_t(0x3FE) << 52) : (uint64_t(0x3FD) << 52)) | frac);
    const uint64_t est = dbits(recipSqrtEstimate(scaled));
    const uint32_t rexp = ((380 - exp) / 2) & 0xFF;
    return (rexp << 23) | uint32_t((est >> 29) & 0x7FFFFF);
}
inline uint32_t neonRecpeU32(uint32_t x) {
    if (!(x >> 31)) return 0xFFFFFFFFu;
    const double scaled = bitsd((uint64_t(0x3FE) << 52) | (uint64_t(x & 0x7FFFFFFFu) << 21));
    return 0x80000000u | uint32_t((dbits(recipEstimate(scaled)) >> 21) & 0x7FFFFFFFu);
}
inline uint32_t neonRsqrteU32(uint32_t x) {
    if (!(x >> 30)) return 0xFFFFFFFFu;
    double scaled;
    if (!(x >> 31)) scaled = bitsd((uint64_t(0x3FD) << 52) | (uint64_t(x & 0x3FFFFFFFu) << 22));
    else scaled = bitsd((uint64_t(0x3FE) << 52) | (uint64_t(x & 0x7FFFFFFFu) << 21));
    return 0x80000000u | uint32_t((dbits(recipSqrtEstimate(scaled)) >> 21) & 0x7FFFFFFFu);
}

// KIND: 0 VRECPE.U32, 1 VRSQRTE.U32, 2 VRECPE.F32, 3 VRSQRTE.F32. a = d, c = m, imm = registers
template <int KIND>
HVM_H(H_NEstimate) {
    uint32_t v[4];
    const uint32_t bytes = op->imm * 8;
    vld(c, op->c, v, bytes);
    for (uint32_t i = 0; i < bytes / 4; ++i) {
        if (KIND == 0) v[i] = neonRecpeU32(v[i]);
        else if (KIND == 1) v[i] = neonRsqrteU32(v[i]);
        else if (KIND == 2) v[i] = neonRecpeF32(v[i]);
        else v[i] = neonRsqrteF32(v[i]);
    }
    vst(c, op->a, v, bytes);
    HVM_NEXT();
}

// VPADDL / VPADAL. TN narrow lane type. a = d, c = m, imm = registers
template <typename TN, bool ACC>
HVM_H(H_NPairLong) {
    using TWs = std::conditional_t<sizeof(TN) == 1, int16_t, std::conditional_t<sizeof(TN) == 2, int32_t, int64_t>>;
    using TW = std::conditional_t<std::is_signed_v<TN>, TWs, std::make_unsigned_t<TWs>>;
    constexpr int L = 16 / int(sizeof(TN));
    TN v[L];
    TW d[L / 2];
    const uint32_t bytes = op->imm * 8;
    const int cnt = int(bytes / sizeof(TW));
    vld(c, op->c, v, bytes);
    if (ACC) vld(c, op->a, d, bytes);
    for (int i = 0; i < cnt; ++i) {
        TW s = TW(TW(v[2 * i]) + TW(v[2 * i + 1]));
        d[i] = ACC ? wadd(d[i], s) : s;
    }
    vst(c, op->a, d, bytes);
    HVM_NEXT();
}

// VREV16/32/64: a = d, c = m, imm = registers, d = element bytes, imm2 = group bytes
HVM_H(H_NRev) {
    uint8_t in[16], out[16];
    const uint32_t bytes = op->imm * 8, e = op->d, g = op->imm2;
    vld(c, op->c, in, bytes);
    for (uint32_t base = 0; base < bytes; base += g)
        for (uint32_t k = 0; k < g / e; ++k)
            std::memcpy(out + base + k * e, in + base + g - (k + 1) * e, e);
    vst(c, op->a, out, bytes);
    HVM_NEXT();
}

// VSWP / VTRN / VUZP / VZIP. a = d, c = m, imm = registers, d = element bytes, imm2: 0 SWP 1 TRN 2 UZP 3 ZIP
HVM_H(H_NPermute) {
    uint8_t x[16], y[16], ox[16], oy[16];
    const uint32_t bytes = op->imm * 8, e = op->d, n = bytes / e;
    vld(c, op->a, x, bytes);
    vld(c, op->c, y, bytes);
    switch (op->imm2) {
        case 0: std::memcpy(ox, y, bytes); std::memcpy(oy, x, bytes); break;
        case 1:
            std::memcpy(ox, x, bytes); std::memcpy(oy, y, bytes);
            for (uint32_t i = 0; i < n; i += 2) {
                std::memcpy(ox + (i + 1) * e, y + i * e, e);
                std::memcpy(oy + i * e, x + (i + 1) * e, e);
            }
            break;
        case 2: {
            uint8_t cat[32];
            std::memcpy(cat, x, bytes); std::memcpy(cat + bytes, y, bytes);
            for (uint32_t i = 0; i < n; ++i) {
                std::memcpy(ox + i * e, cat + (2 * i) * e, e);
                std::memcpy(oy + i * e, cat + (2 * i + 1) * e, e);
            }
            break;
        }
        default: {
            uint8_t cat[32];
            for (uint32_t i = 0; i < n; ++i) {
                std::memcpy(cat + (2 * i) * e, x + i * e, e);
                std::memcpy(cat + (2 * i + 1) * e, y + i * e, e);
            }
            std::memcpy(ox, cat, bytes); std::memcpy(oy, cat + bytes, bytes);
        }
    }
    vst(c, op->a, ox, bytes);
    vst(c, op->c, oy, bytes);
    HVM_NEXT();
}

// VTBL / VTBX: a = d, b = first table register, c = index register, imm = table length (1-4), d = 1 for VTBX
HVM_H(H_NTbl) {
    uint8_t table[32], idx[8], out[8];
    const uint32_t len = op->imm * 8;
    for (uint32_t r = 0; r < op->imm; ++r) vld(c, (op->b + r) & 31, table + 8 * r, 8);
    vld(c, op->c, idx, 8);
    vld(c, op->a, out, 8);
    for (int i = 0; i < 8; ++i) {
        if (idx[i] < len) out[i] = table[idx[i]];
        else if (!op->d) out[i] = 0;
    }
    vst(c, op->a, out, 8);
    HVM_NEXT();
}

// VEXT: a = d, b = n, c = m, imm = byte offset, imm2 = registers
HVM_H(H_NExt) {
    uint8_t cat[32];
    const uint32_t bytes = op->imm2 * 8;
    vld(c, op->b, cat, bytes);
    vld(c, op->c, cat + bytes, bytes);
    vst(c, op->a, cat + op->imm, bytes);
    HVM_NEXT();
}

// VDUP (scalar): a = d, c = m (D register), imm = registers, d = element bytes, imm2 = lane
HVM_H(H_NDupLane) {
    uint8_t src[8], out[16];
    vld(c, op->c, src, 8);
    const uint32_t e = op->d, bytes = op->imm * 8;
    for (uint32_t i = 0; i < bytes; i += e) std::memcpy(out + i, src + op->imm2 * e, e);
    vst(c, op->a, out, bytes);
    HVM_NEXT();
}

// ---------------------------------------------------- core <-> scalar ---

// VMOV Dd[x], Rt: a = d, b = rt, d = element bytes, imm = lane
HVM_H(H_NMovToLane) {
    const uint32_t v = c->r[op->b];
    std::memcpy(vregs(c) + 8 * op->a + op->imm * op->d, &v, op->d);
    HVM_NEXT();
}
// VMOV Rt, Dn[x]: a = rt, b = n, d = element bytes, imm = lane, c = 1 if unsigned
HVM_H(H_NMovFromLane) {
    uint32_t v = 0;
    std::memcpy(&v, vregs(c) + 8 * op->b + op->imm * op->d, op->d);
    if (!op->c) {
        if (op->d == 1) v = uint32_t(int32_t(int8_t(v)));
        else if (op->d == 2) v = uint32_t(int32_t(int16_t(v)));
    }
    c->r[op->a] = v;
    HVM_NEXT();
}
// VDUP Dd, Rt: a = d, b = rt, d = element bytes, imm = registers
HVM_H(H_NDupCore) {
    uint8_t out[16];
    const uint32_t v = c->r[op->b];
    for (uint32_t i = 0; i < op->imm * 8; i += op->d) std::memcpy(out + i, &v, op->d);
    vst(c, op->a, out, op->imm * 8);
    HVM_NEXT();
}

// --------------------------------------------- element / structure loads ---

// Common fields: a = first D register, b = rn, c = rm, d = log2(element bytes)
// imm: bits 0-3 structure members (1-4), 4-7 registers per member, 8-11 register stride,
//      12-15 lane (single-lane form), bit 16: all-lanes (replicate) form
inline void neonWriteback(Cpu* c, const Op* op, uint32_t bytes) {
    if (op->c == 15) return;
    c->r[op->b] += op->c == 13 ? bytes : c->r[op->c];
}

template <bool LOAD>
HVM_H(H_NLdStMulti) {
    const uint32_t e = 1u << op->d, members = op->imm & 15, regs = (op->imm >> 4) & 15, inc = (op->imm >> 8) & 15;
    const uint32_t elems = 8 / e;
    uint32_t addr = c->r[op->b];
    const uint32_t start = addr;
    uint8_t* vr = vregs(c);
    for (uint32_t r = 0; r < regs; ++r)
        for (uint32_t el = 0; el < elems; ++el)
            for (uint32_t k = 0; k < members; ++k) {
                const uint32_t dreg = (op->a + r + k * inc) & 31;
                uint8_t* p = vr + 8 * dreg + el * e;
                if constexpr (LOAD) std::memcpy(p, m + uint64_t(addr), e);
                else std::memcpy(m + uint64_t(addr), p, e);
                addr += e;
            }
    neonWriteback(c, op, addr - start);
    HVM_NEXT();
}

template <bool LOAD>
HVM_H(H_NLdStLane) {
    const uint32_t e = 1u << op->d, members = op->imm & 15, inc = (op->imm >> 8) & 15, lane = (op->imm >> 12) & 15;
    const bool all = (op->imm >> 16) & 1;
    const uint32_t regsAll = (op->imm >> 4) & 15;     // all-lanes VLD1: 1 or 2 registers
    uint32_t addr = c->r[op->b];
    const uint32_t start = addr;
    uint8_t* vr = vregs(c);
    for (uint32_t k = 0; k < members; ++k) {
        const uint32_t dreg = (op->a + k * inc) & 31;
        if (all) {
            uint8_t v[8];
            std::memcpy(v, m + uint64_t(addr), e);
            const uint32_t nr = members == 1 ? regsAll : 1;
            for (uint32_t r = 0; r < nr; ++r)
                for (uint32_t i = 0; i < 8; i += e) std::memcpy(vr + 8 * ((dreg + r) & 31) + i, v, e);
        } else if constexpr (LOAD) {
            std::memcpy(vr + 8 * dreg + lane * e, m + uint64_t(addr), e);
        } else {
            std::memcpy(m + uint64_t(addr), vr + 8 * dreg + lane * e, e);
        }
        addr += e;
    }
    neonWriteback(c, op, addr - start);
    HVM_NEXT();
}

} // namespace Hydrogen::CPU
