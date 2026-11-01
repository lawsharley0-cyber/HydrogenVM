#include "core/cpu/Engine.hpp"
#include <atomic>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <thread>
#include <sys/wait.h>
#include <sys/resource.h>
#include <unistd.h>
#ifdef __APPLE__
#include <mach-o/dyld.h>
#endif

using namespace Hydrogen;

// A real signal on a host callback thread, while two guest threads are in HLE.
// The file (rather than a pipe) deliberately permits a full 200-line report.
int runHostFaultChild(int fd, bool stress) {
        const rlimit noCore{0, 0};
        setrlimit(RLIMIT_CORE, &noCore);
        alarm(15);
        Memory::GuestArena arena;
        CPU::Engine engine(arena);
        CPU::Engine::setFaultReportFd(fd);
        std::atomic<unsigned> ready{0};
        auto hle = engine.registerHle("host_fault_test_wait", [&](CPU::Cpu&) {
            ready.fetch_add(1);
            for (;;) pause();
        });
        auto worker = [&](uint32_t lr) {
            CPU::Cpu cpu;
            cpu.r[CPU::kPC] = hle;
            cpu.r[CPU::kLR] = lr;
            engine.run(cpu);
        };
        std::thread a(worker, 0x11223344), b(worker, 0x55667788);
        while (ready.load() != 2) std::this_thread::yield();
        for (unsigned i = 0; i < 205; ++i) {
            std::string line = "fault-log-" + std::to_string(i) + ":" + std::string(220, 'x');
            CPU::Engine::recordLogLineAsyncSafe(line.data(), line.size());
        }
        auto fault = [&] { *reinterpret_cast<volatile unsigned*>(arena.base() + 0x4000) = 1; };
        if (stress) {
            std::atomic<unsigned> churned{0}, logged{0};
            auto logWorker = [&] {
                const std::string line(250, 'z');
                for (;;) {
                    CPU::Engine::recordLogLineAsyncSafe(line.data(), line.size());
                    logged.fetch_add(1);
                }
            };
            std::thread loggerA(logWorker), loggerB(logWorker);
            std::thread churn([&] {
                for (;;) {
                    // Destroy the Engine's names before its Cpu. The signal
                    // reader must use permanent copies during this churn.
                    CPU::Cpu temporaryCpu;
                    CPU::Engine temporaryEngine(arena);
                    const auto h = temporaryEngine.registerHle(std::string(200, 'q'), [](CPU::Cpu& c) { c.exit = CPU::Exit::Halt; });
                    temporaryCpu.r[CPU::kPC] = h;
                    temporaryEngine.run(temporaryCpu);
                    churned.fetch_add(1);
                }
            });
            while (churned.load() < 20 || logged.load() < 1000) std::this_thread::yield();
            fault();
            _exit(3);
        }
        fault();
        _exit(3);
}

static int runOneHostFaultTest(bool stress) {
    char path[] = "/tmp/hvm-host-fault-XXXXXX";
    int fd = mkstemp(path);
    if (fd < 0) return 2;
    unlink(path);
    char executable[4096] = "/proc/self/exe";
#ifdef __APPLE__
    uint32_t size = sizeof executable;
    if (_NSGetExecutablePath(executable, &size)) return 2;
#endif
    char fdArg[32];
    std::snprintf(fdArg, sizeof fdArg, "%d", fd);
    pid_t child = fork();
    if (child < 0) return 2;
    if (!child) {
        // fulltest may have other host threads. Only async-safe operations are
        // allowed between fork and exec; never construct an Engine here.
        execl(executable, executable, stress ? "hostfault-stress-child" : "hostfault-child", fdArg, static_cast<char*>(nullptr));
        _exit(127);
    }
    int status = 0;
    if (waitpid(child, &status, 0) != child) return 2;
    lseek(fd, 0, SEEK_SET);
    std::string report;
    char buf[4096];
    ssize_t n;
    while ((n = read(fd, buf, sizeof buf)) > 0) report.append(buf, size_t(n));
    close(fd);
    int failures = 0;
    auto check = [&](bool ok, const char* name) {
        std::printf("%s %s\n", ok ? "ok" : "FAIL", name);
        failures += !ok;
    };
    auto has = [&](const char* s) { return report.find(s) != std::string::npos; };
    check(WIFSIGNALED(status) && WTERMSIG(status) == SIGSEGV, "fatal signal preserved");
    check(has("Host Fault Address (si_addr): 0x") && has("Guest Arena Offset: 0x00004000"), "callback fault address and arena offset");
    check(has("LR=0x11223344") && has("LR=0x55667788") && has("PC=0xffff0004") && has("block_pc=0xffff0004"), "both guest thread register snapshots");
    check(has("host_fault_test_wait"), "current HLE name survives signal");
    if (stress) {
        check(has("=== END HOST FAULT DIAGNOSTICS ==="), "complete signal report during concurrent logging and CPU/Engine destruction");
        return failures ? 1 : 0;
    }
    check(!has("fault-log-4:") && has("fault-log-5:") && has("fault-log-204:"), "last 200 lines in ring");
    size_t count = 0, pos = 0;
    while ((pos = report.find("fault-log-", pos)) != std::string::npos) { ++count; ++pos; }
    check(count == 200 && report.size() > 40000 && has("=== END HOST FAULT DIAGNOSTICS ==="), "complete large bundle");

    // The oracle is VM bookkeeping, not a device-equivalence claim: one 16 KB
    // backing must remain until all four constituent 4 KB guest pages are gone.
    Memory::GuestArena arena16(16384);
    const uint32_t a = 0x20000, b = a + 4096;
    arena16.map(a, 4096);
    arena16.write<uint32_t>(a, 0x11223344);
    arena16.map(b, 4096);
    check(arena16.read<uint32_t>(a) == 0x11223344 && arena16.stats().committedHostBytes == 16384,
          "16 KB commit preserves adjacent 4 KB page");
    arena16.protect(a, 4096, Memory::kProtRead);
    check(arena16.isMapped(b) && arena16.read<uint32_t>(a) == 0x11223344,
          "16 KB backing survives guest protection change");
    arena16.write<uint32_t>(b, 0x55667788);
    arena16.unmap(a, 4096);
    check(!arena16.isMapped(a) && arena16.isMapped(b) && arena16.read<uint32_t>(b) == 0x55667788 &&
          arena16.stats().committedHostBytes == 16384, "partial unmap preserves neighboring data and backing");
    arena16.unmap(b, 4096);
    check(arena16.stats().committedHostBytes == 0, "last guest unmap releases 16 KB backing");
    arena16.map(b, 4096);
    check(arena16.read<uint32_t>(b) == 0, "remapped 16 KB backing discards previous contents");
    arena16.unmap(b, 4096);
    return failures ? 1 : 0;
}

int runHostFaultTests() {
    const int single = runOneHostFaultTest(false);
    const int concurrent = runOneHostFaultTest(true);
    return single || concurrent;
}

#ifndef HVM_HOST_FAULT_NO_MAIN
int main(int argc, char** argv) {
    if (argc == 3 && (std::string(argv[1]) == "hostfault-child" || std::string(argv[1]) == "hostfault-stress-child"))
        return runHostFaultChild(std::atoi(argv[2]), std::string(argv[1]) == "hostfault-stress-child");
    return runHostFaultTests();
}
#endif
