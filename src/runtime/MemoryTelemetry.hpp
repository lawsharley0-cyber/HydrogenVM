#pragma once
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace Hydrogen::Runtime {

// A memory counter slot. Other subsystems can hold this index.
using TelemetryCounter = size_t;
constexpr size_t kMaxTelemetryCounters = 32;

struct MemorySample {
    uint64_t timestampNs; // since epoch or origin
    uint64_t residentBytes;
    uint64_t physicalFootprint;
    uint64_t availableBytes;
    uint64_t peakFootprint;
    uint64_t observedPeak;
    std::array<uint64_t, kMaxTelemetryCounters> counters;
    uint8_t stateId; // Index into registered states
    bool memoryWarning;
};

class MemoryTelemetry {
public:
    MemoryTelemetry(std::string bundleId, std::string snapshotPath);
    ~MemoryTelemetry();
    MemoryTelemetry(const MemoryTelemetry&) = delete;
    MemoryTelemetry& operator=(const MemoryTelemetry&) = delete;

    // Call after initializations to start the 1Hz sampling thread
    void start();
    void stop();

    // Register a counter. Returns the index into the counters array.
    // Thread safe, but expected to be called during initialization.
    TelemetryCounter registerCounter(const std::string& name);

    // Update a counter value. Wait-free.
    void updateCounter(TelemetryCounter id, uint64_t value) noexcept;

    // Register a named state (e.g. "battle_start") and return its ID.
    // Thread safe, but expected to be called during initialization.
    uint8_t registerState(const std::string& stateName);

    // Update the current state ID. Wait-free.
    void setState(uint8_t stateId) noexcept;

    // Notify that a memory warning occurred. The next sample will reflect this and flush.
    void notifyMemoryWarning() noexcept;

    // Get the JSON representation of the currently stored samples.
    std::string reportJson() const;

    // Restore any orphaned snapshot from a previous run (if Jetsam killed us).
    std::string recoverOrphanedSnapshot() const;

private:
    void sampleLoop();
    void performSample(bool forceFlush);
    void flushToDisk() const; // writes compact snapshot

    std::string bundleId_;
    std::string snapshotPath_;
    std::atomic<bool> stop_{false};
    std::thread samplerThread_;
    std::mutex sleepMu_;
    std::condition_variable sleepCv_;

    std::mutex metaMu_;
    std::vector<std::string> counterNames_;
    std::vector<std::string> stateNames_;

    std::atomic<uint8_t> currentState_{0};
    std::atomic<bool> memoryWarningFlag_{false};
    std::array<std::atomic<uint64_t>, kMaxTelemetryCounters> currentCounters_{};
    std::atomic<uint64_t> observedPeak_{0};

    // 600 samples = 10 minutes at 1Hz
    static constexpr size_t kHistorySize = 600;
    std::array<MemorySample, kHistorySize> history_{};
    std::atomic<size_t> historyHead_{0}; // points to the *next* index to write
    std::atomic<size_t> historyCount_{0};

    // Time of last disk flush to throttle writes
    std::chrono::steady_clock::time_point lastFlushTime_;
};

} // namespace Hydrogen::Runtime
