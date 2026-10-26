#include "GuestArena.hpp"
#include <sys/mman.h>
#include <unistd.h>
#include <stdexcept>
#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <thread>

namespace Hydrogen::Memory {
namespace {
std::atomic<uint32_t> liveArenas{0};
}

uint32_t GuestArena::liveCount() noexcept { return liveArenas.load(std::memory_order_relaxed); }

GuestArena::GuestArena() {
    host_page_ = size_t(sysconf(_SC_PAGESIZE));
    if (host_page_ < kGuestPageSize || (host_page_ & (host_page_ - 1)))
        throw std::runtime_error("HydrogenVM: unsupported host page size");

    // NOTE (iOS): reserving 4 GB of address space needs the
    // com.apple.developer.kernel.extended-virtual-addressing entitlement on most
    // devices; without it this mmap fails. See docs/ARCHITECTURE.md.
    // The previous game's arena may still be on its way out (its threads are
    // finishing); iOS has room for about one 4 GB reservation, so wait a few
    // seconds for it rather than failing the next launch (Test All Games).
    void* p = MAP_FAILED;
    int lastError = 0;
    for (int attempt = 0; attempt < 20 && p == MAP_FAILED; ++attempt) {
        if (attempt) std::this_thread::sleep_for(std::chrono::milliseconds(250));
        errno = 0;
        p = mmap(nullptr, size_t(kArenaSize), PROT_NONE, MAP_PRIVATE | MAP_ANON | MAP_NORESERVE, -1, 0);
        if (p == MAP_FAILED) lastError = errno;
    }
    if (p == MAP_FAILED)
        throw std::runtime_error("HydrogenVM: failed to reserve 4 GB guest arena: " +
                                 std::string(std::strerror(lastError)) + " (errno " + std::to_string(lastError) +
                                 ", live arenas " + std::to_string(liveCount()) +
                                 "; on iOS, check the extended-virtual-addressing entitlement)");
    base_ = static_cast<uint8_t*>(p);
    prot_.assign(kGuestPageCount, kProtNone);
    host_committed_.assign(size_t(kArenaSize / host_page_), 0);
    liveArenas.fetch_add(1, std::memory_order_relaxed);
}

GuestArena::~GuestArena() {
    if (base_) {
        munmap(base_, size_t(kArenaSize));
        liveArenas.fetch_sub(1, std::memory_order_relaxed);
    }
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
