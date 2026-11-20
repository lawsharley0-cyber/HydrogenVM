#include "BlockCache.hpp"
#include "BlockBuilder.hpp"
#include <cstdio>
#include <cstring>
#include <fstream>
#include <string_view>

#ifndef HVM_COMMIT
#define HVM_COMMIT "unknown"
#endif

namespace Hydrogen::CPU {

namespace {

constexpr uint32_t kCacheMagic = 0x43425648u; // "HVBC"
constexpr uint32_t kCacheVersion = 2u;
constexpr uint32_t kMaxCachedBlocks = 250000u;
constexpr uint32_t kMaxOpsPerBlock = 4096u;
constexpr uint32_t kMaxCodeBytesPerBlock = 16384u;

uint64_t fnv1a64(const void* data, size_t len, uint64_t seed = 14695981039346656037ULL) {
    const auto* p = static_cast<const uint8_t*>(data);
    uint64_t h = seed;
    for (size_t i = 0; i < len; ++i) {
        h ^= uint64_t(p[i]);
        h *= 1099511628211ULL;
    }
    return h ? h : 1ULL;
}

struct HandlerRegistry {
    std::vector<Handler> byId;
    std::unordered_map<Handler, uint32_t> toId;
    uint64_t fingerprint = 0;

    void add(Handler h) {
        if (!h) return;
        if (toId.find(h) != toId.end()) return;
        const uint32_t id = uint32_t(byId.size());
        byId.push_back(h);
        toId.emplace(h, id);
    }

    template <int COND>
    void addBranchConds() {
        add(&H_CmpBranchC<COND, false>);
        add(&H_CmpBranchC<COND, true>);
        add(&H_SubBranchC<COND, false>);
        add(&H_SubBranchC<COND, true>);
        add(&H_AddBranchC<COND, false>);
        add(&H_AddBranchC<COND, true>);
    }

    template <int OP>
    void addVfp3() {
        add(&H_Vfp3<OP, false>);
        add(&H_Vfp3<OP, true>);
    }

    template <int OP>
    void addVfp2() {
        add(&H_Vfp2<OP, false>);
        add(&H_Vfp2<OP, true>);
    }

