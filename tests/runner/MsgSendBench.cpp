// objc_msgSend micro-benchmark: a guest loop sends one message N times to an object whose method is
// guest code (a one-instruction IMP), so the time is the dispatch, not the method.
//   hvm_msgsend_bench [iterations]      prints guest instructions, host seconds and ns per send
#include "runtime/Process.hpp"
#include "runtime/objc/ObjC.hpp"
#include <chrono>
#include <cstdio>
#include <cstdlib>

using namespace Hydrogen;
using namespace Hydrogen::Runtime;

int main(int argc, char** argv) {
    const uint32_t n = argc > 1 ? uint32_t(std::strtoul(argv[1], nullptr, 0)) : 2'000'000u;
    ProcessConfig cfg;
    cfg.homePath = "/tmp/hvm-msgsend-bench";
    Process p(cfg);
    auto& rt = ObjC::rt(p);
    const ObjC::Class cls = rt.defineClass("HVMBenchTarget", "NSObject").cls();
    // Guest code: IMP is "bx lr"; the loop keeps self/sel in r5/r6 and the count in r4.
    const uint32_t code = p.heap().allocAligned(4096, 4096);
    const uint32_t imp = code + 0x100;
    p.mem().write<uint32_t>(imp, 0xE12FFF1E);                      // bx lr
    const uint32_t msgSend = *p.lookupSymbol("_objc_msgSend");
    const uint32_t loop[] = {
        0xE92D40F0,                 // push {r4-r7, lr}
        0xE1A04002,                 // mov r4, r2 (count)
        0xE1A05000,                 // mov r5, r0 (self)
        0xE1A06001,                 // mov r6, r1 (sel)
        0xE1A07003,                 // mov r7, r3 (objc_msgSend)
        0xE1A00005,                 // L: mov r0, r5
        0xE1A01006,                 //    mov r1, r6
        0xE12FFF37,                 //    blx r7
        0xE2544001,                 //    subs r4, r4, #1
        0x1AFFFFFA,                 //    bne L
        0xE8BD80F0,                 // pop {r4-r7, pc}
    };
    for (size_t i = 0; i < std::size(loop); ++i) p.mem().write<uint32_t>(code + 4 * uint32_t(i), loop[i]);
    const ObjC::SEL sel = rt.sel("hvmBenchPing");
    rt.addMethod(cls, sel, imp, "@@:");
    const ObjC::id obj = rt.createInstance(cls);
    CPU::Cpu cpu;
    cpu.r[CPU::kSP] = p.heap().allocZeroed(1 << 16) + (1 << 16) - 64;
    p.callGuest(cpu, code, {obj, sel, 1000, msgSend});   // warm: decode blocks, fill caches
    const auto t0 = std::chrono::steady_clock::now();
    const uint64_t i0 = cpu.insn_count;
    p.callGuest(cpu, code, {obj, sel, n, msgSend});
    const double s = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    std::printf("%u sends: %.1f M guest insns, %.3f s, %.1f ns/send\n", n, (cpu.insn_count - i0) / 1e6, s, s * 1e9 / n);
    return 0;
}
