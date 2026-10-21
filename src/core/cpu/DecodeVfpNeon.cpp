// HydrogenVM — VFP (A7.5–A7.9) and Advanced SIMD (A7.4, A7.7) decoding.
#include "Decoder.hpp"
#include "FpHandlers.hpp"

namespace Hydrogen::CPU {

DecodeResult decodeNeonDataProcessing(BlockBuilder& bb, uint32_t insn);   // DecodeNeon.cpp
DecodeResult decodeNeonLoadStore(BlockBuilder& bb, uint32_t insn);
DecodeResult decodeNeonCoreTransfer(BlockBuilder& bb, uint32_t insn);

namespace {

using R = DecodeResult;

inline uint32_t bits(uint32_t v, int hi, int lo) { return (v >> lo) & ((2u << (hi - lo)) - 1); }
inline uint32_t bit(uint32_t v, int b) { return (v >> b) & 1; }

R undefined(BlockBuilder& bb, uint32_t insn) { bb.exitSimple(&H_Undefined, insn); return R::End; }

inline uint32_t sReg(uint32_t v, uint32_t b) { return (v << 1) | b; }
inline uint32_t dReg(uint32_t v, uint32_t b) { return (b << 4) | v; }

// VFPExpandImm
inline void vfpExpandImm(uint32_t imm8, bool dp, uint32_t& lo, uint32_t& hi) {
    const uint32_t a = imm8 >> 7, b = (imm8 >> 6) & 1, cdefgh = imm8 & 0x3F;
    if (!dp) {
        lo = (a << 31) | ((b ^ 1) << 30) | ((b ? 0x1Fu : 0u) << 25) | (cdefgh << 19);
        hi = 0;
    } else {
        lo = 0;
        hi = (a << 31) | ((b ^ 1) << 30) | ((b ? 0xFFu : 0u) << 22) | (cdefgh << 16);
    }
}

template <int OP>
void emitVfp3(BlockBuilder& bb, bool dp, uint32_t d, uint32_t n, uint32_t m) {
    bb.emit(dp ? &H_Vfp3<OP, true> : &H_Vfp3<OP, false>, d, n, m);
}

R vfpDataProcessing(BlockBuilder& bb, uint32_t insn) {
    const bool dp = bit(insn, 8);
    const uint32_t D = bit(insn, 22), N = bit(insn, 7), M = bit(insn, 5);
    const uint32_t Vd = bits(insn, 15, 12), Vn = bits(insn, 19, 16), Vm = bits(insn, 3, 0);
    const uint32_t d = dp ? dReg(Vd, D) : sReg(Vd, D);
    const uint32_t n = dp ? dReg(Vn, N) : sReg(Vn, N);
    const uint32_t m = dp ? dReg(Vm, M) : sReg(Vm, M);
    const uint32_t opc1 = (bit(insn, 23) << 2) | bits(insn, 21, 20);
    const bool op6 = bit(insn, 6);
    switch (opc1) {
        case 0: op6 ? emitVfp3<kVMls>(bb, dp, d, n, m) : emitVfp3<kVMla>(bb, dp, d, n, m); return R::Continue;
        case 1: op6 ? emitVfp3<kVNMla>(bb, dp, d, n, m) : emitVfp3<kVNMls>(bb, dp, d, n, m); return R::Continue;
        case 2: op6 ? emitVfp3<kVNMul>(bb, dp, d, n, m) : emitVfp3<kVMul>(bb, dp, d, n, m); return R::Continue;
        case 3: op6 ? emitVfp3<kVSub>(bb, dp, d, n, m) : emitVfp3<kVAdd>(bb, dp, d, n, m); return R::Continue;
        case 4: if (op6) break; emitVfp3<kVDiv>(bb, dp, d, n, m); return R::Continue;
        case 5: op6 ? emitVfp3<kVFNma>(bb, dp, d, n, m) : emitVfp3<kVFNms>(bb, dp, d, n, m); return R::Continue;
        case 6: op6 ? emitVfp3<kVFms>(bb, dp, d, n, m) : emitVfp3<kVFma>(bb, dp, d, n, m); return R::Continue;
        default: break;
    }
    if (opc1 != 7) return undefined(bb, insn);

    const uint32_t opc2 = bits(insn, 19, 16), opc3 = bits(insn, 7, 6);
    if ((opc3 & 1) == 0) {                                    // VMOV (immediate)
        uint32_t lo, hi;
        vfpExpandImm((bits(insn, 19, 16) << 4) | bits(insn, 3, 0), dp, lo, hi);
        bb.emit(dp ? &H_VfpMovImm<true> : &H_VfpMovImm<false>, d, 0, 0, 0, lo, hi);
        return R::Continue;
    }
    switch (opc2) {
        case 0:
            if (opc3 == 1) bb.emit(dp ? &H_Vfp2<kVMov, true> : &H_Vfp2<kVMov, false>, d, 0, m);
            else bb.emit(dp ? &H_Vfp2<kVAbs, true> : &H_Vfp2<kVAbs, false>, d, 0, m);
            return R::Continue;
        case 1:
            if (opc3 == 1) bb.emit(dp ? &H_Vfp2<kVNeg, true> : &H_Vfp2<kVNeg, false>, d, 0, m);
            else bb.emit(dp ? &H_Vfp2<kVSqrt, true> : &H_Vfp2<kVSqrt, false>, d, 0, m);
            return R::Continue;
        case 2: case 3: {                                          // VCVTB / VCVTT (half precision)
            if (dp) break;
            const uint32_t sd = sReg(Vd, D), sm = sReg(Vm, M);
            bb.emit(bit(insn, 16) ? &H_VfpCvtHalf<true> : &H_VfpCvtHalf<false>, sd, 0, sm, bit(insn, 7));
            return R::Continue;
        }
        case 4: bb.emit(dp ? &H_VfpCmp<true, false> : &H_VfpCmp<false, false>, d, 0, m); return R::Continue;
        case 5: bb.emit(dp ? &H_VfpCmp<true, true> : &H_VfpCmp<false, true>, d); return R::Continue;
        case 7:
            if (opc3 != 3) break;
            if (dp) bb.emit(&H_VfpCvtSD<false>, sReg(Vd, D), 0, m);       // VCVT.F32.F64 Sd, Dm
            else    bb.emit(&H_VfpCvtSD<true>, dReg(Vd, D), 0, m);        // VCVT.F64.F32 Dd, Sm
            return R::Continue;
        case 8: {                                                  // VCVT int → fp (source is always S)
            const uint32_t sm = sReg(Vm, M);
            const bool sgn = bit(insn, 7);
            static constexpr Handler t[2][2] = { { &H_VfpCvtFromInt<false, false>, &H_VfpCvtFromInt<false, true> },
                                                 { &H_VfpCvtFromInt<true, false>,  &H_VfpCvtFromInt<true, true> } };
            bb.emit(t[dp][sgn], d, 0, sm);
            return R::Continue;
        }
        case 12: case 13: {                                        // VCVT{R} fp → int (dest is always S)
            const bool sgn = bit(insn, 16), rz = bit(insn, 7);
            static constexpr Handler t[2][2][2] = {
                { { &H_VfpCvtToInt<false, false, false>, &H_VfpCvtToInt<false, false, true> },
                  { &H_VfpCvtToInt<false, true, false>,  &H_VfpCvtToInt<false, true, true> } },
                { { &H_VfpCvtToInt<true, false, false>,  &H_VfpCvtToInt<true, false, true> },
                  { &H_VfpCvtToInt<true, true, false>,   &H_VfpCvtToInt<true, true, true> } } };
            bb.emit(t[dp][sgn][rz], sReg(Vd, D), 0, m);
            return R::Continue;
        }
        case 10: case 11: case 14: case 15: {                      // VCVT fp <-> fixed (in place)
            const bool toFixed = bit(insn, 18), uns = bit(insn, 16), sx = bit(insn, 7);
            const uint32_t size = sx ? 32 : 16;
            const uint32_t imm = (bits(insn, 3, 0) << 1) | bit(insn, 5);
            if (imm > size) return undefined(bb, insn);
            const uint32_t fbits = size - imm;
#define HVM_FIX(DP_, TF_) { &H_VfpCvtFixed<DP_, TF_, false, false>, &H_VfpCvtFixed<DP_, TF_, false, true>, \
                            &H_VfpCvtFixed<DP_, TF_, true, false>,  &H_VfpCvtFixed<DP_, TF_, true, true> }
            static constexpr Handler t[2][2][4] = { { HVM_FIX(false, false), HVM_FIX(false, true) },
                                                    { HVM_FIX(true, false),  HVM_FIX(true, true) } };
#undef HVM_FIX
            bb.emit(t[dp][toFixed][(uns << 1) | sx], d, 0, 0, 0, fbits);
            return R::Continue;
        }
        default: break;
    }
    return undefined(bb, insn);
}

R vfpCoreTransfer(BlockBuilder& bb, uint32_t insn) {
    const uint32_t L = bit(insn, 20), C = bit(insn, 8), A = bits(insn, 23, 21);
    const uint32_t rt = bits(insn, 15, 12);
    if (C) return decodeNeonCoreTransfer(bb, insn);
    if (A == 0) {                                                 // VMOV Sn <-> Rt
        const uint32_t sn = sReg(bits(insn, 19, 16), bit(insn, 7));
        if (L) bb.emit(&H_VmovFromS, rt, sn);
        else bb.emit(&H_VmovToS, sn, bb.reg(rt));
        return R::Continue;
    }
    if (A == 7) {
        const uint32_t reg = bits(insn, 19, 16);
        if (L) {                                                  // VMRS
            OpMeta mm;
            if (rt == 15) mm.writes = kLAll;
            bb.emit(&H_Vmrs, rt, 0, 0, 0, reg, 0, mm);
            return R::Continue;
        }
        if (reg == 1) { bb.emit(&H_Vmsr, bb.reg(rt)); return R::Continue; }   // VMSR FPSCR
        return R::Continue;                                      // FPEXC etc.: privileged, ignore
    }
    return undefined(bb, insn);
}

R vfp64Transfer(BlockBuilder& bb, uint32_t insn) {
    const uint32_t toCore = bit(insn, 20), dpForm = bit(insn, 8);
    const uint32_t rt = bits(insn, 15, 12), rt2 = bits(insn, 19, 16);
    const uint32_t w = dpForm ? 2 * dReg(bits(insn, 3, 0), bit(insn, 5)) : sReg(bits(insn, 3, 0), bit(insn, 5));
    if (toCore) bb.emit(&H_Vmov2FromS, w, rt, rt2);
    else bb.emit(&H_Vmov2ToS, w, bb.reg(rt), bb.reg(rt2));
    return R::Continue;
}

R vfpLoadStore(BlockBuilder& bb, uint32_t insn) {
    const uint32_t P = bit(insn, 24), U = bit(insn, 23), D = bit(insn, 22), W = bit(insn, 21), L = bit(insn, 20);
    const uint32_t rn = bits(insn, 19, 16), Vd = bits(insn, 15, 12), imm8 = bits(insn, 7, 0);
    const bool dp = bit(insn, 8);
    const uint32_t first = dp ? 2 * dReg(Vd, D) : sReg(Vd, D);
    const uint32_t base = rn == 15 ? bb.usePC(bb.pcRead() & ~3u) : rn;
    if (P && !W) {                                                // VLDR / VSTR
        const uint32_t off = imm8 * 4;
        bb.emit(L ? &H_VLdrStr<true> : &H_VLdrStr<false>, first, base, 0, dp ? 2 : 1, U ? off : 0u - off);
        return R::Continue;
    }
    if (P == U || rn == 15) return undefined(bb, insn);
    uint32_t words = imm8;
    if (dp && (words & 1)) words -= 1;                            // FLDMX/FSTMX: ignore the format word
    if (!words) return undefined(bb, insn);
    bb.emit(L ? &H_VLdmStm<true> : &H_VLdmStm<false>, first, rn, 0, U | (W << 2), words);
    return R::Continue;
}

} // namespace

