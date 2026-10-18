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
        emit(&H_BranchDirect, thumb & 1, insns_, 0, 0, target & (thumb ? ~1u : ~3u), 0, mm);
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
    }

private:
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
