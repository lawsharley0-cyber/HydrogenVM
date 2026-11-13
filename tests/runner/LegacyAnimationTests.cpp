#include "runtime/foundation/Foundation.hpp"
#include "runtime/uikit/UIKit.hpp"
#include <cstdio>
using namespace Hydrogen;
using namespace Hydrogen::Runtime;
int runLegacyAnimationTests() {
    ProcessConfig cfg; cfg.homePath = "/tmp/hvm-legacy-animation-tests";
    Process p(cfg); CPU::Cpu cpu; cpu.r[13] = p.heap().allocZeroed(65536)+65536-16;
    Ctx x{p,cpu,p.mem()}; auto& r = ObjC::rt(p);
    unsigned calls=0; uint32_t finished=0, context=0, name=0;
    auto cls=r.defineClass("AnimationProbe", "NSObject");
    cls.method("done:finished:context:", [&](Ctx& c) {
        ++calls; name=c.cpu.r[2]; finished=c.cpu.r[3]; context=c.mem.read<uint32_t>(c.cpu.r[13]);
    }, "v@:@c^v");
    auto obj=r.send(cpu,cls.cls(),"new");
    auto view=r.getClass("UIView");
    const auto ident=Foundation::makeString(x,"animation");
    r.send(cpu,view,"beginAnimations:context:",{ident,0x12345678});
    r.send(cpu,view,"setAnimationDelegate:",{uint32_t(obj)});
    r.send(cpu,view,"setAnimationDidStopSelector:",{r.sel("done:finished:context:")});
    r.send(cpu,view,"commitAnimations");
    if(calls) { std::puts("FAIL: completion must be deferred"); return 1; }
    Foundation::mainRunLoop(p).runUntil(cpu,Foundation::now()+0.3);
    if(calls!=1 || finished!=1 || context!=0x12345678 || name!=ident) {
        std::printf("FAIL: animation completion ABI calls=%u finished=%x context=%x\n",calls,finished,context); return 1;
    }
    const auto pool=uint32_t(r.send(cpu,r.getClass("NSAutoreleasePool"),"new"));
    const auto window=uint32_t(r.send(cpu,r.getClass("UIWindow"),"new"));
    const auto ownership=r.retainCount(window);
    r.send(cpu,window,"makeKeyAndVisible");
    r.send(cpu,pool,"drain");
    if(r.retainCount(window)!=ownership) {
        std::puts("FAIL: making a window visible must not retain it"); return 1;
    }
    r.release(cpu,window);
    const auto& windows=UIKit::ui(p).windows;
    if(std::find(windows.begin(),windows.end(),window)!=windows.end() || UIKit::ui(p).keyWindow==window) {
        std::puts("FAIL: released visible window must leave input/compositor registration"); return 1;
    }
    unsigned disposed=0;
    auto ephemeral=r.defineClass("TimerEphemeral", "NSObject");
    ephemeral.method("dealloc",[&](Ctx& c) { ++disposed; r.disposeInstance(c.cpu,c.cpu.r[0]); },"v@:");
    const auto outer=r.poolPush();
    Foundation::mainRunLoop(p).addTimer(0,0,false,[&](CPU::Cpu& c) {
        r.autorelease(c,uint32_t(r.send(c,ephemeral.cls(),"new")));
    });
    UIKit::runApplicationLoop(x,Foundation::now()+0.03);
    if(disposed!=1) { std::puts("FAIL: UIKit timer autoreleases must drain at end of loop turn"); return 1; }
    r.poolPop(cpu,outer);
    std::puts("legacy animation callback and window lifetime: passed");
    return 0;
}

#ifndef HVM_CANABALT_CASES_NO_MAIN
int main() { return runLegacyAnimationTests(); }
#endif
