#include "core/cpu/Engine.hpp"
#include <atomic>
#include <cstdio>
#include <thread>

using namespace Hydrogen;

// A suspended Cpu must stay out of guest code until resumed, including when a new suspension begins in the gap
// between its wake-up from the previous one and its re-entry (the resume-to-suspend handoff). Guest execution is
// observed exactly: every entry runs one host function that counts itself.
int runSuspendTests() {
    Memory::GuestArena memory;
    CPU::Engine engine(memory);
    memory.map(0x10000, 4096);
    // loop: ldr r1,[r0]; add r1,r1,#1; str r1,[r0]; b loop. The counter in guest memory shows exactly when guest
    // instructions ran.
    memory.write<uint32_t>(0x10000, 0xE5901000u);
    memory.write<uint32_t>(0x10004, 0xE2811001u);
    memory.write<uint32_t>(0x10008, 0xE5801000u);
    memory.write<uint32_t>(0x1000C, 0xEAFFFFFBu);
    constexpr uint32_t kCounter = 0x10800;
    CPU::Cpu target;
    std::atomic<bool> quit{false};
    std::thread runner([&] {
        while (!quit) {
            target.r[0] = kCounter;
            target.r[CPU::kPC] = 0x10000;
            engine.run(target, 200);
        }
    });
    unsigned violations = 0;
    for (int i = 0; i < 4000; ++i) {
        engine.suspend(target);
        // Once suspend() has returned, no guest code may start until resume().
        const uint32_t before = __atomic_load_n(reinterpret_cast<uint32_t*>(memory.base() + kCounter), __ATOMIC_SEQ_CST);
        for (int j = 0; j < 100; ++j) {
            if (__atomic_load_n(reinterpret_cast<uint32_t*>(memory.base() + kCounter), __ATOMIC_SEQ_CST) != before) { ++violations; break; }
            std::this_thread::yield();
        }
        engine.resume(target);
    }
    quit = true;
    engine.resume(target);
    runner.join();
    if (violations) {
        std::printf("FAIL a suspended Cpu entered guest code (%u observations)\n", violations);
        return 1;
    }
    return 0;
}
