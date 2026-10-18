// HydrogenVM — execution engine: block cache, dispatcher, HLE table.
#pragma once
#include "Cpu.hpp"
#include "../memory/GuestArena.hpp"
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace Hydrogen::CPU {

class Engine {
public:
    using HleFn = std::function<void(Cpu&)>;

    explicit Engine(Memory::GuestArena& mem);
    ~Engine();

    Memory::GuestArena& memory() { return mem_; }

    // Register a host function. Returns the guest address that, when branched
    // to (in either ARM or Thumb state), calls `fn` and then returns to LR.
    // `fn` may set cpu.r[15]/cpu.thumb itself and request a different
    // continuation by setting `cpu.exit` (e.g. Exit::Halt).
    uint32_t registerHle(std::string name, HleFn fn);
    const std::string& hleName(uint32_t index) const { return hle_[index].name; }
    bool isHleAddress(uint32_t addr) const {
        return addr >= Memory::GuestArena::kHleBase &&
               (addr - Memory::GuestArena::kHleBase) / 4 < hle_.size();
    }

    // Run guest code on `cpu` starting at cpu.r[15] (cpu.thumb selects state)
    // until something exits or `budget` guest instructions have retired.
    Exit run(Cpu& cpu, int64_t budget = INT64_MAX);

    // Discard all decoded blocks. Must not be called while any Cpu is running.
    void flushCache();
    // Discard blocks overlapping [addr, addr+size). Same restriction.
    void invalidateRange(uint32_t addr, uint32_t size);

    // Called from IR handlers.
    Block* enterBlock(Cpu* c, uint32_t key);
    void   callHle(Cpu* c, uint32_t index) { hle_[index].fn(*c); }

    struct Stats { uint64_t blocks_decoded = 0, guest_insns_decoded = 0, ops_emitted = 0; };
    const Stats& stats() const { return stats_; }

private:
    Block* lookupOrDecode(uint32_t key);
    std::unique_ptr<Block> decodeBlock(uint32_t key);
    void decodeArmBlock(Block& b, uint32_t pc);
    void decodeThumbBlock(Block& b, uint32_t pc);

    static constexpr size_t kFastSize = 1u << 14;
    struct FastEntry { uint32_t key; Block* block; };

    Memory::GuestArena& mem_;
    std::unordered_map<uint32_t, std::unique_ptr<Block>> blocks_;
    std::vector<FastEntry> fast_;
    std::mutex mu_;

    struct HleEntry { std::string name; HleFn fn; };
    std::vector<HleEntry> hle_;
    Stats stats_;
};

} // namespace Hydrogen::CPU