    HandlerRegistry() {
        // Core ALU table
        for (size_t o = 0; o < kAluTable.size(); ++o)
            for (size_t s = 0; s < 2; ++s)
                for (size_t k = 0; k < kAluTable[o][s].size(); ++k)
                    add(kAluTable[o][s][k]);

        // Memory load/store tables
        for (int kind = 0; kind < 5; ++kind)
            for (int ld = 0; ld < 2; ++ld)
                for (int am = 0; am < 3; ++am)
                    for (int reg = 0; reg < 2; ++reg)
                        add(memHandler(kind, ld != 0, am, reg != 0));
        for (int ld = 0; ld < 2; ++ld)
            for (int am = 0; am < 3; ++am)
                for (int reg = 0; reg < 2; ++reg)
                    add(memDualHandler(ld != 0, am, reg != 0));

        // Pair superinstructions
        for (size_t i = 0; i < kNumHot; ++i)
            for (size_t j = 0; j < kNumHot; ++j)
                add(kPairTable[i][j]);

        // Control flow, moves, fused branches & calls
        add(&H_Nop);
        add(&H_MovImm);
        add(&H_MovReg);
        add(&H_Movt);
        add(&H_CondSkip);
        add(&H_CbSkip);
        add(&H_TableBranch);
        add(&H_BranchDirect);
        add(&H_BranchDirectX);
        add(&H_BranchCond);
        add(&H_BranchCbz);
        add(&H_BranchIndirect);
        add(&H_CallDirect);
        add(&H_CallDirectX);
        add(&H_CallIndirect);
        add(&H_Multi<false>);
        add(&H_Multi<true>);
        add(&H_MultiPopPc);
        add(&H_Ldr2Imm);
        add(&H_Str2Imm);
        add(&H_LdrAddReg);
        add(&H_LdrAddImm);
        add(&H_StrAddImm);
        add(&H_Mov2Reg);
        add(&H_MovAddReg);
        add(&H_Add2Imm);
        add(&H_Svc);
        add(&H_Undefined);
        add(&H_Breakpoint);
        add(&H_FetchFault);
        add(&H_Fence);
        add(&H_Clrex);
        add(&H_Mrs);
        add(&H_Msr);
        add(&H_ReadTpid);
        add(&H_WriteTpid);
        add(&H_ReadGenericTimer);
        add(&H_ReadGenericTimerFreq);
        add(&H_Swp<false>); add(&H_Swp<true>);
        add(&H_Pkh);
        add(&H_Sel);
        add(&H_Usad8);
        add(&H_Parallel);
        add(&H_QArith<0>); add(&H_QArith<1>); add(&H_QArith<2>); add(&H_QArith<3>);

        // Specialized fused cmp/add/sub + branch
        addBranchConds<0>();  addBranchConds<1>();  addBranchConds<2>();  addBranchConds<3>();
        addBranchConds<4>();  addBranchConds<5>();  addBranchConds<6>();  addBranchConds<7>();
        addBranchConds<8>();  addBranchConds<9>();  addBranchConds<10>(); addBranchConds<11>();
        addBranchConds<12>(); addBranchConds<13>();
        add(&H_CmpBranch<0, false>); add(&H_CmpBranch<0, true>);
        add(&H_CmpBranch<1, false>); add(&H_CmpBranch<1, true>);
        add(&H_CmpBranch<2, false>); add(&H_CmpBranch<2, true>);
        add(&H_CmpBranch<3, false>); add(&H_CmpBranch<3, true>);

        // Bitfield, extend, bitop, mul, div, exclusive, sat
        add(&H_Bitfield<0>); add(&H_Bitfield<1>); add(&H_Bitfield<2>);
        for (int k = 0; k < 6; ++k) {
            // H_Extend<KIND, ADD>
            if (k == 0) { add(&H_Extend<0, false>); add(&H_Extend<0, true>); }
            if (k == 1) { add(&H_Extend<1, false>); add(&H_Extend<1, true>); }
            if (k == 2) { add(&H_Extend<2, false>); add(&H_Extend<2, true>); }
            if (k == 3) { add(&H_Extend<3, false>); add(&H_Extend<3, true>); }
            if (k == 4) { add(&H_Extend<4, false>); add(&H_Extend<4, true>); }
            if (k == 5) { add(&H_Extend<5, false>); add(&H_Extend<5, true>); }
        }
        add(&H_BitOp<0>); add(&H_BitOp<1>); add(&H_BitOp<2>);
        add(&H_BitOp<3>); add(&H_BitOp<4>);
        add(&H_Div<false>); add(&H_Div<true>);
        add(&H_Sat<0>); add(&H_Sat<1>); add(&H_Sat<2>); add(&H_Sat<3>);
        add(&H_Ldrex<0>); add(&H_Ldrex<1>); add(&H_Ldrex<2>); add(&H_Ldrex<3>);
        add(&H_Strex<0>); add(&H_Strex<1>); add(&H_Strex<2>); add(&H_Strex<3>);
        add(&H_Mul<0, false>); add(&H_Mul<0, true>);
        add(&H_Mul<1, false>); add(&H_Mul<1, true>);
        add(&H_Mul<2, false>); add(&H_Mul<2, true>);
        add(&H_MulLong<0, false>); add(&H_MulLong<0, true>);
        add(&H_MulLong<1, false>); add(&H_MulLong<1, true>);
        add(&H_MulLong<2, false>); add(&H_MulLong<2, true>);
        add(&H_MulLong<3, false>); add(&H_MulLong<3, true>);
        add(&H_MulLong<4, false>); add(&H_MulLong<4, true>);
        add(&H_MulHalf<0>); add(&H_MulHalf<1>); add(&H_MulHalf<2>);
        add(&H_MulHalf<3>); add(&H_MulHalf<4>); add(&H_MulHalf<5>);
        add(&H_MulHalf<6>);
        add(&H_MulDual<0>); add(&H_MulDual<1>); add(&H_MulDual<2>); add(&H_MulDual<3>);

        // VFP handlers
        addVfp3<0>(); addVfp3<1>(); addVfp3<2>(); addVfp3<3>();
        addVfp3<4>(); addVfp3<5>(); addVfp3<6>(); addVfp3<7>();
        addVfp3<8>(); addVfp3<9>(); addVfp3<10>(); addVfp3<11>();
        addVfp2<0>(); addVfp2<1>(); addVfp2<2>(); addVfp2<3>();
        add(&H_VfpMovImm<false>); add(&H_VfpMovImm<true>);
        add(&H_VfpCmp<false, false>); add(&H_VfpCmp<false, true>);
        add(&H_VfpCmp<true, false>);  add(&H_VfpCmp<true, true>);
        add(&H_VfpCmpToApsr<false, false>); add(&H_VfpCmpToApsr<false, true>);
        add(&H_VfpCmpToApsr<true, false>);  add(&H_VfpCmpToApsr<true, true>);
        add(&H_VfpCvtSD<false>); add(&H_VfpCvtSD<true>);
        add(&H_VfpCvtToInt<false, false, false>); add(&H_VfpCvtToInt<false, false, true>);
        add(&H_VfpCvtToInt<false, true, false>);  add(&H_VfpCvtToInt<false, true, true>);
        add(&H_VfpCvtToInt<true, false, false>);  add(&H_VfpCvtToInt<true, false, true>);
        add(&H_VfpCvtToInt<true, true, false>);   add(&H_VfpCvtToInt<true, true, true>);
        add(&H_VfpCvtFromInt<false, false>); add(&H_VfpCvtFromInt<false, true>);
        add(&H_VfpCvtFromInt<true, false>);  add(&H_VfpCvtFromInt<true, true>);
        add(&H_VfpCvtHalf<false>); add(&H_VfpCvtHalf<true>);
        add(&H_VLdrStr<false>); add(&H_VLdrStr<true>);
        add(&H_VLdmStm<false>); add(&H_VLdmStm<true>);
        add(&H_Vmrs); add(&H_Vmsr);
        add(&H_VmovToS); add(&H_VmovFromS);
        add(&H_Vmov2ToS); add(&H_Vmov2FromS);

        const std::string_view commit(HVM_COMMIT);
        uint64_t fp = fnv1a64(commit.data(), commit.size());
        const uint32_t count = uint32_t(byId.size());
        fp = fnv1a64(&count, sizeof(count), fp);
        const uint32_t opSz = uint32_t(sizeof(Op));
        fp = fnv1a64(&opSz, sizeof(opSz), fp);
        fingerprint = fp;
    }
};

const HandlerRegistry& registry() {
    static const HandlerRegistry reg;
    return reg;
}

struct FileHeader {
    uint32_t magic;
    uint32_t version;
    uint32_t opSize;
    uint32_t handlerCount;
    uint64_t buildFingerprint;
    uint32_t blockCount;
    uint32_t reserved;
};
static_assert(sizeof(FileHeader) == 32, "BlockCache FileHeader size");

struct BlockHeader {
    uint32_t key;
    uint32_t end;
    uint32_t guest_insns;
    uint32_t opCount;
    uint64_t codeHash;
    uint64_t recordHash;
};
static_assert(sizeof(BlockHeader) == 32, "BlockCache BlockHeader size");

uint64_t blockRecordHash(const BlockHeader& header, const std::vector<SerializedOp>& ops) {
    const auto headerHash = fnv1a64(&header, offsetof(BlockHeader, recordHash));
    return fnv1a64(ops.data(), ops.size() * sizeof(SerializedOp), headerHash);
}

} // namespace

uint64_t hashGuestCode(const Memory::GuestArena& mem, uint32_t start, uint32_t end) {
    if (end <= start) return 0;
    const uint32_t len = end - start;
    if (len > kMaxCodeBytesPerBlock) return 0;
    // A mapped page can still be inaccessible. Decline to cache code we cannot read.
    constexpr uint32_t kPage = Memory::GuestArena::kGuestPageSize;
    for (uint32_t p = start & ~(kPage - 1u); p < end; p += kPage) {
        if (!mem.isMapped(p) || !(mem.protAt(p) & Memory::kProtRead)) return 0;
        if (p > 0xFFFFFFFFu - kPage) break;
    }
    return fnv1a64(mem.base() + start, len);
}

bool saveBlockCacheFile(const std::string& path,
                        const std::vector<const Block*>& blocks,
                        const Memory::GuestArena& mem,
                        size_t& savedOut) {
    savedOut = 0;
    if (path.empty()) return false;
    const auto& reg = registry();

    std::vector<CachedBlock> serializable;
    serializable.reserve(blocks.size());
    for (const Block* b : blocks) {
        if (!b || !b->valid || b->ops.empty() || b->ops.size() > kMaxOpsPerBlock) continue;
        const uint32_t start = b->key & ~1u;
        if (start >= Memory::GuestArena::kHleBase) continue;
        const uint64_t codeHash = hashGuestCode(mem, start, b->end);
        if (!codeHash) continue;

        CachedBlock rec;
        rec.key = b->key;
        rec.end = b->end;
        rec.guest_insns = b->guest_insns;
        rec.codeHash = codeHash;
        rec.ops.reserve(b->ops.size());
        bool ok = true;
        for (const Op& op : b->ops) {
            auto it = reg.toId.find(op.fn);
            if (it == reg.toId.end()) { ok = false; break; }
            rec.ops.push_back(SerializedOp{it->second, op.a, op.b, op.c, op.d, op.imm, op.imm2, op.pc});
        }
        if (ok) serializable.push_back(std::move(rec));
        if (serializable.size() >= kMaxCachedBlocks) break;
    }

    const std::string tmp = path + ".tmp";
    std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
    if (!out) return false;

    FileHeader fh{};
    fh.magic = kCacheMagic;
    fh.version = kCacheVersion;
    fh.opSize = uint32_t(sizeof(Op));
    fh.handlerCount = uint32_t(reg.byId.size());
    fh.buildFingerprint = reg.fingerprint;
    fh.blockCount = uint32_t(serializable.size());
    fh.reserved = 0;
    out.write(reinterpret_cast<const char*>(&fh), sizeof(fh));

    for (const auto& rec : serializable) {
        BlockHeader bh{};
        bh.key = rec.key;
        bh.end = rec.end;
        bh.guest_insns = rec.guest_insns;
        bh.opCount = uint32_t(rec.ops.size());
        bh.codeHash = rec.codeHash;
        bh.recordHash = blockRecordHash(bh, rec.ops);
        out.write(reinterpret_cast<const char*>(&bh), sizeof(bh));
        out.write(reinterpret_cast<const char*>(rec.ops.data()),
                  std::streamsize(rec.ops.size() * sizeof(SerializedOp)));
    }
    out.flush();
    if (!out.good()) {
        out.close();
        std::remove(tmp.c_str());
        return false;
    }
    out.close();
    if (std::rename(tmp.c_str(), path.c_str()) != 0) {
        std::remove(tmp.c_str());
        return false;
    }
    savedOut = serializable.size();
    return true;
}

bool loadBlockCacheFile(const std::string& path,
                        std::unordered_map<uint32_t, CachedBlock>& out) {
    out.clear();
    if (path.empty()) return false;
    std::ifstream in(path, std::ios::binary);
    if (!in) return false;

    const auto& reg = registry();
    FileHeader fh{};
    if (!in.read(reinterpret_cast<char*>(&fh), sizeof(fh))) return false;
    if (fh.magic != kCacheMagic || fh.version != kCacheVersion ||
        fh.opSize != sizeof(Op) || fh.handlerCount != reg.byId.size() ||
        fh.buildFingerprint != reg.fingerprint || fh.blockCount > kMaxCachedBlocks) {
        return false;
    }

    out.reserve(fh.blockCount);
    for (uint32_t i = 0; i < fh.blockCount; ++i) {
        BlockHeader bh{};
        if (!in.read(reinterpret_cast<char*>(&bh), sizeof(bh))) { out.clear(); return false; }
        if (bh.opCount == 0 || bh.opCount > kMaxOpsPerBlock || bh.codeHash == 0) {
            out.clear();
            return false;
        }
        CachedBlock rec;
        rec.key = bh.key;
        rec.end = bh.end;
        rec.guest_insns = bh.guest_insns;
        rec.codeHash = bh.codeHash;
        rec.ops.resize(bh.opCount);
        if (!in.read(reinterpret_cast<char*>(rec.ops.data()),
                     std::streamsize(bh.opCount * sizeof(SerializedOp)))) {
            out.clear();
            return false;
        }
        if (blockRecordHash(bh, rec.ops) != bh.recordHash) { out.clear(); return false; }
        for (const auto& sop : rec.ops) {
            if (sop.handlerId >= reg.byId.size()) { out.clear(); return false; }
        }
        out.emplace(rec.key, std::move(rec));
    }
    return true;
}

std::unique_ptr<Block> materializeCachedBlock(const CachedBlock& rec,
                                              const Memory::GuestArena& mem) {
    const auto& reg = registry();
    const uint32_t start = rec.key & ~1u;
    if (start >= Memory::GuestArena::kHleBase) return nullptr;
    const uint64_t liveHash = hashGuestCode(mem, start, rec.end);
    if (!liveHash || liveHash != rec.codeHash) return nullptr;

    auto b = std::make_unique<Block>();
    b->key = rec.key;
    b->end = rec.end;
    b->guest_insns = rec.guest_insns;
    b->valid = true;
    b->ops.resize(rec.ops.size());
    for (size_t i = 0; i < rec.ops.size(); ++i) {
        const auto& sop = rec.ops[i];
        if (sop.handlerId >= reg.byId.size()) return nullptr;
        Op& op = b->ops[i];
        op.fn = reg.byId[sop.handlerId];
        op.a = sop.a;
        op.b = sop.b;
        op.c = sop.c;
        op.d = sop.d;
        op.imm = sop.imm;
        op.imm2 = sop.imm2;
        op.pc = sop.pc;
        op.link = nullptr;
    }
    return b;
}

} // namespace Hydrogen::CPU
