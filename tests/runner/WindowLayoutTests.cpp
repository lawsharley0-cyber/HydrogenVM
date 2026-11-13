#include "runtime/uikit/UIKit.hpp"
#include <cstdio>
#include <vector>
using namespace Hydrogen;
using namespace Hydrogen::Runtime;
namespace {
std::vector<int> events;
void will(Ctx&, uint32_t, uint32_t, bool) { events.push_back(1); }
void layout(Ctx&, uint32_t, uint32_t) { events.push_back(2); }
void did(Ctx&, uint32_t, uint32_t, bool) { events.push_back(3); }
}
int main() {
    ProcessConfig cfg;
    cfg.homePath = "/tmp/hvm-window-layout-test";
    Process p(cfg);
    CPU::Cpu cpu;
    cpu.r[13] = p.heap().allocZeroed(65536) + 65536 - 16;
    auto& r = ObjC::rt(p);
    auto view = r.defineClass("LayoutProbeView", "UIView");
    view.m<&layout>("layoutSubviews", "v@:");
    auto vcClass = r.defineClass("LayoutProbeController", "UIViewController");
    vcClass.m<&will>("viewWillAppear:", "v@:c").m<&did>("viewDidAppear:", "v@:c");
    auto make = [&](uint32_t cls) { return uint32_t(r.send(cpu, uint32_t(r.send(cpu, cls, "alloc")), "init")); };
    const auto window = make(r.getClass("UIWindow")), vc = make(vcClass.cls()), v = make(view.cls());
    r.send(cpu, vc, "setView:", {v});
    r.send(cpu, window, "setRootViewController:", {vc});
    events.clear();
    r.send(cpu, v, "setNeedsLayout");
    r.send(cpu, window, "makeKeyAndVisible");
    if (events != std::vector<int>({1, 2, 3})) {
        std::printf("FAIL: showing a window must call willAppear, layout, didAppear before returning; got");
        for (auto event : events) std::printf(" %d", event);
        std::puts(""); return 1;
    }
    events.clear();
    r.send(cpu, window, "makeKeyAndVisible");
    if (!events.empty()) { std::puts("FAIL: already visible window repeated appearance/layout"); return 1; }
    const auto child = make(r.getClass("UIViewController"));
    const auto childView = make(r.getClass("UIView"));
    r.send(cpu, child, "setView:", {childView});
    r.send(cpu, childView, "setAutoresizingMask:", {0});
    const auto nav = uint32_t(r.send(cpu, uint32_t(r.send(cpu, r.getClass("UINavigationController"), "alloc")), "initWithRootViewController:", {child}));
    r.send(cpu, nav, "setNavigationBarHidden:", {1});
    const auto navView = uint32_t(r.send(cpu, nav, "view"));
    r.send(cpu, navView, "setBounds:", {0, 0, UIKit::fbits(568), UIKit::fbits(320)});
    r.send(cpu, navView, "layoutIfNeeded");
    Ctx cx{p, cpu, p.mem()};
    const auto bounds = UIKit::getRect(cx, childView, "bounds");
    if (bounds.w != 568 || bounds.h != 320) {
        std::printf("FAIL: navigation content must follow container resize; got %.0fx%.0f\n", bounds.w, bounds.h); return 1;
    }
    std::puts("window initial layout: passed");
}
