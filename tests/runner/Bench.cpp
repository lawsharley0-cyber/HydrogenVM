// HydrogenVM micro-benchmark: runs every t_* function of a test ELF repeatedly
// and reports guest MIPS (millions of guest instructions per host second).
#include "core/cpu/Engine.hpp"
#include "loader/ElfLoader.hpp"
#include <chrono>
#include <cstdio>
#include <string>

using namespace Hydrogen;

int main(int argc, char** argv) {
    if (argc < 2) { std::fprintf(stderr, "usage: %s <elf> [iterations]\n", argv[0]); return 2; }
    const int iters = argc > 2 ? std::atoi(argv[2]) : 20;
    Memory::GuestArena mem;
    CPU::Engine engine(mem);
    auto img = Loader::loadElf(mem, Loader::readFile(argv[1]));
    const uint32_t top = 0x08000000u;
    mem.map(top - 0x100000, 0x100000);
    const uint32_t halt = engine.registerHle("halt", [](CPU::Cpu& c) { c.exit = CPU::Exit::Halt; });

    uint64_t insns = 0;
    auto t0 = std::chrono::steady_clock::now();
    for (int it = 0; it < iters; ++it) {
        for (auto& [name, addr] : img.symbols) {
            if (name.rfind("t_", 0) != 0 || name.find('.') != std::string::npos) continue;
            CPU::Cpu c;
            c.r[CPU::kSP] = top - 64; c.r[CPU::kLR] = halt;
            c.r[CPU::kPC] = addr & ~1u; c.thumb = addr & 1;
            if (engine.run(c) != CPU::Exit::Halt) { std::fprintf(stderr, "%s failed\n", name.c_str()); return 1; }
            insns += c.insn_count;
        }
    }
    double s = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    std::printf("%s: %.1f M guest insns in %.3f s = %.1f MIPS (%llu blocks decoded)\n", argv[1],
                insns / 1e6, s, insns / 1e6 / s, (unsigned long long)engine.stats().blocks_decoded);
    return 0;
}
