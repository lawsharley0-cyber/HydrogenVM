// Linux test-only pthread_create interposition: force launch failure and order
// guest execution before the creating thread returns from pthread_create.
#include "runtime/Process.hpp"
#include "runtime/objc/ObjC.hpp"
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <pthread.h>
#include <thread>
#include <unistd.h>
#include <vector>

using namespace Hydrogen;
using namespace Hydrogen::Runtime;

namespace {
Process* process = nullptr;
int failCount = 0;   // the next N host pthread_create calls return EAGAIN
bool waitForTarget = false;
std::vector<size_t> requestedStacks;   // stack size of every host pthread_create attempt
std::atomic<bool> targetEntered{false};
}

extern "C" int __real_pthread_create(pthread_t*, const pthread_attr_t*, void* (*)(void*), void*);
extern "C" int __wrap_pthread_create(pthread_t* thread, const pthread_attr_t* attr, void* (*fn)(void*), void* arg) {
    size_t stack = 0;
    if (attr) pthread_attr_getstacksize(attr, &stack);
    requestedStacks.push_back(stack);
    if (failCount > 0) {
        // threadsMu_ is held during host creation: record nothing that needs it here.
        --failCount;
        return EAGAIN;
    }
    const int result = __real_pthread_create(thread, attr, fn, arg);
    if (result == 0 && waitForTarget) {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (!targetEntered && std::chrono::steady_clock::now() < deadline) std::this_thread::yield();
    }
    return result;
}

int main() {
    int failed = 0;
    auto check = [&](bool ok, const char* message) { std::printf("%s %s\n", ok ? "ok" : "FAIL", message); failed += !ok; };
    const auto home = std::filesystem::temp_directory_path() / ("hvm-thread-launch-" + std::to_string(getpid()));
    {
        ProcessConfig cfg;
        cfg.homePath = home.string(); cfg.bundlePath = (home / "Test.app").string();
        cfg.log = [](LogLevel, const std::string&) {};
        Process p(cfg); process = &p;
        auto& r = ObjC::rt(p);
        auto& cpu = p.mainThread().cpu;
        p.mem().map(0xBF000000u - (1u << 20), 1u << 20, Memory::kProtRW);
        cpu.r[CPU::kSP] = 0xBF000000u - 256;
        std::atomic<bool> sawExecuting{false};
        auto targetClass = r.defineClass("HVMThreadLaunchTarget", "NSObject");
        targetClass.method("work:", [&](Ctx& cx) {
            const auto current = ObjC::id(r.send(cx.cpu, r.getClass("NSThread"), "currentThread"));
            sawExecuting = (uint32_t(r.send(cx.cpu, current, "isExecuting")) & 0xff) != 0;
            targetEntered = true;
        }, "v@:@");
        const auto target = ObjC::id(r.send(cpu, ObjC::id(r.send(cpu, targetClass.cls(), r.selAlloc)), "init"));
        const auto thread = ObjC::id(r.send(cpu, ObjC::id(r.send(cpu, r.getClass("NSThread"), r.selAlloc)),
                                           "initWithTarget:selector:object:", {target, r.sel("work:"), 0}));
        // Every stack size fails: the launch fails cleanly after trying each one.
        failCount = 4;
        requestedStacks.clear();
        bool threw = false;
        try { r.send(cpu, thread, "start"); } catch (const std::exception&) { threw = true; }
        const auto afterIds = p.liveThreadIds();
        check(threw && afterIds.size() == 1 && afterIds[0] == p.mainThread().id,
              "failed host creation removes the guest thread");
        check(!(uint32_t(r.send(cpu, thread, "isExecuting")) & 0xff), "failed NSThread start resets execution state");
        check(requestedStacks == std::vector<size_t>{8u << 20, 2u << 20, 1u << 20, 512u << 10},
              "a host thread that cannot get 8 MB is retried with 2 MB, 1 MB and 512 KB stacks");
        // The 8 MB stack fails once: the thread starts on the 2 MB retry.
        failCount = 1;
        requestedStacks.clear();
        waitForTarget = true;
        r.send(cpu, thread, "start");
        waitForTarget = false;
        check(targetEntered && sawExecuting, "guest target sees its NSThread executing before pthread_create returns");
        check(requestedStacks == std::vector<size_t>{8u << 20, 2u << 20}, "an EAGAIN on 8 MB falls back to a 2 MB host stack and starts");
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (!(uint32_t(r.send(cpu, thread, "isFinished")) & 0xff) && std::chrono::steady_clock::now() < deadline)
            std::this_thread::yield();
        check((uint32_t(r.send(cpu, thread, "isFinished")) & 0xff) != 0, "the same NSThread can complete after a failed launch");

        ObjC::id reentrantThread = 0;
        bool retainCalled = false, retainedStack = false;
        auto reentrantClass = r.defineClass("HVMThreadRetainTarget", "NSObject");
        reentrantClass.method("retain", [&](Ctx& cx) {
            const auto self = ObjC::id(cx.cpu.r[0]);
            if (reentrantThread) {
                retainCalled = true;
                retainedStack = uint32_t(r.send(cx.cpu, reentrantThread, "stackSize")) == (1u << 20);
                r.send(cx.cpu, reentrantThread, "setStackSize:", {2u << 20});
                r.send(cx.cpu, reentrantThread, "start");
            }
            cx.cpu.r[0] = r.rootRetain(self);
        }, "@@:");
        reentrantClass.method("work:", [](Ctx&) {}, "v@:@");
        const auto reentrantTarget = ObjC::id(r.send(cpu, ObjC::id(r.send(cpu, reentrantClass.cls(), r.selAlloc)), "init"));
        reentrantThread = ObjC::id(r.send(cpu, ObjC::id(r.send(cpu, r.getClass("NSThread"), r.selAlloc)),
                                         "initWithTarget:selector:object:", {reentrantTarget, r.sel("work:"), 0}));
        r.send(cpu, reentrantThread, "setStackSize:", {1u << 20});
        r.send(cpu, reentrantThread, "start");
        const auto reentrantDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (!(uint32_t(r.send(cpu, reentrantThread, "isFinished")) & 0xff) && std::chrono::steady_clock::now() < reentrantDeadline)
            std::this_thread::yield();
        check(retainCalled && retainedStack && uint32_t(r.send(cpu, reentrantThread, "stackSize")) == (1u << 20) &&
              (uint32_t(r.send(cpu, reentrantThread, "isFinished")) & 0xff),
              "retain can inspect the stack and retry start without deadlocking or changing the stack");
        process = nullptr;
    }
    std::filesystem::remove_all(home);
    return failed ? 1 : 0;
}
