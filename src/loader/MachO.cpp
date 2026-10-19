#include "MachO.hpp"
#include <algorithm>
#include <cstring>

namespace Hydrogen::Loader {
namespace {

// ------------------------------------------------------------ constants ---

constexpr uint32_t MH_MAGIC = 0xFEEDFACEu, MH_CIGAM = 0xCEFAEDFEu;
constexpr uint32_t MH_MAGIC_64 = 0xFEEDFACFu;
constexpr uint32_t FAT_MAGIC = 0xCAFEBABEu, FAT_CIGAM = 0xBEBAFECAu;
constexpr uint32_t CPU_TYPE_ARM = 12;
constexpr uint32_t MH_EXECUTE = 2, MH_DYLIB = 6, MH_BUNDLE = 8;
constexpr uint32_t MH_PIE = 0x200000, MH_SPLIT_SEGS = 0x20;

enum : uint32_t {
    LC_SEGMENT = 0x1, LC_SYMTAB = 0x2, LC_THREAD = 0x4, LC_UNIXTHREAD = 0x5, LC_DYSYMTAB = 0xB,
    LC_LOAD_DYLIB = 0xC, LC_ID_DYLIB = 0xD, LC_LAZY_LOAD_DYLIB = 0x20, LC_ENCRYPTION_INFO = 0x21,
    LC_DYLD_INFO = 0x22, LC_REQ_DYLD = 0x80000000u,
    LC_LOAD_WEAK_DYLIB = 0x18 | LC_REQ_DYLD, LC_REEXPORT_DYLIB = 0x1F | LC_REQ_DYLD,
    LC_DYLD_INFO_ONLY = 0x22 | LC_REQ_DYLD, LC_LOAD_UPWARD_DYLIB = 0x23 | LC_REQ_DYLD,
    LC_MAIN = 0x28 | LC_REQ_DYLD, LC_DYLD_CHAINED_FIXUPS = 0x34 | LC_REQ_DYLD,
};

enum : uint32_t {
    S_NON_LAZY_SYMBOL_POINTERS = 0x6, S_LAZY_SYMBOL_POINTERS = 0x7, S_SYMBOL_STUBS = 0x8,
    S_MOD_INIT_FUNC_POINTERS = 0x9, S_LAZY_DYLIB_SYMBOL_POINTERS = 0x10,
    S_ZEROFILL = 0x1, S_GB_ZEROFILL = 0xC,
};
constexpr uint32_t INDIRECT_SYMBOL_LOCAL = 0x80000000u, INDIRECT_SYMBOL_ABS = 0x40000000u;

constexpr uint8_t N_STAB = 0xE0, N_TYPE = 0x0E, N_EXT = 0x01, N_SECT = 0xE, N_ABS = 0x2, N_UNDF = 0x0;
constexpr uint16_t N_ARM_THUMB_DEF = 0x0008, N_WEAK_REF = 0x0040;

// ---------------------------------------------------------------- reader ---

struct Reader {
    const uint8_t* p;
    size_t n;
    template <typename T> T at(size_t off) const {
        if (off > n || n - off < sizeof(T)) throw MachOError("Mach-O: truncated (offset " + std::to_string(off) + ")");
        T v; std::memcpy(&v, p + off, sizeof(T)); return v;
    }
    std::string cstr(size_t off, size_t max) const {
        if (off >= n) throw MachOError("Mach-O: string out of range");
        size_t len = 0;
        const size_t lim = std::min(max, n - off);
        while (len < lim && p[off + len]) ++len;
        return std::string(reinterpret_cast<const char*>(p + off), len);
    }
    std::string fixed(size_t off, size_t len) const {
        if (off + len > n) throw MachOError("Mach-O: truncated name");
        size_t l = 0;
        while (l < len && p[off + l]) ++l;
        return std::string(reinterpret_cast<const char*>(p + off), l);
    }
};

inline uint32_t be32(uint32_t v) { return __builtin_bswap32(v); }

struct MachHeader { uint32_t magic, cputype, cpusubtype, filetype, ncmds, sizeofcmds, flags; };
struct SegmentCmd { uint32_t cmd, cmdsize; char segname[16]; uint32_t vmaddr, vmsize, fileoff, filesize, maxprot, initprot, nsects, flags; };
struct SectionRaw { char sectname[16]; char segname[16]; uint32_t addr, size, offset, align, reloff, nreloc, flags, reserved1, reserved2; };
struct SymtabCmd { uint32_t cmd, cmdsize, symoff, nsyms, stroff, strsize; };
struct DysymtabCmd {
    uint32_t cmd, cmdsize, ilocalsym, nlocalsym, iextdefsym, nextdefsym, iundefsym, nundefsym,
             tocoff, ntoc, modtaboff, nmodtab, extrefsymoff, nextrefsyms, indirectsymoff, nindirectsyms,
             extreloff, nextrel, locreloff, nlocrel;
};
struct DyldInfoCmd {
    uint32_t cmd, cmdsize, rebase_off, rebase_size, bind_off, bind_size, weak_bind_off, weak_bind_size,
             lazy_bind_off, lazy_bind_size, export_off, export_size;
};
struct Nlist { uint32_t n_strx; uint8_t n_type, n_sect; uint16_t n_desc; uint32_t n_value; };
struct RelocInfo { int32_t r_address; uint32_t packed; };

static_assert(sizeof(SegmentCmd) == 56 && sizeof(SectionRaw) == 68 && sizeof(Nlist) == 12, "struct layout");

// ULEB/SLEB from the opcode streams.
struct Stream {
    const uint8_t* p; const uint8_t* end;
    bool done() const { return p >= end; }
    uint8_t byte() { if (p >= end) throw MachOError("Mach-O: opcode stream overrun"); return *p++; }
    uint64_t uleb() {
        uint64_t v = 0; int shift = 0;
        for (;;) {
            const uint8_t b = byte();
            if (shift < 64) v |= uint64_t(b & 0x7F) << shift;
            shift += 7;
            if (!(b & 0x80)) return v;
        }
    }
    int64_t sleb() {
        int64_t v = 0; int shift = 0; uint8_t b;
        do { b = byte(); if (shift < 64) v |= int64_t(b & 0x7F) << shift; shift += 7; } while (b & 0x80);
        if (shift < 64 && (b & 0x40)) v |= -(int64_t(1) << shift);
        return v;
    }
    std::string cstr() {
        const uint8_t* s = p;
        while (p < end && *p) ++p;
        std::string r(reinterpret_cast<const char*>(s), size_t(p - s));
        if (p < end) ++p;
        return r;
    }
};

uint32_t subtypeRank(uint32_t sub) {
    switch (sub) {
        case kCpuArmV7S: return 5;
        case kCpuArmV7:  return 4;
        case kCpuArmV7F: return 3;
        case kCpuArmV6:  return 2;
        default:         return 1;
    }
}

} // namespace

// ----------------------------------------------------------------- slice ---

Slice selectSlice(const std::vector<uint8_t>& file) {
    Reader rd{file.data(), file.size()};
    const uint32_t magic = rd.at<uint32_t>(0);
    if (magic == FAT_CIGAM || magic == FAT_MAGIC) {
        const bool swap = magic == FAT_CIGAM;
        auto fix = [&](uint32_t v) { return swap ? be32(v) : v; };
        const uint32_t n = fix(rd.at<uint32_t>(4));
        if (n > 64) throw MachOError("Mach-O: implausible fat header");
        Slice best{0, 0, 0};
        uint32_t bestRank = 0;
        bool sawArm64 = false;
        for (uint32_t i = 0; i < n; ++i) {
            const size_t o = 8 + size_t(i) * 20;
            const uint32_t cpu = fix(rd.at<uint32_t>(o)), sub = fix(rd.at<uint32_t>(o + 4)) & 0x00FFFFFFu;
            const uint32_t off = fix(rd.at<uint32_t>(o + 8)), size = fix(rd.at<uint32_t>(o + 12));
            if (cpu == (CPU_TYPE_ARM | 0x01000000u)) sawArm64 = true;
            if (cpu != CPU_TYPE_ARM) continue;
            if (uint64_t(off) + size > file.size()) throw MachOError("Mach-O: fat slice out of range");
            const uint32_t rank = subtypeRank(sub);
            if (rank > bestRank) { bestRank = rank; best = {off, size, sub}; }
        }
        if (!bestRank)
            throw MachOError(sawArm64 ? "This app is 64-bit only. HydrogenVM runs 32-bit (armv6/armv7) apps; 64-bit apps can run natively."
                                      : "Mach-O: no 32-bit ARM slice in this binary");
        return best;
    }
    if (magic == MH_MAGIC) {
        const uint32_t cpu = rd.at<uint32_t>(4);
        if (cpu != CPU_TYPE_ARM) throw MachOError("Mach-O: not a 32-bit ARM binary");
        return {0, uint32_t(file.size()), rd.at<uint32_t>(8) & 0x00FFFFFFu};
    }
    if (magic == MH_MAGIC_64) throw MachOError("This app is 64-bit only. HydrogenVM runs 32-bit (armv6/armv7) apps.");
    if (magic == MH_CIGAM) throw MachOError("Mach-O: big-endian binaries aren't supported");
    throw MachOError("not a Mach-O file");
}

// ----------------------------------------------------------------- image ---

const Section* Image::findSection(const char* seg, const char* sect) const {
    for (const auto& s : sections)
        if (s.segname == seg && s.sectname == sect) return &s;
    return nullptr;
}

std::vector<const Section*> Image::sectionsOfType(uint32_t type) const {
    std::vector<const Section*> out;
    for (const auto& s : sections) if (s.type() == type) out.push_back(&s);
    return out;
}

std::string Image::symbolicate(uint32_t addr) const {
    if (addr < lowAddr || addr >= highAddr || symbolsByAddr.empty()) return {};
    auto it = symbolsByAddr.upper_bound(addr | 1);
    if (it == symbolsByAddr.begin()) return {};
    --it;
    const uint32_t base = it->first & ~1u;
    if (addr < base) return {};
    char buf[32];
    std::snprintf(buf, sizeof buf, " + 0x%x", addr - base);
    return it->second + (addr == base ? "" : buf);
}

namespace {

struct Ctx {
    Reader rd;
    Image& img;
    Memory::GuestArena& mem;
    size_t base;                   // offset of the slice in the file
    uint32_t slide;
    const SymtabCmd* symtab = nullptr;
    const DysymtabCmd* dysymtab = nullptr;
    SymtabCmd symtabStorage{};
    DysymtabCmd dysymtabStorage{};

