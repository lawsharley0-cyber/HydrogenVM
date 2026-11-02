#include "runtime/Process.hpp"
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <sys/wait.h>
#include <unistd.h>
#ifdef __APPLE__
#include <mach-o/dyld.h>
#endif

using namespace Hydrogen;
using namespace std::chrono_literals;

int runThreadLifetimeChild() {
    alarm(15);
    Runtime::ProcessConfig cfg;
    cfg.homePath = "/tmp/hvm-lifetime-" + std::to_string(getpid());
    Runtime::Process survivor(cfg);
    const auto arenas = Memory::GuestArena::liveCount();
    auto process = std::make_unique<Runtime::Process>(cfg);
    struct State {
        std::mutex mu;
        std::condition_variable cv;
        bool ready = false, release = false;
        std::atomic<bool> destroyed{false};
    };
    auto state = std::make_shared<State>();
    const auto callback = process->registerHostFunction("held_host_callback", [state](Runtime::Ctx&) {
        // This owned copy survives even if the old destructor deletes the HLE
        // table. On that buggy path the child exits before the callback resumes.
        const auto held = state;
        std::unique_lock<std::mutex> lock(held->mu);
        held->ready = true;
        held->cv.notify_all();
        held->cv.wait(lock, [&] { return held->release; });
    });
    process->startThread(callback, 0, 0, false);
    {
        std::unique_lock<std::mutex> lock(state->mu);
        if (!state->cv.wait_for(lock, 3s, [&] { return state->ready; })) _exit(2);
    }
    std::thread destroyer([&] { process.reset(); state->destroyed.store(true); });
    std::this_thread::sleep_for(2300ms); // exceeds the old fixed two-second wait
    if (state->destroyed.load()) {
        std::fputs("FAIL Process/arena destroyed while a host worker still executes its HLE\n", stderr);
        _exit(1); // never resume a worker after deliberately detecting the old UAF
    }
    if (Memory::GuestArena::liveCount() != arenas + 1) _exit(3);
    {
        std::lock_guard<std::mutex> lock(state->mu);
        state->release = true;
    }
    state->cv.notify_all();
    destroyer.join();
    if (!state->destroyed.load() || Memory::GuestArena::liveCount() != arenas) _exit(4);
    // The deleted Process was constructed on this thread but destroyed on the
    // other one. currentThread must not inspect its now-dangling TLS pointer.
    if (&survivor.currentThread() != &survivor.mainThread()) _exit(6);
    {
        const auto done = survivor.registerHostFunction("completed_worker", [](Runtime::Ctx&) {});
        const auto first = survivor.startThread(done, 0, 0, true);
        auto* thread = survivor.findThread(first);
        while (!thread->finished.load()) std::this_thread::yield();
        const auto second = survivor.startThread(done, 0, 0, true);
        if (thread->hostJoinable) _exit(8); // completed native stacks must not accumulate until Process exit
        while (!survivor.findThread(second)->finished.load()) std::this_thread::yield();
    }
    for (unsigned i = 0; i < 10; ++i) {
        { Runtime::Process next(cfg); }
        if (Memory::GuestArena::liveCount() != arenas) _exit(5);
    }
    {
        Runtime::Process stopped(cfg);
        stopped.requestStop();
        bool rejected = false;
        try { stopped.startThread(callback, 0, 0, false); }
        catch (const Runtime::ProcessExit&) { rejected = true; }
        if (!rejected) _exit(7);
    }
    std::puts("ok teardown joins the native worker before releasing its arena; ten later arenas release");
    return 0;
}

int runThreadLifetimeTests() {
    char executable[4096] = "/proc/self/exe";
#ifdef __APPLE__
    uint32_t size = sizeof executable;
    if (_NSGetExecutablePath(executable, &size)) return 2;
#endif
    const auto child = fork();
    if (child < 0) return 2;
    if (!child) {
        execl(executable, executable, "thread-lifetime-child", static_cast<char*>(nullptr));
        _exit(127);
    }
    int status = 0;
    if (waitpid(child, &status, 0) != child) return 2;
    return WIFEXITED(status) && WEXITSTATUS(status) == 0 ? 0 : 1;
}

#ifndef HVM_THREAD_LIFETIME_NO_MAIN
int main(int argc, char** argv) {
    if (argc == 2 && std::string(argv[1]) == "thread-lifetime-child") return runThreadLifetimeChild();
    return runThreadLifetimeTests();
}
#endif
