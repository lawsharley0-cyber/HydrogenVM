// ArgonVM — minimal ELF32 (ARM, little-endian) loader.
//
// Today this loads statically linked executables (PT_LOAD segments + symbol
// table), which is what the test suite uses. It is also the starting point for
// Phase 3 (Android armeabi-v7a lib*.so), which will add PT_DYNAMIC parsing,
// R_ARM_* relocations and import binding to HLE stubs.
#pragma once
#include "core/memory/GuestArena.hpp"
#include <string>
#include <unordered_map>
#include <vector>

namespace Argon::Loader {

struct ElfImage {
    uint32_t entry = 0;
    uint32_t load_bias = 0;
    uint32_t lowest = 0xFFFFFFFFu, highest = 0;
    std::unordered_map<std::string, uint32_t> symbols;   // name -> address (bit0 = Thumb)
};

// Throws std::runtime_error on malformed input.
ElfImage loadElf(Memory::GuestArena& mem, const std::vector<uint8_t>& file, uint32_t bias = 0);
std::vector<uint8_t> readFile(const std::string& path);

} // namespace Argon::Loader
