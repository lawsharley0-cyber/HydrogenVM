// HydrogenVM — builds one IR block and runs block-local optimisations.
#pragma once
#include "Handlers.hpp"
#include <array>
#include <utility>
#include <vector>

namespace Hydrogen::CPU {

// Flag bits for liveness analysis: N=8 Z=4 C=2 V=1.
enum : uint8_t { kLN = 8, kLZ = 4, kLC = 2, kLV = 1, kLAll = 15 };

struct OpMeta {
    uint8_t reads  = 0;      // flags read
    uint8_t writes = 0;      // flags written
    bool    guarded = false; // executes under a CondSkip (write may not happen)
    Handler noflags = nullptr; // equivalent handler that doesn't write flags
};

// ----------------------------------------------------------- handler tables ---

namespace detail {
template <int O, bool S, size_t... K>
constexpr auto aluRow(std::index_sequence<K...>) {
    return std::array<Handler, sizeof...(K)>{ &H_Alu<O, S, int(K)>... };
}
template <size_t... O>
constexpr auto aluTable(std::index_sequence<O...>) {
    return std::array<std::array<std::array<Handler, kOp2KindCount>, 2>, sizeof...(O)>{
        std::array<std::array<Handler, kOp2KindCount>, 2>{
            aluRow<int(O), false>(std::make_index_sequence<kOp2KindCount>{}),
            aluRow<int(O), true >(std::make_index_sequence<kOp2KindCount>{}) }... };
}
} // namespace detail

inline constexpr auto kAluTable = detail::aluTable(std::make_index_sequence<kAluOpCount>{});

inline Handler memHandler(int kind, bool load, int am, bool reg) {
#define HVM_MEM_ROW(K) \
    { { { &H_Mem<K, false, 0, false>, &H_Mem<K, false, 0, true> }, \
        { &H_Mem<K, false, 1, false>, &H_Mem<K, false, 1, true> }, \
        { &H_Mem<K, false, 2, false>, &H_Mem<K, false, 2, true> } }, \
      { { &H_Mem<K, true, 0, false>,  &H_Mem<K, true, 0, true> }, \
        { &H_Mem<K, true, 1, false>,  &H_Mem<K, true, 1, true> }, \
        { &H_Mem<K, true, 2, false>,  &H_Mem<K, true, 2, true> } } }
    static constexpr Handler t[5][2][3][2] = {
        HVM_MEM_ROW(kMemW), HVM_MEM_ROW(kMemB), HVM_MEM_ROW(kMemH),
        HVM_MEM_ROW(kMemSB), HVM_MEM_ROW(kMemSH) };
#undef HVM_MEM_ROW
    return t[kind][load][am][reg];
}

inline Handler memDualHandler(bool load, int am, bool reg) {
    static constexpr Handler t[2][3][2] = {
        { { &H_MemDual<false, 0, false>, &H_MemDual<false, 0, true> },
          { &H_MemDual<false, 1, false>, &H_MemDual<false, 1, true> },
          { &H_MemDual<false, 2, false>, &H_MemDual<false, 2, true> } },
        { { &H_MemDual<true, 0, false>, &H_MemDual<true, 0, true> },
          { &H_MemDual<true, 1, false>, &H_MemDual<true, 1, true> },
          { &H_MemDual<true, 2, false>, &H_MemDual<true, 2, true> } } };
    return t[load][am][reg];
}

// ------------------------------------------------------- pair superinstructions ---
//
// Two consecutive simple ops run by one handler: one dispatch instead of two.
// The second op stays in place with its own handler, so a CondSkip that jumps
// straight to it still works; the pair handler only runs when the first op does.

template <Body B1, Body B2>
HVM_H(H_Pair) {
    B1(c, op, m);
    B2(c, op + 1, m);
    const Op* _n = op + 2;
    [[clang::musttail]] return _n->fn(c, _n, m);
}

struct HotOp { Handler h; Body b; };
// The most frequent simple ops in compiled ARM/Thumb code (tools/pairprof.py).
inline constexpr HotOp kHotOps[] = {
    {&H_MovReg, &B_MovReg},
    {&H_MovImm, &B_MovImm},
    {&H_Alu<kADD, false, kOp2Imm>, &B_Alu<kADD, false, kOp2Imm>},
    {&H_Alu<kADD, false, kOp2ShImmLSL>, &B_Alu<kADD, false, kOp2ShImmLSL>},
    {&H_Alu<kSUB, false, kOp2Imm>, &B_Alu<kSUB, false, kOp2Imm>},
    {&H_Alu<kSUB, false, kOp2ShImmLSL>, &B_Alu<kSUB, false, kOp2ShImmLSL>},
    {&H_Alu<kADD, true, kOp2Imm>, &B_Alu<kADD, true, kOp2Imm>},
    {&H_Alu<kSUB, true, kOp2Imm>, &B_Alu<kSUB, true, kOp2Imm>},
    {&H_Alu<kMOV, false, kOp2Imm>, &B_Alu<kMOV, false, kOp2Imm>},
    {&H_Alu<kMOV, false, kOp2ShImmLSL>, &B_Alu<kMOV, false, kOp2ShImmLSL>},
    {&H_Alu<kMOV, false, kOp2ShImmLSR>, &B_Alu<kMOV, false, kOp2ShImmLSR>},
    {&H_Alu<kMOV, true, kOp2Imm>, &B_Alu<kMOV, true, kOp2Imm>},
    {&H_Alu<kMOV, true, kOp2ShImmLSL>, &B_Alu<kMOV, true, kOp2ShImmLSL>},
    {&H_Alu<kCMP, true, kOp2Imm>, &B_Alu<kCMP, true, kOp2Imm>},
    {&H_Alu<kCMP, true, kOp2ShImmLSL>, &B_Alu<kCMP, true, kOp2ShImmLSL>},
    {&H_Alu<kAND, false, kOp2Imm>, &B_Alu<kAND, false, kOp2Imm>},
    {&H_Alu<kORR, false, kOp2ShImmLSL>, &B_Alu<kORR, false, kOp2ShImmLSL>},
    {&H_Alu<kEOR, false, kOp2ShImmLSL>, &B_Alu<kEOR, false, kOp2ShImmLSL>},
    {&H_Mem<kMemW, true, kAmOffset, false>, &B_Mem<kMemW, true, kAmOffset, false>},
    {&H_Mem<kMemW, false, kAmOffset, false>, &B_Mem<kMemW, false, kAmOffset, false>},
    {&H_Mem<kMemB, true, kAmOffset, false>, &B_Mem<kMemB, true, kAmOffset, false>},
    {&H_Mem<kMemB, false, kAmOffset, false>, &B_Mem<kMemB, false, kAmOffset, false>},
    {&H_Mem<kMemH, true, kAmOffset, false>, &B_Mem<kMemH, true, kAmOffset, false>},
    {&H_Mem<kMemW, true, kAmOffset, true>, &B_Mem<kMemW, true, kAmOffset, true>},
    {&H_Bitfield<0>, &B_Bitfield<0>},
    {&H_Extend<2, false>, &B_Extend<2, false>},
    {&H_Extend<3, false>, &B_Extend<3, false>},
};
inline constexpr size_t kNumHot = sizeof(kHotOps) / sizeof(kHotOps[0]);

namespace detail {
template <size_t I, size_t... J>
constexpr std::array<Handler, kNumHot> pairRow(std::index_sequence<J...>) {
    return {&H_Pair<kHotOps[I].b, kHotOps[J].b>...};
}
template <size_t... I>
constexpr std::array<std::array<Handler, kNumHot>, kNumHot> pairTable(std::index_sequence<I...>) {
    return {pairRow<I>(std::make_index_sequence<kNumHot>{})...};
}
} // namespace detail
inline constexpr auto kPairTable = detail::pairTable(std::make_index_sequence<kNumHot>{});

inline int hotIndex(Handler h) {
    for (size_t i = 0; i < kNumHot; ++i) if (kHotOps[i].h == h) return int(i);
    return -1;
}

// ------------------------------------------------------------------ builder ---

class BlockBuilder {
public:
    static constexpr uint32_t kMaxInsns = 64;

