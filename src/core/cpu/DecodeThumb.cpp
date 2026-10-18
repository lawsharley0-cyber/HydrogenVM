// ArgonVM — Thumb / Thumb-2 (T32) decoder → IR.
// Encoding references are to the ARMv7-A/R ARM, chapter A6.
//
// IT blocks are resolved at decode time: the engine tracks ITSTATE while it
// decodes a block and passes each instruction its condition, which becomes a
// CondSkip op in front of it. 16-bit instructions that set flags outside an IT
// block do not set them inside one, which is also decided here.
#include "Decoder.hpp"

namespace Argon::CPU {
namespace {

using R = DecodeResult;

inline uint32_t bits(uint32_t v, int hi, int lo) { return (v >> lo) & ((2u << (hi - lo)) - 1); }
inline uint32_t bit(uint32_t v, int b) { return (v >> b) & 1; }
inline int32_t sext(uint32_t v, int width) { return int32_t(v << (32 - width)) >> (32 - width); }

R undefined(BlockBuilder& bb, uint32_t raw) {
    bb.exitSimple(&H_Undefined, raw);
    return R::End;
}

// PC value as read by Thumb instructions, and its word-aligned form (literals / ADR).
inline uint32_t pcT(BlockBuilder& bb) { return bb.curPc() + 4; }
inline uint32_t pcAligned(BlockBuilder& bb) { return (bb.curPc() + 4) & ~3u; }

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

// ThumbExpandImm_C: returns value; carry mode 0 = unchanged, 1/2 = clear/set.
inline uint32_t thumbExpandImm(uint32_t imm12, uint32_t& carry) {
    if ((imm12 >> 10) == 0) {
        carry = 0;
        const uint32_t b = imm12 & 0xFF;
        switch ((imm12 >> 8) & 3) {
            case 0: return b;
            case 1: return (b << 16) | b;
            case 2: return (b << 24) | (b << 8);
            default: return (b << 24) | (b << 16) | (b << 8) | b;
        }
    }
    const uint32_t v = ror32(0x80 | (imm12 & 0x7F), imm12 >> 7);
    carry = (v >> 31) + 1;
    return v;
}

// Thumb PC writes from ALU ops / MOV / ADD: BranchWritePC (no interworking).
R pcWrittenNoInterwork(BlockBuilder& bb) {
    bb.clobberPC();
    bb.exitIndirect(kPC, false);
    return R::End;
}
// Loads to PC (LDR, POP, LDM): LoadWritePC = BXWritePC (interworking).
R pcLoaded(BlockBuilder& bb) {
    bb.clobberPC();
    bb.exitIndirect(kPC, true);
    return R::End;
}

void emitMul(BlockBuilder& bb, bool s, uint32_t rd, uint32_t rn, uint32_t rm) {
    OpMeta mm;
    if (s) { mm.writes = kLN | kLZ; mm.noflags = &H_Mul<0, false>; }
    bb.emit(s ? &H_Mul<0, true> : &H_Mul<0, false>, rd, rn, rm, 0, 0, 0, mm);
}

// ======================================================= 16-bit encodings ===

R thumb16(BlockBuilder& bb, uint32_t hw, bool inIt) {
    const bool setflags = !inIt;
    const uint32_t op6 = hw >> 10;

    // A6.2.1 shift (immediate), add, subtract, move, compare
    if ((op6 & 0x30) == 0x00) {
        const uint32_t opc = bits(hw, 13, 9);
        const uint32_t rd = bits(hw, 2, 0), rn = bits(hw, 5, 3);
        if (opc < 0xC) {                                      // LSL/LSR/ASR #imm (MOVS when LSL #0)
            int kind; uint32_t amt;
            immShiftKind(opc >> 2, bits(hw, 10, 6), kind, amt);
            emitAlu(bb, kMOV, setflags, kind, rd, 0, rn, 0, amt);
            return R::Continue;
        }
        if (opc < 0x10) {                                     // ADD/SUB register or 3-bit immediate
            const uint32_t rm = bits(hw, 8, 6);
            switch (opc) {
                case 0xC: emitAlu(bb, kADD, setflags, kOp2ShImmLSL, rd, rn, rm, 0, 0); return R::Continue;
                case 0xD: emitAlu(bb, kSUB, setflags, kOp2ShImmLSL, rd, rn, rm, 0, 0); return R::Continue;
                case 0xE: emitAlu(bb, kADD, setflags, kOp2Imm, rd, rn, 0, 0, rm); return R::Continue;
                default:  emitAlu(bb, kSUB, setflags, kOp2Imm, rd, rn, 0, 0, rm); return R::Continue;
            }
        }
        const uint32_t rdn = bits(hw, 10, 8), imm8 = bits(hw, 7, 0);
        switch (opc >> 2) {
            case 4: emitAlu(bb, kMOV, setflags, kOp2Imm, rdn, 0, 0, 0, imm8); return R::Continue;
            case 5: emitAlu(bb, kCMP, true, kOp2Imm, 0, rdn, 0, 0, imm8); return R::Continue;
            case 6: emitAlu(bb, kADD, setflags, kOp2Imm, rdn, rdn, 0, 0, imm8); return R::Continue;
            default: emitAlu(bb, kSUB, setflags, kOp2Imm, rdn, rdn, 0, 0, imm8); return R::Continue;
        }
    }

    // A6.2.2 data processing (register)
    if (op6 == 0x10) {
        const uint32_t opc = bits(hw, 9, 6), rm = bits(hw, 5, 3), rdn = bits(hw, 2, 0);
        switch (opc) {
            case 0x0: emitAlu(bb, kAND, setflags, kOp2ShImmLSL, rdn, rdn, rm, 0, 0); break;
            case 0x1: emitAlu(bb, kEOR, setflags, kOp2ShImmLSL, rdn, rdn, rm, 0, 0); break;
            case 0x2: emitAlu(bb, kMOV, setflags, kOp2ShRegLSL, rdn, 0, rdn, rm, 0); break;
            case 0x3: emitAlu(bb, kMOV, setflags, kOp2ShRegLSR, rdn, 0, rdn, rm, 0); break;
            case 0x4: emitAlu(bb, kMOV, setflags, kOp2ShRegASR, rdn, 0, rdn, rm, 0); break;
            case 0x5: emitAlu(bb, kADC, setflags, kOp2ShImmLSL, rdn, rdn, rm, 0, 0); break;
            case 0x6: emitAlu(bb, kSBC, setflags, kOp2ShImmLSL, rdn, rdn, rm, 0, 0); break;
            case 0x7: emitAlu(bb, kMOV, setflags, kOp2ShRegROR, rdn, 0, rdn, rm, 0); break;
            case 0x8: emitAlu(bb, kTST, true, kOp2ShImmLSL, 0, rdn, rm, 0, 0); break;
            case 0x9: emitAlu(bb, kRSB, setflags, kOp2Imm, rdn, rm, 0, 0, 0); break;  // RSBS rd, rn, #0
            case 0xA: emitAlu(bb, kCMP, true, kOp2ShImmLSL, 0, rdn, rm, 0, 0); break;
            case 0xB: emitAlu(bb, kCMN, true, kOp2ShImmLSL, 0, rdn, rm, 0, 0); break;
            case 0xC: emitAlu(bb, kORR, setflags, kOp2ShImmLSL, rdn, rdn, rm, 0, 0); break;
            case 0xD: emitMul(bb, setflags, rdn, rm, rdn); break;
            case 0xE: emitAlu(bb, kBIC, setflags, kOp2ShImmLSL, rdn, rdn, rm, 0, 0); break;
            default:  emitAlu(bb, kMVN, setflags, kOp2ShImmLSL, rdn, 0, rm, 0, 0); break;
        }
        return R::Continue;
    }

    // A6.2.3 special data instructions and branch and exchange
    if (op6 == 0x11) {
        const uint32_t opc = bits(hw, 9, 6);
        const uint32_t rm = bits(hw, 6, 3);
        const uint32_t rdn = (bit(hw, 7) << 3) | bits(hw, 2, 0);
        if (opc < 4) {                                         // ADD (high registers)
            emitAlu(bb, kADD, false, kOp2ShImmLSL, rdn, bb.reg(rdn), bb.reg(rm), 0, 0);
            if (rdn == 15) return pcWrittenNoInterwork(bb);
            return R::Continue;
        }
        if (opc < 8) {                                         // CMP (high registers)
            emitAlu(bb, kCMP, true, kOp2ShImmLSL, 0, bb.reg(rdn), bb.reg(rm), 0, 0);
            return R::Continue;
        }
        if (opc < 12) {                                        // MOV (high registers)
            if (rdn == 15) {
                bb.emit(&H_MovReg, kTmp0, bb.reg(rm));
                bb.emit(&H_MovReg, kPC, kTmp0);
                return pcWrittenNoInterwork(bb);
            }
            bb.emit(&H_MovReg, rdn, bb.reg(rm));
            return R::Continue;
        }
        if (!bit(hw, 7)) {                                     // BX
            bb.exitIndirect(bb.reg(rm), true);
            return R::End;
        }
        bb.emit(&H_MovReg, kTmp0, bb.reg(rm));                 // BLX Rm
        bb.emit(&H_MovImm, kLR, 0, 0, 0, (bb.curPc() + 2) | 1);
        bb.exitIndirect(kTmp0, true);
        return R::End;
    }

    // LDR (literal)
    if ((op6 & 0x3E) == 0x12) {
        const uint32_t rt = bits(hw, 10, 8);
        bb.emit(memHandler(kMemW, true, kAmOffset, false), rt, bb.usePC(pcAligned(bb)), 0, 0, bits(hw, 7, 0) * 4);
        return R::Continue;
    }

    // A6.2.4 load/store single data item
    if ((op6 & 0x3C) == 0x14 || (op6 & 0x38) == 0x18 || (op6 & 0x38) == 0x20) {
        const uint32_t opA = bits(hw, 15, 12), opB = bits(hw, 11, 9);
        const uint32_t rt = bits(hw, 2, 0), rn = bits(hw, 5, 3);
        if (opA == 5) {                                        // register offset
            static constexpr int kinds[8] = { kMemW, kMemH, kMemB, kMemSB, kMemW, kMemH, kMemB, kMemSH };
            const bool load = opB >= 3;
            bb.emit(memHandler(kinds[opB], load, kAmOffset, true), rt, rn, bits(hw, 8, 6), kLSL, 0);
            return R::Continue;
        }
        const uint32_t imm5 = bits(hw, 10, 6);
        const bool load = bit(hw, 11);
        switch (opA) {
            case 6: bb.emit(memHandler(kMemW, load, kAmOffset, false), rt, rn, 0, 0, imm5 * 4); break;
            case 7: bb.emit(memHandler(kMemB, load, kAmOffset, false), rt, rn, 0, 0, imm5); break;
            case 8: bb.emit(memHandler(kMemH, load, kAmOffset, false), rt, rn, 0, 0, imm5 * 2); break;
            default: bb.emit(memHandler(kMemW, load, kAmOffset, false), bits(hw, 10, 8), kSP, 0, 0, bits(hw, 7, 0) * 4); break;
        }
        return R::Continue;
    }

    // ADR / ADD (SP plus immediate)
    if ((op6 & 0x3E) == 0x28) {
        bb.emit(&H_MovImm, bits(hw, 10, 8), 0, 0, 0, pcAligned(bb) + bits(hw, 7, 0) * 4);
        return R::Continue;
    }
    if ((op6 & 0x3E) == 0x2A) {
        emitAlu(bb, kADD, false, kOp2Imm, bits(hw, 10, 8), kSP, 0, 0, bits(hw, 7, 0) * 4);
        return R::Continue;
    }

    // A6.2.5 miscellaneous 16-bit instructions
    if ((op6 & 0x3C) == 0x2C) {
        const uint32_t opc = bits(hw, 11, 5);
        if ((opc & 0x7C) == 0x00) { emitAlu(bb, kADD, false, kOp2Imm, kSP, kSP, 0, 0, bits(hw, 6, 0) * 4); return R::Continue; }
        if ((opc & 0x7C) == 0x04) { emitAlu(bb, kSUB, false, kOp2Imm, kSP, kSP, 0, 0, bits(hw, 6, 0) * 4); return R::Continue; }
        if ((opc & 0x28) == 0x08) {                            // CBZ / CBNZ
            if (inIt) return undefined(bb, hw);
            const uint32_t off = (bit(hw, 9) << 6) | (bits(hw, 7, 3) << 1);
            bb.emit(&H_CbSkip, bits(hw, 2, 0), bit(hw, 11));
            bb.exitDirect(pcT(bb) + off, 1);
            return R::EndNeedsFallthrough;
        }
        const uint32_t rm = bits(hw, 5, 3), rd = bits(hw, 2, 0);
        switch (opc & 0x7E) {
            case 0x10: bb.emit(&H_Extend<1, false>, rd, 0, rm, 0, 0); return R::Continue; // SXTH
            case 0x12: bb.emit(&H_Extend<0, false>, rd, 0, rm, 0, 0); return R::Continue; // SXTB
            case 0x14: bb.emit(&H_Extend<3, false>, rd, 0, rm, 0, 0); return R::Continue; // UXTH
            case 0x16: bb.emit(&H_Extend<2, false>, rd, 0, rm, 0, 0); return R::Continue; // UXTB
            case 0x50: bb.emit(&H_BitOp<2>, rd, rm); return R::Continue;                   // REV
            case 0x52: bb.emit(&H_BitOp<3>, rd, rm); return R::Continue;                   // REV16
            case 0x56: bb.emit(&H_BitOp<4>, rd, rm); return R::Continue;                   // REVSH
            default: break;
        }
        if ((opc & 0x70) == 0x20) {                            // PUSH
            const uint32_t list = bits(hw, 7, 0) | (bit(hw, 8) << 14);
            if (!list) return undefined(bb, hw);
            bb.emit(&H_Multi<false>, 0, kSP, 0, 2 | 4, list);  // STMDB sp!
            return R::Continue;
        }
        if ((opc & 0x70) == 0x60) {                            // POP
            const uint32_t list = bits(hw, 7, 0) | (bit(hw, 8) << 15);
            if (!list) return undefined(bb, hw);
            bb.emit(&H_Multi<true>, 0, kSP, 0, 1 | 4, list);   // LDMIA sp!
            if (list & 0x8000) return pcLoaded(bb);
            return R::Continue;
        }
        if ((opc & 0x78) == 0x70) {                            // BKPT
            bb.exitSimple(&H_Breakpoint, bits(hw, 7, 0));
            return R::End;
        }
        if ((opc & 0x78) == 0x78) {                            // IT (handled by the engine) / hints
            return R::Continue;                                // NOP, YIELD, WFE, WFI, SEV
        }
        if (opc == 0x33) return R::Continue;                   // CPS: privileged, ignore
        return undefined(bb, hw);
    }

    // STM / LDM (16-bit)
    if ((op6 & 0x3E) == 0x30) {
        const uint32_t rn = bits(hw, 10, 8), list = bits(hw, 7, 0);
        if (!list) return undefined(bb, hw);
        bb.emit(&H_Multi<false>, 0, rn, 0, 1 | 4, list);
        return R::Continue;
    }
    if ((op6 & 0x3E) == 0x32) {
        const uint32_t rn = bits(hw, 10, 8), list = bits(hw, 7, 0);
        if (!list) return undefined(bb, hw);
        bb.emit(&H_Multi<true>, 0, rn, 0, 1 | 4, list);       // no writeback if rn in list (handled)
        return R::Continue;
    }

    // B<c> / UDF / SVC
    if ((op6 & 0x3C) == 0x34) {
        const uint32_t cond = bits(hw, 11, 8);
        if (cond == 0xE) { bb.exitSimple(&H_Breakpoint, bits(hw, 7, 0)); return R::End; }   // UDF
        if (cond == 0xF) { bb.exitSimple(&H_Svc, (bb.curPc() + 2) | 0, bits(hw, 7, 0)); return R::End; }
        if (inIt) return undefined(bb, hw);
        const uint32_t target = pcT(bb) + uint32_t(sext(bits(hw, 7, 0), 8) * 2);
        const size_t ci = bb.beginCond(cond);
        bb.exitDirect(target, 1);
        bb.endCond(ci);
        return R::EndNeedsFallthrough;
    }

    // B (unconditional)
    if ((op6 & 0x3E) == 0x38) {
        bb.exitDirect(pcT(bb) + uint32_t(sext(bits(hw, 10, 0), 11) * 2), 1);
        return R::End;
    }

    return undefined(bb, hw);
}

// ======================================================= 32-bit encodings ===

// A6.3.1 data processing (modified immediate)
R dpModifiedImm(BlockBuilder& bb, uint32_t hw1, uint32_t hw2) {
    const uint32_t op = bits(hw1, 8, 5), s = bit(hw1, 4), rn = bits(hw1, 3, 0), rd = bits(hw2, 11, 8);
    const uint32_t imm12 = (bit(hw1, 10) << 11) | (bits(hw2, 14, 12) << 8) | bits(hw2, 7, 0);
    uint32_t carry;
    const uint32_t imm = thumbExpandImm(imm12, carry);
    int opc;
    switch (op) {
        case 0x0: opc = (rd == 15 && s) ? kTST : kAND; break;
        case 0x1: opc = kBIC; break;
        case 0x2: opc = rn == 15 ? kMOV : kORR; break;
        case 0x3: opc = rn == 15 ? kMVN : kORN; break;
        case 0x4: opc = (rd == 15 && s) ? kTEQ : kEOR; break;
        case 0x8: opc = (rd == 15 && s) ? kCMN : kADD; break;
        case 0xA: opc = kADC; break;
        case 0xB: opc = kSBC; break;
        case 0xD: opc = (rd == 15 && s) ? kCMP : kSUB; break;
        case 0xE: opc = kRSB; break;
        default: return undefined(bb, (hw1 << 16) | hw2);
    }
    if (!aluIsLogical(opc)) carry = 0;
    emitAlu(bb, opc, s, kOp2Imm, rd, aluUsesRn(opc) ? bb.reg(rn) : 0, 0, carry, imm);
    return R::Continue;
}

// A6.3.3 data processing (plain binary immediate)
R dpPlainImm(BlockBuilder& bb, uint32_t hw1, uint32_t hw2) {
    const uint32_t op = bits(hw1, 8, 4), rn = bits(hw1, 3, 0), rd = bits(hw2, 11, 8);
    const uint32_t imm12 = (bit(hw1, 10) << 11) | (bits(hw2, 14, 12) << 8) | bits(hw2, 7, 0);
    const uint32_t imm16 = (rn << 12) | imm12;
    const uint32_t imm5 = (bits(hw2, 14, 12) << 2) | bits(hw2, 7, 6);
    switch (op) {
        case 0x00:                                         // ADDW / ADR
            if (rn == 15) bb.emit(&H_MovImm, rd, 0, 0, 0, pcAligned(bb) + imm12);
            else emitAlu(bb, kADD, false, kOp2Imm, rd, rn, 0, 0, imm12);
            return R::Continue;
        case 0x0A:                                         // SUBW / ADR (sub)
            if (rn == 15) bb.emit(&H_MovImm, rd, 0, 0, 0, pcAligned(bb) - imm12);
            else emitAlu(bb, kSUB, false, kOp2Imm, rd, rn, 0, 0, imm12);
            return R::Continue;
        case 0x04: bb.emit(&H_MovImm, rd, 0, 0, 0, imm16); return R::Continue;   // MOVW
        case 0x0C: bb.emit(&H_Movt, rd, 0, 0, 0, imm16); return R::Continue;     // MOVT
        case 0x10: case 0x12: {                            // SSAT / SSAT16
            const uint32_t sh = bit(hw1, 5);
            if (sh && imm5 == 0) { bb.emit(&H_Sat<2>, rd, 0, rn, 0, 0, bits(hw2, 3, 0) + 1); return R::Continue; }
            bb.emit(&H_Sat<0>, rd, 0, rn, sh ? kASR : kLSL, sh && imm5 == 0 ? 32 : imm5, bits(hw2, 4, 0) + 1);
            return R::Continue;
        }
        case 0x18: case 0x1A: {                            // USAT / USAT16
            const uint32_t sh = bit(hw1, 5);
            if (sh && imm5 == 0) { bb.emit(&H_Sat<3>, rd, 0, rn, 0, 0, bits(hw2, 3, 0)); return R::Continue; }
            bb.emit(&H_Sat<1>, rd, 0, rn, sh ? kASR : kLSL, sh && imm5 == 0 ? 32 : imm5, bits(hw2, 4, 0));
            return R::Continue;
        }
        case 0x14: bb.emit(&H_Bitfield<1>, rd, rn, 0, 0, imm5, bits(hw2, 4, 0) + 1); return R::Continue; // SBFX
        case 0x1C: bb.emit(&H_Bitfield<0>, rd, rn, 0, 0, imm5, bits(hw2, 4, 0) + 1); return R::Continue; // UBFX
        case 0x16: {                                       // BFI / BFC
            const uint32_t msb = bits(hw2, 4, 0);
            if (msb < imm5) return undefined(bb, (hw1 << 16) | hw2);
            if (rn == 15) bb.emit(&H_Bitfield<3>, rd, 0, 0, 0, imm5, msb - imm5 + 1);
            else bb.emit(&H_Bitfield<2>, rd, rn, 0, 0, imm5, msb - imm5 + 1);
            return R::Continue;
        }
        default: return undefined(bb, (hw1 << 16) | hw2);
    }
}

// A6.3.4 branches and miscellaneous control
R branchesMisc(BlockBuilder& bb, uint32_t hw1, uint32_t hw2, bool inIt) {
    const uint32_t op1 = bits(hw2, 14, 12), op = bits(hw1, 10, 4);
    const uint32_t raw = (hw1 << 16) | hw2;
    const uint32_t S = bit(hw1, 10), J1 = bit(hw2, 13), J2 = bit(hw2, 11);

    if ((op1 & 5) == 0) {
        if ((op & 0x38) != 0x38) {                         // B<c>.W (T3)
            if (inIt) return undefined(bb, raw);
            const uint32_t imm = (S << 20) | (J2 << 19) | (J1 << 18) | (bits(hw1, 5, 0) << 12) | (bits(hw2, 10, 0) << 1);
            const uint32_t target = pcT(bb) + uint32_t(sext(imm, 21));
            const size_t ci = bb.beginCond(bits(hw1, 9, 6));
            bb.exitDirect(target, 1);
            bb.endCond(ci);
            return R::EndNeedsFallthrough;
        }
        if ((op & 0x7E) == 0x38) {                         // MSR (register)
            const uint32_t mask = bits(hw2, 11, 10);
            if (bit(hw1, 4)) return undefined(bb, raw);    // SPSR
            OpMeta mm; if (mask & 2) mm.writes = kLAll;
            bb.emit(&H_Msr, bb.reg(bits(hw1, 3, 0)), 0, 0, (mask >> 1) | ((mask & 1) << 1), 0, 0, mm);
            return R::Continue;
        }
        if (op == 0x3A) return R::Continue;                // hints: NOP/YIELD/WFE/WFI/SEV/DBG
        if (op == 0x3B) {                                  // misc control
            switch (bits(hw2, 7, 4)) {
                case 2: bb.emit(&H_Clrex); return R::Continue;
                case 4: case 5: bb.emit(&H_Fence); return R::Continue;
                case 6: return R::Continue;                // ISB
                default: return undefined(bb, raw);
            }
        }
        if ((op & 0x7E) == 0x3E) {                         // MRS
            if (bit(hw1, 4)) return undefined(bb, raw);
            OpMeta mm; mm.reads = kLAll;
            bb.emit(&H_Mrs, bits(hw2, 11, 8), 0, 0, 0, 0, 0, mm);
            return R::Continue;
        }
        if (op1 == 2 && op == 0x7F) { bb.exitSimple(&H_Breakpoint, raw); return R::End; }   // UDF.W
        return undefined(bb, raw);
    }
    const uint32_t I1 = !(J1 ^ S), I2 = !(J2 ^ S);
    const uint32_t imm25 = (S << 24) | (I1 << 23) | (I2 << 22) | (bits(hw1, 9, 0) << 12) | (bits(hw2, 10, 0) << 1);
    const int32_t off = sext(imm25, 25);
    if ((op1 & 5) == 1) {                                  // B.W (T4)
        bb.exitDirect(pcT(bb) + uint32_t(off), 1);
        return R::End;
    }
    bb.emit(&H_MovImm, kLR, 0, 0, 0, (bb.curPc() + 4) | 1);
    if ((op1 & 5) == 5) {                                  // BL
        bb.exitDirect(pcT(bb) + uint32_t(off), 1);
    } else {                                               // BLX (immediate) → ARM
        if (hw2 & 1) return undefined(bb, raw);
        bb.exitDirect(pcAligned(bb) + uint32_t(off & ~3), 0);
    }
    return R::End;
}

// A6.3.17 load/store multiple
R loadStoreMultiple(BlockBuilder& bb, uint32_t hw1, uint32_t hw2) {
    const uint32_t op = bits(hw1, 8, 7), w = bit(hw1, 5), l = bit(hw1, 4), rn = bits(hw1, 3, 0);
    const uint32_t list = hw2;
    if (op == 0 || op == 3 || rn == 15 || !list) return undefined(bb, (hw1 << 16) | hw2);
    const uint32_t mode = (op == 1 ? 1 : 2) | (w << 2);   // IA or DB
    bb.emit(l ? &H_Multi<true> : &H_Multi<false>, 0, rn, 0, mode, list);
    if (l && (list & 0x8000)) return pcLoaded(bb);
    return R::Continue;
}

// A6.3.16 load/store dual, exclusive, table branch
R loadStoreDualExclusive(BlockBuilder& bb, uint32_t hw1, uint32_t hw2) {
    const uint32_t op1 = bits(hw1, 8, 7), op2 = bits(hw1, 5, 4), op3 = bits(hw2, 7, 4);
    const uint32_t rn = bits(hw1, 3, 0), rt = bits(hw2, 15, 12), rt2 = bits(hw2, 11, 8);
    const uint32_t raw = (hw1 << 16) | hw2;
    if (op1 == 0 && op2 == 0) { bb.emit(&H_Strex<4>, rt2, rn, rt, 0, bits(hw2, 7, 0) * 4); return R::Continue; }
    if (op1 == 0 && op2 == 1) { bb.emit(&H_Ldrex<4>, rt, rn, 0, 0, bits(hw2, 7, 0) * 4); return R::Continue; }
    if ((op1 & 2) == 0 && (op2 & 2) == 0 && op1 == 1) {
        const uint32_t rd = bits(hw2, 3, 0);
        if (op2 == 0) {
            switch (op3) {
                case 4: bb.emit(&H_Strex<1>, rd, rn, rt); return R::Continue;
                case 5: bb.emit(&H_Strex<2>, rd, rn, rt); return R::Continue;
                case 7: bb.emit(&H_Strex<8>, rd, rn, rt, 0, 0, rt2); return R::Continue;
                default: return undefined(bb, raw);
            }
        }
        switch (op3) {
            case 0: case 1: {                              // TBB / TBH
                bb.emit(&H_TableBranch, bb.reg(rn), 0, bits(hw2, 3, 0), op3, pcT(bb));
                bb.clobberPC();
                bb.exitIndirect(kPC, false);
                return R::End;
            }
            case 4: bb.emit(&H_Ldrex<1>, rt, rn); return R::Continue;
            case 5: bb.emit(&H_Ldrex<2>, rt, rn); return R::Continue;
            case 7: bb.emit(&H_Ldrex<8>, rt, rn, 0, 0, 0, rt2); return R::Continue;
            default: return undefined(bb, raw);
        }
    }
    // LDRD / STRD (immediate). P = hw1[8], U = hw1[7], W = hw1[5]
    const uint32_t p = bit(hw1, 8), u = bit(hw1, 7), w = bit(hw1, 5), l = bit(hw1, 4);
    const uint32_t imm = bits(hw2, 7, 0) * 4;
    const int am = !p ? kAmPost : (w ? kAmPre : kAmOffset);
    const uint32_t base = rn == 15 ? bb.usePC(pcAligned(bb)) : rn;
    bb.emit(memDualHandler(l, am, false), rt, base, 0, 0, u ? imm : 0u - imm, rt2);
    return R::Continue;
}

// A6.3.11 data processing (shifted register)
R dpShiftedReg(BlockBuilder& bb, uint32_t hw1, uint32_t hw2) {
    const uint32_t op = bits(hw1, 8, 5), s = bit(hw1, 4), rn = bits(hw1, 3, 0);
    const uint32_t rd = bits(hw2, 11, 8), rm = bits(hw2, 3, 0), type = bits(hw2, 5, 4);
    const uint32_t imm5 = (bits(hw2, 14, 12) << 2) | bits(hw2, 7, 6);
    if (op == 0x6) {                                       // PKHBT / PKHTB
        const uint32_t tb = bit(hw2, 5);
        bb.emit(&H_Pkh, rd, rn, rm, tb, tb && imm5 == 0 ? 32 : imm5);
        return R::Continue;
    }
    int opc;
    switch (op) {
        case 0x0: opc = (rd == 15 && s) ? kTST : kAND; break;
        case 0x1: opc = kBIC; break;
        case 0x2: opc = rn == 15 ? kMOV : kORR; break;
        case 0x3: opc = rn == 15 ? kMVN : kORN; break;
        case 0x4: opc = (rd == 15 && s) ? kTEQ : kEOR; break;
        case 0x8: opc = (rd == 15 && s) ? kCMN : kADD; break;
        case 0xA: opc = kADC; break;
        case 0xB: opc = kSBC; break;
        case 0xD: opc = (rd == 15 && s) ? kCMP : kSUB; break;
        case 0xE: opc = kRSB; break;
        default: return undefined(bb, (hw1 << 16) | hw2);
    }
    int kind; uint32_t amt;
    immShiftKind(type, imm5, kind, amt);
    emitAlu(bb, opc, s, kind, rd, aluUsesRn(opc) ? bb.reg(rn) : 0, bb.reg(rm), 0, amt);
    return R::Continue;
}

// A6.3.7-A6.3.10 load/store single
R loadStoreSingle(BlockBuilder& bb, uint32_t hw1, uint32_t hw2) {
    const uint32_t raw = (hw1 << 16) | hw2;
    const uint32_t size = bits(hw1, 6, 5), sgn = bit(hw1, 8), l = bit(hw1, 4);
    const uint32_t rn = bits(hw1, 3, 0), rt = bits(hw2, 15, 12);
    if (size == 3 || (sgn && (size == 2 || !l))) return undefined(bb, raw);
    int kind;
    if (size == 0) kind = sgn ? kMemSB : kMemB;
    else if (size == 1) kind = sgn ? kMemSH : kMemH;
    else kind = kMemW;
    const bool load = l;
    // PLD / PLI / unallocated memory hints: byte/half loads to PC.
    if (load && rt == 15 && size != 2) return R::Continue;

    const uint32_t src = load ? rt : bb.reg(rt);
    if (rn == 15) {                                        // literal
        if (!load) return undefined(bb, raw);
        const uint32_t imm12 = bits(hw2, 11, 0);
        bb.emit(memHandler(kind, true, kAmOffset, false), rt, bb.usePC(pcAligned(bb)), 0, 0,
                bit(hw1, 7) ? imm12 : 0u - imm12);
    } else if (bit(hw1, 7)) {                              // imm12, positive offset
        bb.emit(memHandler(kind, load, kAmOffset, false), src, rn, 0, 0, bits(hw2, 11, 0));
    } else if (bit(hw2, 11)) {                             // imm8 with P/U/W
        const uint32_t p = bit(hw2, 10), u = bit(hw2, 9), w = bit(hw2, 8), imm8 = bits(hw2, 7, 0);
        const int am = !p ? kAmPost : (w ? kAmPre : kAmOffset);   // P=1,U=1,W=0 is LDRT-class: plain offset
        bb.emit(memHandler(kind, load, am, false), src, rn, 0, 0, u ? imm8 : 0u - imm8);
    } else if (bits(hw2, 11, 6) == 0) {                    // register offset, LSL #imm2
        bb.emit(memHandler(kind, load, kAmOffset, true), src, rn, bb.reg(bits(hw2, 3, 0)), kLSL, bits(hw2, 5, 4));
    } else {
        return undefined(bb, raw);
    }
    if (load && rt == 15) return pcLoaded(bb);
    return R::Continue;
}

// A6.3.12 data processing (register)
R dpRegister(BlockBuilder& bb, uint32_t hw1, uint32_t hw2) {
    const uint32_t op1 = bits(hw1, 7, 4), op2 = bits(hw2, 7, 4);
    const uint32_t rn = bits(hw1, 3, 0), rd = bits(hw2, 11, 8), rm = bits(hw2, 3, 0);
    const uint32_t raw = (hw1 << 16) | hw2;
    if ((op1 & 8) == 0 && op2 == 0) {                      // LSL/LSR/ASR/ROR (register)
        const int kind = kOp2ShRegLSL + int(bits(hw1, 6, 5));
        emitAlu(bb, kMOV, bit(hw1, 4), kind, rd, 0, rn, rm, 0);
        return R::Continue;
    }
    if ((op1 & 8) == 0 && (op2 & 8)) {                     // extends
        const uint32_t rot = bits(hw2, 5, 4) * 8;
        static constexpr Handler plain[6] = { &H_Extend<1, false>, &H_Extend<3, false>, &H_Extend<4, false>,
                                              &H_Extend<5, false>, &H_Extend<0, false>, &H_Extend<2, false> };
        static constexpr Handler add[6]   = { &H_Extend<1, true>, &H_Extend<3, true>, &H_Extend<4, true>,
                                              &H_Extend<5, true>, &H_Extend<0, true>, &H_Extend<2, true> };
        if (op1 > 5) return undefined(bb, raw);
        if (rn == 15) bb.emit(plain[op1], rd, 0, rm, 0, rot);
        else bb.emit(add[op1], rd, rn, rm, 0, rot);
        return R::Continue;
    }
    if ((op1 & 8) && (op2 & 8) == 0) {                     // parallel add/subtract
        static constexpr int opmap[8] = { 4, 0, 1, -1, 7, 3, 2, -1 };   // op1[2:0] → ADD8, ADD16, ASX, -, SUB8, SUB16, SAX
        const int o = opmap[op1 & 7];
        const uint32_t pfxBits = bits(hw2, 5, 4);
        if (o < 0 || pfxBits == 3) return undefined(bb, raw);
        const uint32_t prefix = (bit(hw2, 6) ? 3 : 0) + pfxBits;    // S/Q/SH or U/UQ/UH
        bb.emit(&H_Parallel, rd, rn, rm, prefix, uint32_t(o));
        return R::Continue;
    }
    if ((op1 & 0xC) == 0x8 && (op2 & 0xC) == 0x8) {        // A6.3.15 miscellaneous operations
        const uint32_t mop1 = bits(hw1, 5, 4), mop2 = bits(hw2, 5, 4);
        switch (mop1) {
            case 0: {
                static constexpr Handler q[4] = { &H_QArith<0>, &H_QArith<2>, &H_QArith<1>, &H_QArith<3> };
                bb.emit(q[mop2], rd, rn, rm);              // QADD, QDADD, QSUB, QDSUB (Rn is the 2nd operand)
                return R::Continue;
            }
            case 1: {
                static constexpr Handler r[4] = { &H_BitOp<2>, &H_BitOp<3>, &H_BitOp<1>, &H_BitOp<4> };
                bb.emit(r[mop2], rd, rm);
                return R::Continue;
            }
            case 2: if (mop2 == 0) { bb.emit(&H_Sel, rd, rn, rm); return R::Continue; } break;
            case 3: if (mop2 == 0) { bb.emit(&H_BitOp<0>, rd, rm); return R::Continue; } break;
        }
    }
    return undefined(bb, raw);
}

// A6.3.16 multiply, multiply accumulate, absolute difference
R multiplyAcc(BlockBuilder& bb, uint32_t hw1, uint32_t hw2) {
    const uint32_t op1 = bits(hw1, 6, 4), op2 = bits(hw2, 5, 4);
    const uint32_t rn = bits(hw1, 3, 0), ra = bits(hw2, 15, 12), rd = bits(hw2, 11, 8), rm = bits(hw2, 3, 0);
    const uint32_t raw = (hw1 << 16) | hw2;
    const uint32_t x = bit(hw2, 4);
    switch (op1) {
        case 0:
            if (op2 == 0) { if (ra == 15) bb.emit(&H_Mul<0, false>, rd, rn, rm); else bb.emit(&H_Mul<1, false>, rd, rn, rm, ra); return R::Continue; }
            if (op2 == 1) { bb.emit(&H_Mul<2, false>, rd, rn, rm, ra); return R::Continue; }
            break;
        case 1: {
            const uint32_t sel = bit(hw2, 5) | (bit(hw2, 4) << 1);
            if (ra == 15) bb.emit(&H_MulHalf<0>, rd, rn, rm, 0, sel);
            else bb.emit(&H_MulHalf<1>, rd, rn, rm, ra, sel);
            return R::Continue;
        }
        case 2: if (op2 < 2) { if (ra == 15) bb.emit(&H_MulDual<7>, rd, rn, rm, 0, x); else bb.emit(&H_MulDual<0>, rd, rn, rm, ra, x); return R::Continue; } break;
        case 3: if (op2 < 2) { if (ra == 15) bb.emit(&H_MulHalf<2>, rd, rn, rm, 0, x << 1); else bb.emit(&H_MulHalf<3>, rd, rn, rm, ra, x << 1); return R::Continue; } break;
        case 4: if (op2 < 2) { if (ra == 15) bb.emit(&H_MulDual<8>, rd, rn, rm, 0, x); else bb.emit(&H_MulDual<1>, rd, rn, rm, ra, x); return R::Continue; } break;
        case 5: if (op2 < 2) { if (ra == 15) bb.emit(&H_MulDual<6>, rd, rn, rm, 0, x); else bb.emit(&H_MulDual<4>, rd, rn, rm, ra, x); return R::Continue; } break;
        case 6: if (op2 < 2) { bb.emit(&H_MulDual<5>, rd, rn, rm, ra, x); return R::Continue; } break;
        case 7: if (op2 == 0) { bb.emit(&H_Usad8, rd, rn, rm, ra == 15 ? 0xFF : ra); return R::Continue; } break;
    }
    return undefined(bb, raw);
}

// A6.3.17 long multiply, long multiply accumulate, divide
R longMultiplyDivide(BlockBuilder& bb, uint32_t hw1, uint32_t hw2) {
    const uint32_t op1 = bits(hw1, 6, 4), op2 = bits(hw2, 7, 4);
    const uint32_t rn = bits(hw1, 3, 0), lo = bits(hw2, 15, 12), hi = bits(hw2, 11, 8), rm = bits(hw2, 3, 0);
    const uint32_t raw = (hw1 << 16) | hw2;
    switch (op1) {
        case 0: if (op2 == 0) { bb.emit(&H_MulLong<2, false>, lo, hi, rn, rm); return R::Continue; } break;   // SMULL
        case 1: if (op2 == 0xF) { bb.emit(&H_Div<true>, hi, rn, rm); return R::Continue; } break;             // SDIV
        case 2: if (op2 == 0) { bb.emit(&H_MulLong<0, false>, lo, hi, rn, rm); return R::Continue; } break;   // UMULL
        case 3: if (op2 == 0xF) { bb.emit(&H_Div<false>, hi, rn, rm); return R::Continue; } break;            // UDIV
        case 4:
            if (op2 == 0) { bb.emit(&H_MulLong<3, false>, lo, hi, rn, rm); return R::Continue; }              // SMLAL
            if ((op2 & 0xC) == 0x8) { bb.emit(&H_MulHalf<4>, lo, rn, rm, hi, bit(hw2, 5) | (bit(hw2, 4) << 1)); return R::Continue; }
            if ((op2 & 0xE) == 0xC) { bb.emit(&H_MulDual<2>, lo, rn, rm, hi, bit(hw2, 4)); return R::Continue; } // SMLALD
            break;
        case 5: if ((op2 & 0xE) == 0xC) { bb.emit(&H_MulDual<3>, lo, rn, rm, hi, bit(hw2, 4)); return R::Continue; } break; // SMLSLD
        case 6:
            if (op2 == 0) { bb.emit(&H_MulLong<1, false>, lo, hi, rn, rm); return R::Continue; }              // UMLAL
            if (op2 == 6) { bb.emit(&H_MulLong<4, false>, lo, hi, rn, rm); return R::Continue; }              // UMAAL
            break;
    }
    return undefined(bb, raw);
}

R thumb32(BlockBuilder& bb, uint32_t hw1, uint32_t hw2, bool inIt) {
    const uint32_t op1 = bits(hw1, 12, 11), op2 = bits(hw1, 10, 4), op = bit(hw2, 15);
    const uint32_t raw = (hw1 << 16) | hw2;
    if (op1 == 1) {
        if ((op2 & 0x64) == 0x00) return loadStoreMultiple(bb, hw1, hw2);
        if ((op2 & 0x64) == 0x04) return loadStoreDualExclusive(bb, hw1, hw2);
        if ((op2 & 0x60) == 0x20) return dpShiftedReg(bb, hw1, hw2);
        return decodeThumbVfpNeon(bb, raw);               // coprocessor
    }
    if (op1 == 2) {
        if (op) return branchesMisc(bb, hw1, hw2, inIt);
        if ((op2 & 0x20) == 0) return dpModifiedImm(bb, hw1, hw2);
        return dpPlainImm(bb, hw1, hw2);
    }
    // op1 == 3
    if ((op2 & 0x71) == 0x00) return loadStoreSingle(bb, hw1, hw2);             // store single
    if ((op2 & 0x67) == 0x01 || (op2 & 0x67) == 0x03 || (op2 & 0x67) == 0x05) return loadStoreSingle(bb, hw1, hw2);
    if ((op2 & 0x71) == 0x10) return decodeThumbVfpNeon(bb, raw);              // NEON element load/store
    if ((op2 & 0x70) == 0x20) return dpRegister(bb, hw1, hw2);
    if ((op2 & 0x78) == 0x30) return multiplyAcc(bb, hw1, hw2);
    if ((op2 & 0x78) == 0x38) return longMultiplyDivide(bb, hw1, hw2);
    if (op2 & 0x40) return decodeThumbVfpNeon(bb, raw);                        // coprocessor
    return undefined(bb, raw);
}

} // namespace

DecodeResult decodeThumb(BlockBuilder& bb, uint16_t hw1, uint16_t hw2, uint32_t itCond, bool inIt, uint32_t& len) {
    const bool wide = thumbIs32(hw1);
    len = wide ? 4 : 2;
    const size_t ci = inIt ? bb.beginCond(itCond) : SIZE_MAX;
    R r = wide ? thumb32(bb, hw1, hw2, inIt) : thumb16(bb, hw1, inIt);
    bb.endCond(ci);
    if (r == R::End && inIt && itCond < 14) return R::EndNeedsFallthrough;
    return r;
}

} // namespace Argon::CPU
