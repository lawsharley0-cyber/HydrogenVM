#include "runtime/uikit/UIKit.hpp"
#include <cstdio>
using namespace Hydrogen;
using namespace Hydrogen::Runtime;
int main() {
    ProcessConfig cfg; cfg.homePath = "/tmp/hvm-kvc-struct-test";
    Process p(cfg); CPU::Cpu cpu;
    cpu.r[13] = p.heap().allocZeroed(65536) + 65536 - 16;
    Ctx x{p, cpu, p.mem()}; auto& r = ObjC::rt(p);
    const auto view = uint32_t(r.send(cpu, uint32_t(r.send(cpu, r.getClass("UIView"), "alloc")), "init"));
    CG::CGRect frame{{1.25f, 2.5f}, {80.5f, 40.25f}};
    const auto boxed = Foundation::makeValue(x, &frame, sizeof(frame), "{CGRect={CGPoint=ff}{CGSize=ff}}");
    r.send(cpu, view, "setValue:forKey:", {boxed, Foundation::constString(p, "frame")});
    const auto got = UIKit::getRect(x, view, "frame");
    if (got.x != frame.origin.x || got.y != frame.origin.y || got.w != frame.size.width || got.h != frame.size.height) {
        std::puts("FAIL: KVC must unbox all CGRect words into the setter"); return 1;
    }
    const auto back = uint32_t(r.send(cpu, view, "valueForKey:", {Foundation::constString(p, "frame")}));
    const auto buffer = p.heap().allocZeroed(16);
    if (!back) { std::puts("FAIL: KVC must box struct getters as NSValue"); return 1; }
    r.send(cpu, back, "getValue:", {buffer});
    if (std::memcmp(p.mem().host(buffer), &frame, sizeof(frame)) != 0) { std::puts("FAIL: KVC CGRect roundtrip"); return 1; }
    bool raisedGuest = false;
    try { r.send(cpu, view, "setValue:forKey:", {0, Foundation::constString(p, "frame")}); }
    catch (const CPU::GuestFault& e) {
        const std::string message = e.what();
        raisedGuest = message.find("*** KVC struct setter requires NSValue") != std::string::npos;
    }
    catch (const ObjC::ObjCError& e) {
        // This host-only runner has no guest exception frame. TL-AF verifies
        // actual @catch delivery; here require the Cocoa error bridge marker.
        raisedGuest = std::string(e.what()).rfind("*** ", 0) == 0;
    }
    if (!raisedGuest) { std::puts("FAIL: invalid struct KVC value raises a guest NSException"); return 1; }
    std::puts("KVC struct properties: passed");
}
