// ArgonVM — IR handlers.
//
// Every handler has the same signature and ends by tail-calling the next Op
// (ARGON_NEXT) or by returning to the dispatcher. Because every call is a
// guaranteed tail call there is no stack growth no matter how long the guest runs.
//
// Register values live in Cpu::r[] (L1-resident). The original handoff pinned
// r0-r2 in host argument registers via GET_REG/SET_REG ternary chains; in
// practice that turns every operand access into 2-3 compare-and-branch
// sequences, which costs more than the L1 load it was trying to save, and it
// caps the useful number of pinned registers at what the host ABI passes in
// registers. Instead we pin only what every handler needs: cpu, op and the
// arena base.
#pragma once
#include "Cpu.hpp"
#include <cstring>
#include <climits>

namespace Argon::CPU {

// Implemented by the Engine.
Block* argonEnterBlock(Cpu* c, uint32_t key);   // lookup-or-decode; nullptr => stop
void   argonCallHle(Cpu* c, uint32_t index);

#define ARGON_H(name) inline ARGON_HOT_ABI void name(Cpu* c, const Op* op, uint8_t* m)
#define ARGON_NEXT()  do { const Op* _n = op + 1; [[clang::musttail]] return _n->fn(c, _n, m); } while (0)
#define ARGON_GOTO(o) do { const Op* _n = (o); [[clang::musttail]] return _n->fn(c, _n, m); } while (0)

// ---------------------------------------------------------------- helpers ---

template <typename T> inline T ld(const uint8_t* m, uint32_t a) { T v; std::memcpy(&v, m + uint64_t(a), sizeof(T)); return v; }
template <typename T> inline void st(uint8_t* m, uint32_t a, T v) { std::memcpy(m + uint64_t(a), &v, sizeof(T)); }

inline uint32_t ror32(uint32_t v, uint32_t s) { s &= 31; return s ? (v >> s) | (v << (32 - s)) : v; }
inline uint32_t nzOf(uint32_t r) { return (r & kFlagN) | (r == 0 ? kFlagZ : 0); }

struct AddResult { uint32_t r; uint32_t flags; };
// AddWithCarry() from the ARM ARM, returning result and full NZCV.
inline AddResult addWithCarry(uint32_t x, uint32_t y, uint32_t cin) {
    uint64_t u = uint64_t(x) + uint64_t(y) + cin;
    uint32_t r = uint32_t(u);
    uint32_t f = nzOf(r);
    if (u >> 32) f |= kFlagC;
    if (((x ^ r) & (y ^ r)) >> 31) f |= kFlagV;
    return {r, f};
}

enum ShiftType : uint8_t { kLSL = 0, kLSR = 1, kASR = 2, kROR = 3, kRRX = 4 };

// Shift by an immediate that the decoder has already normalised
// (LSR/ASR #32 are passed as 32, RRX as its own type). cin/cout are 0/1.
template <int T>
inline uint32_t shiftImm(uint32_t v, uint32_t n, uint32_t cin, uint32_t& cout) {
    if constexpr (T == kLSL) {
        if (n == 0) { cout = cin; return v; }
        cout = (v >> (32 - n)) & 1; return v << n;
    } else if constexpr (T == kLSR) {
        if (n == 32) { cout = v >> 31; return 0; }
        cout = (v >> (n - 1)) & 1; return v >> n;
    } else if constexpr (T == kASR) {
        if (n == 32) { cout = v >> 31; return uint32_t(int32_t(v) >> 31); }
        cout = (v >> (n - 1)) & 1; return uint32_t(int32_t(v) >> n);
    } else if constexpr (T == kROR) {
        uint32_t r = ror32(v, n); cout = r >> 31; return r;
    } else { // RRX
        cout = v & 1; return (v >> 1) | (cin << 31);
    }
}

// Shift by a register amount (bottom byte of Rs), with full ARM semantics.
template <int T>
inline uint32_t shiftReg(uint32_t v, uint32_t n, uint32_t cin, uint32_t& cout) {
    n &= 0xFF;
    if (n == 0) { cout = cin; return v; }
    if constexpr (T == kLSL) {
        if (n < 32)  { cout = (v >> (32 - n)) & 1; return v << n; }
        cout = n == 32 ? (v & 1) : 0; return 0;
    } else if constexpr (T == kLSR) {
        if (n < 32)  { cout = (v >> (n - 1)) & 1; return v >> n; }
        cout = n == 32 ? (v >> 31) : 0; return 0;
    } else if constexpr (T == kASR) {
        if (n < 32)  { cout = (v >> (n - 1)) & 1; return uint32_t(int32_t(v) >> n); }
        cout = v >> 31; return uint32_t(int32_t(v) >> 31);
    } else { // ROR
        uint32_t r = ror32(v, n); cout = r >> 31; return r;
    }
}

// Runtime-typed immediate shift (used for load/store register offsets).
inline uint32_t shiftImmDyn(uint32_t v, uint32_t type, uint32_t n, uint32_t cin) {
    uint32_t co;
    switch (type) {
        case kLSL: return shiftImm<kLSL>(v, n, cin, co);
        case kLSR: return shiftImm<kLSR>(v, n, cin, co);
        case kASR: return shiftImm<kASR>(v, n, cin, co);
        case kROR: return shiftImm<kROR>(v, n, cin, co);
        default:   return shiftImm<kRRX>(v, n, cin, co);
    }
}

// --------------------------------------------------------- control / misc ---

ARGON_H(H_Nop) { ARGON_NEXT(); }

// r[a] = imm
ARGON_H(H_MovImm) { c->r[op->a] = op->imm; ARGON_NEXT(); }

// r[a] = r[b]
ARGON_H(H_MovReg) { c->r[op->a] = c->r[op->b]; ARGON_NEXT(); }

// Conditional execution: if cond (a) fails, skip the next imm ops.
ARGON_H(H_CondSkip) {
    if (condPasses(op->a, c->nzcv)) ARGON_NEXT();
    ARGON_GOTO(op + 1 + op->imm);
}

inline void retire(Cpu* c, uint32_t n) { c->budget -= n; c->insn_count += n; }

// CBZ/CBNZ: a = rn, b = 1 for CBNZ. Falls into the next op (the taken-branch
// exit) when the branch is taken, otherwise skips it.
ARGON_H(H_CbSkip) {
    if ((c->r[op->a] != 0) == bool(op->b)) ARGON_NEXT();
    ARGON_GOTO(op + 2);
}

// TBB/TBH: a = rn, c = rm, d = 1 for TBH, imm = PC value (insn + 4).
// Writes the branch target to r15; followed by an indirect exit.
ARGON_H(H_TableBranch) {
    const uint32_t base = c->r[op->a], idx = c->r[op->c];
    const uint32_t off = op->d ? ld<uint16_t>(m, base + idx * 2) : ld<uint8_t>(m, base + idx);
    c->r[kPC] = op->imm + off * 2;
    ARGON_NEXT();
}

// Direct branch to imm (address), a = target thumb bit, b = insns retired.
ARGON_H(H_BranchDirect) {
    c->r[kPC] = op->imm;
    c->thumb = op->a;
    retire(c, op->b);
    if (c->budget <= 0) { c->exit = Exit::Budget; return; }
    Block* nb = op->link;
    if (__builtin_expect(nb == nullptr, 0)) {
        nb = argonEnterBlock(c, op->imm | op->a);
        if (!nb) return;
        op->link = nb;
    }
    c->block_pc = op->imm;
    const Op* first = nb->ops.data();
    [[clang::musttail]] return first->fn(c, first, m);
}

// Indirect branch to r[a]. c = 0: interworking (BX semantics, bit0 selects Thumb)
//                          c = 1: stay in current state (BranchWritePC)
// b = insns retired.
ARGON_H(H_BranchIndirect) {
    uint32_t t = c->r[op->a];
    if (op->c == 0) {
        c->thumb = t & 1;
        t &= c->thumb ? ~1u : ~3u;
    } else {
        t &= c->thumb ? ~1u : ~3u;
    }
    c->r[kPC] = t;
    retire(c, op->b);
    if (c->budget <= 0) { c->exit = Exit::Budget; return; }
    Block* nb = argonEnterBlock(c, t | c->thumb);
    if (!nb) return;
    c->block_pc = t;
    const Op* first = nb->ops.data();
    [[clang::musttail]] return first->fn(c, first, m);
}

// SVC #imm2: next pc in imm, thumb in a.
ARGON_H(H_Svc) {
    c->r[kPC] = op->imm; c->thumb = op->a; retire(c, op->b);
    c->exit = Exit::Svc; c->exit_info = op->imm2;
}

// Undefined / unimplemented instruction at op->pc (raw encoding in imm).
ARGON_H(H_Undefined) {
    c->r[kPC] = op->pc; c->thumb = op->a; retire(c, op->b);
    c->exit = Exit::Undefined; c->exit_info = op->pc;
}

// BKPT / UDF used as a trap: report and stop at the instruction.
ARGON_H(H_Breakpoint) {
    c->r[kPC] = op->pc; c->thumb = op->a; retire(c, op->b);
    c->exit = Exit::Breakpoint; c->exit_info = op->imm;
}

// Instruction fetch from unmapped memory.
ARGON_H(H_FetchFault) {
    c->r[kPC] = op->pc; c->thumb = op->a; retire(c, op->b);
    c->exit = Exit::MemFault; c->exit_info = op->pc;
}

// High-level-emulated function: call host function #imm, then continue.
ARGON_H(H_Hle) {
    argonCallHle(c, op->imm);
    if (c->exit != Exit::None) return;
    ARGON_NEXT();
}

// MRC p15 thread ID registers. a = rd, imm: 2 = TPIDRURW, 3 = TPIDRURO
ARGON_H(H_ReadTpid) { c->r[op->a] = op->imm == 2 ? c->tpidrurw : c->tpidruro; ARGON_NEXT(); }
// MCR p15 TPIDRURW (only the RW one is writable from user mode). a = rt
ARGON_H(H_WriteTpid) { c->tpidrurw = c->r[op->a]; ARGON_NEXT(); }

ARGON_H(H_Fence) { __atomic_thread_fence(__ATOMIC_SEQ_CST); ARGON_NEXT(); }

// ------------------------------------------------------- data processing ---

enum AluOp : uint8_t {
    kAND = 0, kEOR, kSUB, kRSB, kADD, kADC, kSBC, kRSC,
    kTST, kTEQ, kCMP, kCMN, kORR, kMOV, kBIC, kMVN, kORN,
    kAluOpCount
};
constexpr bool aluIsLogical(int o) {
    return o == kAND || o == kEOR || o == kTST || o == kTEQ || o == kORR ||
           o == kMOV || o == kBIC || o == kMVN || o == kORN;
}
constexpr bool aluIsTest(int o) { return o == kTST || o == kTEQ || o == kCMP || o == kCMN; }
constexpr bool aluUsesRn(int o) { return o != kMOV && o != kMVN; }

// Operand-2 kinds.
enum Op2Kind : uint8_t {
    kOp2Imm = 0,                                              // imm, d = carry mode (0 keep, 1 clear, 2 set)
    kOp2ShImmLSL, kOp2ShImmLSR, kOp2ShImmASR, kOp2ShImmROR, kOp2ShImmRRX, // c = rm, imm = amount
    kOp2ShRegLSL, kOp2ShRegLSR, kOp2ShRegASR, kOp2ShRegROR,              // c = rm, d = rs
    kOp2KindCount
};

template <int K>
inline uint32_t operand2(const Cpu* c, const Op* op, uint32_t cin, uint32_t& cout) {
    if constexpr (K == kOp2Imm) {
        cout = op->d == 0 ? cin : op->d - 1;
        return op->imm;
    } else if constexpr (K <= kOp2ShImmRRX) {
        return shiftImm<K - kOp2ShImmLSL>(c->r[op->c], op->imm, cin, cout);
    } else {
        return shiftReg<K - kOp2ShRegLSL>(c->r[op->c], c->r[op->d], cin, cout);
    }
}

// a = rd, b = rn, c = rm, d = rs / carry mode, imm = immediate / shift amount
template <int OPC, bool S, int K>
ARGON_H(H_Alu) {
    const uint32_t cin = (c->nzcv >> 29) & 1;
    uint32_t sc;
    const uint32_t y = operand2<K>(c, op, cin, sc);
    const uint32_t x = aluUsesRn(OPC) ? c->r[op->b] : 0;
    uint32_t r;
    uint32_t flags = 0;
    if constexpr (aluIsLogical(OPC)) {
        if constexpr (OPC == kAND || OPC == kTST) r = x & y;
        else if constexpr (OPC == kEOR || OPC == kTEQ) r = x ^ y;
        else if constexpr (OPC == kORR) r = x | y;
        else if constexpr (OPC == kORN) r = x | ~y;
        else if constexpr (OPC == kBIC) r = x & ~y;
        else if constexpr (OPC == kMOV) r = y;
        else r = ~y; // MVN
        if constexpr (S) flags = nzOf(r) | (sc << 29) | (c->nzcv & kFlagV);
    } else {
        AddResult ar;
        if constexpr (OPC == kADD || OPC == kCMN) ar = addWithCarry(x, y, 0);
        else if constexpr (OPC == kADC) ar = addWithCarry(x, y, cin);
        else if constexpr (OPC == kSUB || OPC == kCMP) ar = addWithCarry(x, ~y, 1);
        else if constexpr (OPC == kSBC) ar = addWithCarry(x, ~y, cin);
        else if constexpr (OPC == kRSB) ar = addWithCarry(y, ~x, 1);
        else ar = addWithCarry(y, ~x, cin); // RSC
        r = ar.r;
        if constexpr (S) flags = ar.flags;
    }
    if constexpr (!aluIsTest(OPC)) c->r[op->a] = r;
    if constexpr (S) c->nzcv = flags;
    ARGON_NEXT();
}

// ---------------------------------------------------------------- multiply ---

// a = rd, b = rn, c = rm, d = ra ; imm: 0 MUL, 1 MLA, 2 MLS
template <int KIND, bool S>
ARGON_H(H_Mul) {
    uint32_t r = c->r[op->b] * c->r[op->c];
    if constexpr (KIND == 1) r = r + c->r[op->d];
    if constexpr (KIND == 2) r = c->r[op->d] - r;
    c->r[op->a] = r;
    if constexpr (S) c->nzcv = (c->nzcv & (kFlagC | kFlagV)) | nzOf(r);
    ARGON_NEXT();
}

// Long multiplies. a = rdlo, b = rdhi, c = rn, d = rm.
// KIND: 0 UMULL, 1 UMLAL, 2 SMULL, 3 SMLAL, 4 UMAAL
template <int KIND, bool S>
ARGON_H(H_MulLong) {
    uint64_t r;
    if constexpr (KIND == 0 || KIND == 1 || KIND == 4) r = uint64_t(c->r[op->c]) * c->r[op->d];
    else r = uint64_t(int64_t(int32_t(c->r[op->c])) * int64_t(int32_t(c->r[op->d])));
    if constexpr (KIND == 1 || KIND == 3) r += (uint64_t(c->r[op->b]) << 32) | c->r[op->a];
    if constexpr (KIND == 4) r += uint64_t(c->r[op->a]) + uint64_t(c->r[op->b]);
    c->r[op->a] = uint32_t(r);
    c->r[op->b] = uint32_t(r >> 32);
    if constexpr (S) c->nzcv = (c->nzcv & (kFlagC | kFlagV)) | (uint32_t(r >> 32) & kFlagN) | (r == 0 ? kFlagZ : 0);
    ARGON_NEXT();
}

inline int32_t half(uint32_t v, uint32_t top) { return top ? int32_t(v) >> 16 : int32_t(int16_t(v)); }

// Signed halfword multiplies.
// a = rd(or rdlo), b = rn, c = rm, d = ra (or rdhi)
// imm bits: 0 = rn top half (x), 1 = rm top half (y)
// KIND: 0 SMULxy, 1 SMLAxy, 2 SMULWy, 3 SMLAWy, 4 SMLALxy
template <int KIND>
ARGON_H(H_MulHalf) {
    const uint32_t xt = op->imm & 1, yt = (op->imm >> 1) & 1;
    if constexpr (KIND == 0 || KIND == 1) {
        int32_t p = half(c->r[op->b], xt) * half(c->r[op->c], yt);
        if constexpr (KIND == 1) {
            int64_t s = int64_t(p) + int32_t(c->r[op->d]);
            if (s != int32_t(s)) c->q = 1;
            p = int32_t(uint32_t(s));
        }
        c->r[op->a] = uint32_t(p);
    } else if constexpr (KIND == 2 || KIND == 3) {
        int64_t p = (int64_t(int32_t(c->r[op->b])) * half(c->r[op->c], yt)) >> 16;
        if constexpr (KIND == 3) {
            int64_t s = p + int32_t(c->r[op->d]);
            if (s != int32_t(s)) c->q = 1;
            p = s;
        }
        c->r[op->a] = uint32_t(p);
    } else {
        int64_t acc = int64_t((uint64_t(c->r[op->d]) << 32) | c->r[op->a]);
        acc += int64_t(half(c->r[op->b], xt)) * half(c->r[op->c], yt);
        c->r[op->a] = uint32_t(acc);
        c->r[op->d] = uint32_t(uint64_t(acc) >> 32);
    }
    ARGON_NEXT();
}

// Dual 16-bit / most-significant-word multiplies.
// a = rd (rdlo), b = rn, c = rm, d = ra (rdhi), imm: bit0 = X (swap rm halves) / R (round)
// KIND: 0 SMLAD, 1 SMLSD, 2 SMLALD, 3 SMLSLD, 4 SMMLA, 5 SMMLS, 6 SMMUL, 7 SMUAD, 8 SMUSD
template <int KIND>
ARGON_H(H_MulDual) {
    uint32_t n = c->r[op->b], mm = c->r[op->c];
    if constexpr (KIND <= 3 || KIND >= 7) {
        if (op->imm & 1) mm = ror32(mm, 16);
        int64_t p1 = int64_t(int16_t(n)) * int16_t(mm);
        int64_t p2 = int64_t(int16_t(n >> 16)) * int16_t(mm >> 16);
        if constexpr (KIND == 0 || KIND == 7) {
            int64_t s = p1 + p2 + (KIND == 0 ? int64_t(int32_t(c->r[op->d])) : 0);
            if (s != int32_t(s)) c->q = 1;
            c->r[op->a] = uint32_t(s);
        } else if constexpr (KIND == 1 || KIND == 8) {
            int64_t s = p1 - p2 + (KIND == 1 ? int64_t(int32_t(c->r[op->d])) : 0);
            if (s != int32_t(s)) c->q = 1;
            c->r[op->a] = uint32_t(s);
        } else {
            int64_t acc = int64_t((uint64_t(c->r[op->d]) << 32) | c->r[op->a]);
            acc += KIND == 2 ? (p1 + p2) : (p1 - p2);
            c->r[op->a] = uint32_t(acc);
            c->r[op->d] = uint32_t(uint64_t(acc) >> 32);
        }
    } else {
        int64_t p = int64_t(int32_t(n)) * int32_t(mm);
        int64_t r;
        if constexpr (KIND == 4) r = (int64_t(uint64_t(c->r[op->d]) << 32)) + p;
        else if constexpr (KIND == 5) r = (int64_t(uint64_t(c->r[op->d]) << 32)) - p;
        else r = p;
        if (op->imm & 1) r += 0x80000000LL;
        c->r[op->a] = uint32_t(uint64_t(r) >> 32);
    }
    ARGON_NEXT();
}

// a = rd, b = rn, c = rm.  SIGNED: SDIV else UDIV. Division by zero yields 0.
template <bool SIGNED>
ARGON_H(H_Div) {
    uint32_t n = c->r[op->b], d = c->r[op->c], r;
    if (d == 0) r = 0;
    else if constexpr (SIGNED) {
        if (n == 0x80000000u && d == 0xFFFFFFFFu) r = 0x80000000u;
        else r = uint32_t(int32_t(n) / int32_t(d));
    } else r = n / d;
    c->r[op->a] = r;
    ARGON_NEXT();
}

// ------------------------------------------------------------ bit twiddling ---

// a = rd, b = rm. KIND: 0 CLZ, 1 RBIT, 2 REV, 3 REV16, 4 REVSH
template <int KIND>
ARGON_H(H_BitOp) {
    uint32_t v = c->r[op->b], r;
    if constexpr (KIND == 0) r = v ? uint32_t(__builtin_clz(v)) : 32;
    else if constexpr (KIND == 1) r = __builtin_bitreverse32(v);
    else if constexpr (KIND == 2) r = __builtin_bswap32(v);
    else if constexpr (KIND == 3) r = ((v & 0x00FF00FFu) << 8) | ((v & 0xFF00FF00u) >> 8);
    else r = uint32_t(int32_t(int16_t(uint16_t(((v & 0xFF) << 8) | ((v >> 8) & 0xFF)))));
    c->r[op->a] = r;
    ARGON_NEXT();
}

// Extends. a = rd, b = rn (0xFF = none), c = rm, imm = rotation (0/8/16/24)
// KIND: 0 SXTB, 1 SXTH, 2 UXTB, 3 UXTH, 4 SXTB16, 5 UXTB16
template <int KIND, bool ADD>
ARGON_H(H_Extend) {
    uint32_t v = ror32(c->r[op->c], op->imm), r;
    if constexpr (KIND == 0) r = uint32_t(int32_t(int8_t(v)));
    else if constexpr (KIND == 1) r = uint32_t(int32_t(int16_t(v)));
    else if constexpr (KIND == 2) r = v & 0xFF;
    else if constexpr (KIND == 3) r = v & 0xFFFF;
    else if constexpr (KIND == 4) r = (uint32_t(int32_t(int8_t(v))) & 0xFFFF) | (uint32_t(int32_t(int8_t(v >> 16))) << 16);
    else r = v & 0x00FF00FFu;
    if constexpr (ADD) {
        uint32_t n = c->r[op->b];
        if constexpr (KIND >= 4)
            r = ((n + r) & 0xFFFF) | (((n >> 16) + (r >> 16)) << 16);
        else r = n + r;
    }
    c->r[op->a] = r;
    ARGON_NEXT();
}

// Bitfields. a = rd, b = rn, imm = lsb, imm2 = width
// KIND: 0 UBFX, 1 SBFX, 2 BFI, 3 BFC
template <int KIND>
ARGON_H(H_Bitfield) {
    const uint32_t lsb = op->imm, w = op->imm2;
    const uint32_t mask = w >= 32 ? 0xFFFFFFFFu : ((1u << w) - 1);
    if constexpr (KIND == 0) c->r[op->a] = (c->r[op->b] >> lsb) & mask;
    else if constexpr (KIND == 1) c->r[op->a] = uint32_t(int32_t(c->r[op->b] << (32 - lsb - w)) >> (32 - w));
    else if constexpr (KIND == 2) c->r[op->a] = (c->r[op->a] & ~(mask << lsb)) | ((c->r[op->b] & mask) << lsb);
    else c->r[op->a] &= ~(mask << lsb);
    ARGON_NEXT();
}

// MOVT: a = rd, imm = imm16
ARGON_H(H_Movt) { c->r[op->a] = (c->r[op->a] & 0xFFFF) | (op->imm << 16); ARGON_NEXT(); }

// ------------------------------------------------------------- saturation ---

inline int32_t ssat(int64_t v, uint32_t bits, uint32_t& sat) {   // bits 1..32
    int64_t hi = (int64_t(1) << (bits - 1)) - 1, lo = -(int64_t(1) << (bits - 1));
    if (v > hi) { sat = 1; return int32_t(hi); }
    if (v < lo) { sat = 1; return int32_t(lo); }
    return int32_t(v);
}
inline uint32_t usat(int64_t v, uint32_t bits, uint32_t& sat) {  // bits 0..31
    int64_t hi = (int64_t(1) << bits) - 1;
    if (v > hi) { sat = 1; return uint32_t(hi); }
    if (v < 0)  { sat = 1; return 0; }
    return uint32_t(v);
}

// a = rd, c = rn, d = shift type (LSL/ASR), imm = shift amount, imm2 = saturate bits
// KIND: 0 SSAT, 1 USAT, 2 SSAT16, 3 USAT16
template <int KIND>
ARGON_H(H_Sat) {
    uint32_t sat = 0, v = c->r[op->c], r;
    if constexpr (KIND <= 1) {
        uint32_t co;
        // Decoder normalises ASR #0 to 32.
        int32_t sv = op->d == kASR ? int32_t(shiftImm<kASR>(v, op->imm, 0, co))
                                   : int32_t(v << op->imm);
        r = KIND == 0 ? uint32_t(ssat(sv, op->imm2, sat)) : usat(sv, op->imm2, sat);
    } else {
        int16_t lo = int16_t(v), hi = int16_t(v >> 16);
        uint32_t l = KIND == 2 ? uint32_t(ssat(lo, op->imm2, sat)) : usat(lo, op->imm2, sat);
        uint32_t h = KIND == 2 ? uint32_t(ssat(hi, op->imm2, sat)) : usat(hi, op->imm2, sat);
        r = (l & 0xFFFF) | (h << 16);
    }
    if (sat) c->q = 1;
    c->r[op->a] = r;
    ARGON_NEXT();
}

// QADD/QSUB/QDADD/QDSUB. a = rd, b = rn, c = rm. KIND: 0 QADD, 1 QSUB, 2 QDADD, 3 QDSUB
template <int KIND>
ARGON_H(H_QArith) {
    uint32_t sat = 0;
    int64_t n = int32_t(c->r[op->b]), x = int32_t(c->r[op->c]);
    if constexpr (KIND >= 2) n = ssat(2 * n, 32, sat);
    int64_t r = (KIND == 0 || KIND == 2) ? x + n : x - n;
    c->r[op->a] = uint32_t(ssat(r, 32, sat));
    if (sat) c->q = 1;
    ARGON_NEXT();
}

// Parallel add/subtract. a = rd, b = rn, c = rm,
// d = prefix (0 S, 1 Q, 2 SH, 3 U, 4 UQ, 5 UH), imm = op (0 ADD16, 1 ASX, 2 SAX, 3 SUB16, 4 ADD8, 7 SUB8)
ARGON_H(H_Parallel) {
    const uint32_t n = c->r[op->b], mm = c->r[op->c], pre = op->d, o = op->imm;
    const bool sgn = pre <= 2;
    uint32_t res = 0, ge = 0;
    auto lane = [&](int64_t v, int bits, int idx, int gebits) {
        int64_t out = v;
        if (pre == 1 || pre == 4) {             // saturating
            uint32_t dummy = 0;
            out = pre == 1 ? ssat(v, bits, dummy) : usat(v, bits, dummy);
        } else if (pre == 2 || pre == 5) {      // halving
            out = v >> 1;
        } else if (pre == 0) {                  // signed: GE when result >= 0
            if (v >= 0) ge |= ((1u << gebits) - 1) << (idx * gebits);
        }                                       // unsigned GE handled by uge()
        const uint32_t msk = (1u << bits) - 1;
        res |= (uint32_t(out) & msk) << (idx * bits);
    };
    auto ext = [&](uint32_t v, int bits, int idx) -> int64_t {
        uint32_t x = (v >> (idx * bits)) & ((1u << bits) - 1);
        if (!sgn) return x;
        return bits == 16 ? int64_t(int16_t(x)) : int64_t(int8_t(x));
    };
    auto uge = [&](int64_t v, int bits, bool add, int idx, int gebits) {
        if (pre != 3) return;
        bool g = add ? v >= (int64_t(1) << bits) : v >= 0;
        if (g) ge |= ((1u << gebits) - 1) << (idx * gebits);
    };
    if (o <= 3) {
        int64_t a0 = ext(n, 16, 0), a1 = ext(n, 16, 1), b0 = ext(mm, 16, 0), b1 = ext(mm, 16, 1);
        int64_t r0, r1; bool add0, add1;
        switch (o) {
            case 0:  r0 = a0 + b0; r1 = a1 + b1; add0 = add1 = true; break;   // ADD16
            case 1:  r0 = a0 - b1; r1 = a1 + b0; add0 = false; add1 = true; break; // ASX
            case 2:  r0 = a0 + b1; r1 = a1 - b0; add0 = true; add1 = false; break; // SAX
            default: r0 = a0 - b0; r1 = a1 - b1; add0 = add1 = false; break;  // SUB16
        }
        lane(r0, 16, 0, 2); lane(r1, 16, 1, 2);
        uge(r0, 16, add0, 0, 2); uge(r1, 16, add1, 1, 2);
    } else {
        const bool add = o == 4;
        for (int i = 0; i < 4; ++i) {
            int64_t r = add ? ext(n, 8, i) + ext(mm, 8, i) : ext(n, 8, i) - ext(mm, 8, i);
            lane(r, 8, i, 1);
            uge(r, 8, add, i, 1);
        }
    }
    c->r[op->a] = res;
    if (pre == 0 || pre == 3) c->ge = ge << 16;
    ARGON_NEXT();
}

// SEL: a = rd, b = rn, c = rm
ARGON_H(H_Sel) {
    uint32_t r = 0;
    for (int i = 0; i < 4; ++i) {
        uint32_t msk = 0xFFu << (i * 8);
        r |= ((c->ge >> (16 + i)) & 1 ? c->r[op->b] : c->r[op->c]) & msk;
    }
    c->r[op->a] = r;
    ARGON_NEXT();
}

// PKHBT / PKHTB: a = rd, b = rn, c = rm, imm = shift, d = 0 BT (LSL) / 1 TB (ASR)
ARGON_H(H_Pkh) {
    uint32_t co, mm = c->r[op->c], n = c->r[op->b];
    if (op->d == 0) {
        uint32_t s = shiftImm<kLSL>(mm, op->imm, 0, co);
        c->r[op->a] = (n & 0xFFFF) | (s & 0xFFFF0000u);
    } else {
        uint32_t s = shiftImm<kASR>(mm, op->imm ? op->imm : 32, 0, co);
        c->r[op->a] = (n & 0xFFFF0000u) | (s & 0xFFFF);
    }
    ARGON_NEXT();
}

// USAD8 / USADA8: a = rd, b = rn, c = rm, d = ra (0xFF = none)
ARGON_H(H_Usad8) {
    uint32_t n = c->r[op->b], mm = c->r[op->c], s = 0;
    for (int i = 0; i < 4; ++i) {
        int x = int((n >> (i * 8)) & 0xFF) - int((mm >> (i * 8)) & 0xFF);
        s += uint32_t(x < 0 ? -x : x);
    }
    if (op->d != 0xFF) s += c->r[op->d];
    c->r[op->a] = s;
    ARGON_NEXT();
}

// ---------------------------------------------------------- status register ---

// MRS rd, APSR
ARGON_H(H_Mrs) { c->r[op->a] = c->nzcv | (c->q << 27) | c->ge | 0x10; ARGON_NEXT(); }

// MSR APSR_<mask>, rn|imm. a = rn (0xFF => use imm), d: bit0 nzcvq, bit1 g
ARGON_H(H_Msr) {
    uint32_t v = op->a == 0xFF ? op->imm : c->r[op->a];
    if (op->d & 1) { c->nzcv = v & kFlagMaskNZCV; c->q = (v >> 27) & 1; }
    if (op->d & 2) c->ge = v & 0x000F0000u;
    ARGON_NEXT();
}

// ------------------------------------------------------------ loads/stores ---

enum MemKind : uint8_t { kMemW = 0, kMemB, kMemH, kMemSB, kMemSH };
enum AddrMode : uint8_t { kAmOffset = 0, kAmPre = 1, kAmPost = 2 };

template <int K> inline uint32_t memLoad(const uint8_t* m, uint32_t a) {
    if constexpr (K == kMemW) return ld<uint32_t>(m, a);
    else if constexpr (K == kMemB) return ld<uint8_t>(m, a);
    else if constexpr (K == kMemH) return ld<uint16_t>(m, a);
    else if constexpr (K == kMemSB) return uint32_t(int32_t(ld<int8_t>(m, a)));
    else return uint32_t(int32_t(ld<int16_t>(m, a)));
}
template <int K> inline void memStore(uint8_t* m, uint32_t a, uint32_t v) {
    if constexpr (K == kMemW) st<uint32_t>(m, a, v);
    else if constexpr (K == kMemB) st<uint8_t>(m, a, uint8_t(v));
    else st<uint16_t>(m, a, uint16_t(v));
}

// Single load/store.
// a = rt, b = rn, c = rm, d = (shift type) | 0x80 if register offset is subtracted
// imm = signed immediate offset (REG = false) or shift amount (REG = true)
template <int K, bool LOAD, int AM, bool REG>
ARGON_H(H_Mem) {
    const uint32_t base = c->r[op->b];
    uint32_t off;
    if constexpr (REG) {
        off = shiftImmDyn(c->r[op->c], op->d & 0x7F, op->imm, (c->nzcv >> 29) & 1);
        if (op->d & 0x80) off = 0u - off;
    } else {
        off = op->imm;
    }
    const uint32_t ea = base + off;
    const uint32_t addr = AM == kAmPost ? base : ea;
    if constexpr (LOAD) {
        uint32_t v = memLoad<K>(m, addr);
        if constexpr (AM != kAmOffset) c->r[op->b] = ea;
        c->r[op->a] = v;
    } else {
        memStore<K>(m, addr, c->r[op->a]);
        if constexpr (AM != kAmOffset) c->r[op->b] = ea;
    }
    ARGON_NEXT();
}

// LDRD/STRD. a = rt, imm2 = rt2, b = rn, c = rm, d as H_Mem, imm offset/shift
template <bool LOAD, int AM, bool REG>
ARGON_H(H_MemDual) {
    const uint32_t base = c->r[op->b];
    uint32_t off = REG ? c->r[op->c] : op->imm;
    if (REG && (op->d & 0x80)) off = 0u - off;
    const uint32_t ea = base + off;
    const uint32_t addr = AM == kAmPost ? base : ea;
    if constexpr (LOAD) {
        uint32_t lo = ld<uint32_t>(m, addr), hi = ld<uint32_t>(m, addr + 4);
        if constexpr (AM != kAmOffset) c->r[op->b] = ea;
        c->r[op->a] = lo; c->r[op->imm2] = hi;
    } else {
        st<uint32_t>(m, addr, c->r[op->a]); st<uint32_t>(m, addr + 4, c->r[op->imm2]);
        if constexpr (AM != kAmOffset) c->r[op->b] = ea;
    }
    ARGON_NEXT();
}

// LDM/STM. b = rn, imm = register list, d: bit0 increment, bit1 before, bit2 writeback
template <bool LOAD>
ARGON_H(H_Multi) {
    const uint32_t list = op->imm, n = uint32_t(__builtin_popcount(list));
    const uint32_t base = c->r[op->b];
    const bool inc = op->d & 1, before = op->d & 2, wb = op->d & 4;
    uint32_t addr = inc ? (before ? base + 4 : base) : (before ? base - 4 * n : base - 4 * n + 4);
    const uint32_t newbase = inc ? base + 4 * n : base - 4 * n;
    if constexpr (LOAD) {
        for (uint32_t l = list; l; l &= l - 1) {
            c->r[__builtin_ctz(l)] = ld<uint32_t>(m, addr);
            addr += 4;
        }
        if (wb && !((list >> op->b) & 1)) c->r[op->b] = newbase;
    } else {
        for (uint32_t l = list; l; l &= l - 1) {
            st<uint32_t>(m, addr, c->r[__builtin_ctz(l)]);
            addr += 4;
        }
        if (wb) c->r[op->b] = newbase;
    }
    ARGON_NEXT();
}

// ---------------------------------------------------------------- atomics ---

// LDREX{B,H,,D}: a = rt, imm2 = rt2 (D only), b = rn, imm = offset. SZ in bytes.
template <int SZ>
ARGON_H(H_Ldrex) {
    const uint32_t addr = c->r[op->b] + op->imm;
    uint64_t v;
    if constexpr (SZ == 1) v = __atomic_load_n(reinterpret_cast<uint8_t*>(m + uint64_t(addr)), __ATOMIC_ACQUIRE);
    else if constexpr (SZ == 2) v = __atomic_load_n(reinterpret_cast<uint16_t*>(m + uint64_t(addr)), __ATOMIC_ACQUIRE);
    else if constexpr (SZ == 4) v = __atomic_load_n(reinterpret_cast<uint32_t*>(m + uint64_t(addr)), __ATOMIC_ACQUIRE);
    else v = __atomic_load_n(reinterpret_cast<uint64_t*>(m + uint64_t(addr)), __ATOMIC_ACQUIRE);
    c->excl_addr = addr; c->excl_value = v; c->excl_size = SZ;
    if constexpr (SZ == 8) { c->r[op->a] = uint32_t(v); c->r[op->imm2] = uint32_t(v >> 32); }
    else c->r[op->a] = uint32_t(v);
    ARGON_NEXT();
}

// STREX{B,H,,D}: a = rd (status), c = rt, imm2 = rt2 (D only), b = rn, imm = offset.
template <int SZ>
ARGON_H(H_Strex) {
    const uint32_t addr = c->r[op->b] + op->imm;
    uint32_t status = 1;
    if (c->excl_size == SZ && c->excl_addr == addr) {
        bool ok;
        if constexpr (SZ == 1) { uint8_t e = uint8_t(c->excl_value); ok = __atomic_compare_exchange_n(reinterpret_cast<uint8_t*>(m + uint64_t(addr)), &e, uint8_t(c->r[op->c]), false, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST); }
        else if constexpr (SZ == 2) { uint16_t e = uint16_t(c->excl_value); ok = __atomic_compare_exchange_n(reinterpret_cast<uint16_t*>(m + uint64_t(addr)), &e, uint16_t(c->r[op->c]), false, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST); }
        else if constexpr (SZ == 4) { uint32_t e = uint32_t(c->excl_value); ok = __atomic_compare_exchange_n(reinterpret_cast<uint32_t*>(m + uint64_t(addr)), &e, c->r[op->c], false, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST); }
        else { uint64_t e = c->excl_value, nv = (uint64_t(c->r[op->imm2]) << 32) | c->r[op->c]; ok = __atomic_compare_exchange_n(reinterpret_cast<uint64_t*>(m + uint64_t(addr)), &e, nv, false, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST); }
        status = ok ? 0 : 1;
    }
    c->excl_size = 0;
    c->r[op->a] = status;
    ARGON_NEXT();
}

ARGON_H(H_Clrex) { c->excl_size = 0; ARGON_NEXT(); }

// SWP / SWPB: a = rt, c = rt2 (value), b = rn
template <bool BYTE>
ARGON_H(H_Swp) {
    const uint32_t addr = c->r[op->b];
    if constexpr (BYTE) c->r[op->a] = __atomic_exchange_n(reinterpret_cast<uint8_t*>(m + uint64_t(addr)), uint8_t(c->r[op->c]), __ATOMIC_SEQ_CST);
    else c->r[op->a] = __atomic_exchange_n(reinterpret_cast<uint32_t*>(m + uint64_t(addr)), c->r[op->c], __ATOMIC_SEQ_CST);
    c->excl_size = 0;
    ARGON_NEXT();
}

} // namespace Argon::CPU
