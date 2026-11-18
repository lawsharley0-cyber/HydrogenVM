// Guest page permissions in host page protection (plan I6, backlog B-101/B-102).
//
// Expected results follow from the ARM and POSIX rules, not from emulator output: a store to a
// read-only page and a load from a PROT_NONE page abort at that instruction with the target
// address; instructions before it in the same block have already completed; host code may still
// write a read-only guest page (the loader and library functions do, as dyld and the kernel do).
#include "core/cpu/Engine.hpp"
#include <cstdio>
#include <functional>
#include <string>

using namespace Hydrogen;

namespace {

constexpr uint32_t kCode = 0x10000, kData = 0x20000, kStack = 0x40000;

struct Rig {
    Memory::GuestArena mem;
    CPU::Engine engine{mem};
    uint32_t halt = 0;
    explicit Rig(size_t hostPage = 0) : mem(hostPage), engine(mem) {
        halt = engine.registerHle("halt", [](CPU::Cpu& c) { c.exit = CPU::Exit::Halt; });
        mem.map(kCode, 0x1000, Memory::kProtRW);
        mem.map(kData, 0x4000, Memory::kProtRW);
        mem.map(kStack - 0x4000, 0x4000, Memory::kProtRW);
    }
    void load(const std::initializer_list<uint32_t>& words) {
        uint32_t a = kCode;
        for (uint32_t w : words) { mem.write<uint32_t>(a, w); a += 4; }
        mem.protect(kCode, 0x1000, Memory::kProtRX);
    }
    CPU::Exit run(CPU::Cpu& c) {
        c.r[CPU::kSP] = kStack;
        c.r[CPU::kLR] = halt;
        c.r[CPU::kPC] = kCode;
        CPU::Exit e;
        while ((e = engine.run(c, 1'000'000)) == CPU::Exit::Budget) {}
        return e;
    }
};

// mov r1, #0x20000; mov r0, #5; add r2, r0, #1; str r0, [r1]; mov r3, #9; bx lr
const std::initializer_list<uint32_t> kStoreProgram = {0xE3A01802, 0xE3A00005, 0xE2802001, 0xE5810000,
                                                       0xE3A03009, 0xE12FFF1E};
// mov r1, #0x20000; mov r0, #7; ldr r4, [r1, #4]; mov r3, #9; bx lr
const std::initializer_list<uint32_t> kLoadProgram = {0xE3A01802, 0xE3A00007, 0xE5914004, 0xE3A03009,
                                                      0xE12FFF1E};

} // namespace

int runPageProtectionTests(const std::function<void(bool, const char*)>& check) {
    int failures = 0;
    auto ck = [&](bool ok, const char* what) { check(ok, what); failures += !ok; };
    {
        Rig rig;
        rig.load(kStoreProgram);
        rig.mem.protect(kData, 0x1000, Memory::kProtRead);
        CPU::Cpu c;
        const CPU::Exit e = rig.run(c);
        ck(e == CPU::Exit::MemFault && c.exit_info == kData && c.r[0] == 5 && c.r[2] == 6 && c.r[3] == 0 &&
               rig.mem.read<uint32_t>(kData) == 0,
           "a guest store to a read-only page faults at its address; earlier instructions completed, later ones did not");
        ck(c.r[CPU::kPC] == kCode + 12, "the fault names the store instruction itself, not the start of its block");
        if (c.r[CPU::kPC] != kCode + 12) std::printf("       (fault pc 0x%x)\n", c.r[CPU::kPC]);
        rig.mem.write<uint32_t>(kData, 0x1234);   // host write to a read-only guest page
        ck(rig.mem.read<uint32_t>(kData) == 0x1234 && rig.mem.liftedHostPages() == 1,
           "host code can still write a read-only guest page (lifted once)");
        rig.mem.protect(kData, 0x1000, Memory::kProtRW);
        CPU::Cpu c2;
        ck(rig.run(c2) == CPU::Exit::Halt && rig.mem.read<uint32_t>(kData) == 5 && c2.r[3] == 9,
           "after mprotect(PROT_READ|PROT_WRITE) the same store succeeds");
    }
    {
        Rig rig;
        rig.load(kLoadProgram);
        rig.mem.write<uint32_t>(kData + 4, 0xABCD);
        rig.mem.protect(kData, 0x1000, 0);   // PROT_NONE guard page
        ck(rig.mem.isMapped(kData) && !(rig.mem.protAt(kData) & Memory::kProtRead),
           "PROT_NONE leaves the page mapped but inaccessible");
        CPU::Cpu c;
        ck(rig.run(c) == CPU::Exit::MemFault && c.exit_info == kData + 4 && c.r[0] == 7 && c.r[4] == 0 &&
               c.r[CPU::kPC] == kCode + 8,
           "a guest load from a PROT_NONE page faults at the load");
        rig.mem.protect(kData, 0x1000, Memory::kProtRead);
        CPU::Cpu c2;
        ck(rig.run(c2) == CPU::Exit::Halt && c2.r[4] == 0xABCD, "the page's contents survive PROT_NONE");
    }
    {
        // 16 KB host pages over 4 KB guest pages: a host page whose four guest pages agree is enforced;
        // one whose guest pages disagree keeps the union of their permissions.
        Rig rig(16384);
        rig.load(kStoreProgram);   // the code host page is split: one RX guest page, three unmapped
        const uint64_t splitBefore = rig.mem.splitHostPages();
        rig.mem.protect(kData, 0x4000, Memory::kProtRead);
        CPU::Cpu c;
        ck(rig.run(c) == CPU::Exit::MemFault && c.exit_info == kData && rig.mem.splitHostPages() == splitBefore,
           "16 KB host page: four read-only guest pages are enforced");
        rig.mem.protect(kData + 0x1000, 0x1000, Memory::kProtRW);
        CPU::Cpu c2;
        ck(rig.run(c2) == CPU::Exit::Halt && rig.mem.read<uint32_t>(kData) == 5 &&
               rig.mem.splitHostPages() == splitBefore + 1,
           "16 KB host page with mixed guest permissions is split and keeps the more permissive one");
    }
    return failures;
}
