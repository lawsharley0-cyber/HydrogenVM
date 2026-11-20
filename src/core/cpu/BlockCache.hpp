// HydrogenVM — persistent decoded ARM/Thumb block cache (Phase E1).
//
// Stores decoded Op records per game/build on disk with NO host function
// pointers (handlers are encoded as dense registry IDs) and NO block links.
// Every cached block carries a 64-bit FNV-1a hash of the guest source bytes
// [start, end) and is verified lazily on first use. Pure data, not machine code.
#pragma once
#include "Cpu.hpp"
#include "../memory/GuestArena.hpp"
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace Hydrogen::CPU {

struct SerializedOp {
    uint32_t handlerId = 0;
    uint8_t  a = 0, b = 0, c = 0, d = 0;
    uint32_t imm = 0;
    uint32_t imm2 = 0;
    uint32_t pc = 0;
};
static_assert(sizeof(SerializedOp) == 20, "SerializedOp wire size");

struct CachedBlock {
    uint32_t key = 0;
    uint32_t end = 0;
    uint32_t guest_insns = 0;
    uint64_t codeHash = 0;
    std::vector<SerializedOp> ops;
};

struct BlockCacheStats {
    size_t diskEntries = 0;
    size_t diskHits = 0;
    size_t diskHashMismatches = 0;
    size_t diskSaved = 0;
};

// 64-bit FNV-1a hash of guest instruction bytes in [start, end).
// Returns 0 if the range is empty, oversized, or contains an unreadable byte.
uint64_t hashGuestCode(const Memory::GuestArena& mem, uint32_t start, uint32_t end);

// Persist decoded blocks to `path` atomically (temp file + rename).
// HLE blocks, blocks with unmapped source bytes, or blocks using handlers
// outside the portable registry are skipped cleanly.
bool saveBlockCacheFile(const std::string& path,
                        const std::vector<const Block*>& blocks,
                        const Memory::GuestArena& mem,
                        size_t& savedOut);

// Load cached block records from `path` into `out` (lazy — does not touch guest
// memory until materializeCachedBlock() is called on first execution).
// Returns false if the file is missing, truncated, corrupt, or from a different build.
bool loadBlockCacheFile(const std::string& path,
                        std::unordered_map<uint32_t, CachedBlock>& out);

// Verify `rec.codeHash` against live guest memory and reconstruct a runnable Block.
// Returns nullptr if the guest code hash changed or any handler ID is invalid.
std::unique_ptr<Block> materializeCachedBlock(const CachedBlock& rec,
                                              const Memory::GuestArena& mem);

} // namespace Hydrogen::CPU
