#include "MemoryDiagnostics.hpp"
#include "Process.hpp"
#include "audio/Audio.hpp"
#include <fstream>
#include <sstream>
#include <sys/resource.h>
#include <unistd.h>
#if defined(__APPLE__)
#include <TargetConditionals.h>
#include <mach/mach.h>
#if TARGET_OS_IPHONE
#include <os/proc.h>
#endif
#endif

namespace Hydrogen::Runtime {

HostMemoryStats hostMemoryStats() {
    HostMemoryStats out;
#if defined(__APPLE__)
    task_vm_info_data_t vm{};
    mach_msg_type_number_t count = TASK_VM_INFO_COUNT;
    if (task_info(mach_task_self(), TASK_VM_INFO, reinterpret_cast<task_info_t>(&vm), &count) == KERN_SUCCESS) {
        out.residentBytes = vm.resident_size;
        if (count >= TASK_VM_INFO_REV1_COUNT) out.physicalFootprintBytes = vm.phys_footprint;
    }
#if TARGET_OS_IPHONE
    out.availableBytes = os_proc_available_memory();
#endif
#elif defined(__linux__)
    uint64_t total = 0, resident = 0;
    const long page = sysconf(_SC_PAGESIZE);
    if (std::ifstream("/proc/self/statm") >> total >> resident; page > 0 && resident)
        out.residentBytes = resident * uint64_t(page);
    // MemAvailable is system-wide: it is not the iOS process headroom metric.
    // No portable Linux equivalent of phys_footprint; retain nulls.
#endif
    rusage usage{};
    if (getrusage(RUSAGE_SELF, &usage) == 0) {
#if defined(__APPLE__)
        out.lifetimePeakResidentBytes = uint64_t(usage.ru_maxrss);
#else
        out.lifetimePeakResidentBytes = uint64_t(usage.ru_maxrss) * 1024;
#endif
    }
    return out;
}

std::string memorySnapshotJson(Process& p, const char* event) {
    const auto host = hostMemoryStats();
    const auto heap = p.heap().stats();
    const auto arena = p.mem().stats();
    const auto audio = p.mixer().memoryStats();
    const auto engine = p.engine().stats();
    std::ostringstream out;
    // event is an internal constant, never guest text or user input.
    out << "{\"schema\":\"hvm-memory/v1\",\"event\":\"" << event << "\",\"elapsedSeconds\":"
        << std::chrono::duration<double>(std::chrono::steady_clock::now() - p.perf().origin).count()
        << ",\"hostPageBytes\":" << p.mem().hostPageSize()
        << ",\"backgrounded\":" << (p.backgrounded() ? "true" : "false");
    auto optional = [&](const char* key, std::optional<uint64_t> n) {
        out << ",\"" << key << "\":";
        if (n) out << *n; else out << "null";
    };
    optional("residentBytes", host.residentBytes);
    optional("physicalFootprintBytes", host.physicalFootprintBytes);
    optional("availableBytes", host.availableBytes);
    optional("lifetimePeakResidentBytes", host.lifetimePeakResidentBytes);
    out << ",\"guestHeapLiveBytes\":" << heap.liveBytes << ",\"guestHeapBlockCount\":" << heap.liveBlocks
        << ",\"guestHeapCommittedBytes\":" << heap.committed << ",\"guestHeapBadFreeCount\":" << heap.badFrees
        << ",\"guestMappedBytes\":" << arena.mappedBytes << ",\"guestCommittedHostBytes\":" << arena.committedHostBytes
        << ",\"liveArenaCount\":" << Memory::GuestArena::liveCount()
        << ",\"audioVoiceCount\":" << audio.voiceCount << ",\"audioQueuedBufferCount\":" << audio.queuedBufferCount
        << ",\"audioReferencedPcmBytes\":" << audio.referencedPcmBytes
        << ",\"audioProcessedTagCount\":" << audio.processedTagCount
        << ",\"audioProcessedTagCapacityBytes\":" << audio.processedTagCapacityBytes
        << ",\"engineDecodedBlockCount\":" << engine.blocks_decoded
        << ",\"engineEmittedOpCount\":" << engine.ops_emitted
        << ",\"framesPresentedCount\":" << p.perf().framesPresented.load()
        // Explicit coverage gaps: zero would falsely imply measured emptiness.
        << ",\"glTextureCount\":null,\"glTextureEstimatedBytes\":null"
        << ",\"glFramebufferCount\":null,\"glRenderbufferCount\":null"
        << ",\"assetCacheBytes\":null,\"textureCacheBytes\":null"
        << ",\"audioDecodedCacheBytes\":null,\"checkpointCount\":null,\"checkpointBytes\":null"
        << ",\"offlinePendingMessageCount\":null,\"offlineQueueBytes\":null}";
    return out.str();
}

void Process::recordMemorySample(const char* event) {
    std::string sample;
    {
        std::lock_guard<std::mutex> lock(memorySamplesMu_);
        sample = memorySnapshotJson(*this, event);
        if (memorySamples_.size() == 120) memorySamples_.pop_front();
        memorySamples_.push_back(sample);
        // Existing shell journaling persists the log tail even after a host kill.
        // A sample is <2 KB; no per-frame/per-allocation traffic is added. Logged under the
        // lock so log order matches sample timestamps across lifecycle and interval threads.
        log(LogLevel::Info, "[memory] " + sample);
    }
}

std::string Process::memorySamplesJson() const {
    std::lock_guard<std::mutex> lock(memorySamplesMu_);
    std::string out = "[";
    for (const auto& sample : memorySamples_) {
        if (out.size() > 1) out += ',';
        out += sample;
    }
    return out + ']';
}
} // namespace Hydrogen::Runtime
