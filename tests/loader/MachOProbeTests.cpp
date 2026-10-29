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

    std::printf("MachOProbe: %d passed, %d failed\n", passed, failed);
    return failed ? 1 : 0;
}
