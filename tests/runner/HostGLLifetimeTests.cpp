// HostGL TLS must not retain a context from a destroyed guest process.
// This test deliberately leaves the first EAGLContext current on the host
// thread, then constructs a second Process in the same stack slot.
#include "runtime/Process.hpp"
#include "runtime/gl/HostGL.hpp"
#include "runtime/objc/ObjC.hpp"

#include <cstdint>
#include <cstdio>
#include <memory>

using namespace Hydrogen::Runtime;

struct FakeGL final : HostGL::Platform {
    const char* name() const override { return "lifetime-test"; }
    void* createContext(int, void*, bool& desktop) override {
        desktop = false;
        return new uint8_t(1);
    }
    void destroyContext(void* context) override { delete static_cast<uint8_t*>(context); }
    bool makeCurrent(void*) override { return true; }
    void* proc(void*, const char*) override { return nullptr; }
};

int main() {
    ProcessConfig cfg;
    cfg.homePath = "/tmp/hvm_hostgl_lifetime_test";
    cfg.hostGL = std::make_shared<FakeGL>();
    for (int i = 0; i != 2; ++i) {
        Process process(cfg);
        auto& runtime = ObjC::rt(process);
        auto& cpu = process.mainThread().cpu;
        const auto cls = runtime.getClass("EAGLContext");
        const auto context = ObjC::id(runtime.send(cpu, ObjC::id(runtime.send(cpu, ObjC::id(cls), runtime.selAlloc)),
                                                   "initWithAPI:", {2}));
        const auto ok = runtime.send(cpu, ObjC::id(cls), "setCurrentContext:", {context});
        if (!ok) {
            std::fprintf(stderr, "EAGL context bind failed on launch %d\n", i + 1);
            return 1;
        }
        // Intentionally no setCurrentContext:nil: catch stale TLS on relaunch.
    }
    std::puts("HostGL sequential EAGL process lifetime: passed");
    return 0;
}
