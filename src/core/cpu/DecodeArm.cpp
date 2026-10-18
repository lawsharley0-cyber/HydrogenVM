// HydrogenVM — ARM (A32) instruction decoder → IR.
// Encoding references are to the ARMv7-A/R Architecture Reference Manual, chapter A5.
#include "Decoder.hpp"

namespace Hydrogen::CPU {
namespace {

inline uint32_t bits(uint32_t v, int hi, int lo) { return (v >> lo) & ((2u << (hi - lo)) - 1); }
inline uint32_t bit(uint32_t v, int b) { return (v >> b) & 1; }
inline int32_t sext(uint32_t v, int width) { return int32_t(v << (32 - width)) >> (32 - width); }

// DecodeImmShift for data processing (op2 kinds).
inline void immShiftKind(uint32_t type, uint32_t imm5, int& kind, uint32_t& amt) {
    switch (type) {
        case 0: kind = kOp2ShImmLSL; amt = imm5; break;
        case 1: kind = kOp2ShImmLSR; amt = imm5 ? imm5 : 32; break;
        case 2: kind = kOp2ShImmASR; amt = imm5 ? imm5 : 32; break;
        default:
            if (imm5 == 0) { kind = kOp2ShImmRRX; amt = 1; }
            else { kind = kOp2ShImmROR; amt = imm5; }
    }
}
// DecodeImmShift for addressing (ShiftType + amount).
inline void immShiftType(uint32_t type, uint32_t imm5, uint32_t& st, uint32_t& amt) {
    switch (type) {
        case 0: st = kLSL; amt = imm5; break;
        case 1: st = kLSR; amt = imm5 ? imm5 : 32; break;
        case 2: st = kASR; amt = imm5 ? imm5 : 32; break;
        default:
            if (imm5 == 0) { st = kRRX; amt = 1; } else { st = kROR; amt = imm5; }
    }
}

using R = DecodeResult;

R undefined(BlockBuilder& bb, uint32_t insn) {
    bb.exitSimple(&H_Undefined, insn);
    return R::End;
}

// Write to PC from an ALU or load: branch to r15 (BXWritePC in ARM state).
R pcWritten(BlockBuilder& bb) {
    bb.clobberPC();
    bb.exitIndirect(kPC, true);
    return R::End;
}

// ----------------------------------------------------------- data processing

R dataProcessing(BlockBuilder& bb, uint32_t insn, bool immForm) {
    const int opc = int(bits(insn, 24, 21));
    bool s = bit(insn, 20);
    const uint32_t rn = bits(insn, 19, 16), rd = bits(insn, 15, 12);
    const bool test = aluIsTest(opc);
    if (rd == 15 && !test) s = false;   // "S + PC" is an exception return; not valid in user mode

    uint32_t rnr = aluUsesRn(opc) ? bb.reg(rn) : 0;
    if (immForm) {
        const uint32_t rot = bits(insn, 11, 8) * 2;
        const uint32_t v = ror32(bits(insn, 7, 0), rot);
        const uint32_t carry = rot ? (v >> 31) + 1 : 0;
        emitAlu(bb, opc, s, kOp2Imm, rd, rnr, 0, carry, v);
    } else if (!bit(insn, 4)) {
        int kind; uint32_t amt;
        immShiftKind(bits(insn, 6, 5), bits(insn, 11, 7), kind, amt);
        emitAlu(bb, opc, s, kind, rd, rnr, bb.reg(bits(insn, 3, 0)), 0, amt);
    } else {
        const int kind = kOp2ShRegLSL + int(bits(insn, 6, 5));
        emitAlu(bb, opc, s, kind, rd, rnr, bb.reg(bits(insn, 3, 0)), bb.reg(bits(insn, 11, 8)), 0);
    }
    if (rd == 15 && !test) return pcWritten(bb);
    return R::Continue;
}

// -------------------------------------------------------------- multiplies

R multiply(BlockBuilder& bb, uint32_t insn) {
    const uint32_t op = bits(insn, 23, 20);
    const bool s = bit(insn, 20);
    const uint32_t hi = bits(insn, 19, 16), lo = bits(insn, 15, 12);
    const uint32_t rm = bits(insn, 11, 8), rn = bits(insn, 3, 0);
    OpMeta mm;
    if (s) mm.writes = kLN | kLZ;
    switch (op >> 1) {
        case 0: // MUL
            if (s) mm.noflags = &H_Mul<0, false>;
            bb.emit(s ? &H_Mul<0, true> : &H_Mul<0, false>, hi, rn, rm, 0, 0, 0, mm); return R::Continue;
        case 1: // MLA
            if (s) mm.noflags = &H_Mul<1, false>;
            bb.emit(s ? &H_Mul<1, true> : &H_Mul<1, false>, hi, rn, rm, lo, 0, 0, mm); return R::Continue;
        case 2: // UMAAL (0100)
            if (op != 4) return undefined(bb, insn);
            bb.emit(&H_MulLong<4, false>, lo, hi, rn, rm); return R::Continue;
        case 3: // MLS (0110)
            if (op != 6) return undefined(bb, insn);
            bb.emit(&H_Mul<2, false>, hi, rn, rm, lo); return R::Continue;
        case 4: if (s) mm.noflags = &H_MulLong<0, false>;
                bb.emit(s ? &H_MulLong<0, true> : &H_MulLong<0, false>, lo, hi, rn, rm, 0, 0, mm); return R::Continue;
        case 5: if (s) mm.noflags = &H_MulLong<1, false>;
                bb.emit(s ? &H_MulLong<1, true> : &H_MulLong<1, false>, lo, hi, rn, rm, 0, 0, mm); return R::Continue;
        case 6: if (s) mm.noflags = &H_MulLong<2, false>;
                bb.emit(s ? &H_MulLong<2, true> : &H_MulLong<2, false>, lo, hi, rn, rm, 0, 0, mm); return R::Continue;
        default: if (s) mm.noflags = &H_MulLong<3, false>;
                bb.emit(s ? &H_MulLong<3, true> : &H_MulLong<3, false>, lo, hi, rn, rm, 0, 0, mm); return R::Continue;
    }
}

R halfwordMultiply(BlockBuilder& bb, uint32_t insn) {
    const uint32_t op1 = bits(insn, 22, 21);
    const uint32_t rd = bits(insn, 19, 16), ra = bits(insn, 15, 12);
    const uint32_t rm = bits(insn, 11, 8), rn = bits(insn, 3, 0);
    const uint32_t x = bit(insn, 5), y = bit(insn, 6);
    const uint32_t sel = x | (y << 1);
    switch (op1) {
        case 0: bb.emit(&H_MulHalf<1>, rd, rn, rm, ra, sel); break;               // SMLAxy
        case 1: if (x) bb.emit(&H_MulHalf<2>, rd, rn, rm, 0, y << 1);             // SMULWy
                else   bb.emit(&H_MulHalf<3>, rd, rn, rm, ra, y << 1);            // SMLAWy
                break;
        case 2: bb.emit(&H_MulHalf<4>, ra, rn, rm, rd, sel); break;               // SMLALxy (lo=ra, hi=rd)
        default: bb.emit(&H_MulHalf<0>, rd, rn, rm, 0, sel); break;               // SMULxy
    }
    return R::Continue;
}

// ------------------------------------------------------------ misc (A5.2.12)

R miscellaneous(BlockBuilder& bb, uint32_t insn) {
    const uint32_t op2 = bits(insn, 6, 4), op = bits(insn, 22, 21);
    switch (op2) {
        case 0:
            if (op == 0 || op == 2) {             // MRS Rd, APSR
                if (bit(insn, 9) || bit(insn, 22)) return undefined(bb, insn); // banked / SPSR
                OpMeta mm; mm.reads = kLAll;
                bb.emit(&H_Mrs, bits(insn, 15, 12), 0, 0, 0, 0, 0, mm);
                return R::Continue;
            }
            if (op == 1) {                        // MSR APSR_<mask>, Rn
                const uint32_t mask = bits(insn, 19, 18);
                OpMeta mm; if (mask & 2) mm.writes = kLAll;
                bb.emit(&H_Msr, bb.reg(bits(insn, 3, 0)), 0, 0, (mask >> 1) | ((mask & 1) << 1), 0, 0, mm);
                return R::Continue;
            }
            return undefined(bb, insn);
        case 1:
            if (op == 1) { bb.exitIndirect(bb.reg(bits(insn, 3, 0)), true); return R::End; }   // BX
            if (op == 3) { bb.emit(&H_BitOp<0>, bits(insn, 15, 12), bb.reg(bits(insn, 3, 0))); return R::Continue; } // CLZ
            return undefined(bb, insn);
        case 2:
            if (op == 1) { bb.exitIndirect(bb.reg(bits(insn, 3, 0)), true); return R::End; }   // BXJ → BX
            return undefined(bb, insn);
        case 3:
            if (op == 1) {                        // BLX Rm
                bb.emit(&H_MovReg, kTmp0, bb.reg(bits(insn, 3, 0)));
                bb.emit(&H_MovImm, kLR, 0, 0, 0, bb.curPc() + 4);
                bb.exitIndirect(kTmp0, true);
                return R::End;
            }
            return undefined(bb, insn);
        case 5: {                                 // QADD/QSUB/QDADD/QDSUB
            const uint32_t rd = bits(insn, 15, 12), rn = bits(insn, 19, 16), rm = bits(insn, 3, 0);
            static constexpr Handler t[4] = { &H_QArith<0>, &H_QArith<1>, &H_QArith<2>, &H_QArith<3> };
            bb.emit(t[op], rd, rn, rm);
            return R::Continue;
        }
        case 7:
            if (op == 1) {                        // BKPT
                bb.exitSimple(&H_Breakpoint, (bits(insn, 19, 8) << 4) | bits(insn, 3, 0));
                return R::End;
            }
            return undefined(bb, insn);
        default:
            return undefined(bb, insn);
    }
}

// ---------------------------------------------------- synchronisation (A5.2.10)

R synchronization(BlockBuilder& bb, uint32_t insn) {
    const uint32_t op = bits(insn, 23, 20);
    const uint32_t rn = bits(insn, 19, 16), rt = bits(insn, 15, 12), rt2 = bits(insn, 3, 0);
    if ((op & 0xB) == 0) {                         // SWP / SWPB
        if (bit(insn, 22)) bb.emit(&H_Swp<true>, rt, rn, rt2);
        else bb.emit(&H_Swp<false>, rt, rn, rt2);
        return R::Continue;
    }
    switch (op) {
        case 0x8: bb.emit(&H_Strex<4>, rt, rn, rt2, 0, 0); return R::Continue;            // STREX Rd, Rt, [Rn]
        case 0x9: bb.emit(&H_Ldrex<4>, rt, rn, 0, 0, 0); return R::Continue;
        case 0xA: bb.emit(&H_Strex<8>, rt, rn, rt2, 0, 0, rt2 + 1); return R::Continue;   // STREXD
        case 0xB: bb.emit(&H_Ldrex<8>, rt, rn, 0, 0, 0, rt + 1); return R::Continue;
        case 0xC: bb.emit(&H_Strex<1>, rt, rn, rt2, 0, 0); return R::Continue;
        case 0xD: bb.emit(&H_Ldrex<1>, rt, rn, 0, 0, 0); return R::Continue;
        case 0xE: bb.emit(&H_Strex<2>, rt, rn, rt2, 0, 0); return R::Continue;
        case 0xF: bb.emit(&H_Ldrex<2>, rt, rn, 0, 0, 0); return R::Continue;
        default: return undefined(bb, insn);
    }
}

// --------------------------------------------------------- loads and stores

int addrMode(uint32_t p, uint32_t w) { return !p ? kAmPost : (w ? kAmPre : kAmOffset); }

R extraLoadStore(BlockBuilder& bb, uint32_t insn) {
    const uint32_t p = bit(insn, 24), u = bit(insn, 23), i = bit(insn, 22), w = bit(insn, 21), l = bit(insn, 20);
    const uint32_t rn = bits(insn, 19, 16), rt = bits(insn, 15, 12), rm = bits(insn, 3, 0);
    const uint32_t op2 = bits(insn, 6, 5);
    const int am = addrMode(p, w);
    const uint32_t imm8 = (bits(insn, 11, 8) << 4) | bits(insn, 3, 0);
    const uint32_t base = bb.reg(rn);

    if (op2 == 1 || l) {                          // STRH / LDRH / LDRSB / LDRSH
        int kind; bool load = l;
        if (op2 == 1) kind = kMemH;
        else if (op2 == 2) kind = kMemSB;
        else kind = kMemSH;
        if (!load && kind != kMemH) return undefined(bb, insn);
        const uint32_t src = load ? rt : bb.reg(rt);
        if (i) bb.emit(memHandler(kind, load, am, false), src, base, 0, 0, u ? imm8 : 0u - imm8);
        else   bb.emit(memHandler(kind, load, am, true), src, base, bb.reg(rm), kLSL | (u ? 0 : 0x80), 0);
        if (load && rt == 15) return pcWritten(bb);
        return R::Continue;
    }
    // LDRD (op2 = 2, L = 0) / STRD (op2 = 3, L = 0)
    const bool load = op2 == 2;
    if (rt & 1) return undefined(bb, insn);
    if (i) bb.emit(memDualHandler(load, am, false), rt, base, 0, 0, u ? imm8 : 0u - imm8, rt + 1);
    else   bb.emit(memDualHandler(load, am, true), rt, base, bb.reg(rm), u ? 0 : 0x80, 0, rt + 1);
    return R::Continue;
}

R loadStoreWordByte(BlockBuilder& bb, uint32_t insn) {
    const bool reg = bit(insn, 25);
    const uint32_t p = bit(insn, 24), u = bit(insn, 23), b = bit(insn, 22), w = bit(insn, 21), l = bit(insn, 20);
    const uint32_t rn = bits(insn, 19, 16), rt = bits(insn, 15, 12);
    const int am = addrMode(p, w);
    const int kind = b ? kMemB : kMemW;
    const uint32_t base = bb.reg(rn);
    const uint32_t src = l ? rt : bb.reg(rt);
    if (!reg) {
        const uint32_t imm12 = bits(insn, 11, 0);
        bb.emit(memHandler(kind, l, am, false), src, base, 0, 0, u ? imm12 : 0u - imm12);
    } else {
        uint32_t st, amt;
        immShiftType(bits(insn, 6, 5), bits(insn, 11, 7), st, amt);
        OpMeta mm; if (st == kRRX) mm.reads = kLC;
        bb.emit(memHandler(kind, l, am, true), src, base, bb.reg(bits(insn, 3, 0)), st | (u ? 0 : 0x80), amt, 0, mm);
    }
    if (l && rt == 15) return pcWritten(bb);
    return R::Continue;
}

R blockTransfer(BlockBuilder& bb, uint32_t insn) {
    const uint32_t list = bits(insn, 15, 0), rn = bits(insn, 19, 16);
    const bool load = bit(insn, 20);
    if (!list || rn == 15) return undefined(bb, insn);
    if (bit(insn, 22)) {
        // User-bank / exception-return forms. Only meaningful in privileged modes;
        // in user mode the plain LDM/STM is the closest sane behaviour.
    }
    const uint32_t mode = bit(insn, 23) | (bit(insn, 24) << 1) | (bit(insn, 21) << 2);
    if (!load && (list & 0x8000)) bb.usePC(bb.curPc() + 8);
    bb.emit(load ? &H_Multi<true> : &H_Multi<false>, 0, rn, 0, mode, list);
    if (load && (list & 0x8000)) return pcWritten(bb);
    return R::Continue;
}

// ----------------------------------------------------------- media (A5.4)

R packUnpackSatRev(BlockBuilder& bb, uint32_t insn) {
    const uint32_t op1 = bits(insn, 22, 20), op2 = bits(insn, 7, 5), a = bits(insn, 19, 16);
    const uint32_t rd = bits(insn, 15, 12), rm = bits(insn, 3, 0);
    const uint32_t rot = bits(insn, 11, 10) * 8;
    auto ext = [&](Handler plain, Handler add) {
        if (a == 15) bb.emit(plain, rd, 0, rm, 0, rot);
        else bb.emit(add, rd, a, rm, 0, rot);
        return R::Continue;
    };
    if (op1 == 0 && (op2 & 1) == 0) {               // PKHBT / PKHTB
        const uint32_t tb = bit(insn, 6), imm5 = bits(insn, 11, 7);
        bb.emit(&H_Pkh, rd, a, rm, tb, tb && imm5 == 0 ? 32 : imm5);
        return R::Continue;
    }
    if ((op1 & 6) == 2 && (op2 & 1) == 0) {         // SSAT
        const uint32_t sh = bit(insn, 6), imm5 = bits(insn, 11, 7);
        bb.emit(&H_Sat<0>, rd, 0, rm, sh ? kASR : kLSL, sh && imm5 == 0 ? 32 : imm5, bits(insn, 20, 16) + 1);
        return R::Continue;
    }
    if ((op1 & 6) == 6 && (op2 & 1) == 0) {         // USAT
        const uint32_t sh = bit(insn, 6), imm5 = bits(insn, 11, 7);
        bb.emit(&H_Sat<1>, rd, 0, rm, sh ? kASR : kLSL, sh && imm5 == 0 ? 32 : imm5, bits(insn, 20, 16));
        return R::Continue;
    }
    switch (op1) {
        case 0:
            if (op2 == 3) return ext(&H_Extend<4, false>, &H_Extend<4, true>);    // SXTB16 / SXTAB16
            if (op2 == 5) { bb.emit(&H_Sel, rd, a, rm); return R::Continue; }   // SEL
            break;
        case 2:
            if (op2 == 1) { bb.emit(&H_Sat<2>, rd, 0, rm, 0, 0, bits(insn, 19, 16) + 1); return R::Continue; } // SSAT16
            if (op2 == 3) return ext(&H_Extend<0, false>, &H_Extend<0, true>);    // SXTB / SXTAB
            break;
        case 3:
            if (op2 == 1) { bb.emit(&H_BitOp<2>, rd, rm); return R::Continue; } // REV
            if (op2 == 3) return ext(&H_Extend<1, false>, &H_Extend<1, true>);    // SXTH / SXTAH
            if (op2 == 5) { bb.emit(&H_BitOp<3>, rd, rm); return R::Continue; } // REV16
            break;
        case 4:
            if (op2 == 3) return ext(&H_Extend<5, false>, &H_Extend<5, true>);    // UXTB16 / UXTAB16
            break;
        case 6:
            if (op2 == 1) { bb.emit(&H_Sat<3>, rd, 0, rm, 0, 0, bits(insn, 19, 16)); return R::Continue; } // USAT16
            if (op2 == 3) return ext(&H_Extend<2, false>, &H_Extend<2, true>);    // UXTB / UXTAB
            break;
        case 7:
            if (op2 == 1) { bb.emit(&H_BitOp<1>, rd, rm); return R::Continue; } // RBIT
            if (op2 == 3) return ext(&H_Extend<3, false>, &H_Extend<3, true>);    // UXTH / UXTAH
            if (op2 == 5) { bb.emit(&H_BitOp<4>, rd, rm); return R::Continue; } // REVSH
            break;
    }
    return undefined(bb, insn);
}

R signedMultiplies(BlockBuilder& bb, uint32_t insn) {
    const uint32_t op1 = bits(insn, 22, 20), op2 = bits(insn, 7, 5);
    const uint32_t rd = bits(insn, 19, 16), ra = bits(insn, 15, 12), rm = bits(insn, 11, 8), rn = bits(insn, 3, 0);
    const uint32_t x = bit(insn, 5);
    switch (op1) {
        case 0:
            if (op2 < 2) { if (ra == 15) bb.emit(&H_MulDual<7>, rd, rn, rm, 0, x); else bb.emit(&H_MulDual<0>, rd, rn, rm, ra, x); return R::Continue; }
            if (op2 < 4) { if (ra == 15) bb.emit(&H_MulDual<8>, rd, rn, rm, 0, x); else bb.emit(&H_MulDual<1>, rd, rn, rm, ra, x); return R::Continue; }
            break;
        case 1: if (op2 == 0) { bb.emit(&H_Div<true>, rd, rn, rm); return R::Continue; } break;
        case 3: if (op2 == 0) { bb.emit(&H_Div<false>, rd, rn, rm); return R::Continue; } break;
        case 4:
            if (op2 < 2) { bb.emit(&H_MulDual<2>, ra, rn, rm, rd, x); return R::Continue; } // SMLALD lo=ra hi=rd
            if (op2 < 4) { bb.emit(&H_MulDual<3>, ra, rn, rm, rd, x); return R::Continue; } // SMLSLD
            break;
        case 5:
            if (op2 < 2) { if (ra == 15) bb.emit(&H_MulDual<6>, rd, rn, rm, 0, x); else bb.emit(&H_MulDual<4>, rd, rn, rm, ra, x); return R::Continue; }
            if (op2 >= 6) { bb.emit(&H_MulDual<5>, rd, rn, rm, ra, x); return R::Continue; }
            break;
    }
    return undefined(bb, insn);
}

R media(BlockBuilder& bb, uint32_t insn) {
    const uint32_t op1 = bits(insn, 24, 20), op2 = bits(insn, 7, 5);
    const uint32_t rd = bits(insn, 15, 12), rn = bits(insn, 19, 16), rm = bits(insn, 3, 0);
    if ((op1 & 0x18) == 0) {                        // parallel add/subtract
        const uint32_t pfx = bits(insn, 21, 20);
        if (pfx == 0 || op2 == 5 || op2 == 6) return undefined(bb, insn);
        const uint32_t prefix = (bit(insn, 22) ? 3 : 0) + pfx - 1;
        bb.emit(&H_Parallel, rd, rn, rm, prefix, op2);
        return R::Continue;
    }
    if ((op1 & 0x18) == 0x08) return packUnpackSatRev(bb, insn);
    if ((op1 & 0x18) == 0x10) return signedMultiplies(bb, insn);
    if (op1 == 0x18 && op2 == 0) {                  // USAD8 / USADA8
        bb.emit(&H_Usad8, rn, rm, bits(insn, 11, 8), rd == 15 ? 0xFF : rd);
        return R::Continue;
    }
    if ((op1 & 0x1E) == 0x1A && (op2 & 3) == 2) {   // SBFX
        bb.emit(&H_Bitfield<1>, rd, rm, 0, 0, bits(insn, 11, 7), bits(insn, 20, 16) + 1);
        return R::Continue;
    }
    if ((op1 & 0x1E) == 0x1E && (op2 & 3) == 2) {   // UBFX
        bb.emit(&H_Bitfield<0>, rd, rm, 0, 0, bits(insn, 11, 7), bits(insn, 20, 16) + 1);
        return R::Continue;
    }
    if ((op1 & 0x1E) == 0x1C && (op2 & 3) == 0) {   // BFC / BFI
        const uint32_t lsb = bits(insn, 11, 7), msb = bits(insn, 20, 16);
        if (msb < lsb) return undefined(bb, insn);
        if (rm == 15) bb.emit(&H_Bitfield<3>, rd, 0, 0, 0, lsb, msb - lsb + 1);
        else bb.emit(&H_Bitfield<2>, rd, rm, 0, 0, lsb, msb - lsb + 1);
        return R::Continue;
    }
    if (op1 == 0x1F && op2 == 7) {                  // UDF (permanently undefined) — used as a trap
        bb.exitSimple(&H_Breakpoint, (bits(insn, 19, 8) << 4) | bits(insn, 3, 0));
        return R::End;
    }
    return undefined(bb, insn);
}

// --------------------------------------------------------- coprocessor / SVC

R coprocessor(BlockBuilder& bb, uint32_t insn) {
    if (bits(insn, 27, 24) == 0xF) {               // SVC
        bb.exitSimple(&H_Svc, bb.curPc() + 4, bits(insn, 23, 0));
        return R::End;
    }
    const uint32_t cp = bits(insn, 11, 8);
    if (cp == 15 && bits(insn, 27, 24) == 0xE && bit(insn, 4) &&
        bits(insn, 23, 21) == 0 && bits(insn, 19, 16) == 13 && bits(insn, 3, 0) == 0) {
        const uint32_t opc2 = bits(insn, 7, 5), rt = bits(insn, 15, 12);
        if (bit(insn, 20) && (opc2 == 2 || opc2 == 3)) { bb.emit(&H_ReadTpid, rt, 0, 0, 0, opc2); return R::Continue; }
        if (!bit(insn, 20) && opc2 == 2) { bb.emit(&H_WriteTpid, rt); return R::Continue; }
    }
    // cp10/cp11 = VFP/Advanced SIMD (Phase 2).
    if (cp == 10 || cp == 11) return decodeArmVfp(bb, insn);
    return undefined(bb, insn);
}

R unconditional(BlockBuilder& bb, uint32_t insn) {
    if (bits(insn, 27, 25) == 5) {                  // BLX <imm>
        const int32_t off = sext(bits(insn, 23, 0), 24) * 4 + int32_t(bit(insn, 24) << 1);
        bb.emit(&H_MovImm, kLR, 0, 0, 0, bb.curPc() + 4);
        bb.exitDirect(bb.curPc() + 8 + uint32_t(off), 1);
        return R::End;
    }
    if ((insn & 0xFFFFFFF0u) == 0xF57FF010u) { bb.emit(&H_Clrex); return R::Continue; }      // CLREX
    if ((insn & 0xFFFFFF00u) == 0xF57FF000u) {                                               // DSB/DMB/ISB
        const uint32_t op = bits(insn, 7, 4);
        if (op == 4 || op == 5) { bb.emit(&H_Fence); return R::Continue; }
        if (op == 6) return R::Continue;
    }
    // PLD/PLDW/PLI (immediate and register forms): hints → no-op.
    if ((bits(insn, 27, 24) & 0xC) == 0x4 && bits(insn, 22, 20) == 5 && bits(insn, 15, 12) == 0xF) return R::Continue;
    if ((bits(insn, 27, 24) & 0xC) == 0x4 && bits(insn, 22, 20) == 1 && bits(insn, 15, 12) == 0xF) return R::Continue;
    // Advanced SIMD data processing / element load-store (Phase 2).
    if (bits(insn, 27, 25) == 1 || (bits(insn, 27, 24) == 4 && !bit(insn, 20)))
        return decodeArmNeon(bb, insn);
    return undefined(bb, insn);
}

R body(BlockBuilder& bb, uint32_t insn) {
    const uint32_t op1 = bits(insn, 27, 25);
    switch (op1) {
        case 0: case 1: {
            const bool immForm = op1 == 1;
            const uint32_t op = bits(insn, 24, 20);
            if (!immForm) {
                const uint32_t o2 = bits(insn, 7, 4);
                if ((o2 & 9) == 9) {                        // 1xx1: multiplies, sync, extra load/store
                    if (o2 == 9) return (op & 0x10) ? synchronization(bb, insn) : multiply(bb, insn);
                    return extraLoadStore(bb, insn);
                }
                if ((op & 0x19) == 0x10) {                  // 10xx0: misc / halfword multiply
                    if (o2 & 8) return halfwordMultiply(bb, insn);
                    return miscellaneous(bb, insn);
                }
                return dataProcessing(bb, insn, false);
            }
            if (op == 0x10) {                               // MOVW
                bb.emit(&H_MovImm, bits(insn, 15, 12), 0, 0, 0, (bits(insn, 19, 16) << 12) | bits(insn, 11, 0));
                return R::Continue;
            }
            if (op == 0x14) {                               // MOVT
                bb.emit(&H_Movt, bits(insn, 15, 12), 0, 0, 0, (bits(insn, 19, 16) << 12) | bits(insn, 11, 0));
                return R::Continue;
            }
            if ((op & 0x1B) == 0x12) {                      // MSR (imm) / hints
                const uint32_t mask = bits(insn, 19, 18);
                if (bit(insn, 22) == 0 && mask == 0) return R::Continue;  // NOP/YIELD/WFE/WFI/SEV/DBG
                if (bit(insn, 22)) return undefined(bb, insn);
                const uint32_t v = ror32(bits(insn, 7, 0), bits(insn, 11, 8) * 2);
                OpMeta mm; if (mask & 2) mm.writes = kLAll;
                bb.emit(&H_Msr, 0xFF, 0, 0, (mask >> 1) | ((mask & 1) << 1), v, 0, mm);
                return R::Continue;
            }
            return dataProcessing(bb, insn, true);
        }
        case 2: return loadStoreWordByte(bb, insn);
        case 3:
            if (bit(insn, 4)) return media(bb, insn);
            return loadStoreWordByte(bb, insn);
        case 4: return blockTransfer(bb, insn);
        case 5: {                                           // B / BL
            const uint32_t target = bb.curPc() + 8 + uint32_t(sext(bits(insn, 23, 0), 24) * 4);
            if (bit(insn, 24)) bb.emit(&H_MovImm, kLR, 0, 0, 0, bb.curPc() + 4);
            bb.exitDirect(target, 0);
            return R::End;
        }
        default: return coprocessor(bb, insn);
    }
}

} // namespace

DecodeResult decodeArm(BlockBuilder& bb, uint32_t insn) {
    const uint32_t cond = insn >> 28;
    if (cond == 0xF) return unconditional(bb, insn);
    const size_t ci = bb.beginCond(cond);
    R r = body(bb, insn);
    bb.endCond(ci);
    if (r == R::End && cond < 14) return R::EndNeedsFallthrough;
    return r;
}

} // namespace Hydrogen::CPU
