// HydrogenVM — guest address space.
//
// The whole 32-bit guest address space is one contiguous host reservation, so a
// guest address converts to a host pointer with a single add (base + zext(addr)).
//
// Changes from the original handoff version:
//  * Commits are tracked per *host* page. iOS hosts use 16 KB pages while 32-bit
//    iOS binaries are laid out on 4 KB pages, so two guest segments routinely share
//    one host page. The old MapRegion re-mmap'd with MAP_FIXED on every call, which
//    silently zeroed data an earlier segment had already loaded into that page.
//  * Guest page permissions are tracked in software (4 KB granularity) instead of
//    being pushed into host protections, which can't express them at 16 KB.
//    Host pages are simply PROT_NONE (unmapped) or PROT_READ|PROT_WRITE.
//  * Unaligned / type-punned access goes through memcpy (no UB).
//  * Faults on PROT_NONE pages are turned into guest faults by FaultGuard.
#pragma once
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>
#include <mutex>

namespace Hydrogen::Memory {

enum GuestProt : uint8_t {
    kProtNone  = 0,
    kProtRead  = 1,
    kProtWrite = 2,
    kProtExec  = 4,
    kProtRW    = kProtRead | kProtWrite,
    kProtRX    = kProtRead | kProtExec,
    kProtRWX   = kProtRead | kProtWrite | kProtExec,
};

class GuestArena {
public:
    static constexpr uint64_t kArenaSize      = 0x100000000ULL;   // 4 GB
    static constexpr uint32_t kGuestPageShift = 12;               // guest pages are 4 KB
    static constexpr uint32_t kGuestPageSize  = 1u << kGuestPageShift;
    static constexpr uint32_t kGuestPageCount = uint32_t(kArenaSize >> kGuestPageShift);

    // Region reserved for host-implemented (HLE) functions. Guest code reaching an
    // address in here is dispatched to C++ by address; nothing is ever decoded there.
    static constexpr uint32_t kHleBase = 0xFFFF0000u;
    static constexpr uint32_t kHleSize = 0x00010000u;

    GuestArena();
    ~GuestArena();
    GuestArena(const GuestArena&) = delete;
    GuestArena& operator=(const GuestArena&) = delete;

    uint8_t* base() const noexcept { return base_; }
    size_t   hostPageSize() const noexcept { return host_page_; }

    // Commit [addr, addr+size) with the given guest permissions. Safe to call on
    // ranges that overlap already-committed memory; existing contents are kept.
    void map(uint32_t addr, uint32_t size, uint8_t prot = kProtRW);
    // Decommit (contents discarded). Host pages still used by other guest pages stay.
    void unmap(uint32_t addr, uint32_t size);
    void protect(uint32_t addr, uint32_t size, uint8_t prot);

    bool    isMapped(uint32_t addr) const noexcept { return prot_[addr >> kGuestPageShift] != kProtNone; }
    uint8_t protAt(uint32_t addr) const noexcept   { return prot_[addr >> kGuestPageShift]; }

    // Find a free, page-aligned range of `size` bytes at or above `hint`.
    // Returns 0 on failure (address 0 is never handed out).
    uint32_t findFree(uint32_t size, uint32_t hint = 0x10000000u) const;

    template <typename T>
    __attribute__((always_inline)) T read(uint32_t a) const noexcept {
        T v; std::memcpy(&v, base_ + uint64_t(a), sizeof(T)); return v;
    }
    template <typename T>
    __attribute__((always_inline)) void write(uint32_t a, T v) noexcept {
        std::memcpy(base_ + uint64_t(a), &v, sizeof(T));
    }
    __attribute__((always_inline)) uint8_t* host(uint32_t a) const noexcept { return base_ + uint64_t(a); }
    // Null-preserving conversion, for passing guest pointers to host APIs.
    __attribute__((always_inline)) void* hostOrNull(uint32_t a) const noexcept { return a ? host(a) : nullptr; }
    // Host pointer -> guest address. Caller must ensure p lies in the arena.
    uint32_t guestAddr(const void* p) const noexcept {
        return uint32_t(static_cast<const uint8_t*>(p) - base_);
    }
    bool containsHost(const void* p) const noexcept {
        auto q = static_cast<const uint8_t*>(p);
        return q >= base_ && q < base_ + kArenaSize;
    }

    void copyIn(uint32_t dst, const void* src, size_t n)  { std::memcpy(host(dst), src, n); }
    void copyOut(void* dst, uint32_t src, size_t n) const { std::memcpy(dst, host(src), n); }

private:
    void commitHostPages(uint64_t first, uint64_t last);   // inclusive host-page indices
    void releaseHostPage(uint64_t idx);

    uint8_t*             base_ = nullptr;
    size_t               host_page_ = 4096;
    std::vector<uint8_t> prot_;           // per guest page
    std::vector<uint8_t> host_committed_; // per host page
    mutable std::mutex   mu_;
};

} // namespace Hydrogen::Memory
