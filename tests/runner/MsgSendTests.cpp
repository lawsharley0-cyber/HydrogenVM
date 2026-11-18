// objc_msgSend fast path (plan I7): cached (isa, selector) -> IMP sends must see every change a
// full lookup would. Expected values are the IMPs' own return constants.
#include "runtime/Process.hpp"
#include "runtime/objc/ObjC.hpp"
#include <functional>
#include <string>

using namespace Hydrogen;
using namespace Hydrogen::Runtime;

int runMsgSendTests(const std::function<void(bool, const char*)>& check) {
    int failures = 0;
    auto ck = [&](bool ok, const char* what) { check(ok, what); failures += !ok; };
    ProcessConfig cfg;
    cfg.homePath = "/tmp/hvm-msgsend-tests";
    Process p(cfg);
    auto& rt = ObjC::rt(p);
    const uint32_t code = p.heap().allocAligned(4096, 4096);
    // IMPs: mov r0, #N; bx lr
    auto impReturning = [&](uint32_t slot, uint32_t n) {
        const uint32_t a = code + 0x200 + slot * 8;
        p.mem().write<uint32_t>(a, 0xE3A00000u | n);
        p.mem().write<uint32_t>(a + 4, 0xE12FFF1E);
        return a;
    };
    // Caller: blx r2 (objc_msgSend) then return its r0 and r1.
    p.mem().write<uint32_t>(code + 0, 0xE92D4010);   // push {r4, lr}
    p.mem().write<uint32_t>(code + 4, 0xE12FFF32);   // blx r2
    p.mem().write<uint32_t>(code + 8, 0xE8BD8010);   // pop {r4, pc}
    const uint32_t msgSend = *p.lookupSymbol("_objc_msgSend");
    CPU::Cpu cpu;
    cpu.r[CPU::kSP] = p.heap().allocZeroed(1 << 16) + (1 << 16) - 64;
    auto send = [&](ObjC::id obj, ObjC::SEL s) { return uint32_t(p.callGuest(cpu, code, {obj, s, msgSend})); };

    const ObjC::Class base = rt.defineClass("HVMSendBase", "NSObject").cls();
    const ObjC::Class sub = rt.defineClass("HVMSendSub", "HVMSendBase").cls();
    const ObjC::Class other = rt.defineClass("HVMSendOther", "NSObject").cls();
    const ObjC::SEL ping = rt.sel("hvmPing");
    rt.addMethod(base, ping, impReturning(0, 1), "i@:");
    rt.addMethod(other, ping, impReturning(1, 7), "i@:");
    const ObjC::id a = rt.createInstance(base), b = rt.createInstance(sub);

    ck(send(a, ping) == 1 && send(a, ping) == 1 && send(a, ping) == 1, "repeated sends reach the method (cached after the first)");
    rt.addMethod(base, ping, impReturning(2, 2), "i@:", true);
    ck(send(a, ping) == 2, "replacing a method is seen by the next cached send");
    ck(send(b, ping) == 2 && send(b, ping) == 2, "a subclass inherits the replaced method");
    rt.addMethod(sub, ping, impReturning(3, 3), "i@:");
    ck(send(b, ping) == 3 && send(a, ping) == 2, "an override added to the subclass wins for it only");
    p.mem().write<uint32_t>(b, other);   // isa swizzle (object_setClass)
    ck(send(b, ping) == 7, "changing an object's class changes the method it reaches");
    cpu.r[1] = 0xFFFFFFFFu;
    const uint32_t nilResult = send(0, ping);
    ck(nilResult == 0, "a message to nil returns 0");
    // A host-implemented method reached through the cache: -[NSObject self] returns the receiver.
    const ObjC::SEL selfSel = rt.sel("self");
    ck(send(a, selfSel) == a && send(a, selfSel) == a, "a host IMP reached through the cache returns its result");
    auto* ring = cpu.call_ring.load();
    const uint64_t before = ring->total();
    send(a, ping);
    send(a, ping);
    send(0, ping);
    send(a, selfSel);
    const uint32_t sendIndex = p.engine().hleIndex(msgSend);
    unsigned sendCount = 0;
    for (const auto& entry : ring->last(size_t(ring->total() - before))) {
        if (entry.kind == Diag::CallKind::Hle && entry.id == sendIndex) ++sendCount;
    }
    ck(sendCount == 4, "crash and replay history include each guest, nil and host method send");
    return failures;
}