    BlockBuilder(Block& b, uint32_t start, bool thumb) : blk_(b), thumb_(thumb), pc_(start) {
        blk_.ops.reserve(96);
        meta_.reserve(96);
    }

    // --- per-instruction bookkeeping (called by the decoder loop)
    void beginInsn(uint32_t addr) { pc_ = addr; ++insns_; pcValid_ = false; }
    uint32_t insns() const { return insns_; }
    bool thumb() const { return thumb_; }
    uint32_t curPc() const { return pc_; }

    // --- raw emission
    Op& emit(Handler fn, uint32_t a = 0, uint32_t b = 0, uint32_t c = 0, uint32_t d = 0,
             uint32_t imm = 0, uint32_t imm2 = 0, OpMeta meta = {}) {
        Op op{};
        op.fn = fn; op.a = uint8_t(a); op.b = uint8_t(b); op.c = uint8_t(c); op.d = uint8_t(d);
        op.imm = imm; op.imm2 = imm2; op.pc = pc_; op.link = nullptr;
        meta.guarded = condDepth_ > 0;
        blk_.ops.push_back(op);
        meta_.push_back(meta);
        return blk_.ops.back();
    }

    // Materialise the architectural PC value into r15 so ops can read it as a
    // normal register. (r15 in the register file is only otherwise meaningful at
    // block exits, so clobbering it here is free.)
    uint8_t usePC(uint32_t value) {
        if (!pcValid_ || pcValue_ != value) {
            emit(&H_MovImm, kPC, 0, 0, 0, value);
            pcValid_ = true; pcValue_ = value;
        }
        return kPC;
    }
    // Architectural PC as read by most instructions (+8 ARM, +4 Thumb).
    uint32_t pcRead() const { return pc_ + (thumb_ ? 4 : 8); }
    uint8_t reg(uint32_t r) { return r == 15 ? usePC(pcRead()) : uint8_t(r); }
    // Anything that writes r15 invalidates our cached PC materialisation.
    void clobberPC() { pcValid_ = false; }

