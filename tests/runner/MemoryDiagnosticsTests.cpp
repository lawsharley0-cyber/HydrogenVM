#include "runtime/Process.hpp"
#include "runtime/MemoryDiagnostics.hpp"
#include "runtime/audio/Audio.hpp"
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <condition_variable>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#include <unistd.h>
using namespace Hydrogen;
using namespace Hydrogen::Runtime;

// Minimal allocator retention experiment, not a game or a leak oracle.
static int heapProbe() {
    Memory::GuestArena mem;
    Heap heap(mem, 0x10000000, 0x30000000);
    const auto start = std::chrono::steady_clock::now();
    auto sample = [&](int iteration, const char* event) {
        auto h = heap.stats(); auto host = hostMemoryStats();
        const auto resident = host.residentBytes ? std::to_string(*host.residentBytes) : "null";
        std::printf("{\"elapsedSeconds\":%.6f,\"iteration\":%d,\"event\":\"%s\","
                    "\"residentBytes\":%s,\"guestHeapLiveBytes\":%llu,\"guestHeapCommittedBytes\":%llu}\n",
                    std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count(), iteration, event,
                    resident.c_str(),
                    (unsigned long long)h.liveBytes, (unsigned long long)h.committed);
    };
    sample(0, "baseline");
    for (int n = 1; n <= 10; ++n) {
        const auto block = heap.alloc(64u << 20);
        if (!block) return 1;
        for (uint32_t offset = 0; offset < (64u << 20); offset += 4096)
            mem.write<uint8_t>(block + offset, 0x5a);
        sample(n, "allocated");
        heap.free(block);
        sample(n, "freed");
    }
    return 0;
}

