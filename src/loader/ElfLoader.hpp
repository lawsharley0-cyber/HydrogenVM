// HydrogenVM — minimal ELF32 (ARM, little-endian) loader.
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

namespace Hydrogen::Loader {

struct ElfImage {
    uint32_t entry = 0;
    uint32_t load_bias = 0;
    uint32_t lowest = 0xFFFFFFFFu, highest = 0;
    std::unordered_map<std::string, uint32_t> symbols;   // name -> address (bit0 = Thumb)
};

// A shared object (Android lib*.so) loaded at `bias`: segments mapped, R_ARM_RELATIVE
// relocations applied, and the relocations that need another library's symbol listed
// for the caller to bind.
struct ElfImport {
    uint32_t address = 0;       // the word to write
    uint32_t type = 0;          // R_ARM_ABS32 (2), R_ARM_GLOB_DAT (21), R_ARM_JUMP_SLOT (22)
    std::string symbol;         // empty: no symbol (should not happen for these types)
    bool weak = false;
};
struct ElfShared {
    ElfImage image;                                       // symbols: the dynamic symbols it defines
    std::string soname;
    std::vector<std::string> needed;
    std::vector<ElfImport> imports;
    std::vector<uint32_t> initializers;                   // DT_INIT, then DT_INIT_ARRAY, in order
    std::vector<uint32_t> finalizers;                     // DT_FINI_ARRAY (run in reverse), then DT_FINI
    uint32_t exidx = 0, exidxCount = 0;                   // PT_ARM_EXIDX: entries of 8 bytes
};
ElfShared loadElfShared(Memory::GuestArena& mem, const std::vector<uint8_t>& file, uint32_t bias);

// Throws std::runtime_error on malformed input.
ElfImage loadElf(Memory::GuestArena& mem, const std::vector<uint8_t>& file, uint32_t bias = 0);
std::vector<uint8_t> readFile(const std::string& path);

} // namespace Hydrogen::Loader