    Nlist sym(uint32_t i) const {
        if (!symtab || i >= symtab->nsyms) throw MachOError("Mach-O: symbol index out of range");
        return rd.at<Nlist>(base + symtab->symoff + size_t(i) * sizeof(Nlist));
    }
    std::string symName(const Nlist& s) const {
        if (s.n_strx >= symtab->strsize) return {};
        return rd.cstr(base + symtab->stroff + s.n_strx, symtab->strsize - s.n_strx);
    }
    uint32_t segAddr(uint32_t segIndex, uint64_t off) const {
        if (segIndex >= img.segments.size()) throw MachOError("Mach-O: bad segment index in opcodes");
        const Segment& s = img.segments[segIndex];
        if (off >= s.vmsize) throw MachOError("Mach-O: opcode address outside segment " + s.name);
        return s.vmaddr + uint32_t(off);
    }
};

void doRebase(Ctx& cx, uint32_t addr, uint8_t type) {
    if (!cx.slide) return;
    switch (type) {
        case 1: case 2: cx.mem.write<uint32_t>(addr, cx.mem.read<uint32_t>(addr) + cx.slide); break;
        case 3: cx.mem.write<uint32_t>(addr, cx.mem.read<uint32_t>(addr) - cx.slide); break;
        default: throw MachOError("Mach-O: unknown rebase type");
    }
}

void runRebase(Ctx& cx, const uint8_t* p, size_t n) {
    Stream s{p, p + n};
    uint8_t type = 1;
    uint32_t seg = 0;
    uint64_t off = 0;
    while (!s.done()) {
        const uint8_t b = s.byte(), opc = b & 0xF0, imm = b & 0x0F;
        switch (opc) {
            case 0x00: return;                                           // DONE
            case 0x10: type = imm; break;                                // SET_TYPE_IMM
            case 0x20: seg = imm; off = s.uleb(); break;                 // SET_SEGMENT_AND_OFFSET_ULEB
            case 0x30: off += s.uleb(); break;                           // ADD_ADDR_ULEB
            case 0x40: off += uint64_t(imm) * 4; break;                  // ADD_ADDR_IMM_SCALED
            case 0x50: for (int i = 0; i < imm; ++i) { doRebase(cx, cx.segAddr(seg, off), type); off += 4; } break;
            case 0x60: { const uint64_t c = s.uleb(); for (uint64_t i = 0; i < c; ++i) { doRebase(cx, cx.segAddr(seg, off), type); off += 4; } break; }
            case 0x70: doRebase(cx, cx.segAddr(seg, off), type); off += 4 + s.uleb(); break;
            case 0x80: {
                const uint64_t c = s.uleb(), skip = s.uleb();
                for (uint64_t i = 0; i < c; ++i) { doRebase(cx, cx.segAddr(seg, off), type); off += 4 + skip; }
                break;
            }
            default: throw MachOError("Mach-O: unknown rebase opcode");
        }
    }
}

void runBind(Ctx& cx, const uint8_t* p, size_t n, bool lazy, bool weak) {
    Stream s{p, p + n};
    BindRecord cur;
    cur.lazy = lazy;
    uint32_t seg = 0;
    uint64_t off = 0;
    auto emit = [&] {
        if (weak) { ++cx.img.weakBindCount; return; }
        BindRecord r = cur;
        r.address = cx.segAddr(seg, off);
        cx.img.binds.push_back(std::move(r));
    };
    while (!s.done()) {
        const uint8_t b = s.byte(), opc = b & 0xF0, imm = b & 0x0F;
        switch (opc) {
            case 0x00: if (!lazy) return; break;                         // DONE (lazy stream: entry separator)
            case 0x10: cur.dylibOrdinal = imm; break;
            case 0x20: cur.dylibOrdinal = int32_t(s.uleb()); break;
            case 0x30: cur.dylibOrdinal = imm ? int32_t(int8_t(0xF0 | imm)) : 0; break;
            case 0x40: cur.weakImport = imm & 1; cur.symbol = s.cstr(); break;
            case 0x50: cur.kind = BindKind(imm); break;
            case 0x60: cur.addend = int32_t(s.sleb()); break;
            case 0x70: seg = imm; off = s.uleb(); break;
            case 0x80: off += s.uleb(); break;
            case 0x90: emit(); off += 4; break;
            case 0xA0: emit(); off += 4 + s.uleb(); break;
            case 0xB0: emit(); off += 4 + uint64_t(imm) * 4; break;
            case 0xC0: {
                const uint64_t c = s.uleb(), skip = s.uleb();
                for (uint64_t i = 0; i < c; ++i) { emit(); off += 4 + skip; }
                break;
            }
            default: throw MachOError("Mach-O: unknown bind opcode " + std::to_string(opc >> 4));
        }
    }
}

// Pre-dyld-info binaries: bind symbol pointer sections through the indirect
// symbol table, and data references through external relocations.
void legacyBinds(Ctx& cx) {
    if (!cx.dysymtab || !cx.symtab) return;
    const DysymtabCmd& d = *cx.dysymtab;
    auto indirect = [&](uint32_t i) {
        if (i >= d.nindirectsyms) throw MachOError("Mach-O: indirect symbol index out of range");
        return cx.rd.at<uint32_t>(cx.base + d.indirectsymoff + size_t(i) * 4);
    };
    for (const Section& sec : cx.img.sections) {
        const uint32_t t = sec.type();
        if (t != S_NON_LAZY_SYMBOL_POINTERS && t != S_LAZY_SYMBOL_POINTERS && t != S_LAZY_DYLIB_SYMBOL_POINTERS) continue;
        for (uint32_t i = 0; i < sec.size / 4; ++i) {
            const uint32_t si = indirect(sec.reserved1 + i);
            const uint32_t addr = sec.addr + i * 4;
            if (si & (INDIRECT_SYMBOL_LOCAL | INDIRECT_SYMBOL_ABS)) {
                if ((si & INDIRECT_SYMBOL_LOCAL) && !(si & INDIRECT_SYMBOL_ABS)) doRebase(cx, addr, 1);
                continue;
            }
            const Nlist s = cx.sym(si);
            BindRecord r;
            r.address = addr;
            r.symbol = cx.symName(s);
            r.dylibOrdinal = int32_t((s.n_desc >> 8) & 0xFF);
            r.weakImport = s.n_desc & N_WEAK_REF;
            r.lazy = t != S_NON_LAZY_SYMBOL_POINTERS;
            if ((s.n_type & N_TYPE) != N_UNDF) {
                // Pointer to a symbol defined in this image (e.g. a private extern).
                r.addend = 0;
                r.dylibOrdinal = 0;
            }
            cx.img.binds.push_back(std::move(r));
        }
    }
    // External relocations: r_address is relative to the first segment's
    // address (as dyld does, that includes __PAGEZERO, so it's normally 0 for
    // executables), or to the first writable segment with MH_SPLIT_SEGS.
    uint32_t relocBase = 0;
    for (const Segment& s : cx.img.segments) {
        if ((cx.img.flags & MH_SPLIT_SEGS) && !(s.initprot & 2)) continue;
        relocBase = s.vmaddr;
        break;
    }
    for (uint32_t i = 0; i < d.nextrel; ++i) {
        const RelocInfo ri = cx.rd.at<RelocInfo>(cx.base + d.extreloff + size_t(i) * 8);
        if (ri.r_address < 0) continue;                          // scattered: never external
        const uint32_t symnum = ri.packed & 0x00FFFFFFu;
        const bool ext = (ri.packed >> 27) & 1;
        const uint32_t len = (ri.packed >> 25) & 3;
        if (!ext || len != 2) continue;
        const Nlist s = cx.sym(symnum);
        BindRecord r;
        r.address = relocBase + uint32_t(ri.r_address);
        r.symbol = cx.symName(s);
        r.dylibOrdinal = int32_t((s.n_desc >> 8) & 0xFF);
        r.weakImport = s.n_desc & N_WEAK_REF;
        r.kind = BindKind::ExternalReloc;                        // value += symbol address
        cx.img.binds.push_back(std::move(r));
    }
    // Local relocations only matter when the image is slid.
    if (cx.slide) {
        for (uint32_t i = 0; i < d.nlocrel; ++i) {
            const RelocInfo ri = cx.rd.at<RelocInfo>(cx.base + d.locreloff + size_t(i) * 8);
            uint32_t addr;
            if (ri.r_address < 0) addr = relocBase + (uint32_t(ri.r_address) & 0x00FFFFFFu);   // scattered
            else addr = relocBase + uint32_t(ri.r_address);
            doRebase(cx, addr, 1);
        }
    }
}

} // namespace

ProbeResult probeMachO(const std::vector<uint8_t>& file) {
    ProbeResult pr;
    try {
        const Slice sl = selectSlice(file);
        pr.isMachO = true;
        pr.hasArm32 = true;
        pr.cpuSubtype = sl.cpuSubtype;
        Reader rd{file.data() + sl.offset, sl.size};
        const auto h = rd.at<MachHeader>(0);
        size_t off = sizeof(MachHeader);
        for (uint32_t i = 0; i < h.ncmds; ++i) {
            const uint32_t cmd = rd.at<uint32_t>(off), size = rd.at<uint32_t>(off + 4);
            if (size < 8) break;
            if (cmd == LC_ENCRYPTION_INFO && rd.at<uint32_t>(off + 16) != 0) pr.encrypted = true;
            off += size;
        }
    } catch (const std::exception& e) {
        pr.error = e.what();
    }
    return pr;
}

Image loadMachO(Memory::GuestArena& mem, const std::vector<uint8_t>& file, const std::string& path, const LoadOptions& opts) {
    const Slice sl = selectSlice(file);
    Image img;
    img.path = path;
    img.cpuSubtype = sl.cpuSubtype;
    Ctx cx{Reader{file.data(), file.size()}, img, mem, sl.offset, 0};
    const Reader slice{file.data() + sl.offset, sl.size};

    const auto h = slice.at<MachHeader>(0);
    if (h.magic != MH_MAGIC) throw MachOError("Mach-O: bad magic");
    if (h.filetype != MH_EXECUTE && h.filetype != MH_DYLIB && h.filetype != MH_BUNDLE)
        throw MachOError("Mach-O: unsupported file type " + std::to_string(h.filetype));
    img.fileType = h.filetype;
    img.flags = h.flags;
    if (uint64_t(sizeof(MachHeader)) + h.sizeofcmds > sl.size) throw MachOError("Mach-O: load commands truncated");

    // ---- pass 1: load commands
    const DyldInfoCmd* dyldInfo = nullptr;
    DyldInfoCmd dyldInfoStorage{};
    uint32_t unixThreadPc = 0, mainEntryOff = 0;
    bool haveMain = false, haveThread = false;
    size_t off = sizeof(MachHeader);
    for (uint32_t i = 0; i < h.ncmds; ++i) {
        const uint32_t cmd = slice.at<uint32_t>(off), size = slice.at<uint32_t>(off + 4);
        if (size < 8 || off + size > sizeof(MachHeader) + h.sizeofcmds) throw MachOError("Mach-O: bad load command size");
        switch (cmd) {
            case LC_SEGMENT: {
                const auto sc = slice.at<SegmentCmd>(off);
                Segment seg;
                seg.name = slice.fixed(off + 8, 16);
                seg.vmaddr = sc.vmaddr; seg.vmsize = sc.vmsize; seg.fileoff = sc.fileoff; seg.filesize = sc.filesize;
                seg.maxprot = sc.maxprot; seg.initprot = sc.initprot;
                if (uint64_t(seg.fileoff) + seg.filesize > sl.size) throw MachOError("Mach-O: segment " + seg.name + " extends past end of file");
                img.segments.push_back(seg);
                for (uint32_t k = 0; k < sc.nsects; ++k) {
                    const size_t so = off + sizeof(SegmentCmd) + size_t(k) * sizeof(SectionRaw);
                    const auto sr = slice.at<SectionRaw>(so);
                    Section s;
                    s.sectname = slice.fixed(so, 16);
                    s.segname = slice.fixed(so + 16, 16);
                    s.addr = sr.addr; s.size = sr.size; s.offset = sr.offset; s.flags = sr.flags;
                    s.reserved1 = sr.reserved1; s.reserved2 = sr.reserved2;
                    img.sections.push_back(s);
                }
                break;
            }
            case LC_SYMTAB: cx.symtabStorage = slice.at<SymtabCmd>(off); cx.symtab = &cx.symtabStorage; break;
            case LC_DYSYMTAB: cx.dysymtabStorage = slice.at<DysymtabCmd>(off); cx.dysymtab = &cx.dysymtabStorage; break;
            case LC_DYLD_INFO: case LC_DYLD_INFO_ONLY:
                dyldInfoStorage = slice.at<DyldInfoCmd>(off); dyldInfo = &dyldInfoStorage; break;
            case LC_LOAD_DYLIB: case LC_LOAD_WEAK_DYLIB: case LC_REEXPORT_DYLIB:
            case LC_LAZY_LOAD_DYLIB: case LC_LOAD_UPWARD_DYLIB:
                img.dylibs.push_back(slice.cstr(off + slice.at<uint32_t>(off + 8), size));
                break;
            case LC_ID_DYLIB: img.installName = slice.cstr(off + slice.at<uint32_t>(off + 8), size); break;
            case LC_MAIN:
                mainEntryOff = uint32_t(slice.at<uint64_t>(off + 8));
                img.stackSize = uint32_t(slice.at<uint64_t>(off + 16));
                haveMain = true;
                break;
            case LC_UNIXTHREAD: case LC_THREAD: {
                const uint32_t flavor = slice.at<uint32_t>(off + 8);
                if (flavor == 1 /*ARM_THREAD_STATE*/) {
                    unixThreadPc = slice.at<uint32_t>(off + 16 + 15 * 4);
                    const uint32_t cpsr = slice.at<uint32_t>(off + 16 + 16 * 4);
                    if (cpsr & 0x20) unixThreadPc |= 1;
                    haveThread = true;
                }
                break;
            }
            case LC_ENCRYPTION_INFO:
                if (slice.at<uint32_t>(off + 16) != 0)
                    throw EncryptedBinary("This app is encrypted (App Store FairPlay DRM). HydrogenVM needs a decrypted copy of the .ipa.");
                break;
            case LC_DYLD_CHAINED_FIXUPS:
                throw MachOError("Mach-O: chained fixups aren't used by 32-bit iOS binaries; file is unsupported");
            default: break;
        }
        off += size;
    }

    // ---- placement
    uint32_t lo = 0xFFFFFFFFu, hi = 0;
    for (const auto& s : img.segments) {
        if (!s.vmsize || s.name == "__PAGEZERO") continue;
        lo = std::min(lo, s.vmaddr);
        hi = std::max(hi, s.vmaddr + s.vmsize);
    }
    if (lo >= hi) throw MachOError("Mach-O: no loadable segments");
    if (opts.loadAddress && opts.loadAddress != lo) {
        const bool movable = h.filetype != MH_EXECUTE || (h.flags & MH_PIE);
        if (!movable) throw MachOError("Mach-O: non-PIE executable can't be moved");
        cx.slide = opts.loadAddress - lo;
    }
    img.slide = cx.slide;
    if (cx.slide) {
        for (auto& s : img.segments) s.vmaddr += cx.slide;
        for (auto& s : img.sections) s.addr += cx.slide;
    }
    img.lowAddr = lo + cx.slide;
    img.highAddr = hi + cx.slide;
    if (uint64_t(img.highAddr) > Memory::GuestArena::kHleBase) throw MachOError("Mach-O: image overlaps the HLE region");

    // ---- map segments
    for (const auto& s : img.segments) {
        if (!s.vmsize || s.name == "__PAGEZERO") continue;
        for (uint32_t a = s.vmaddr; a < s.vmaddr + s.vmsize; a += Memory::GuestArena::kGuestPageSize)
            if (mem.isMapped(a)) throw MachOError("Mach-O: segment " + s.name + " overlaps existing memory");
        uint8_t prot = 0;
        if (s.initprot & 1) prot |= Memory::kProtRead;
        if (s.initprot & 2) prot |= Memory::kProtWrite;
        if (s.initprot & 4) prot |= Memory::kProtExec;
        mem.map(s.vmaddr, s.vmsize, prot | Memory::kProtWrite);   // writable while we patch
        if (s.filesize) mem.copyIn(s.vmaddr, file.data() + sl.offset + s.fileoff, std::min(s.filesize, s.vmsize));
        if (s.fileoff == 0 && s.filesize) img.headerAddr = s.vmaddr;
    }
    // Zero-fill sections inside file-backed segments are already zero (fresh pages).

    // ---- symbols
    if (cx.symtab) {
        for (uint32_t i = 0; i < cx.symtab->nsyms; ++i) {
            const Nlist s = cx.sym(i);
            if (s.n_type & N_STAB) continue;
            const uint8_t t = s.n_type & N_TYPE;
            if (t != N_SECT && t != N_ABS) continue;
            std::string name = cx.symName(s);
            if (name.empty()) continue;
            uint32_t addr = s.n_value + (t == N_SECT ? cx.slide : 0);
            if (s.n_desc & N_ARM_THUMB_DEF) addr |= 1;
            if (s.n_type & N_EXT) img.exports.emplace(name, addr);
            if (t == N_SECT) img.symbolsByAddr.emplace(addr, std::move(name));
        }
    }

    // ---- fixups
    if (dyldInfo) {
        auto stream = [&](uint32_t o, uint32_t n) -> const uint8_t* {
            if (!n) return nullptr;
            if (uint64_t(o) + n > sl.size) throw MachOError("Mach-O: dyld info out of range");
            return file.data() + sl.offset + o;
        };
        if (cx.slide && dyldInfo->rebase_size) runRebase(cx, stream(dyldInfo->rebase_off, dyldInfo->rebase_size), dyldInfo->rebase_size);
        if (dyldInfo->bind_size) runBind(cx, stream(dyldInfo->bind_off, dyldInfo->bind_size), dyldInfo->bind_size, false, false);
        if (dyldInfo->lazy_bind_size) runBind(cx, stream(dyldInfo->lazy_bind_off, dyldInfo->lazy_bind_size), dyldInfo->lazy_bind_size, true, false);
        if (dyldInfo->weak_bind_size) runBind(cx, stream(dyldInfo->weak_bind_off, dyldInfo->weak_bind_size), dyldInfo->weak_bind_size, false, true);
    } else {
        legacyBinds(cx);
    }

    // ---- initialisers
    for (const auto* sec : img.sectionsOfType(S_MOD_INIT_FUNC_POINTERS))
        for (uint32_t a = sec->addr; a + 4 <= sec->addr + sec->size; a += 4)
            img.initializers.push_back(mem.read<uint32_t>(a));

    // ---- entry point
    if (haveMain) {
        const Segment* text = nullptr;
        for (const auto& s : img.segments) if (s.name == "__TEXT") text = &s;
        if (!text) throw MachOError("Mach-O: LC_MAIN without __TEXT");
        // ld64 stores the Thumb bit in entryoff itself.
        img.entry = text->vmaddr + mainEntryOff;
        img.entryIsMain = true;
    } else if (haveThread) {
        img.entry = unixThreadPc + cx.slide;
    } else if (h.filetype == MH_EXECUTE) {
        throw MachOError("Mach-O: executable has no entry point");
    }

    // ---- final protections (keep text readable; we never execute from host)
    for (const auto& s : img.segments) {
        if (!s.vmsize || s.name == "__PAGEZERO") continue;
        uint8_t prot = 0;
        if (s.initprot & 1) prot |= Memory::kProtRead;
        if (s.initprot & 2) prot |= Memory::kProtWrite;
        if (s.initprot & 4) prot |= Memory::kProtExec;
        mem.protect(s.vmaddr, s.vmsize, prot);
    }
    return img;
}

} // namespace Hydrogen::Loader