int main(int argc, char** argv) {
    if (argc == 2 && std::string(argv[1]) == "--heap-probe") return heapProbe();
    int failed = 0, passed = 0;
    auto check = [&](bool ok, const char* name) {
        std::printf("%s %s\n", ok ? "ok" : "FAIL", name);
        ok ? ++passed : ++failed;
    };
    {
        Memory::GuestArena mem;
        check(mem.stats().mappedBytes == 0 && mem.stats().committedHostBytes == 0, "reservation is not mapped or committed backing");
        const uint32_t base = 0x10000000;
        mem.map(base, 16384);
        mem.map(base + 4096, 4096); // overlap must not count twice
        auto stats = mem.stats();
        check(stats.mappedBytes == 16384 && stats.committedHostBytes == mem.hostPageSize() * ((16384 + mem.hostPageSize() - 1) / mem.hostPageSize()),
              "arena counts guest mappings separately from host backing");
        mem.protect(base, 4096, Memory::kProtRX);
        mem.unmap(base + 4096, 4096);
        check(mem.stats().mappedBytes == 12288, "partial unmap and protect preserve mapped accounting");
        mem.unmap(base, 16384);
        check(mem.stats().mappedBytes == 0 && mem.stats().committedHostBytes == 0, "complete unmap removes host backing");
    }
    {
        Audio::Mixer mixer;
        auto pcm = std::make_shared<const std::vector<int16_t>>(882, 1000);
        const auto voice = mixer.createStream({});
        mixer.enqueue(voice, pcm, 44100, 1);
        mixer.enqueue(voice, pcm, 44100, 2);
        auto stats = mixer.memoryStats();
        check(stats.voiceCount == 1 && stats.queuedBufferCount == 2 && stats.referencedPcmBytes == 2 * pcm->capacity() * sizeof(int16_t),
              "PCM accounting explicitly counts shared references twice");
        mixer.stop(voice);
        stats = mixer.memoryStats();
        check(stats.queuedBufferCount == 0 && stats.referencedPcmBytes == 0 && stats.processedTagCount == 2,
              "stopping drops queued PCM and retains completion tags");
        mixer.takeProcessed(voice);
        check(mixer.memoryStats().processedTagCount == 0 && mixer.memoryStats().processedTagCapacityBytes >= 16,
              "processed size and retained capacity are distinguished");
        mixer.release(voice);
        check(mixer.memoryStats().voiceCount == 0 && mixer.memoryStats().processedTagCapacityBytes == 0,
              "voice release drops completion storage");
    }
    const auto home = std::filesystem::temp_directory_path() / ("hvm-memory-tests-" + std::to_string(getpid()));
    {
        ProcessConfig cfg;
        cfg.homePath = home.string(); cfg.bundlePath = (home / "Test.app").string();
        cfg.log = [](LogLevel, const std::string&) {};
        Process process(cfg);
        const auto before = process.heap().stats();
        const auto start = std::chrono::steady_clock::now();
        for (int n = 0; n < 125; ++n) process.recordMemorySample("test");
        const auto elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
        const auto samples = process.memorySamplesJson();
        size_t count = 0, offset = 0;
        while ((offset = samples.find("\"schema\":", offset)) != std::string::npos) { ++count; ++offset; }
        check(count == 120, "runtime history is bounded independently of log volume");
        check(samples.find("\"glTextureCount\":null") != std::string::npos && samples.find("\"checkpointBytes\":null") != std::string::npos,
              "unavailable graphics and guest counters remain null");
        check(process.heap().stats().liveBytes == before.liveBytes && process.heap().stats().committed == before.committed,
              "sampling does not allocate guest memory");
        const auto host = hostMemoryStats();
#if defined(__linux__)
        check(host.residentBytes.has_value() && !host.physicalFootprintBytes && !host.availableBytes,
              "Linux reports RSS without fabricating iOS footprint or headroom");
#endif
        std::printf("diagnostic average: %.3f ms/sample (125 samples)\n", elapsed * 1000 / 125);
    }
    {
        unsetenv("HVM_MEMORY_DIAG");
        ProcessConfig cfg;
        cfg.homePath = home.string(); cfg.bundlePath = (home / "Test.app").string();
        cfg.log = [](LogLevel, const std::string&) {};
        auto process = std::make_unique<Process>(cfg);
        process->setBackgrounded(true); // Should not emit a sample
        check(process->memorySamplesJson() == "[]", "sampler is disabled without HVM_MEMORY_DIAG");
    }
    {
        setenv("HVM_MEMORY_DIAG", "1", 1);
        ProcessConfig cfg;
        cfg.homePath = home.string(); cfg.bundlePath = (home / "Test.app").string();
        cfg.log = [](LogLevel, const std::string&) {};
        auto process = std::make_unique<Process>(cfg);
        process->setBackgrounded(true); // Should emit a sample
        check(process->memorySamplesJson() != "[]", "sampler is enabled with HVM_MEMORY_DIAG");
    }
    {
        // Process::run() and setBackgrounded() share one gate.
        unsetenv("HVM_MEMORY_DIAG");
        const bool off = !Process::memoryDiagEnabled();
        setenv("HVM_MEMORY_DIAG", "1", 1);
        check(off && Process::memoryDiagEnabled(), "memory sampling gate follows HVM_MEMORY_DIAG");
    }
    {
        // A worker must be able to record a later sample while an earlier one is still being logged; the
        // earlier sample's log call is held until then (or a timeout). Logging under the sample lock keeps order.
        setenv("HVM_MEMORY_DIAG", "1", 1);
        std::mutex mu;
        std::condition_variable cv;
        std::vector<std::string> logged;
        bool firstInLog = false, secondLogged = false;
        ProcessConfig cfg;
        cfg.homePath = home.string(); cfg.bundlePath = (home / "Test.app").string();
        cfg.log = [&](LogLevel, const std::string& m) {
            if (m.rfind("[memory] ", 0) != 0) return;
            std::unique_lock<std::mutex> lk(mu);
            const bool first = !firstInLog;
            if (first) {
                firstInLog = true;
                cv.notify_all();
                cv.wait_for(lk, std::chrono::milliseconds(500), [&] { return secondLogged; });
                logged.push_back(m.substr(9));
            } else {
                logged.push_back(m.substr(9));
                secondLogged = true;
                cv.notify_all();
            }
        };
        auto process = std::make_unique<Process>(cfg);
        std::thread a([&] { process->recordMemorySample("interval"); });
        {
            std::unique_lock<std::mutex> lk(mu);
            cv.wait_for(lk, std::chrono::seconds(5), [&] { return firstInLog; });
        }
        std::thread b([&] { process->recordMemorySample("foregrounded"); });
        a.join(); b.join();
        std::string joined = "[";
        for (size_t i = 0; i < logged.size(); ++i) { if (i) joined += ','; joined += logged[i]; }
        check(logged.size() == 2 && joined + "]" == process->memorySamplesJson(),
              "concurrent memory samples are logged in recorded order");
    }
    std::filesystem::remove_all(home);
    std::printf("memory tests: %d passed, %d failed; host page %ld\n", passed, failed, sysconf(_SC_PAGESIZE));
    return failed ? 1 : 0;
}
