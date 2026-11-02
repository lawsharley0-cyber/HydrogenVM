#include "core/cpu/Engine.hpp"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <string>
#include <sys/resource.h>
#include <sys/wait.h>
#include <unistd.h>
#ifdef __APPLE__
#include <mach-o/dyld.h>
#endif

using namespace Hydrogen;

int runGuestCallBoundaryChild(int mode) {
    alarm(10);
    const rlimit noCore{0, 0};
    setrlimit(RLIMIT_CORE, &noCore);
    Memory::GuestArena memory;
    CPU::Engine engine(memory);
    CPU::Cpu cpu;
    memory.map(0x21000, 4096);
    memory.write<uint32_t>(0x21ff0, 0xDEADBEEFu);
    if (mode == 0) memory.map(0x22000, 4096);
    if (mode == 3) memory.protect(0x21000, 4096, Memory::kProtRead);
    bool invoked = false;
    const auto target = engine.registerHle("checked_stack_arguments", [&](CPU::Cpu& c) {
        invoked = true;
        if (mode == 0) {
            uint32_t sum = c.r[0] + c.r[1] + c.r[2] + c.r[3];
            for (unsigned i = 0; i < 5; ++i) sum += memory.read<uint32_t>(c.r[CPU::kSP] + i * 4);
            c.r[0] = sum;
        }
    });
    cpu.r[0] = 0xAABBCCDD;
    cpu.r[CPU::kPC] = 0x12345678;
    cpu.r[CPU::kLR] = 0x87654320;
    cpu.r[CPU::kSP] = mode == 1 ? 0x30000 : mode == 4 ? 0 : mode == 3 ? 0x22000 : 0x22008;
    cpu.call_boundary_sp = 0x77777777;
    uint32_t saved[CPU::kNumRegs];
    std::memcpy(saved, cpu.r, sizeof saved);
    const uint32_t args[] = {1, 2, 3, 4, 5, 6, 7, 8, 9};
    const size_t count = mode == 5 ? std::numeric_limits<size_t>::max() : (mode == 1 || mode == 3) ? 5 : 9;
    const uint32_t expectedFault = mode == 1 ? 0x2fff8 : mode == 3 ? 0x21ff8 : mode == 4 ? 0 : 0x22008;
    bool faulted = false;
    CPU::Engine::lastFault() = {};
    try {
        const auto value = engine.callGuest(cpu, target, args, count);
        if (mode != 0 || uint32_t(value) != 45) return 1;
    } catch (const CPU::GuestFault& fault) {
        faulted = true;
        const uint32_t address = mode == 2 ? 0x22000 : expectedFault;
        if (mode == 0 || fault.exit != CPU::Exit::MemFault || fault.info != address ||
            !CPU::Engine::lastFault().valid || CPU::Engine::lastFault().info != address) return 1;
    }
    if (faulted != (mode != 0) || invoked != (mode == 0)) return 1;
    if (std::memcmp(saved, cpu.r, sizeof saved) || cpu.call_boundary_sp != 0x77777777) return 1;
    if (mode == 2 && memory.read<uint32_t>(0x21ff0) != 0xDEADBEEFu) return 1;
    return 0;
}

int runGuestCallBoundaryTests() {
    char executable[4096] = "/proc/self/exe";
#ifdef __APPLE__
    uint32_t size = sizeof executable;
    if (_NSGetExecutablePath(executable, &size)) return 2;
#endif
    const char* names[] = {"valid cross-page arguments", "unmapped stack", "partial cross-page write", "read-only stack", "stack underflow", "argument count overflow"};
    int failures = 0;
    for (int mode = 0; mode < 6; ++mode) {
        char arg[16];
        std::snprintf(arg, sizeof arg, "%d", mode);
        const auto child = fork();
        if (child < 0) return 2;
        if (!child) {
            execl(executable, executable, "guest-call-boundary-child", arg, static_cast<char*>(nullptr));
            _exit(127);
        }
        int status = 0;
        if (waitpid(child, &status, 0) != child) return 2;
        const bool passed = WIFEXITED(status) && WEXITSTATUS(status) == 0;
        std::printf("%s %s", passed ? "ok" : "FAIL", names[mode]);
        if (WIFSIGNALED(status)) std::printf(" escaped as host signal %d", WTERMSIG(status));
        std::puts("");
        failures += !passed;
    }
    return failures ? 1 : 0;
}

#ifndef HVM_GUEST_CALL_BOUNDARY_NO_MAIN
int main(int argc, char** argv) {
    if (argc == 3 && std::string(argv[1]) == "guest-call-boundary-child") return runGuestCallBoundaryChild(std::atoi(argv[2]));
    return runGuestCallBoundaryTests();
}
#endif
