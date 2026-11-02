#include "core/cpu/Engine.hpp"
#include <atomic>
#include <condition_variable>
#include <cstdio>
#include <mutex>
#include <thread>

using namespace Hydrogen;

int runThreadProgressTests() {
    Memory::GuestArena memory;
    CPU::Engine engine(memory);
    CPU::Cpu busy;
    busy.progress.enabled = true;
    memory.map(0x10000, 4096);
    memory.write<uint32_t>(0x10000, 0xEAFFFFFEu); // ARM B to itself: one retired instruction per iteration
    busy.r[CPU::kPC] = 0x10000;
    engine.run(busy, 40000);
    CPU::ThreadProgressSnapshot first;
    if (!busy.progress.snapshot(first) || first.instructions != 40000 || first.pc != 0x10000) return 1;
    engine.run(busy, 10000);
    CPU::ThreadProgressSnapshot second;
    if (!busy.progress.snapshot(second) || second.instructions != 50000) return 1;
    const auto returning = engine.registerHle("returning_progress_test", [](CPU::Cpu& c) { c.r[0] = 7; });
    engine.callGuest(busy, returning, {});
    CPU::ThreadProgressSnapshot restored;
    if (!busy.progress.snapshot(restored) || restored.pc != busy.r[CPU::kPC] || restored.lr != busy.r[CPU::kLR]) {
        std::puts("FAIL nested guest return leaves a stale progress PC/LR");
        return 1;
    }

    std::mutex mu;
    std::condition_variable cv;
    bool ready = false, release = false;
    auto hle = engine.registerHle("parked_progress_test", [&](CPU::Cpu& cpu) {
        std::unique_lock<std::mutex> lock(mu);
        ready = true;
        cv.notify_all();
        cv.wait(lock, [&] { return release; });
        cpu.exit = CPU::Exit::Halt;
    });
    CPU::Cpu parked;
    parked.progress.enabled = true;
    parked.r[CPU::kPC] = hle;
    parked.r[CPU::kLR] = 0x12345678;
    parked.r[0] = 0xCAFE0000;
    parked.r[1] = 0xFACE0000;
    std::thread worker([&] { engine.run(parked); });
    {
        std::unique_lock<std::mutex> lock(mu);
        cv.wait(lock, [&] { return ready; });
    }
    CPU::ThreadProgressSnapshot sample;
    const bool valid = parked.progress.snapshot(sample);
    const bool correct = valid && sample.pc == hle && sample.lr == 0x12345678 &&
        sample.blockPc == hle && sample.hle == engine.hleIndex(hle) &&
        sample.argument0 == 0xCAFE0000 && sample.argument1 == 0xFACE0000 && !sample.inGuest;
    {
        std::lock_guard<std::mutex> lock(mu);
        release = true;
    }
    cv.notify_all();
    worker.join();
    std::puts(correct ? "ok per-thread instruction progress and parked HLE PC/LR/wait arguments" : "FAIL thread progress snapshot");
    return correct ? 0 : 1;
}

#ifndef HVM_THREAD_PROGRESS_NO_MAIN
int main() { return runThreadProgressTests(); }
#endif