    // --- conditional execution
    size_t beginCond(uint32_t cond) {
        if (cond >= 14) return SIZE_MAX;
        OpMeta mm; mm.reads = condReads(cond);
        size_t idx = blk_.ops.size();
        emit(&H_CondSkip, cond, 0, 0, 0, 0, 0, mm);
        ++condDepth_;
        pcValid_ = false;       // a materialised PC inside the guarded region may be skipped
        return idx;
    }
    void endCond(size_t idx) {
        if (idx == SIZE_MAX) return;
        blk_.ops[idx].imm = uint32_t(blk_.ops.size() - idx - 1);
        --condDepth_;
        pcValid_ = false;
    }

    // --- exits
    void exitDirect(uint32_t target, uint32_t thumb) {
        OpMeta mm; mm.reads = kLAll;
        emit(bool(thumb & 1) == thumb_ ? &H_BranchDirect : &H_BranchDirectX, thumb & 1, insns_, 0, 0,
             target & (thumb ? ~1u : ~3u), 0, mm);
    }
    // interwork: BX-style (bit0 selects state); otherwise stay in current state.
    void exitIndirect(uint8_t reg, bool interwork) {
        OpMeta mm; mm.reads = kLAll;
        emit(&H_BranchIndirect, reg, insns_, interwork ? 0 : 1, 0, 0, 0, mm);
    }
    void exitSimple(Handler fn, uint32_t imm = 0, uint32_t imm2 = 0) {
        OpMeta mm; mm.reads = kLAll;
        emit(fn, thumb_ ? 1 : 0, insns_, 0, 0, imm, imm2, mm);
    }

    // Finish: append a fall-through exit if needed and run block-local passes.
    void finish(uint32_t endAddr, bool needFallthrough) {
        if (needFallthrough) exitDirect(endAddr, thumb_);
        blk_.end = endAddr;
        blk_.guest_insns = insns_;
        eliminateDeadFlags();
        fuseBranches();
#ifndef HVM_NO_FUSEOPS
        fuseOps();
#endif
#ifndef HVM_NO_PAIRS
        fusePairs();
#endif
    }

private:
    void fusePairs() {
        auto& ops = blk_.ops;
        for (size_t i = 0; i + 1 < ops.size();) {
            const int a = hotIndex(ops[i].fn);
            const int b = a >= 0 ? hotIndex(ops[i + 1].fn) : -1;
            if (b < 0) { ++i; continue; }
            ops[i].fn = kPairTable[size_t(a)][size_t(b)];
            i += 2;
        }
    }

