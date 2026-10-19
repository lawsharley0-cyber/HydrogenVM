// Loads a Mach-O into a fresh arena and prints what the loader found, in a
// stable text form that tools/check_macho.py compares against llvm-objdump.
#include "loader/MachO.hpp"
#include "loader/ElfLoader.hpp"
#include <algorithm>
#include <cstdio>

using namespace Hydrogen;

int main(int argc, char** argv) {
    if (argc < 2) { std::fprintf(stderr, "usage: %s <macho> [loadaddr]\n", argv[0]); return 2; }
    try {
        Memory::GuestArena mem;
        Loader::LoadOptions opts;
        if (argc > 2) opts.loadAddress = uint32_t(std::strtoul(argv[2], nullptr, 0));
        auto file = Loader::readFile(argv[1]);
        auto img = Loader::loadMachO(mem, file, argv[1], opts);
        std::printf("subtype %u filetype %u slide 0x%x entry 0x%x %s\n", img.cpuSubtype, img.fileType, img.slide,
                    img.entry, img.entryIsMain ? "main" : "start");
        for (auto& s : img.segments) std::printf("segment %s 0x%x 0x%x\n", s.name.c_str(), s.vmaddr, s.vmsize);
        for (auto& d : img.dylibs) std::printf("dylib %s\n", d.c_str());
        for (auto a : img.initializers) std::printf("init 0x%x %s\n", a, img.symbolicate(a & ~1u).c_str());
        auto binds = img.binds;
        std::sort(binds.begin(), binds.end(), [](auto& a, auto& b) { return a.address < b.address; });
        for (auto& b : binds)
            std::printf("bind 0x%08x %s %s%s addend %d\n", b.address, b.symbol.c_str(),
                        b.lazy ? "lazy" : "nonlazy", b.kind == Loader::BindKind::ExternalReloc ? " extreloc" : "", b.addend);
        std::printf("exports %zu symbols %zu weakbinds %u\n", img.exports.size(), img.symbolsByAddr.size(), img.weakBindCount);
        auto it = img.exports.find("_main");
        if (it != img.exports.end()) std::printf("_main 0x%x\n", it->second);
    } catch (const std::exception& e) {
        std::printf("error: %s\n", e.what());
        return 1;
    }
    return 0;
}