DecodeResult decodeArmVfp(BlockBuilder& bb, uint32_t insn) {
    if ((insn & 0x0FE00000u) == 0x0C400000u) return vfp64Transfer(bb, insn);
    if (bits(insn, 27, 25) == 6) return vfpLoadStore(bb, insn);
    if (bits(insn, 27, 24) == 0xE) return bit(insn, 4) ? vfpCoreTransfer(bb, insn) : vfpDataProcessing(bb, insn);
    return undefined(bb, insn);
}

DecodeResult decodeArmNeon(BlockBuilder& bb, uint32_t insn) {
    if (bits(insn, 27, 25) == 1) return decodeNeonDataProcessing(bb, insn);
    return decodeNeonLoadStore(bb, insn);
}

// Thumb encodings of VFP/NEON are the ARM ones with a different prefix:
//   VFP:            1110 11xx …   (identical to ARM cond=AL)
//   NEON data:      111U 1111 …   ↔ ARM 1111 001U …
//   NEON load/store 1111 1001 …   ↔ ARM 1111 0100 …
DecodeResult decodeThumbVfpNeon(BlockBuilder& bb, uint32_t insn) {
    if ((insn & 0xEF000000u) == 0xEF000000u)
        return decodeNeonDataProcessing(bb, (insn & 0x00FFFFFFu) | 0xF2000000u | (bit(insn, 28) << 24));
    if ((insn & 0xFF100000u) == 0xF9000000u)
        return decodeNeonLoadStore(bb, (insn & 0x00FFFFFFu) | 0xF4000000u);
    if ((insn >> 28) == 0xE) {
        const uint32_t cp = bits(insn, 11, 8);
        if (cp == 15 && bits(insn, 27, 24) == 0xE && bit(insn, 4)) {
            if (bits(insn, 23, 21) == 0 && bits(insn, 19, 16) == 13 && bits(insn, 3, 0) == 0) {
                const uint32_t opc2 = bits(insn, 7, 5), rt = bits(insn, 15, 12);
                if (bit(insn, 20) && (opc2 == 2 || opc2 == 3)) { bb.emit(&H_ReadTpid, rt, 0, 0, 0, opc2); return DecodeResult::Continue; }
                if (!bit(insn, 20) && opc2 == 2) { bb.emit(&H_WriteTpid, rt); return DecodeResult::Continue; }
            }
            if (!bit(insn, 20) && bits(insn, 19, 16) == 7) { bb.emit(&H_Fence); return DecodeResult::Continue; }
        }
        if (cp == 10 || cp == 11) return decodeArmVfp(bb, insn);
    }
    return undefined(bb, insn);
}

} // namespace Hydrogen::CPU
