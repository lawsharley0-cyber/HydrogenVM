#include "runtime/foundation/Foundation.hpp"
#include <cstdio>
using namespace Hydrogen;
using namespace Hydrogen::Runtime;
int main() {
    ProcessConfig cfg; cfg.homePath = "/tmp/hvm-gamekit-coding-test";
    Process p(cfg); CPU::Cpu cpu;
    cpu.r[13] = p.heap().allocZeroed(65536) + 65536 - 16;
    auto& r = ObjC::rt(p);
    const auto score = uint32_t(r.send(cpu, uint32_t(r.send(cpu, r.getClass("GKScore"), "alloc")),
        "initWithCategory:", {Foundation::constString(p, "test-board")}));
    if (!r.respondsTo(score, r.sel("setValue:")) || !r.respondsTo(score, r.sel("encodeWithCoder:"))) {
        std::puts("FAIL: GKScore value and NSCoding are missing"); return 1;
    }
    r.send(cpu, score, "setValue:", {0x89abcdef, 0xfedcba98});
    r.send(cpu, score, "setContext:", {0x76543210, 0x12345678});
    const auto data = uint32_t(r.send(cpu, r.getClass("NSKeyedArchiver"), "archivedDataWithRootObject:", {score}));
    const auto restored = uint32_t(r.send(cpu, r.getClass("NSKeyedUnarchiver"), "unarchiveObjectWithData:", {data}));
    const bool equal = restored && r.send(cpu, restored, "value") == 0xfedcba9889abcdefull &&
        r.send(cpu, restored, "context") == 0x1234567876543210ull &&
        (r.send(cpu, uint32_t(r.send(cpu, restored, "category")), "isEqualToString:", {Foundation::constString(p, "test-board")}) & 0xff);
    if (!equal) { std::puts("FAIL: GKScore archive must preserve category and full 64-bit signed value/context"); return 1; }
    std::puts("GameKit local score coding: passed");
}