    // Branch fusion (after flag liveness, which still needs the unfused ops):
    //   CondSkip(cond, 1); BranchDirect        -> BranchCond
    //   CbSkip; BranchDirect                   -> BranchCbz
    //   CMP/CMN/TST (imm or plain reg); BranchCond -> CmpBranch
    // These only ever appear at the end of a block (a conditional branch ends
    // it), so no CondSkip span crosses the ops being removed.
    static Handler cmpBranchFor(Handler alu, const Op& o, bool& reg) {
        struct Pat { int opc; int kind; Handler fused; bool reg; };
        static const Pat pats[] = {
            {kCMP, kOp2Imm, &H_CmpBranch<kCmpCMP, false>, false}, {kCMP, kOp2ShImmLSL, &H_CmpBranch<kCmpCMP, true>, true},
            {kCMN, kOp2Imm, &H_CmpBranch<kCmpCMN, false>, false}, {kCMN, kOp2ShImmLSL, &H_CmpBranch<kCmpCMN, true>, true},
            {kTST, kOp2Imm, &H_CmpBranch<kCmpTST, false>, false}, {kTST, kOp2ShImmLSL, &H_CmpBranch<kCmpTST, true>, true},
        };
        for (const Pat& p : pats) {
            if (alu != kAluTable[p.opc][1][p.kind]) continue;
            if (p.kind == kOp2Imm && p.opc == kTST && o.d != 0) return nullptr;   // TST imm that sets carry
            if (p.kind == kOp2ShImmLSL && o.imm != 0) return nullptr;            // shifted register
            reg = p.reg;
            return p.fused;
        }
        return nullptr;
    }

