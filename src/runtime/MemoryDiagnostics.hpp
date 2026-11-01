// Read-only memory diagnostics. No guest calls, cache eviction or GPU queries.
#pragma once
#include <cstdint>
#include <optional>
#include <string>

namespace Hydrogen::Runtime {
class Process;
struct HostMemoryStats {
    std::optional<uint64_t> residentBytes, physicalFootprintBytes, availableBytes, lifetimePeakResidentBytes;
};
HostMemoryStats hostMemoryStats();
std::string memorySnapshotJson(Process& process, const char* event);
} // namespace Hydrogen::Runtime
