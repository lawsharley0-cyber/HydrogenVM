#include "loader/MachO.hpp"
#include <cstdio>
#include <vector>
#include <string>

using Bytes = std::vector<uint8_t>;
using Hydrogen::Loader::probeMachO;
using Hydrogen::Loader::ProbeResult;

void num32(Bytes& b, uint32_t v) {
    b.push_back(v & 0xff);
    b.push_back((v >> 8) & 0xff);
    b.push_back((v >> 16) & 0xff);
    b.push_back((v >> 24) & 0xff);
}

void num32_be(Bytes& b, uint32_t v) {
    b.push_back((v >> 24) & 0xff);
    b.push_back((v >> 16) & 0xff);
    b.push_back((v >> 8) & 0xff);
    b.push_back(v & 0xff);
}

int main() {
    int passed = 0, failed = 0;
    auto check = [&](const std::string& name, const Bytes& file, bool isMachO, bool hasArm32, bool encrypted, uint32_t cpuSubtype, const std::string& errorSnippet = "") {
        ProbeResult pr = probeMachO(file);
        bool ok = true;
        if (pr.isMachO != isMachO) { ok = false; std::printf("%s: isMachO %d != expected %d\n", name.c_str(), pr.isMachO, isMachO); }
        if (pr.hasArm32 != hasArm32) { ok = false; std::printf("%s: hasArm32 %d != expected %d\n", name.c_str(), pr.hasArm32, hasArm32); }
        if (pr.encrypted != encrypted) { ok = false; std::printf("%s: encrypted %d != expected %d\n", name.c_str(), pr.encrypted, encrypted); }
        if (pr.cpuSubtype != cpuSubtype) { ok = false; std::printf("%s: cpuSubtype %d != expected %d\n", name.c_str(), pr.cpuSubtype, cpuSubtype); }
        if (!errorSnippet.empty() && pr.error.find(errorSnippet) == std::string::npos) {
            ok = false; std::printf("%s: error '%s' does not contain '%s'\n", name.c_str(), pr.error.c_str(), errorSnippet.c_str());
        } else if (errorSnippet.empty() && !pr.error.empty()) {
            ok = false; std::printf("%s: unexpected error '%s'\n", name.c_str(), pr.error.c_str());
        }

        if (ok) {
            std::printf("PASS %s\n", name.c_str());
            ++passed;
        } else {
            std::printf("FAIL %s\n", name.c_str());
            ++failed;
        }
    };

    // 1. Not a Mach-O file
    check("empty", {}, false, false, false, 0, "Mach-O: truncated (offset 0)");
    check("random_bytes", {1, 2, 3, 4}, false, false, false, 0, "not a Mach-O file");

    // 2. Simple MH_MAGIC
    Bytes simple_armv7;
    num32(simple_armv7, 0xfeedface); // MH_MAGIC (little endian)
    num32(simple_armv7, 12); // CPU_TYPE_ARM
    num32(simple_armv7, 9); // CPU_SUBTYPE_ARM_V7
    num32(simple_armv7, 2); // MH_EXECUTE
    num32(simple_armv7, 0); // ncmds
    num32(simple_armv7, 0); // sizeofcmds
    num32(simple_armv7, 0); // flags
    simple_armv7.resize(28, 0);
    check("simple_armv7", simple_armv7, true, true, false, 9);

    // 3. Simple MH_MAGIC, not ARM
    Bytes simple_x86;
    num32(simple_x86, 0xfeedface); // MH_MAGIC
    num32(simple_x86, 7); // CPU_TYPE_I386
    num32(simple_x86, 3); // CPU_SUBTYPE_I386_ALL
    num32(simple_x86, 2); // MH_EXECUTE
    num32(simple_x86, 0); // ncmds
    num32(simple_x86, 0); // sizeofcmds
    num32(simple_x86, 0); // flags
    check("simple_x86", simple_x86, false, false, false, 0, "not a 32-bit ARM binary");

    // 4. FAT binary with ARM slice
    Bytes fat_binary;
    num32_be(fat_binary, 0xcafebabe); // FAT_MAGIC
    num32_be(fat_binary, 2); // nfat_arch
    // slice 1 (not arm)
    num32_be(fat_binary, 7); // CPU_TYPE_I386
    num32_be(fat_binary, 3); // CPU_SUBTYPE_I386_ALL
    num32_be(fat_binary, 4096); // offset
    num32_be(fat_binary, 28); // size
    num32_be(fat_binary, 12); // align
    // slice 2 (armv7)
    num32_be(fat_binary, 12); // CPU_TYPE_ARM
    num32_be(fat_binary, 9); // CPU_SUBTYPE_ARM_V7
    num32_be(fat_binary, 8192); // offset
    num32_be(fat_binary, 28); // size
    num32_be(fat_binary, 12); // align

    // Add padded bytes so slice offsets are valid
    fat_binary.resize(8192, 0);
    // Add armv7 slice
    Bytes fat_armv7;
    num32(fat_armv7, 0xfeedface); // MH_MAGIC
    num32(fat_armv7, 12); // CPU_TYPE_ARM
    num32(fat_armv7, 9); // CPU_SUBTYPE_ARM_V7
    num32(fat_armv7, 2); // MH_EXECUTE
    num32(fat_armv7, 0); // ncmds
    num32(fat_armv7, 0); // sizeofcmds
    num32(fat_armv7, 0); // flags
    fat_armv7.resize(28, 0); // padding for mach header
    fat_binary.insert(fat_binary.end(), fat_armv7.begin(), fat_armv7.end());
    check("fat_binary_armv7", fat_binary, true, true, false, 9);

    // 5. Encrypted binary
    Bytes encrypted_armv7;
    num32(encrypted_armv7, 0xfeedface); // MH_MAGIC
    num32(encrypted_armv7, 12); // CPU_TYPE_ARM
    num32(encrypted_armv7, 9); // CPU_SUBTYPE_ARM_V7
    num32(encrypted_armv7, 2); // MH_EXECUTE
    num32(encrypted_armv7, 1); // ncmds
    num32(encrypted_armv7, 20); // sizeofcmds
    num32(encrypted_armv7, 0); // flags

    // LC_ENCRYPTION_INFO
    num32(encrypted_armv7, 0x21); // LC_ENCRYPTION_INFO
    num32(encrypted_armv7, 20); // cmdsize
    num32(encrypted_armv7, 4096); // cryptoff
    num32(encrypted_armv7, 8192); // cryptsize
    num32(encrypted_armv7, 1); // cryptid (encrypted!)
    check("encrypted_armv7", encrypted_armv7, true, true, true, 9);

    // 6. MH_DYLIB loadMachO with slide, LC_ROUTINES, LC_LOAD_WEAK_DYLIB and export trie
    {
        Bytes dylib(0x1100, 0);
        auto put32 = [&](size_t off, uint32_t v) {
            dylib[off + 0] = v & 0xff;
            dylib[off + 1] = (v >> 8) & 0xff;
            dylib[off + 2] = (v >> 16) & 0xff;
            dylib[off + 3] = (v >> 24) & 0xff;
        };
        auto putStr = [&](size_t off, const char* s) {
            for (size_t i = 0; s[i]; ++i) dylib[off + i] = uint8_t(s[i]);
        };
        // MachHeader: MH_DYLIB (6), 4 load commands: LC_SEGMENT(__TEXT), LC_ROUTINES, LC_LOAD_WEAK_DYLIB, LC_DYLD_INFO_ONLY
        // cmd sizes: 56 + 40 + 32 + 48 = 176
        put32(0, 0xfeedface);
        put32(4, 12);
        put32(8, 9);
        put32(12, 6); // MH_DYLIB
        put32(16, 4); // ncmds
        put32(20, 176); // sizeofcmds
        put32(24, 0);
        // cmd 0 (off 28): LC_SEGMENT __TEXT vmaddr=0 vmsize=0x1000 fileoff=0 filesize=0x1000
        put32(28, 0x1);
        put32(32, 56);
        putStr(36, "__TEXT");
        put32(52, 0);
        put32(56, 0x1000);
        put32(60, 0);
        put32(64, 0x1000);
        put32(68, 7);
        put32(72, 5);
        put32(76, 0);
        put32(80, 0);
        // cmd 1 (off 84): LC_ROUTINES (0x11), cmdsize=40, init_address=0x0401 (Thumb)
        put32(84, 0x11);
        put32(88, 40);
        put32(92, 0x0401);
        // cmd 2 (off 124): LC_LOAD_WEAK_DYLIB (0x80000018), cmdsize=32, name="@rpath/w.dylib"
        put32(124, 0x80000018u);
        put32(128, 32);
        put32(132, 24);
        putStr(148, "@rpath/w.dylib");
        // cmd 3 (off 156): LC_DYLD_INFO_ONLY (0x80000022), cmdsize=48, export_off=0x1000, export_size=17
        put32(156, 0x80000022u);
        put32(160, 48);
        put32(196, 0x1000);
        put32(200, 17);
        const uint8_t trieValid[17] = {
            0x00, 0x01, '_', 't', 'r', 'i', 'e', '_', 'f', 'n', 0x00, 12,
            0x03, 0x00, 0xa0, 0x08, 0x00
        };
        for (size_t i = 0; i < sizeof(trieValid); ++i) dylib[0x1000 + i] = trieValid[i];

        Hydrogen::Memory::GuestArena arena;
        auto img = Hydrogen::Loader::loadMachO(arena, dylib, "test.dylib", {0x04000000u});
        bool dylibOk = img.fileType == 6 && img.slide == 0x04000000u &&
                       img.dylibs.size() == 1 && img.dylibWeak.size() == 1 && img.dylibWeak[0] &&
                       img.initializers.size() == 1 && img.initializers[0] == 0x04000401u &&
                       img.exports.count("_trie_fn") == 1 && img.exports["_trie_fn"] == 0x04000420u;

        // Shared terminal export trie: root node with two child edges ("_a" and "_b") both pointing to
        // offset 10 (terminal at 0x0420) must retain both exports.
        const uint8_t sharedTrie[15] = {
            0x00, 0x02, '_', 'a', 0x00, 10, '_', 'b', 0x00, 10,
            0x03, 0x00, 0xa0, 0x08, 0x00
        };
        put32(200, sizeof(sharedTrie));
        for (size_t i = 0; i < sizeof(sharedTrie); ++i) dylib[0x1000 + i] = sharedTrie[i];
        Hydrogen::Memory::GuestArena sharedArena;
        auto sharedImg = Hydrogen::Loader::loadMachO(sharedArena, dylib, "shared.dylib", {0x04000000u});
        dylibOk = dylibOk && sharedImg.exports.count("_a") == 1 && sharedImg.exports.count("_b") == 1 &&
                  sharedImg.exports["_a"] == 0x04000420u && sharedImg.exports["_b"] == 0x04000420u;

        // Self-referencing export trie (root node with two child edges pointing back to offset 0):
        // must terminate immediately instead of exploring 2^256 cyclic paths.
        const uint8_t cyclicTrie[8] = {
            0x00, 0x02, 'a', 0x00, 0x00, 'b', 0x00, 0x00
        };
        put32(200, sizeof(cyclicTrie));
        for (size_t i = 0; i < sizeof(cyclicTrie); ++i) dylib[0x1000 + i] = cyclicTrie[i];
        Hydrogen::Memory::GuestArena cyclicArena;
        auto cyclicImg = Hydrogen::Loader::loadMachO(cyclicArena, dylib, "cyclic.dylib", {0x04000000u});
        dylibOk = dylibOk && cyclicImg.exports.empty();

        if (dylibOk) {
            std::printf("PASS mh_dylib_routines_weak_and_export_trie\n");
            ++passed;
        } else {
            std::printf("FAIL mh_dylib_routines_weak_and_export_trie\n");
            ++failed;
        }
    }

    std::printf("MachOProbe: %d passed, %d failed\n", passed, failed);
    return failed ? 1 : 0;
}
