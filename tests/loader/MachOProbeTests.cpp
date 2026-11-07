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

// Construct a trie with 65,536 nodes, optionally adding one more reachable node.
Bytes largeExportTrie(bool exceedLimit) {
    std::vector<std::vector<size_t>> children(1);
    for (size_t i = 0; i < 255; ++i) {
        children[0].push_back(children.size());
        children.emplace_back();
    }
    for (size_t parent = 1; parent <= 255; ++parent) {
        for (size_t i = 0; i < 255; ++i) {
            children[parent].push_back(children.size());
            children.emplace_back();
        }
    }
    for (size_t i = 0; i < 255; ++i) {
        children[256].push_back(children.size());
        children.emplace_back();
    }
    if (exceedLimit) {
        children[65280].push_back(children.size()); // last leaf in traversal order
        children.emplace_back();
    }

    // Child offsets use three-byte ULEBs so node sizes are known in advance.
    std::vector<size_t> offsets(children.size());
    size_t size = 0;
    for (size_t i = 0; i < children.size(); ++i) {
        offsets[i] = size;
        size += (exceedLimit && i == children.size() - 1 ? 4 : 2) + 5 * children[i].size();
    }
    Bytes trie;
    trie.reserve(size);
    for (size_t index = 0; index < children.size(); ++index) {
        const auto& node = children[index];
        if (exceedLimit && index == children.size() - 1) {
            trie.insert(trie.end(), {2, 0, 1}); // terminal export after the work limit
        } else {
            trie.push_back(0); // no terminal
        }
        trie.push_back(uint8_t(node.size()));
        for (size_t i = 0; i < node.size(); ++i) {
            trie.push_back(uint8_t(i + 1)); // unique, non-NUL edge
            trie.push_back(0);
            size_t off = offsets[node[i]];
            trie.push_back(uint8_t((off & 0x7f) | 0x80));
            trie.push_back(uint8_t(((off >> 7) & 0x7f) | 0x80));
            trie.push_back(uint8_t(off >> 14));
        }
    }
    return trie;
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

        if (dylibOk) {
            std::printf("PASS mh_dylib_routines_weak_and_export_trie\n");
            ++passed;
        } else {
            std::printf("FAIL mh_dylib_routines_weak_and_export_trie\n");
            ++failed;
        }

        auto checkTrie = [&](const std::string& name, const Bytes& trie, const std::string& expectedError) {
            dylib.resize(0x1000);
            dylib.insert(dylib.end(), trie.begin(), trie.end());
            put32(200, uint32_t(trie.size()));
            bool ok = false;
            try {
                Hydrogen::Memory::GuestArena testArena;
                Hydrogen::Loader::loadMachO(testArena, dylib, name, {0x04000000u});
                ok = expectedError.empty();
            } catch (const Hydrogen::Loader::MachOError& e) {
                ok = !expectedError.empty() && std::string(e.what()).find(expectedError) != std::string::npos;
                if (!ok) std::printf("%s: unexpected error '%s'\n", name.c_str(), e.what());
            }
            std::printf("%s %s\n", ok ? "PASS" : "FAIL", name.c_str());
            if (ok) ++passed; else ++failed;
        };

        checkTrie("export_trie_child_at_end", {0, 1, 'x', 0, 5}, "export trie child offset out of range");
        checkTrie("export_trie_child_past_end", {0, 1, 'x', 0, 6}, "export trie child offset out of range");
        checkTrie("export_trie_at_work_limit", largeExportTrie(false), "");
        // A normal tree past the old fixed cap of 65,536 visits loads (big games have one).
        checkTrie("export_trie_large_tree_beyond_old_cap", largeExportTrie(true), "");
        // A small trie whose nodes are shared (each of 20 nodes points twice at the next one)
        // needs 2^21 visits from about 200 bytes: the work cap, linear in the trie size, stops it.
        {
            Bytes shared;
            const int k = 20;
            auto uleb2 = [&](size_t v) { shared.push_back(uint8_t((v & 0x7f) | 0x80)); shared.push_back(uint8_t(v >> 7)); };
            for (int i = 0; i < k; ++i) {
                const size_t next = size_t(i + 1) * 10;   // every internal node is 10 bytes
                shared.push_back(0);                      // no terminal
                shared.push_back(2);                      // two children ...
                for (char e : {'a', 'b'}) { shared.push_back(uint8_t(e)); shared.push_back(0); uleb2(next); }   // ... both the same node
            }
            shared.insert(shared.end(), {0, 0});          // the leaf
            checkTrie("export_trie_shared_nodes_hit_work_limit", shared, "export trie work limit exceeded");
        }
        // Few visits but long edges into shared nodes: the root and a middle node each have
        // 255 children with 250-byte edges, all pointing at the same next node. That is about
        // 65,000 visits (under the visit cap for this 130 KB trie) building 32 MB of names.
        // The byte budget, also linear in the trie size, stops it.
        {
            Bytes longEdges;
            const size_t children = 255, edgeLen = 250, nodeSize = 2 + children * (edgeLen + 1 + 3);
            auto uleb3 = [&](size_t v) {
                longEdges.push_back(uint8_t((v & 0x7f) | 0x80));
                longEdges.push_back(uint8_t(((v >> 7) & 0x7f) | 0x80));
                longEdges.push_back(uint8_t(v >> 14));
            };
            for (size_t node = 0; node < 2; ++node) {
                longEdges.push_back(0);
                longEdges.push_back(uint8_t(children));
                for (size_t c = 0; c < children; ++c) {
                    longEdges.insert(longEdges.end(), edgeLen, uint8_t('a' + c % 26));
                    longEdges.push_back(0);
                    uleb3((node + 1) * nodeSize);
                }
            }
            longEdges.insert(longEdges.end(), {0, 0});
            checkTrie("export_trie_long_shared_edges_hit_byte_limit", longEdges, "export trie work limit exceeded");
        }
        // Empty labels into a shared node whose terminal is long: 255 x 255 visits each read the
        // 60 KB terminal header region. Counting the bytes each visit reads stops it.
        {
            Bytes longTerminal;
            const size_t children = 255, entry = 1 + 3, rootSize = 2 + children * entry, midSize = 2 + children * entry;
            auto uleb3 = [&](size_t v) {
                longTerminal.push_back(uint8_t((v & 0x7f) | 0x80));
                longTerminal.push_back(uint8_t(((v >> 7) & 0x7f) | 0x80));
                longTerminal.push_back(uint8_t(v >> 14));
            };
            longTerminal.push_back(0); longTerminal.push_back(uint8_t(children));
            for (size_t c = 0; c < children; ++c) { longTerminal.push_back(0); uleb3(rootSize); }   // "" -> middle
            longTerminal.push_back(0); longTerminal.push_back(uint8_t(children));
            for (size_t c = 0; c < children; ++c) { longTerminal.push_back(0); uleb3(rootSize + midSize); }   // "" -> leaf
            const size_t pad = 60000;   // the leaf: a terminal of 60,000 bytes and no children
            longTerminal.push_back(uint8_t((pad & 0x7f) | 0x80)); longTerminal.push_back(uint8_t(((pad >> 7) & 0x7f) | 0x80));
            longTerminal.push_back(uint8_t(pad >> 14));
            longTerminal.insert(longTerminal.end(), pad, 0);
            longTerminal.push_back(0);
            checkTrie("export_trie_shared_long_terminal_hits_byte_limit", longTerminal, "export trie work limit exceeded");
        }
        // The bind opcode stream's SLEB (BIND_OPCODE_SET_ADDEND_SLEB) has the same 10-byte limit.
        auto checkBind = [&](const std::string& name, const Bytes& binds, const std::string& expectedError) {
            dylib.resize(0x1000);
            dylib.insert(dylib.end(), binds.begin(), binds.end());
            put32(172, 0x1000);
            put32(176, uint32_t(binds.size()));
            put32(200, 0);
            bool ok = false;
            try {
                Hydrogen::Memory::GuestArena testArena;
                Hydrogen::Loader::loadMachO(testArena, dylib, name, {0x04000000u});
                ok = expectedError.empty();
            } catch (const Hydrogen::Loader::MachOError& e) {
                ok = !expectedError.empty() && std::string(e.what()).find(expectedError) != std::string::npos;
                if (!ok) std::printf("%s: unexpected error '%s'\n", name.c_str(), e.what());
            }
            put32(172, 0);
            put32(176, 0);
            std::printf("%s %s\n", ok ? "PASS" : "FAIL", name.c_str());
            if (ok) ++passed; else ++failed;
        };
        Bytes tenByteSleb{0x60};
        tenByteSleb.insert(tenByteSleb.end(), 9, 0x80);
        tenByteSleb.insert(tenByteSleb.end(), {0x00, 0x00});   // ...last byte, then BIND_OPCODE_DONE
        checkBind("bind_ten_byte_sleb", tenByteSleb, "");
        Bytes elevenByteSleb{0x60};
        elevenByteSleb.insert(elevenByteSleb.end(), 10, 0x80);
        elevenByteSleb.insert(elevenByteSleb.end(), {0x00, 0x00});
        checkBind("bind_overlong_sleb", elevenByteSleb, "LEB128 value too long");
        checkTrie("export_trie_overlong_uleb", Bytes{0, 1, 'x', 0, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x01},
                  "LEB128 value too long");
        checkTrie("export_trie_cycle", {0x00, 0x02, 'a', 0x00, 0x00, 'b', 0x00, 0x00}, "export trie cycle detected");
    }

    std::printf("MachOProbe: %d passed, %d failed\n", passed, failed);
    return failed ? 1 : 0;
}
