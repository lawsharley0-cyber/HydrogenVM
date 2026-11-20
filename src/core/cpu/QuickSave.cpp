#include "QuickSave.hpp"
#include <cstdio>
#include <cstring>
#include <fstream>

namespace Hydrogen::CPU {

namespace {

constexpr uint32_t kSnapMagic = 0x53515648u; // "HVQS"
constexpr uint32_t kSnapVersion = 1u;
constexpr uint32_t kMaxSavedPages = 262144u; // 1 GB cap
constexpr uint32_t kMaxSavedThreads = 128u;

uint64_t fnv1a64Update(uint64_t h, const void* data, size_t len) {
    const auto* p = static_cast<const uint8_t*>(data);
    for (size_t i = 0; i < len; ++i) {
        h ^= uint64_t(p[i]);
        h *= 1099511628211ULL;
    }
    return h;
}

struct SnapHeader {
    uint32_t magic;
    uint32_t version;
    uint32_t pageCount;
    uint32_t threadCount;
    uint64_t payloadHash;
};
static_assert(sizeof(SnapHeader) == 24, "QuickSave SnapHeader size");

struct PageRecordHeader {
    uint32_t addr;
    uint32_t prot;
};
static_assert(sizeof(PageRecordHeader) == 8, "QuickSave PageRecordHeader size");

QuickSaveThreadState captureThread(const Cpu& c) {
    QuickSaveThreadState st{};
    std::memcpy(st.r, c.r, sizeof(st.r));
    st.nzcv = c.nzcv;
    st.q = c.q;
    st.ge = c.ge;
    st.thumb = c.thumb;
    std::memcpy(st.d, c.d, sizeof(st.d));
    st.fpscr = c.fpscr;
    st.tpidrurw = c.tpidrurw;
    st.tpidruro = c.tpidruro;
    return st;
}

} // namespace

void applyQuickSaveThread(Cpu& dst, const QuickSaveThreadState& src) {
    std::memcpy(dst.r, src.r, sizeof(src.r));
    dst.nzcv = src.nzcv;
    dst.q = src.q;
    dst.ge = src.ge;
    dst.thumb = src.thumb;
    std::memcpy(dst.d, src.d, sizeof(src.d));
    dst.fpscr = src.fpscr;
    dst.tpidrurw = src.tpidrurw;
    dst.tpidruro = src.tpidruro;
}

bool saveQuickSnapshot(const std::string& path,
                       const Memory::GuestArena& mem,
                       const std::vector<const Cpu*>& threads,
                       QuickSaveStats* statsOut) {
    if (path.empty() || threads.size() > kMaxSavedThreads) return false;

    constexpr uint32_t kPage = Memory::GuestArena::kGuestPageSize;
    constexpr uint32_t kNumPages = Memory::GuestArena::kHleBase >> Memory::GuestArena::kGuestPageShift;

    std::vector<PageRecordHeader> pages;
    for (uint32_t idx = 1; idx < kNumPages; ++idx) {
        const uint32_t addr = idx << Memory::GuestArena::kGuestPageShift;
        if (!mem.isMapped(addr)) continue;
        const uint8_t prot = mem.protAt(addr);
        // Only snapshot readable/accessible pages (skip PROT_NONE guard pages).
        if ((prot & Memory::kProtRead) == 0) continue;
        if (pages.size() == kMaxSavedPages) return false;
        pages.push_back(PageRecordHeader{addr, uint32_t(prot)});
    }

    std::vector<QuickSaveThreadState> tstates;
    tstates.reserve(threads.size());
    for (const Cpu* c : threads) {
        if (!c) return false;
        tstates.push_back(captureThread(*c));
    }

    uint64_t hash = 14695981039346656037ULL;
    for (const auto& st : tstates) {
        hash = fnv1a64Update(hash, &st, sizeof(st));
    }
    for (const auto& ph : pages) {
        hash = fnv1a64Update(hash, &ph, sizeof(ph));
        hash = fnv1a64Update(hash, mem.base() + ph.addr, kPage);
    }

    const std::string tmp = path + ".tmp";
    std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
    if (!out) return false;

    SnapHeader hdr{kSnapMagic, kSnapVersion, uint32_t(pages.size()), uint32_t(tstates.size()), hash};
    out.write(reinterpret_cast<const char*>(&hdr), sizeof(hdr));
    if (!tstates.empty()) {
        out.write(reinterpret_cast<const char*>(tstates.data()),
                  std::streamsize(tstates.size() * sizeof(QuickSaveThreadState)));
    }
    for (const auto& ph : pages) {
        out.write(reinterpret_cast<const char*>(&ph), sizeof(ph));
        out.write(reinterpret_cast<const char*>(mem.base() + ph.addr), kPage);
    }
    out.flush();
    if (!out.good()) {
        out.close();
        std::remove(tmp.c_str());
        return false;
    }
    out.close();
    if (std::rename(tmp.c_str(), path.c_str()) != 0) {
        std::remove(tmp.c_str());
        return false;
    }
    if (statsOut) {
        statsOut->pagesSaved = pages.size();
        statsOut->threadsSaved = tstates.size();
        statsOut->bytesWritten = sizeof(SnapHeader) +
                                 tstates.size() * sizeof(QuickSaveThreadState) +
                                 pages.size() * (sizeof(PageRecordHeader) + kPage);
    }
    return true;
}

bool restoreQuickSnapshot(const std::string& path,
                          Memory::GuestArena& mem,
                          std::vector<QuickSaveThreadState>& threadsOut) {
    threadsOut.clear();
    if (path.empty()) return false;
    std::ifstream in(path, std::ios::binary);
    if (!in) return false;

    SnapHeader hdr{};
    if (!in.read(reinterpret_cast<char*>(&hdr), sizeof(hdr))) return false;
    if (hdr.magic != kSnapMagic || hdr.version != kSnapVersion ||
        hdr.pageCount > kMaxSavedPages || hdr.threadCount > kMaxSavedThreads) {
        return false;
    }

    std::vector<QuickSaveThreadState> tstates(hdr.threadCount);
    if (hdr.threadCount > 0) {
        if (!in.read(reinterpret_cast<char*>(tstates.data()),
                     std::streamsize(hdr.threadCount * sizeof(QuickSaveThreadState)))) {
            return false;
        }
    }

    constexpr uint32_t kPage = Memory::GuestArena::kGuestPageSize;
    struct LoadedPage {
        PageRecordHeader hdr;
        std::vector<uint8_t> data;
    };
    std::vector<LoadedPage> pages;
    pages.reserve(hdr.pageCount);

    uint64_t hash = 14695981039346656037ULL;
    for (const auto& st : tstates) {
        hash = fnv1a64Update(hash, &st, sizeof(st));
    }
    for (uint32_t i = 0; i < hdr.pageCount; ++i) {
        LoadedPage lp;
        if (!in.read(reinterpret_cast<char*>(&lp.hdr), sizeof(lp.hdr))) return false;
        if ((lp.hdr.addr & (kPage - 1u)) != 0 || lp.hdr.addr >= Memory::GuestArena::kHleBase) return false;
        lp.data.resize(kPage);
        if (!in.read(reinterpret_cast<char*>(lp.data.data()), kPage)) return false;
        hash = fnv1a64Update(hash, &lp.hdr, sizeof(lp.hdr));
        hash = fnv1a64Update(hash, lp.data.data(), kPage);
        pages.push_back(std::move(lp));
    }
    if (hash != hdr.payloadHash) return false;

    for (const auto& lp : pages) {
        mem.map(lp.hdr.addr, kPage, Memory::kProtRW);
        std::memcpy(mem.base() + lp.hdr.addr, lp.data.data(), kPage);
        if (lp.hdr.prot != Memory::kProtRW) {
            mem.protect(lp.hdr.addr, kPage, uint8_t(lp.hdr.prot));
        }
    }
    threadsOut = std::move(tstates);
    return true;
}

} // namespace Hydrogen::CPU
