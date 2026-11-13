#include "runtime/Process.hpp"
#include "runtime/foundation/Foundation.hpp"
#include <bit>
#include <cstdio>

using namespace Hydrogen;
using namespace Hydrogen::Runtime;

int main() {
    ProcessConfig cfg;
    cfg.bundlePath = "/tmp/geometry-dictionary.app";
    cfg.homePath = "/tmp/hvm-geometry-dictionary-tests";
    Process p(cfg);
    CPU::Cpu cpu{};
    const auto stack = p.heap().allocZeroed(65536);
    cpu.r[13] = stack + 65536 - 16;
    auto create = p.lookupSymbol("_CGRectCreateDictionaryRepresentation");
    auto restore = p.lookupSymbol("_CGRectMakeWithDictionaryRepresentation");
    if (!create || !restore) { std::puts("FAIL: CGRect dictionary APIs missing"); return 1; }
    auto f = [](float v) { return std::bit_cast<uint32_t>(v); };
    auto& rt = ObjC::rt(p);
    const uint32_t dict = p.callGuest(cpu, *create, {f(-1.25f), f(2.5f), f(-3.75f), f(4.125f)});
    auto number = [&](const char* key) {
        const auto value = rt.send(cpu, dict, "objectForKey:", {Foundation::constString(p, key)});
        return rt.send(cpu, uint32_t(value), "isEqualToNumber:", {uint32_t(rt.send(cpu,
            rt.getClass("NSNumber"), "numberWithFloat:", {f(key[0] == 'X' ? -1.25f : key[0] == 'Y' ? 2.5f : key[0] == 'W' ? -3.75f : 4.125f)}))}) & 0xff;
    };
    if (!dict || uint32_t(rt.send(cpu, dict, "count")) != 4 || !number("X") || !number("Y") || !number("Width") || !number("Height")) {
        std::puts("FAIL: CGRect dictionary must preserve all signed fractional fields"); return 1;
    }
    const uint32_t out = p.heap().allocZeroed(16);
    if (!(p.callGuest(cpu, *restore, {dict, out}) & 0xff) || p.mem().read<uint32_t>(out) != f(-1.25f) ||
        p.mem().read<uint32_t>(out + 4) != f(2.5f) || p.mem().read<uint32_t>(out + 8) != f(-3.75f) ||
        p.mem().read<uint32_t>(out + 12) != f(4.125f)) { std::puts("FAIL: CGRect dictionary round trip"); return 1; }
    const auto empty = rt.send(cpu, rt.getClass("NSDictionary"), "dictionary");
    if ((p.callGuest(cpu, *restore, {uint32_t(empty), out}) & 0xff) || p.mem().read<uint32_t>(out) != f(-1.25f)) {
        std::puts("FAIL: malformed CGRect dictionary must fail without changing output"); return 1;
    }
    rt.release(cpu, dict);
    p.heap().free(stack);
    std::puts("geometry dictionary: passed");
}
