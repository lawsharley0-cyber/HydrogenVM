#include "MemoryTelemetry.hpp"
#include <chrono>
#include <fstream>
#include <sstream>
#include <iomanip>
#include <iostream>

#ifdef __APPLE__
#include <TargetConditionals.h>
#include <mach/mach.h>
#include <mach/task_info.h>
#include <libproc.h>
#if TARGET_OS_IOS || TARGET_OS_TV || TARGET_OS_WATCH
#include <os/proc.h>
#endif
#include <sys/resource.h>
#endif

namespace Hydrogen::Runtime {

namespace {
std::string escapeJson(const std::string& s) {
    std::ostringstream o;
    o << '"';
    for (char c : s) {
        if (c == '"') o << "\\\"";
        else if (c == '\\') o << "\\\\";
        else if (c == '\b') o << "\\b";
        else if (c == '\f') o << "\\f";
        else if (c == '\n') o << "\\n";
        else if (c == '\r') o << "\\r";
        else if (c == '\t') o << "\\t";
        else if (c >= 0 && c < 32) {
            o << "\\u" << std::hex << std::setw(4) << std::setfill('0') << int(c);
        } else o << c;
    }
    o << '"';
    return o.str();
}
}

MemoryTelemetry::MemoryTelemetry(std::string bundleId, std::string snapshotPath)
    : bundleId_(std::move(bundleId)), snapshotPath_(std::move(snapshotPath)) {
    // Register the 0 state as "unknown"
    stateNames_.push_back("unknown");
    lastFlushTime_ = std::chrono::steady_clock::now();
}

MemoryTelemetry::~MemoryTelemetry() {
    stop();
}

void MemoryTelemetry::start() {
    stop_ = false;
    samplerThread_ = std::thread([this]() { sampleLoop(); });
}

void MemoryTelemetry::stop() {
    if (!stop_.exchange(true) && samplerThread_.joinable()) {
        sleepCv_.notify_one();
        samplerThread_.join();
        // One final flush before exiting
        performSample(true);
    }
}

TelemetryCounter MemoryTelemetry::registerCounter(const std::string& name) {
    std::lock_guard<std::mutex> lk(metaMu_);
    if (counterNames_.size() >= kMaxTelemetryCounters) return kMaxTelemetryCounters; // invalid id
    size_t id = counterNames_.size();
    counterNames_.push_back(name);
    return id;
}

void MemoryTelemetry::updateCounter(TelemetryCounter id, uint64_t value) noexcept {
    if (id < kMaxTelemetryCounters) {
        currentCounters_[id].store(value, std::memory_order_relaxed);
    }
}

uint8_t MemoryTelemetry::registerState(const std::string& stateName) {
    std::lock_guard<std::mutex> lk(metaMu_);
    if (stateNames_.size() >= 255) return 0;
    uint8_t id = static_cast<uint8_t>(stateNames_.size());
    stateNames_.push_back(stateName);
    return id;
}

void MemoryTelemetry::setState(uint8_t stateId) noexcept {
    currentState_.store(stateId, std::memory_order_relaxed);
}

void MemoryTelemetry::notifyMemoryWarning() noexcept {
    memoryWarningFlag_.store(true, std::memory_order_relaxed);
    sleepCv_.notify_one();
}

void MemoryTelemetry::sampleLoop() {
    while (!stop_.load(std::memory_order_relaxed)) {
        performSample(false);
        std::unique_lock<std::mutex> lk(sleepMu_);
        sleepCv_.wait_for(lk, std::chrono::seconds(1), [this] {
            return stop_.load(std::memory_order_relaxed) || memoryWarningFlag_.load(std::memory_order_relaxed);
        });
    }
}

void MemoryTelemetry::performSample(bool forceFlush) {
    uint64_t residentBytes = ~0ull;
    uint64_t physicalFootprint = ~0ull;
    uint64_t availableBytes = ~0ull;
    uint64_t peakFootprint = ~0ull;

#ifdef __APPLE__
    task_vm_info_data_t vmInfo;
    mach_msg_type_number_t count = TASK_VM_INFO_COUNT;
    kern_return_t kr = task_info(mach_task_self(), TASK_VM_INFO, (task_info_t)&vmInfo, &count);
    if (kr == KERN_SUCCESS) {
        residentBytes = vmInfo.resident_size;
        physicalFootprint = vmInfo.phys_footprint;
        peakFootprint = vmInfo.lifetime_max_phys_footprint;
    }

    // os_proc_available_memory is iOS/tvOS/watchOS specific, but we check availability
    if (__builtin_available(iOS 13.0, macOS 10.15, tvOS 13.0, watchOS 6.0, *)) {
#if TARGET_OS_IOS || TARGET_OS_TV || TARGET_OS_WATCH
        availableBytes = os_proc_available_memory();
#endif
    }
#endif

    uint64_t obsPeak = observedPeak_.load(std::memory_order_relaxed);
    if (physicalFootprint != ~0ull && physicalFootprint > obsPeak) {
        observedPeak_.store(physicalFootprint, std::memory_order_relaxed);
        obsPeak = physicalFootprint;
    }

    bool memWarning = memoryWarningFlag_.exchange(false, std::memory_order_relaxed);

    MemorySample sample;
    sample.timestampNs = std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    sample.residentBytes = residentBytes;
    sample.physicalFootprint = physicalFootprint;
    sample.availableBytes = availableBytes;
    sample.peakFootprint = peakFootprint;
    sample.observedPeak = obsPeak;
    sample.stateId = currentState_.load(std::memory_order_relaxed);
    sample.memoryWarning = memWarning;

    for (size_t i = 0; i < kMaxTelemetryCounters; ++i) {
        sample.counters[i] = currentCounters_[i].load(std::memory_order_relaxed);
    }

    size_t head = historyHead_.load(std::memory_order_relaxed);
    history_[head] = sample;
    historyHead_.store((head + 1) % kHistorySize, std::memory_order_relaxed);

    size_t cnt = historyCount_.load(std::memory_order_relaxed);
    if (cnt < kHistorySize) historyCount_.store(cnt + 1, std::memory_order_relaxed);

    auto now = std::chrono::steady_clock::now();
    if (forceFlush || memWarning || (now - lastFlushTime_ > std::chrono::seconds(30))) {
        flushToDisk();
        lastFlushTime_ = now;
    }
}

std::string MemoryTelemetry::reportJson() const {
    std::ostringstream out;
    out << "{";
    out << "\"bundleId\":" << escapeJson(bundleId_) << ",";

    std::vector<std::string> cnames;
    std::vector<std::string> snames;
    {
        std::lock_guard<std::mutex> lk(const_cast<std::mutex&>(metaMu_));
        cnames = counterNames_;
        snames = stateNames_;
    }

    out << "\"counterNames\":[";
    for (size_t i = 0; i < cnames.size(); ++i) {
        if (i > 0) out << ",";
        out << escapeJson(cnames[i]);
    }
    out << "],";

    out << "\"stateNames\":[";
    for (size_t i = 0; i < snames.size(); ++i) {
        if (i > 0) out << ",";
        out << escapeJson(snames[i]);
    }
    out << "],";

    out << "\"samples\":[";
    size_t cnt = historyCount_.load(std::memory_order_relaxed);
    size_t head = historyHead_.load(std::memory_order_relaxed);
    size_t start = (cnt < kHistorySize) ? 0 : head;

    for (size_t i = 0; i < cnt; ++i) {
        if (i > 0) out << ",";
        size_t idx = (start + i) % kHistorySize;
        const auto& s = history_[idx];
        out << "{";
        out << "\"timestampNs\":" << s.timestampNs << ",";
        out << "\"residentBytes\":" << s.residentBytes << ",";
        out << "\"physicalFootprint\":" << s.physicalFootprint << ",";
        out << "\"availableBytes\":" << s.availableBytes << ",";
        out << "\"peakFootprint\":" << s.peakFootprint << ",";
        out << "\"observedPeak\":" << s.observedPeak << ",";
        out << "\"stateId\":" << (int)s.stateId << ",";
        out << "\"memoryWarning\":" << (s.memoryWarning ? "true" : "false") << ",";
        out << "\"counters\":[";
        for (size_t j = 0; j < cnames.size(); ++j) {
            if (j > 0) out << ",";
            out << s.counters[j];
        }
        out << "]";
        out << "}";
    }
    out << "]}";
    return out.str();
}

void MemoryTelemetry::flushToDisk() const {
    if (snapshotPath_.empty()) return;
    std::string json = reportJson();
    std::ofstream out(snapshotPath_, std::ios::trunc);
    if (out) {
        out << json;
        out.flush();
    }
}

std::string MemoryTelemetry::recoverOrphanedSnapshot() const {
    if (snapshotPath_.empty()) return "";
    std::ifstream in(snapshotPath_);
    if (!in) return "";
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

} // namespace Hydrogen::Runtime