    void fuseBranches() {
        auto& ops = blk_.ops;
        for (size_t i = 0; i + 1 < ops.size(); ++i) {
            const bool condBr = ops[i].fn == &H_CondSkip && ops[i].imm == 1 && ops[i + 1].fn == &H_BranchDirect;
            const bool cbz = ops[i].fn == &H_CbSkip && ops[i + 1].fn == &H_BranchDirect;
            if (!condBr && !cbz) continue;
            Op br = ops[i + 1];
            if (cbz) {
                br.fn = &H_BranchCbz;
                br.c = ops[i].a;
                br.d = ops[i].b;
            } else {
                br.fn = &H_BranchCond;
                br.c = ops[i].a;
                bool reg = false;
                Handler fused = (i > 0 && !meta_[i - 1].guarded) ? cmpBranchFor(ops[i - 1].fn, ops[i - 1], reg) : nullptr;
                if (fused) {
                    const Op& cmp = ops[i - 1];
                    Op f = br;
                    f.fn = fused;
                    if (fused == &H_CmpBranch<kCmpCMP, false> || fused == &H_CmpBranch<kCmpCMP, true>) {
                        // CMP: a handler specialised on the condition.
                        static constexpr Handler byCond[2][14] = {
#define HVM_CC(R) { &H_CmpBranchC<0, R>, &H_CmpBranchC<1, R>, &H_CmpBranchC<2, R>, &H_CmpBranchC<3, R>, \
                    &H_CmpBranchC<4, R>, &H_CmpBranchC<5, R>, &H_CmpBranchC<6, R>, &H_CmpBranchC<7, R>, \
                    &H_CmpBranchC<8, R>, &H_CmpBranchC<9, R>, &H_CmpBranchC<10, R>, &H_CmpBranchC<11, R>, \
                    &H_CmpBranchC<12, R>, &H_CmpBranchC<13, R> }
                            HVM_CC(false), HVM_CC(true)
#undef HVM_CC
                        };
                        if (ops[i].a < 14) f.fn = byCond[reg][ops[i].a];
                    }
                    f.c = cmp.b;                       // rn
                    f.d = ops[i].a;                    // cond
                    f.imm2 = reg ? cmp.c : cmp.imm;    // rm or immediate
                    ops[i - 1] = f;
                    ops.erase(ops.begin() + long(i), ops.begin() + long(i) + 2);
                    meta_.erase(meta_.begin() + long(i), meta_.begin() + long(i) + 2);
                    continue;
                }
                // SUBS / ADDS (imm or unshifted reg) + BranchCond -> SubBranchC / AddBranchC
                if (i > 0 && !meta_[i - 1].guarded && ops[i].a < 14) {
                    const Op& alu = ops[i - 1];
                    const bool subImm = alu.fn == kAluTable[kSUB][1][kOp2Imm];
                    const bool subReg = alu.fn == kAluTable[kSUB][1][kOp2ShImmLSL] && alu.imm == 0;
                    const bool addImm = alu.fn == kAluTable[kADD][1][kOp2Imm];
                    const bool addReg = alu.fn == kAluTable[kADD][1][kOp2ShImmLSL] && alu.imm == 0;
                    if (subImm || subReg || addImm || addReg) {
                        static constexpr Handler subByCond[2][14] = {
#define HVM_SC(R) { &H_SubBranchC<0, R>, &H_SubBranchC<1, R>, &H_SubBranchC<2, R>, &H_SubBranchC<3, R>, \
                    &H_SubBranchC<4, R>, &H_SubBranchC<5, R>, &H_SubBranchC<6, R>, &H_SubBranchC<7, R>, \
                    &H_SubBranchC<8, R>, &H_SubBranchC<9, R>, &H_SubBranchC<10, R>, &H_SubBranchC<11, R>, \
                    &H_SubBranchC<12, R>, &H_SubBranchC<13, R> }
                            HVM_SC(false), HVM_SC(true)
#undef HVM_SC
                        };
                        static constexpr Handler addByCond[2][14] = {
#define HVM_AC(R) { &H_AddBranchC<0, R>, &H_AddBranchC<1, R>, &H_AddBranchC<2, R>, &H_AddBranchC<3, R>, \
                    &H_AddBranchC<4, R>, &H_AddBranchC<5, R>, &H_AddBranchC<6, R>, &H_AddBranchC<7, R>, \
                    &H_AddBranchC<8, R>, &H_AddBranchC<9, R>, &H_AddBranchC<10, R>, &H_AddBranchC<11, R>, \
                    &H_AddBranchC<12, R>, &H_AddBranchC<13, R> }
                            HVM_AC(false), HVM_AC(true)
#undef HVM_AC
                        };
                        const bool isReg = subReg || addReg;
                        Op f = br;
                        f.fn = (subImm || subReg) ? subByCond[isReg][ops[i].a] : addByCond[isReg][ops[i].a];
                        f.c = alu.b;                       // rn
                        f.d = alu.a;                       // rd
                        f.imm2 = isReg ? alu.c : alu.imm;  // rm or immediate
                        ops[i - 1] = f;
                        ops.erase(ops.begin() + long(i), ops.begin() + long(i) + 2);
                        meta_.erase(meta_.begin() + long(i), meta_.begin() + long(i) + 2);
                        continue;
                    }
                }
            }
            ops[i] = br;
            ops.erase(ops.begin() + long(i) + 1);
            meta_.erase(meta_.begin() + long(i) + 1);
        }
    }

    // Intra-block superinstruction fusion on unguarded ops (never crosses a CondSkip/CbSkip span).
    void fuseOps() {
        auto& ops = blk_.ops;
        const Handler ldrWImm = memHandler(kMemW, true, kAmOffset, false);
        const Handler strWImm = memHandler(kMemW, false, kAmOffset, false);
        const Handler movRegAlu = kAluTable[kMOV][0][kOp2ShImmLSL];
        const Handler addRegAlu = kAluTable[kADD][0][kOp2ShImmLSL];
        const Handler addImmAlu = kAluTable[kADD][0][kOp2Imm];
        const Handler subImmAlu = kAluTable[kSUB][0][kOp2Imm];

        for (size_t i = 0; i + 1 < ops.size(); ++i) {
            if (meta_[i].guarded || meta_[i + 1].guarded) continue;
            Op& a = ops[i];
            const Op& b = ops[i + 1];

            // 1. MOVW rd, #lo16 + MOVT rd, #hi16 -> H_MovImm rd, #(lo16 | (hi16 << 16))
            if (a.fn == &H_MovImm && b.fn == &H_Movt && a.a == b.a) {
                a.imm = (a.imm & 0xFFFFu) | (b.imm << 16);
                ops.erase(ops.begin() + long(i) + 1);
                meta_.erase(meta_.begin() + long(i) + 1);
                --i;
                continue;
            }

            // 2. BL / BLX: H_MovImm(kLR, ret) + BranchDirect / BranchDirectX / BranchIndirect
            if (a.fn == &H_MovImm && a.a == kLR) {
                if (b.fn == &H_BranchDirect || b.fn == &H_BranchDirectX || b.fn == &H_BranchIndirect) {
                    const uint32_t lrVal = a.imm;
                    Op f = b;
                    f.fn = (b.fn == &H_BranchDirect) ? &H_CallDirect
                         : (b.fn == &H_BranchDirectX) ? &H_CallDirectX : &H_CallIndirect;
                    f.imm2 = lrVal;
                    ops[i] = f;
                    ops.erase(ops.begin() + long(i) + 1);
                    meta_.erase(meta_.begin() + long(i) + 1);
                    continue;
                }
            }

            // 3. Epilogue POP/LDM {..., pc}: H_Multi<true> (with PC in list) + H_BranchIndirect(kPC)
            if (a.fn == &H_Multi<true> && (a.imm & (1u << kPC)) && b.fn == &H_BranchIndirect && b.a == kPC) {
                a.fn = &H_MultiPopPc;
                a.a = b.b;   // insns retired
                a.c = b.c;   // interwork mode
                ops.erase(ops.begin() + long(i) + 1);
                meta_.erase(meta_.begin() + long(i) + 1);
                continue;
            }

            // 4. Consecutive 32-bit offset loads, or LDR + ADD/SUB:
            if (a.fn == ldrWImm) {
                if (b.fn == ldrWImm && a.a != b.b && a.a != b.a) {
                    a.fn = &H_Ldr2Imm;
                    a.c = b.a;      // rt2
                    a.d = b.b;      // rn2
                    a.imm2 = b.imm; // off2
                    ops.erase(ops.begin() + long(i) + 1);
                    meta_.erase(meta_.begin() + long(i) + 1);
                    continue;
                }
                if (b.fn == addRegAlu && b.imm == 0) {
                    a.fn = &H_LdrAddReg;
                    a.c = b.a;      // rd
                    a.d = b.b;      // rx
                    a.imm2 = b.c;   // rm
                    ops.erase(ops.begin() + long(i) + 1);
                    meta_.erase(meta_.begin() + long(i) + 1);
                    continue;
                }
                if (b.fn == addImmAlu || b.fn == subImmAlu) {
                    a.fn = &H_LdrAddImm;
                    a.c = b.a;      // rd
                    a.d = b.b;      // rx
                    a.imm2 = b.fn == subImmAlu ? (0u - b.imm) : b.imm;
                    ops.erase(ops.begin() + long(i) + 1);
                    meta_.erase(meta_.begin() + long(i) + 1);
                    continue;
                }
            }

            // 5. Consecutive 32-bit offset stores, or STR + ADD/SUB:
            if (a.fn == strWImm) {
                if (b.fn == strWImm) {
                    a.fn = &H_Str2Imm;
                    a.c = b.a;      // rt2
                    a.d = b.b;      // rn2
                    a.imm2 = b.imm; // off2
                    ops.erase(ops.begin() + long(i) + 1);
                    meta_.erase(meta_.begin() + long(i) + 1);
                    continue;
                }
                if (b.fn == addImmAlu || b.fn == subImmAlu) {
                    a.fn = &H_StrAddImm;
                    a.c = b.a;      // rd
                    a.d = b.b;      // rx
                    a.imm2 = b.fn == subImmAlu ? (0u - b.imm) : b.imm;
                    ops.erase(ops.begin() + long(i) + 1);
                    meta_.erase(meta_.begin() + long(i) + 1);
                    continue;
                }
            }

            // 6. Plain MOV rd1, rm1 + plain ADD rd2, rn2, rm2 (or plain MOV rd2, rm2)
            const bool aIsMov = (a.fn == &H_MovReg) || (a.fn == movRegAlu && a.imm == 0);
            if (aIsMov) {
                const uint8_t rm1 = (a.fn == &H_MovReg) ? a.b : a.c;
                const bool bIsMov = (b.fn == &H_MovReg) || (b.fn == movRegAlu && b.imm == 0);
                if (bIsMov) {
                    const uint8_t rm2 = (b.fn == &H_MovReg) ? b.b : b.c;
                    a.fn = &H_Mov2Reg;
                    a.b = rm1;
                    a.c = b.a;
                    a.d = rm2;
                    ops.erase(ops.begin() + long(i) + 1);
                    meta_.erase(meta_.begin() + long(i) + 1);
                    continue;
                }
                if (b.fn == addRegAlu && b.imm == 0) {
                    a.fn = &H_MovAddReg;
                    a.b = rm1;
                    a.c = b.a;   // rd2
                    a.d = b.b;   // rn2
                    a.imm = b.c; // rm2
                    ops.erase(ops.begin() + long(i) + 1);
                    meta_.erase(meta_.begin() + long(i) + 1);
                    continue;
                }
            }

            // 7. Consecutive ADD/SUB immediate (no flags):
            const bool aAdd = a.fn == addImmAlu || a.fn == subImmAlu;
            const bool bAdd = b.fn == addImmAlu || b.fn == subImmAlu;
            if (aAdd && bAdd) {
                const uint32_t imm1 = a.fn == subImmAlu ? (0u - a.imm) : a.imm;
                const uint32_t imm2 = b.fn == subImmAlu ? (0u - b.imm) : b.imm;
                a.fn = &H_Add2Imm;
                a.c = b.a;
                a.d = b.b;
                a.imm = imm1;
                a.imm2 = imm2;
                ops.erase(ops.begin() + long(i) + 1);
                meta_.erase(meta_.begin() + long(i) + 1);
                continue;
            }
        }
    }

    // Backward liveness over the four condition flags. Flags are assumed live
    // at every exit, so this is always safe; it removes the flag computation
    // from the (very common) S-suffixed ops whose flags are overwritten before
    // anything reads them, and turns dead CMP/TST into no-ops.
    void eliminateDeadFlags() {
        uint8_t live = kLAll;
        for (size_t i = blk_.ops.size(); i-- > 0;) {
            OpMeta& mm = meta_[i];
            uint8_t w = mm.writes;
            if (w && mm.noflags && !(live & w)) {
                blk_.ops[i].fn = mm.noflags;
                w = 0;
            }
            const uint8_t kill = mm.guarded ? 0 : w;
            live = uint8_t((live & ~kill) | mm.reads);
        }
    }

    Block&              blk_;
    std::vector<OpMeta> meta_;
    bool     thumb_;
    uint32_t pc_;
    uint32_t insns_ = 0;
    int      condDepth_ = 0;
    bool     pcValid_ = false;
    uint32_t pcValue_ = 0;
};

