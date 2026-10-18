// HydrogenVM — Advanced SIMD (NEON) decoding, ARM encodings (A7.4, A7.7, A7.8).
// Thumb encodings are converted to these by decodeThumbVfpNeon().
#include "Decoder.hpp"
#include "NeonHandlers.hpp"

namespace Hydrogen::CPU {
namespace {

using R = DecodeResult;

inline uint32_t bits(uint32_t v, int hi, int lo) { return (v >> lo) & ((2u << (hi - lo)) - 1); }
inline uint32_t bit(uint32_t v, int b) { return (v >> b) & 1; }

R undefined(BlockBuilder& bb, uint32_t insn) { bb.exitSimple(&H_Undefined, insn); return R::End; }

inline uint32_t regD(uint32_t insn) { return (bit(insn, 22) << 4) | bits(insn, 15, 12); }
inline uint32_t regN(uint32_t insn) { return (bit(insn, 7) << 4) | bits(insn, 19, 16); }
inline uint32_t regM(uint32_t insn) { return (bit(insn, 5) << 4) | bits(insn, 3, 0); }

// ------------------------------------------------------- handler pickers ---

template <class F> Handler pickInt(uint32_t size, bool uns) {
    switch (size) {
        case 0: return uns ? &H_N3<F, uint8_t>  : &H_N3<F, int8_t>;
        case 1: return uns ? &H_N3<F, uint16_t> : &H_N3<F, int16_t>;
        case 2: return uns ? &H_N3<F, uint32_t> : &H_N3<F, int32_t>;
        default: return uns ? &H_N3<F, uint64_t> : &H_N3<F, int64_t>;
    }
}
template <class F> Handler pickPair(uint32_t size, bool uns) {
    switch (size) {
        case 0: return uns ? &H_NPair<F, uint8_t>  : &H_NPair<F, int8_t>;
        case 1: return uns ? &H_NPair<F, uint16_t> : &H_NPair<F, int16_t>;
        default: return uns ? &H_NPair<F, uint32_t> : &H_NPair<F, int32_t>;
    }
}
template <class F> Handler pickUnary(uint32_t size, bool uns) {
    switch (size) {
        case 0: return uns ? &H_NUnary<F, uint8_t>  : &H_NUnary<F, int8_t>;
        case 1: return uns ? &H_NUnary<F, uint16_t> : &H_NUnary<F, int16_t>;
        case 2: return uns ? &H_NUnary<F, uint32_t> : &H_NUnary<F, int32_t>;
        default: return uns ? &H_NUnary<F, uint64_t> : &H_NUnary<F, int64_t>;
    }
}
template <int OP> Handler pickShImm(uint32_t esize, bool uns) {
    switch (esize) {
        case 8:  return uns ? &H_NShImm<OP, uint8_t>  : &H_NShImm<OP, int8_t>;
        case 16: return uns ? &H_NShImm<OP, uint16_t> : &H_NShImm<OP, int16_t>;
        case 32: return uns ? &H_NShImm<OP, uint32_t> : &H_NShImm<OP, int32_t>;
        default: return uns ? &H_NShImm<OP, uint64_t> : &H_NShImm<OP, int64_t>;
    }
}
// esize = narrow (destination) element size; MODE as in H_NShNarrow; signedness of the wide input.
template <int MODE> Handler pickNarrow(uint32_t esize, bool unsignedInput) {
    switch (esize) {
        case 8:  return unsignedInput ? &H_NShNarrow<uint16_t, MODE> : &H_NShNarrow<int16_t, MODE>;
        case 16: return unsignedInput ? &H_NShNarrow<uint32_t, MODE> : &H_NShNarrow<int32_t, MODE>;
        default: return unsignedInput ? &H_NShNarrow<uint64_t, MODE> : &H_NShNarrow<int64_t, MODE>;
    }
}
Handler pickWiden(uint32_t esize, bool uns) {
    switch (esize) {
        case 8:  return uns ? &H_NWiden<uint8_t>  : &H_NWiden<int8_t>;
        case 16: return uns ? &H_NWiden<uint16_t> : &H_NWiden<int16_t>;
        default: return uns ? &H_NWiden<uint32_t> : &H_NWiden<int32_t>;
    }
}
template <int OP> Handler pickLong(uint32_t size, bool uns) {
    switch (size) {
        case 0: return uns ? &H_N3Long<OP, uint8_t>  : &H_N3Long<OP, int8_t>;
        case 1: return uns ? &H_N3Long<OP, uint16_t> : &H_N3Long<OP, int16_t>;
        default: return uns ? &H_N3Long<OP, uint32_t> : &H_N3Long<OP, int32_t>;
    }
}

// ------------------------------------------------- 3 registers, same len ---

R threeSame(BlockBuilder& bb, uint32_t insn) {
    const uint32_t A = bits(insn, 11, 8), B = bit(insn, 4), U = bit(insn, 24), size = bits(insn, 21, 20);
    const uint32_t regs = bit(insn, 6) ? 2 : 1;
    const uint32_t d = regD(insn), n = regN(insn), m = regM(insn);
    auto emit = [&](Handler h) { bb.emit(h, d, n, m, 0, regs); return R::Continue; };
    auto emitShift = [&](Handler h) { bb.emit(h, d, m, n, 0, regs); return R::Continue; };   // value Vm, amount Vn
    auto emitPair = [&](Handler h) { if (regs != 1) return undefined(bb, insn); bb.emit(h, d, n, m); return R::Continue; };
    const bool sz1 = bit(insn, 21);   // float op selector
    switch (A) {
        case 0: if (!B && size == 3) break; return emit(B ? pickInt<NQAdd>(size, U) : pickInt<NHAdd>(size, U));
        case 1:
            if (!B) { if (size == 3) break; return emit(pickInt<NRHAdd>(size, U)); }
            if (!U) {
                static constexpr Handler t[4] = { &H_N3<NAnd, uint64_t>, &H_N3<NBic, uint64_t>, &H_N3<NOrr, uint64_t>, &H_N3<NOrn, uint64_t> };
                return emit(t[size]);
            } else {
                static constexpr Handler t[4] = { &H_N3<NEor, uint64_t>, &H_N3<NBsl, uint64_t>, &H_N3<NBit, uint64_t>, &H_N3<NBif, uint64_t> };
                return emit(t[size]);
            }
        case 2: if (!B && size == 3) break; return emit(B ? pickInt<NQSub>(size, U) : pickInt<NHSub>(size, U));
        case 3: if (size == 3) break; return emit(B ? pickInt<NCge>(size, U) : pickInt<NCgt>(size, U));
        case 4: return emitShift(B ? pickInt<NQShl>(size, U) : pickInt<NShl>(size, U));
        case 5: return emitShift(B ? pickInt<NQRShl>(size, U) : pickInt<NRShl>(size, U));
        case 6: if (size == 3) break; return emit(B ? pickInt<NMin>(size, U) : pickInt<NMax>(size, U));
        case 7: if (size == 3) break; return emit(B ? pickInt<NAba>(size, U) : pickInt<NAbd>(size, U));
        case 8:
            if (!B) return emit(U ? pickInt<NSub>(size, true) : pickInt<NAdd>(size, true));
            if (size == 3) break;
            return emit(U ? pickInt<NCeq>(size, true) : pickInt<NTst>(size, true));
        case 9:
            if (size == 3) break;
            if (!B) return emit(U ? pickInt<NMls>(size, true) : pickInt<NMla>(size, true));
            if (U) { if (size != 0) break; return emit(&H_N3<NPMul, uint8_t>); }
            return emit(pickInt<NMul>(size, true));
        case 10: if (size == 3) break; return emitPair(B ? pickPair<NMin>(size, U) : pickPair<NMax>(size, U));
        case 11:
            if (!B) {
                if (size == 0 || size == 3) break;
                return emit(U ? pickInt<NQRDMulh>(size, false) : pickInt<NQDMulh>(size, false));
            }
            if (U || size == 3) break;
            return emitPair(pickPair<NAdd>(size, true));
        case 12:
            if (!B || U || bit(insn, 20)) break;
            return emit(sz1 ? &H_N3<FFms, float> : &H_N3<FFma, float>);
        case 13:
            if (bit(insn, 20)) break;
            if (!B) {
                if (!U) return emit(sz1 ? &H_N3<FSub, float> : &H_N3<FAdd, float>);
                if (sz1) return emit(&H_N3<FAbd, float>);
                return emitPair(&H_NPair<FAdd, float>);
            }
            if (!U) return emit(sz1 ? &H_N3<FMls, float> : &H_N3<FMla, float>);
            if (!sz1) return emit(&H_N3<FMul, float>);
            break;
        case 14:
            if (bit(insn, 20)) break;
            if (!B) {
                if (!U) { if (sz1) break; return emit(&H_NFCmp<0, false>); }
                return emit(sz1 ? &H_NFCmp<2, false> : &H_NFCmp<1, false>);
            }
            if (!U) break;
            return emit(sz1 ? &H_NFCmp<4, false> : &H_NFCmp<3, false>);
        case 15:
            if (bit(insn, 20)) break;
            if (!B) {
                if (!U) return emit(sz1 ? &H_N3<FMin, float> : &H_N3<FMax, float>);
                return emitPair(sz1 ? &H_NPair<FMin, float> : &H_NPair<FMax, float>);
            }
            if (U) break;
            return emit(sz1 ? &H_N3<FRsqrts, float> : &H_N3<FRecps, float>);
    }
    return undefined(bb, insn);
}

// ---------------------------------------------- 1 register + immediate ---

R oneRegImm(BlockBuilder& bb, uint32_t insn) {
    const uint32_t cmode = bits(insn, 11, 8), op = bit(insn, 5);
    const uint32_t imm8 = (bit(insn, 24) << 7) | (bits(insn, 18, 16) << 4) | bits(insn, 3, 0);
    const uint32_t regs = bit(insn, 6) ? 2 : 1, d = regD(insn);
    uint64_t v = 0;
    auto rep32 = [](uint32_t x) { return (uint64_t(x) << 32) | x; };
    auto rep16 = [](uint32_t x) { uint64_t h = x & 0xFFFF; return h | (h << 16) | (h << 32) | (h << 48); };
    switch (cmode >> 1) {
        case 0: v = rep32(imm8); break;
        case 1: v = rep32(imm8 << 8); break;
        case 2: v = rep32(imm8 << 16); break;
        case 3: v = rep32(imm8 << 24); break;
        case 4: v = rep16(imm8); break;
        case 5: v = rep16(imm8 << 8); break;
        case 6: v = rep32((cmode & 1) ? ((imm8 << 16) | 0xFFFF) : ((imm8 << 8) | 0xFF)); break;
        default:
            if (!(cmode & 1)) {
                if (!op) { v = 0x0101010101010101ull * imm8; }
                else { for (int i = 0; i < 8; ++i) if ((imm8 >> i) & 1) v |= 0xFFull << (8 * i); }
            } else {
                if (op) return undefined(bb, insn);
                const uint32_t a = imm8 >> 7, b = (imm8 >> 6) & 1;
                const uint32_t f = (a << 31) | ((b ^ 1) << 30) | ((b ? 0x1Fu : 0u) << 25) | ((imm8 & 0x3F) << 19);
                v = rep32(f);
            }
    }
    uint32_t kind;                         // 0 MOV, 1 MVN, 2 ORR, 3 BIC
    if (!op) kind = (cmode < 12 && (cmode & 1)) ? 2 : 0;
    else {
        if (cmode == 14) kind = 0;
        else if (cmode < 12 && (cmode & 1)) kind = 3;
        else kind = 1;
    }
    bb.emit(&H_NImm, d, 0, regs, kind, uint32_t(v), uint32_t(v >> 32));
    return R::Continue;
}

// ------------------------------------------ 2 registers + shift amount ---

R twoRegShift(BlockBuilder& bb, uint32_t insn) {
    const uint32_t L = bit(insn, 7), imm6 = bits(insn, 21, 16), A = bits(insn, 11, 8), U = bit(insn, 24), B = bit(insn, 6);
    const uint32_t d = regD(insn), m = regM(insn);
    uint32_t esize;
    if (L) esize = 64;
    else if (imm6 & 0x20) esize = 32;
    else if (imm6 & 0x10) esize = 16;
    else esize = 8;
    const uint32_t rshift = (L ? 64 : 2 * esize) - imm6;
    const uint32_t lshift = L ? imm6 : imm6 - esize;
    const uint32_t regs = B ? 2 : 1;
    auto emit = [&](Handler h, uint32_t s) { bb.emit(h, d, 0, m, s, regs); return R::Continue; };
    switch (A) {
        case 0: return emit(pickShImm<kNShr>(esize, U), rshift);
        case 1: return emit(pickShImm<kNSra>(esize, U), rshift);
        case 2: return emit(pickShImm<kNRShr>(esize, U), rshift);
        case 3: return emit(pickShImm<kNRSra>(esize, U), rshift);
        case 4: if (!U) break; return emit(pickShImm<kNSri>(esize, true), rshift);
        case 5: return emit(U ? pickShImm<kNSli>(esize, true) : pickShImm<kNShl>(esize, true), lshift);
        case 6: if (!U) break; return emit(pickShImm<kNQShlU>(esize, false), lshift);
        case 7: return emit(pickShImm<kNQShl>(esize, U), lshift);
        case 8: case 9: {
            if (L) break;
            Handler h;
            if (A == 8 && !U) h = B ? pickNarrow<1>(esize, true) : pickNarrow<0>(esize, true);       // VRSHRN / VSHRN
            else if (A == 8) h = B ? pickNarrow<5>(esize, false) : pickNarrow<4>(esize, false);       // VQRSHRUN / VQSHRUN
            else h = B ? pickNarrow<3>(esize, U) : pickNarrow<2>(esize, U);                           // VQRSHRN / VQSHRN
            bb.emit(h, d, 0, m, rshift);
            return R::Continue;
        }
        case 10:
            if (L || B) break;
            bb.emit(pickWiden(esize, U), d, 0, m, lshift);                                         // VSHLL / VMOVL
            return R::Continue;
        case 14: case 15:
            if (!(imm6 & 0x20)) break;
            bb.emit(A == 15 ? (U ? &H_NCvtFixed<true, true> : &H_NCvtFixed<true, false>)
                            : (U ? &H_NCvtFixed<false, true> : &H_NCvtFixed<false, false>), d, 0, m, 64 - imm6, regs);
            return R::Continue;
    }
    return undefined(bb, insn);
}

// --------------------------------------------- 3 registers, different ---

R threeDiff(BlockBuilder& bb, uint32_t insn) {
    const uint32_t A = bits(insn, 11, 8), U = bit(insn, 24), size = bits(insn, 21, 20);
    const uint32_t d = regD(insn), n = regN(insn), m = regM(insn);
    auto emit = [&](Handler h) { bb.emit(h, d, n, m); return R::Continue; };
    auto wide = [&](bool sub) -> Handler {
        switch (size) {
            case 0: return U ? (sub ? &H_N3Wide<true, uint8_t>  : &H_N3Wide<false, uint8_t>)  : (sub ? &H_N3Wide<true, int8_t>  : &H_N3Wide<false, int8_t>);
            case 1: return U ? (sub ? &H_N3Wide<true, uint16_t> : &H_N3Wide<false, uint16_t>) : (sub ? &H_N3Wide<true, int16_t> : &H_N3Wide<false, int16_t>);
            default: return U ? (sub ? &H_N3Wide<true, uint32_t> : &H_N3Wide<false, uint32_t>) : (sub ? &H_N3Wide<true, int32_t> : &H_N3Wide<false, int32_t>);
        }
    };
    auto high = [&](bool sub, bool rnd) -> Handler {
        switch (size) {
            case 0: return sub ? (rnd ? &H_N3NarrowHigh<uint16_t, true, true> : &H_N3NarrowHigh<uint16_t, true, false>)
                               : (rnd ? &H_N3NarrowHigh<uint16_t, false, true> : &H_N3NarrowHigh<uint16_t, false, false>);
            case 1: return sub ? (rnd ? &H_N3NarrowHigh<uint32_t, true, true> : &H_N3NarrowHigh<uint32_t, true, false>)
                               : (rnd ? &H_N3NarrowHigh<uint32_t, false, true> : &H_N3NarrowHigh<uint32_t, false, false>);
            default: return sub ? (rnd ? &H_N3NarrowHigh<uint64_t, true, true> : &H_N3NarrowHigh<uint64_t, true, false>)
                                : (rnd ? &H_N3NarrowHigh<uint64_t, false, true> : &H_N3NarrowHigh<uint64_t, false, false>);
        }
    };
    switch (A) {
        case 0: return emit(pickLong<kNAddL>(size, U));
        case 1: return emit(wide(false));
        case 2: return emit(pickLong<kNSubL>(size, U));
        case 3: return emit(wide(true));
        case 4: return emit(high(false, U));
        case 5: return emit(pickLong<kNAbaL>(size, U));
        case 6: return emit(high(true, U));
        case 7: return emit(pickLong<kNAbdL>(size, U));
        case 8: return emit(pickLong<kNMlaL>(size, U));
        case 10: return emit(pickLong<kNMlsL>(size, U));
        case 9:  if (U || size == 0) break; return emit(pickLong<kNQDMlaL>(size, false));
        case 11: if (U || size == 0) break; return emit(pickLong<kNQDMlsL>(size, false));
        case 12: return emit(pickLong<kNMulL>(size, U));
        case 13: if (U || size == 0) break; return emit(pickLong<kNQDMulL>(size, false));
        case 14: if (U || size != 0) break; return emit(&H_N3Long<kNPMulL, uint8_t>);
    }
    return undefined(bb, insn);
}

// --------------------------------------------- 2 registers + scalar ---

R twoRegScalar(BlockBuilder& bb, uint32_t insn) {
    const uint32_t A = bits(insn, 11, 8), size = bits(insn, 21, 20), Qu = bit(insn, 24);
    const uint32_t d = regD(insn), n = regN(insn);
    if (size == 0 || size == 3) return undefined(bb, insn);
    const uint32_t vm = bits(insn, 3, 0), M = bit(insn, 5);
    uint32_t dm, index;
    if (size == 1) { dm = vm & 7; index = (M << 1) | (vm >> 3); }
    else { dm = vm; index = M; }
    const uint32_t ebytes = size == 1 ? 2 : 4;
    const bool longOp = A == 2 || A == 3 || A == 6 || A == 7 || A == 10 || A == 11;
    const uint32_t regs = longOp ? 1 : (Qu ? 2 : 1);
    // Broadcast the scalar into a scratch register, then use the vector form.
    bb.emit(&H_NDupLane, kDTmp0, 0, dm, ebytes, regs, index);
    auto emit = [&](Handler h) { bb.emit(h, d, n, kDTmp0, 0, regs); return R::Continue; };
    const bool F = bit(insn, 8);
    switch (A) {
        case 0: case 1:
            if (F) { if (size != 2) break; return emit(&H_N3<FMla, float>); }
            return emit(pickInt<NMla>(size, true));
        case 4: case 5:
            if (F) { if (size != 2) break; return emit(&H_N3<FMls, float>); }
            return emit(pickInt<NMls>(size, true));
        case 8: case 9:
            if (F) { if (size != 2) break; return emit(&H_N3<FMul, float>); }
            return emit(pickInt<NMul>(size, true));
        case 2:  return emit(pickLong<kNMlaL>(size, Qu));
        case 6:  return emit(pickLong<kNMlsL>(size, Qu));
        case 10: return emit(pickLong<kNMulL>(size, Qu));
        case 3:  if (Qu) break; return emit(pickLong<kNQDMlaL>(size, false));
        case 7:  if (Qu) break; return emit(pickLong<kNQDMlsL>(size, false));
        case 11: if (Qu) break; return emit(pickLong<kNQDMulL>(size, false));
        case 12: return emit(pickInt<NQDMulh>(size, false));
        case 13: return emit(pickInt<NQRDMulh>(size, false));
    }
    return undefined(bb, insn);
}

// -------------------------------------------------- 2 registers, misc ---

R twoRegMisc(BlockBuilder& bb, uint32_t insn) {
    const uint32_t size = bits(insn, 19, 18), A = bits(insn, 17, 16), B = bits(insn, 10, 6);
    const uint32_t regs = bit(insn, 6) ? 2 : 1;
    const uint32_t d = regD(insn), m = regM(insn);
    auto emit = [&](Handler h) { bb.emit(h, d, 0, m, 0, regs); return R::Continue; };
    const uint32_t ebytes = 1u << size;
    switch (A) {
        case 0: {
            const uint32_t b4 = B >> 1;
            if (b4 <= 2) {                                         // VREV64 / VREV32 / VREV16
                const uint32_t group = b4 == 0 ? 8 : (b4 == 1 ? 4 : 2);
                if (ebytes >= group) break;
                bb.emit(&H_NRev, d, 0, m, ebytes, regs, group);
                return R::Continue;
            }
            if ((B >> 2) == 2 || (B >> 2) == 6) {                  // VPADDL / VPADAL
                if (size == 3) break;
                const bool uns = bit(insn, 7), acc = (B >> 2) == 6;
                Handler h;
                switch (size) {
                    case 0: h = uns ? (acc ? &H_NPairLong<uint8_t, true>  : &H_NPairLong<uint8_t, false>)  : (acc ? &H_NPairLong<int8_t, true>  : &H_NPairLong<int8_t, false>); break;
                    case 1: h = uns ? (acc ? &H_NPairLong<uint16_t, true> : &H_NPairLong<uint16_t, false>) : (acc ? &H_NPairLong<int16_t, true> : &H_NPairLong<int16_t, false>); break;
                    default: h = uns ? (acc ? &H_NPairLong<uint32_t, true> : &H_NPairLong<uint32_t, false>) : (acc ? &H_NPairLong<int32_t, true> : &H_NPairLong<int32_t, false>); break;
                }
                return emit(h);
            }
            switch (b4) {
                case 8:  if (size == 3) break; return emit(pickUnary<UCls>(size, false));
                case 9:  if (size == 3) break; return emit(pickUnary<UClz>(size, true));
                case 10: if (size != 0) break; return emit(&H_NUnary<UCnt, uint8_t>);
                case 11: if (size != 0) break; return emit(&H_NUnary<UMvn, uint64_t>);
                case 14: if (size == 3) break; return emit(pickUnary<UQAbs>(size, false));
                case 15: if (size == 3) break; return emit(pickUnary<UQNeg>(size, false));
            }
            break;
        }
        case 1: {
            const bool F = bit(insn, 10);
            const uint32_t op = bits(insn, 9, 7);
            if (size == 3) break;
            if (F) {
                if (size != 2) break;
                switch (op) {
                    case 0: return emit(&H_NFCmp<2, true>);       // VCGT #0
                    case 1: return emit(&H_NFCmp<1, true>);       // VCGE #0
                    case 2: return emit(&H_NFCmp<0, true>);       // VCEQ #0
                    case 3: return emit(&H_NFCmp<5, true>);       // VCLE #0
                    case 4: return emit(&H_NFCmp<6, true>);       // VCLT #0
                    case 6: return emit(&H_NUnary<UFAbs, float>);
                    case 7: return emit(&H_NUnary<UFNeg, float>);
                }
                break;
            }
            switch (op) {
                case 0: return emit(pickUnary<UCgt0>(size, false));
                case 1: return emit(pickUnary<UCge0>(size, false));
                case 2: return emit(pickUnary<UCeq0>(size, false));
                case 3: return emit(pickUnary<UCle0>(size, false));
                case 4: return emit(pickUnary<UClt0>(size, false));
                case 6: return emit(pickUnary<UAbs>(size, false));
                case 7: return emit(pickUnary<UNeg>(size, false));
            }
            break;
        }
        case 2: {
            const uint32_t b4 = B >> 1;
            if (b4 <= 3) {                                          // VSWP / VTRN / VUZP / VZIP
                if (b4 == 0 && size != 0) break;
                if (b4 >= 2 && regs == 1 && size == 2) break;       // 32-bit VUZP/VZIP on D regs = VTRN, UNDEF
                bb.emit(&H_NPermute, d, 0, m, ebytes, regs, b4);
                return R::Continue;
            }
            if (b4 == 4 || b4 == 5) {                               // VMOVN / VQMOVUN / VQMOVN
                if (size == 3) break;
                const uint32_t esize = 8u << size, opv = bits(insn, 7, 6);
                Handler h;
                if (opv == 0) h = pickNarrow<0>(esize, true);
                else if (opv == 1) h = pickNarrow<4>(esize, false);
                else h = pickNarrow<2>(esize, opv == 3);
                bb.emit(h, d, 0, m, 0);
                return R::Continue;
            }
            if ((B & 0x1B) == 0x18) {                               // VCVT between f16 and f32
                if (size != 1) break;
                bb.emit(bit(insn, 8) ? &H_NCvtHalf<false> : &H_NCvtHalf<true>, d, 0, m);
                return R::Continue;
            }
            if (B == 12) {                                          // VSHLL #esize
                if (size == 3) break;
                bb.emit(pickWiden(8u << size, false), d, 0, m, 8u << size);
                return R::Continue;
            }
            break;
        }
        case 3: {
            if ((B & 0x18) == 0x10) {                               // VRECPE / VRSQRTE
                if (size != 2) break;
                const bool F = bit(insn, 8), rsq = bit(insn, 7);
                static constexpr Handler t[2][2] = { { &H_NEstimate<0>, &H_NEstimate<1> }, { &H_NEstimate<2>, &H_NEstimate<3> } };
                return emit(t[F][rsq]);
            }
            if ((B & 0x18) == 0x18) {                               // VCVT f32 <-> s32/u32
                if (size != 2) break;
                const uint32_t op = bits(insn, 8, 7);
                Handler h = op == 0 ? &H_NCvtFixed<false, false> : op == 1 ? &H_NCvtFixed<false, true>
                          : op == 2 ? &H_NCvtFixed<true, false> : &H_NCvtFixed<true, true>;
                bb.emit(h, d, 0, m, 0, regs);
                return R::Continue;
            }
            break;
        }
    }
    return undefined(bb, insn);
}

} // namespace

DecodeResult decodeNeonDataProcessing(BlockBuilder& bb, uint32_t insn) {
    if (!bit(insn, 23)) return threeSame(bb, insn);
    if (bit(insn, 4)) {
        if (bits(insn, 21, 19) == 0 && !bit(insn, 7)) return oneRegImm(bb, insn);
        return twoRegShift(bb, insn);
    }
    if (bits(insn, 21, 20) != 3) return bit(insn, 6) ? twoRegScalar(bb, insn) : threeDiff(bb, insn);
    if (!bit(insn, 24)) {                                         // VEXT
        const uint32_t regs = bit(insn, 6) ? 2 : 1, imm4 = bits(insn, 11, 8);
        if (regs == 1 && imm4 > 7) return undefined(bb, insn);
        bb.emit(&H_NExt, regD(insn), regN(insn), regM(insn), 0, imm4, regs);
        return R::Continue;
    }
    if (!bit(insn, 11)) return twoRegMisc(bb, insn);
    if (bits(insn, 11, 10) == 2) {                                // VTBL / VTBX
        bb.emit(&H_NTbl, regD(insn), regN(insn), regM(insn), bit(insn, 6), bits(insn, 9, 8) + 1);
        return R::Continue;
    }
    if (bits(insn, 11, 7) == 0x18) {                              // VDUP (scalar)
        const uint32_t imm4 = bits(insn, 19, 16), regs = bit(insn, 6) ? 2 : 1;
        uint32_t e, idx;
        if (imm4 & 1) { e = 1; idx = imm4 >> 1; }
        else if (imm4 & 2) { e = 2; idx = imm4 >> 2; }
        else if (imm4 & 4) { e = 4; idx = imm4 >> 3; }
        else return undefined(bb, insn);
        bb.emit(&H_NDupLane, regD(insn), 0, regM(insn), e, regs, idx);
        return R::Continue;
    }
    return undefined(bb, insn);
}

DecodeResult decodeNeonCoreTransfer(BlockBuilder& bb, uint32_t insn) {
    const uint32_t rt = bits(insn, 15, 12), dreg = (bit(insn, 7) << 4) | bits(insn, 19, 16);
    const uint32_t opc1 = bits(insn, 22, 21), opc2 = bits(insn, 6, 5);
    auto lane = [&](uint32_t& e, uint32_t& idx) {
        if (opc1 & 2) { e = 1; idx = ((opc1 & 1) << 2) | opc2; return true; }
        if (opc2 & 1) { e = 2; idx = ((opc1 & 1) << 1) | (opc2 >> 1); return true; }
        if (opc2 == 0) { e = 4; idx = opc1 & 1; return true; }
        return false;
    };
    if (!bit(insn, 20)) {
        if (!bit(insn, 23)) {                                     // VMOV Dd[x], Rt
            uint32_t e, idx;
            if (!lane(e, idx)) return undefined(bb, insn);
            bb.emit(&H_NMovToLane, dreg, bb.reg(rt), 0, e, idx);
            return R::Continue;
        }
        const uint32_t be = (bit(insn, 22) << 1) | bit(insn, 5);  // VDUP Dd, Rt
        const uint32_t e = be == 2 ? 1 : be == 1 ? 2 : be == 0 ? 4 : 0;
        if (!e) return undefined(bb, insn);
        bb.emit(&H_NDupCore, dreg, bb.reg(rt), 0, e, bit(insn, 21) ? 2 : 1);
        return R::Continue;
    }
    uint32_t e, idx;                                              // VMOV Rt, Dn[x]
    if (!lane(e, idx)) return undefined(bb, insn);
    bb.emit(&H_NMovFromLane, rt, dreg, bit(insn, 23), e, idx);
    return R::Continue;
}

DecodeResult decodeNeonLoadStore(BlockBuilder& bb, uint32_t insn) {
    const uint32_t A = bit(insn, 23), L = bit(insn, 21), rn = bits(insn, 19, 16), rm = bits(insn, 3, 0);
    const uint32_t d = regD(insn), B = bits(insn, 11, 8);
    if (rn == 15) return undefined(bb, insn);
    if (!A) {
        uint32_t members, regs, inc = 1;
        switch (B) {
            case 7:  members = 1; regs = 1; break;
            case 10: members = 1; regs = 2; break;
            case 6:  members = 1; regs = 3; break;
            case 2:  members = 1; regs = 4; break;
            case 8:  members = 2; regs = 1; inc = 1; break;
            case 9:  members = 2; regs = 1; inc = 2; break;
            case 3:  members = 2; regs = 2; inc = 2; break;
            case 4:  members = 3; regs = 1; inc = 1; break;
            case 5:  members = 3; regs = 1; inc = 2; break;
            case 0:  members = 4; regs = 1; inc = 1; break;
            case 1:  members = 4; regs = 1; inc = 2; break;
            default: return undefined(bb, insn);
        }
        const uint32_t size = bits(insn, 7, 6);
        if (size == 3 && members > 1) return undefined(bb, insn);
        bb.emit(L ? &H_NLdStMulti<true> : &H_NLdStMulti<false>, d, rn, rm, size, members | (regs << 4) | (inc << 8));
        return R::Continue;
    }
    const uint32_t members = (B & 3) + 1;
    if ((B >> 2) == 3) {                                          // VLDn (all lanes)
        if (!L) return undefined(bb, insn);
        uint32_t size = bits(insn, 7, 6);
        const uint32_t T = bit(insn, 5);
        if (size == 3) {                                          // VLD4.32 {..[]} with 128-bit alignment
            if (members != 4 || !bit(insn, 4)) return undefined(bb, insn);
            size = 2;
        }
        const uint32_t regs = members == 1 ? (T ? 2 : 1) : 1, inc = members == 1 ? 1 : (T ? 2 : 1);
        bb.emit(&H_NLdStLane<true>, d, rn, rm, size, members | (regs << 4) | (inc << 8) | (1u << 16));
        return R::Continue;
    }
    const uint32_t size = bits(insn, 11, 10), ia = bits(insn, 7, 4);
    uint32_t index, inc = 1;
    if (size == 0) index = ia >> 1;
    else if (size == 1) { index = ia >> 2; inc = (ia & 2) ? 2 : 1; }
    else { index = ia >> 3; inc = (ia & 4) ? 2 : 1; }
    bb.emit(L ? &H_NLdStLane<true> : &H_NLdStLane<false>, d, rn, rm, size, members | (inc << 8) | (index << 12));
    return R::Continue;
}

} // namespace Hydrogen::CPU
