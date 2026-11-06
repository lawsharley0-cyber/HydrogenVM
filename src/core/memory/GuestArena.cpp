#include "GuestArena.hpp"
#include <sys/mman.h>
#include <unistd.h>
#include <stdexcept>
#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <sstream>
#include <string>
#include <thread>
#include <vector>
#if defined(__APPLE__)
#include <mach/mach.h>
#include <mach/vm_region.h>
#include <mach/mach_vm.h>
#include <os/log.h>
#endif

namespace Hydrogen::Memory {
namespace {
std::atomic<uint32_t> liveArenas{0};

// One 4 GB range, PROT_NONE and owned by nobody, waiting for the next
// GuestArena. Destroying an arena returns its pages but keeps the range, so a
// batch run never asks the host for another 4 GB between games.
std::mutex poolMu;
uint8_t* pooledBase = nullptr;

#if defined(__APPLE__)
// Largest reservations in this task, for a failed 4 GB reservation. Read-only:
// no guest memory, no new mappings, no GPU calls.
std::string largestMappings() {
    struct Region { mach_vm_address_t start; mach_vm_size_t size; vm_prot_t prot; };
    std::vector<Region> regions;
    mach_vm_address_t address = 0;
    while (regions.size() < 4096) {
        mach_vm_size_t size = 0;
        vm_region_basic_info_data_64_t info{};
        mach_msg_type_number_t count = VM_REGION_BASIC_INFO_COUNT_64;
        mach_port_t object = MACH_PORT_NULL;
        if (mach_vm_region(mach_task_self(), &address, &size, VM_REGION_BASIC_INFO_64,
                           reinterpret_cast<vm_region_info_t>(&info), &count, &object) != KERN_SUCCESS)
            break;
        if (object != MACH_PORT_NULL) mach_port_deallocate(mach_task_self(), object);
        if (!size) break;
        regions.push_back({address, size, info.protection});
        address += size;
    }
    std::sort(regions.begin(), regions.end(),
              [](const Region& a, const Region& b) { return a.size > b.size; });
    std::ostringstream out;
    const size_t shown = std::min<size_t>(regions.size(), 5);
    for (size_t i = 0; i < shown; ++i) {
        if (i) out << ", ";
        out << "0x" << std::hex << regions[i].start << "+0x" << regions[i].size
            << " prot=0x" << int(regions[i].prot) << std::dec;
    }
    if (!shown) out << "none";
    out << " (" << regions.size() << " regions)";
    return out.str();
}

std::string hostAddressSpaceSummary() {
    task_vm_info_data_t vm{};
    mach_msg_type_number_t count = TASK_VM_INFO_COUNT;
    std::ostringstream out;
    if (task_info(mach_task_self(), TASK_VM_INFO, reinterpret_cast<task_info_t>(&vm), &count) == KERN_SUCCESS) {
        out << "virtual_size=" << vm.virtual_size << " bytes, resident_size=" << vm.resident_size << " bytes";
        if (count >= TASK_VM_INFO_REV1_COUNT) out << ", phys_footprint=" << vm.phys_footprint << " bytes";
        else out << ", phys_footprint=unavailable";
    } else {
        out << "TASK_VM_INFO unavailable";
    }
    out << "; largest mappings: " << largestMappings();
    return out.str();
}
#endif
} // namespace

uint32_t GuestArena::liveCount() noexcept { return liveArenas.load(std::memory_order_acquire); }

bool GuestArena::waitForRelease(double timeoutSeconds, uint32_t targetCount) noexcept {
    if (liveCount() <= targetCount) return true;
    const auto start = std::chrono::steady_clock::now();
    const auto timeout = std::chrono::duration<double>(timeoutSeconds);
    while (liveCount() > targetCount) {
        if (std::chrono::steady_clock::now() - start >= timeout) return false;
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    return true;
}

GuestArena::Stats GuestArena::stats() const {
    std::lock_guard<std::mutex> lk(mu_);
    Stats out;
    for (auto prot : prot_) if (prot != kProtNone) out.mappedBytes += kGuestPageSize;
    for (auto committed : host_committed_) if (committed) out.committedHostBytes += host_page_;
    return out;
}

GuestArena::GuestArena(size_t hostPage) {
    const size_t nativePage = size_t(sysconf(_SC_PAGESIZE));
    host_page_ = hostPage ? hostPage : nativePage;
    if (host_page_ < kGuestPageSize || host_page_ < nativePage ||
        host_page_ > kArenaSize || (host_page_ & (host_page_ - 1)))
        throw std::runtime_error("HydrogenVM: unsupported host page size");

    // NOTE (iOS): reserving 4 GB of address space needs the
    // com.apple.developer.kernel.extended-virtual-addressing entitlement on most
    // devices; without it this mmap fails. See docs/ARCHITECTURE.md.
    // A destroyed GuestArena leaves its range in the pool, so this normally takes
    // over that reservation instead of asking the host for another 4 GB.
    void* p = MAP_FAILED;
    const auto takePooled = [&p] {
        std::lock_guard<std::mutex> lk(poolMu);
        if (pooledBase) {
            p = pooledBase;
            pooledBase = nullptr;
        }
    };
    takePooled();
    int lastError = 0;
    // The previous game's arena may still be on its way out (its threads are
    // finishing); iOS has room for about one 4 GB reservation, so wait a few
    // seconds for it rather than failing the next launch (Test All Games).
    for (int attempt = 0; p == MAP_FAILED && attempt < 20; ++attempt) {
        if (attempt) std::this_thread::sleep_for(std::chrono::milliseconds(250));
        // An arena that was still live at the first look may have been destroyed meanwhile and
        // published its range to the pool; take that instead of asking the host for another 4 GB.
        takePooled();
        if (p != MAP_FAILED) break;
        errno = 0;
        p = mmap(nullptr, size_t(kArenaSize), PROT_NONE, MAP_PRIVATE | MAP_ANON | MAP_NORESERVE, -1, 0);
        if (p == MAP_FAILED) lastError = errno;
    }
    if (p == MAP_FAILED) {
        std::string message = "HydrogenVM: failed to reserve 4 GB guest arena: " +
                              std::string(std::strerror(lastError)) + " (errno " + std::to_string(lastError) +
                              ", live arenas " + std::to_string(liveCount()) + ", pooled range none" +
                              "; on iOS, check the extended-virtual-addressing entitlement)";
#if defined(__APPLE__)
        // Issue #307: distinguish our own arenas from whatever else holds the
        // address space, and keep the numbers even if the exception is swallowed.
        message += "; host address space: " + hostAddressSpaceSummary();
        os_log_error(OS_LOG_DEFAULT, "HydrogenVM: %{public}s", message.c_str());
#endif
        throw std::runtime_error(message);
    }
    base_ = static_cast<uint8_t*>(p);
    prot_.assign(kGuestPageCount, kProtNone);
    host_committed_.assign(size_t(kArenaSize / host_page_), 0);
    liveArenas.fetch_add(1, std::memory_order_release);
}

GuestArena::~GuestArena() {
    if (!base_) return;
    uint8_t* base = base_;
    base_ = nullptr;
    // Commit nothing, discard everything: one MAP_FIXED PROT_NONE mapping replaces
    // the reservation and every committed host page in it (so the next arena
    // reads zero and a stale guest pointer faults), while the 4 GB address range
    // stays reserved for the next GuestArena.
    void* reset = mmap(base, size_t(kArenaSize), PROT_NONE,
                       MAP_PRIVATE | MAP_ANON | MAP_FIXED | MAP_NORESERVE, -1, 0);
    bool pooled = false;
    if (reset != MAP_FAILED) {
        std::lock_guard<std::mutex> lk(poolMu);
        if (!pooledBase) {
            pooledBase = base;
            pooled = true;
        }
    }
    // Keep liveCount() meaning "live arenas": the pooled range is an address-space
    // reservation, not an owned guest address space.
    liveArenas.fetch_sub(1, std::memory_order_release);
    if (!pooled) munmap(base, size_t(kArenaSize));
}

void GuestArena::commitHostPages(uint64_t first, uint64_t last) {
    // Commit runs of uncommitted host pages; never touch committed ones, so data
    // already loaded into a shared host page survives.
    uint64_t i = first;
    while (i <= last) {
        if (host_committed_[i]) { ++i; continue; }
        uint64_t j = i;
        while (j + 1 <= last && !host_committed_[j + 1]) ++j;
        void* want = base_ + i * host_page_;
        size_t len = size_t((j - i + 1) * host_page_);
        void* r = mmap(want, len, PROT_READ | PROT_WRITE,
                       MAP_PRIVATE | MAP_ANON | MAP_FIXED, -1, 0);
        if (r == MAP_FAILED) throw std::runtime_error("HydrogenVM: failed to commit guest memory");
        for (uint64_t k = i; k <= j; ++k) host_committed_[k] = 1;
        i = j + 1;
    }
}

void GuestArena::releaseHostPage(uint64_t idx) {
    if (!host_committed_[idx]) return;
    mmap(base_ + idx * host_page_, host_page_, PROT_NONE,
         MAP_PRIVATE | MAP_ANON | MAP_FIXED | MAP_NORESERVE, -1, 0);
    host_committed_[idx] = 0;
}

void GuestArena::map(uint32_t addr, uint32_t size, uint8_t prot) {
    if (!size) return;
    std::lock_guard<std::mutex> lk(mu_);
    uint64_t start = addr & ~uint64_t(kGuestPageSize - 1);
    uint64_t end   = (uint64_t(addr) + size + kGuestPageSize - 1) & ~uint64_t(kGuestPageSize - 1);
    if (end > kArenaSize) throw std::runtime_error("HydrogenVM: map beyond 4 GB");
    commitHostPages(start / host_page_, (end - 1) / host_page_);
    for (uint64_t g = start >> kGuestPageShift; g < (end >> kGuestPageShift); ++g)
        prot_[g] = prot ? prot : kProtRead;   // committed pages are always at least visible
}

void GuestArena::protect(uint32_t addr, uint32_t size, uint8_t prot) {
    std::lock_guard<std::mutex> lk(mu_);
    uint64_t start = addr >> kGuestPageShift;
    uint64_t end   = (uint64_t(addr) + size + kGuestPageSize - 1) >> kGuestPageShift;
    for (uint64_t g = start; g < end; ++g)
        if (prot_[g] != kProtNone) prot_[g] = prot ? prot : kProtRead;
}

void GuestArena::unmap(uint32_t addr, uint32_t size) {
    if (!size) return;
    std::lock_guard<std::mutex> lk(mu_);
    uint64_t start = addr >> kGuestPageShift;
    uint64_t end   = (uint64_t(addr) + size + kGuestPageSize - 1) >> kGuestPageShift;
    for (uint64_t g = start; g < end; ++g) prot_[g] = kProtNone;
    const uint64_t per = host_page_ >> kGuestPageShift;
    for (uint64_t h = (start << kGuestPageShift) / host_page_;
         h <= ((end << kGuestPageShift) - 1) / host_page_; ++h) {
        bool used = false;
        for (uint64_t g = h * per; g < (h + 1) * per; ++g) used |= prot_[g] != kProtNone;
        if (!used) {
            releaseHostPage(h);
        } else {
            // Host page still shared with live guest pages: just zero ours.
            for (uint64_t g = std::max(start, h * per); g < std::min(end, (h + 1) * per); ++g)
                std::memset(base_ + (g << kGuestPageShift), 0, kGuestPageSize);
        }
    }
}

bool GuestArena::copyInWritable(uint32_t addr, const void* src, size_t bytes, uint32_t& fault) noexcept {
    fault = addr;
    if (bytes > kArenaSize || uint64_t(addr) + bytes > kArenaSize) return false;
    if (!bytes) return true;
    const uint64_t end = uint64_t(addr) + bytes;
    std::lock_guard<std::mutex> lk(mu_);
    for (uint64_t page = uint64_t(addr) >> kGuestPageShift; page <= (end - 1) >> kGuestPageShift; ++page) {
        if (!(prot_[page] & kProtWrite)) {
            fault = uint32_t(std::max<uint64_t>(addr, page << kGuestPageShift));
            return false;
        }
    }
    std::memcpy(base_ + uint64_t(addr), src, bytes);
    return true;
}

bool GuestArena::copyCStringWritable(uint32_t addr, const char* src, size_t chars) noexcept {
    const uint64_t end = uint64_t(addr) + chars + 1;
    if (end > kArenaSize) return false;
    std::lock_guard<std::mutex> lk(mu_);
    for (uint64_t page = uint64_t(addr) >> kGuestPageShift;
         page <= (end - 1) >> kGuestPageShift; ++page) {
        if (!(prot_[page] & kProtWrite)) return false;
    }
    std::memcpy(base_ + uint64_t(addr), src, chars);
    base_[uint64_t(addr) + chars] = 0;
    return true;
}

bool GuestArena::copyWideWritable(uint32_t addr, const char* chars, size_t count) noexcept {
    if (count > (UINT64_MAX / 4) - 1) return false;
    const uint64_t bytes = (uint64_t(count) + 1) * 4;
    const uint64_t end = uint64_t(addr) + bytes;
    if (end < uint64_t(addr) || end > kArenaSize) return false;
    std::lock_guard<std::mutex> lk(mu_);
    for (uint64_t page = uint64_t(addr) >> kGuestPageShift;
         page <= (end - 1) >> kGuestPageShift; ++page) {
        if (!(prot_[page] & kProtWrite)) return false;
    }
    for (size_t i = 0; i < count; ++i) {
        const uint32_t wc = uint8_t(chars[i]);
        std::memcpy(base_ + uint64_t(addr) + i * 4, &wc, 4);
    }
    const uint32_t zero = 0;
    std::memcpy(base_ + uint64_t(addr) + count * 4, &zero, 4);
    return true;
}

uint32_t GuestArena::findFree(uint32_t size, uint32_t hint) const {
    std::lock_guard<std::mutex> lk(mu_);
    // Allocate at host-page granularity so fresh allocations never share a host
    // page with something else (keeps unmap cheap and exact).
    const uint64_t step  = host_page_;
    const uint64_t need  = (uint64_t(size) + step - 1) & ~(step - 1);
    const uint64_t limit = kHleBase;
    auto freeRange = [&](uint64_t a) {
        for (uint64_t g = a >> kGuestPageShift; g < (a + need) >> kGuestPageShift; ++g)
            if (prot_[g] != kProtNone) return false;
        return true;
    };
    for (int pass = 0; pass < 2; ++pass) {
        uint64_t a = pass == 0 ? ((uint64_t(hint) + step - 1) & ~(step - 1)) : step;
        for (; a + need <= limit; a += step)
            if (freeRange(a)) return uint32_t(a);
    }
    return 0;
}

} // namespace Hydrogen::Memory