// Helper used by both decoders to emit a data-processing op with correct
// flag metadata and a no-flags twin for dead-flag elimination.
inline void emitAlu(BlockBuilder& bb, int opc, bool s, int kind,
                    uint32_t rd, uint32_t rn, uint32_t rm, uint32_t rs_or_carry, uint32_t imm) {
    OpMeta mm;
    const bool logical = aluIsLogical(opc);
    if (opc == kADC || opc == kSBC || opc == kRSC) mm.reads |= kLC;
    if (kind == kOp2ShImmRRX) mm.reads |= kLC;
    if (s) {
        if (logical) {
            mm.writes = kLN | kLZ;
            const bool carryKept = (kind == kOp2Imm && rs_or_carry == 0) ||
                                   (kind == kOp2ShImmLSL && imm == 0);
            const bool regShift = kind >= kOp2ShRegLSL;
            if (!carryKept) mm.writes |= kLC;
            if (regShift) mm.reads |= kLC;   // amount may be zero -> carry kept
        } else {
            mm.writes = kLAll;
        }
        mm.noflags = kAluTable[opc][0][kind];
    }
    bb.emit(kAluTable[opc][s][kind], rd, rn, rm, rs_or_carry, imm, 0, mm);
}

} // namespace Hydrogen::CPU
