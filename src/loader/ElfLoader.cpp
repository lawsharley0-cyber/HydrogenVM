#include "ElfLoader.hpp"
#include <cstring>
#include <fstream>
#include <stdexcept>
#include <algorithm>

namespace Hydrogen::Loader {
namespace {

struct Ehdr { uint8_t ident[16]; uint16_t type, machine; uint32_t version, entry, phoff, shoff, flags;
              uint16_t ehsize, phentsize, phnum, shentsize, shnum, shstrndx; };
struct Phdr { uint32_t type, offset, vaddr, paddr, filesz, memsz, flags, align; };
struct Shdr { uint32_t name, type, flags, addr, offset, size, link, info, addralign, entsize; };
struct Sym  { uint32_t name, value, size; uint8_t info, other; uint16_t shndx; };

template <typename T> T at(const std::vector<uint8_t>& f, size_t off) {
    if (off + sizeof(T) > f.size()) throw std::runtime_error("ELF: truncated");
    T v; std::memcpy(&v, f.data() + off, sizeof(T)); return v;
}

} // namespace

std::vector<uint8_t> readFile(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) throw std::runtime_error("cannot open " + path);
    return std::vector<uint8_t>((std::istreambuf_iterator<char>(in)), {});
}

ElfImage loadElf(Memory::GuestArena& mem, const std::vector<uint8_t>& f, uint32_t bias) {
    const auto eh = at<Ehdr>(f, 0);
    if (std::memcmp(eh.ident, "\x7F" "ELF", 4) || eh.ident[4] != 1 /*32-bit*/ || eh.ident[5] != 1 /*LE*/)
        throw std::runtime_error("ELF: not a 32-bit little-endian ELF");
    if (eh.machine != 40 /*EM_ARM*/) throw std::runtime_error("ELF: not an ARM binary");

    ElfImage img;
    img.load_bias = bias;
    img.entry = eh.entry + bias;
    for (uint32_t i = 0; i < eh.phnum; ++i) {
        const auto ph = at<Phdr>(f, eh.phoff + size_t(i) * eh.phentsize);
        if (ph.type != 1 /*PT_LOAD*/ || ph.memsz == 0) continue;
        if (ph.filesz > ph.memsz || size_t(ph.offset) + ph.filesz > f.size())
            throw std::runtime_error("ELF: bad segment");
        const uint32_t va = ph.vaddr + bias;
        uint8_t prot = 0;
        if (ph.flags & 4) prot |= Memory::kProtRead;
        if (ph.flags & 2) prot |= Memory::kProtWrite;
        if (ph.flags & 1) prot |= Memory::kProtExec;
        mem.map(va, ph.memsz, prot);
        mem.copyIn(va, f.data() + ph.offset, ph.filesz);
        img.lowest = std::min(img.lowest, va);
        img.highest = std::max(img.highest, va + ph.memsz);
    }
    // Symbol table (optional).
    if (eh.shoff && eh.shnum) {
        for (uint32_t i = 0; i < eh.shnum; ++i) {
            const auto sh = at<Shdr>(f, eh.shoff + size_t(i) * eh.shentsize);
            if (sh.type != 2 /*SHT_SYMTAB*/) continue;
            const auto strs = at<Shdr>(f, eh.shoff + size_t(sh.link) * eh.shentsize);
            for (uint32_t off = 0; off + sizeof(Sym) <= sh.size; off += sizeof(Sym)) {
                const auto s = at<Sym>(f, sh.offset + off);
                if (!s.name || !s.shndx) continue;
                const size_t p = strs.offset + s.name;
                if (p >= f.size()) continue;
                const char* nm = reinterpret_cast<const char*>(f.data() + p);
                img.symbols[std::string(nm, strnlen(nm, f.size() - p))] = s.value + bias;
            }
        }
    }
    return img;
}

ElfShared loadElfShared(Memory::GuestArena& mem, const std::vector<uint8_t>& f, uint32_t bias) {
    const auto eh = at<Ehdr>(f, 0);
    if (std::memcmp(eh.ident, "\x7F" "ELF", 4) || eh.ident[4] != 1 || eh.ident[5] != 1)
        throw std::runtime_error("ELF: not a 32-bit little-endian ELF");
    if (eh.machine != 40) throw std::runtime_error("ELF: not an ARM binary");
    if (eh.type != 3 /*ET_DYN*/) throw std::runtime_error("ELF: not a shared object");

    ElfShared out;
    ElfImage& img = out.image;
    img.load_bias = bias;
    img.entry = 0;
    struct Seg { uint32_t va, size; uint8_t prot; };
    std::vector<Seg> segs;
    uint32_t dynOff = 0, dynSize = 0;
    for (uint32_t i = 0; i < eh.phnum; ++i) {
        const auto ph = at<Phdr>(f, eh.phoff + size_t(i) * eh.phentsize);
        if (ph.type == 2 /*PT_DYNAMIC*/) { dynOff = ph.vaddr; dynSize = ph.memsz; }
        if (ph.type == 0x70000001 /*PT_ARM_EXIDX*/) { out.exidx = ph.vaddr + bias; out.exidxCount = ph.memsz / 8; }
        if (ph.type != 1 /*PT_LOAD*/ || ph.memsz == 0) continue;
        if (ph.filesz > ph.memsz || size_t(ph.offset) + ph.filesz > f.size())
            throw std::runtime_error("ELF: bad segment");
        const uint32_t va = ph.vaddr + bias;
        uint8_t prot = 0;
        if (ph.flags & 4) prot |= Memory::kProtRead;
        if (ph.flags & 2) prot |= Memory::kProtWrite;
        if (ph.flags & 1) prot |= Memory::kProtExec;
        // Page-align the mapping; the loader writes relocations before the final protections.
        const uint32_t lo = va & ~0xFFFu, hi = (va + ph.memsz + 0xFFFu) & ~0xFFFu;
        mem.map(lo, hi - lo, Memory::kProtRead | Memory::kProtWrite);
        mem.copyIn(va, f.data() + ph.offset, ph.filesz);
        segs.push_back({lo, hi - lo, prot});
        img.lowest = std::min(img.lowest, lo);
        img.highest = std::max(img.highest, hi);
    }
    if (!dynOff) throw std::runtime_error("ELF: no dynamic section");

    // Dynamic section, read from guest memory (it lies inside a loaded segment).
    auto rd = [&](uint32_t va) { return mem.read<uint32_t>(va + bias); };
    uint32_t strtab = 0, symtab = 0, rel = 0, relSize = 0, jmprel = 0, pltSize = 0, hash = 0;
    uint32_t init = 0, initArray = 0, initArraySz = 0, fini = 0, finiArray = 0, finiArraySz = 0, soname = 0;
    std::vector<uint32_t> neededOffs;
    for (uint32_t off = 0; off + 8 <= dynSize; off += 8) {
        const uint32_t tag = rd(dynOff + off), val = rd(dynOff + off + 4);
        if (tag == 0) break;
        switch (tag) {
            case 1: neededOffs.push_back(val); break;            // DT_NEEDED
            case 2: pltSize = val; break;                        // DT_PLTRELSZ
            case 4: hash = val; break;                           // DT_HASH
            case 5: strtab = val; break;                         // DT_STRTAB
            case 6: symtab = val; break;                         // DT_SYMTAB
            case 7: case 8: throw std::runtime_error("ELF: RELA relocations are not supported on ARM");
            case 12: init = val; break;                          // DT_INIT
            case 13: fini = val; break;                          // DT_FINI
            case 14: soname = val; break;                        // DT_SONAME
            case 17: rel = val; break;                           // DT_REL
            case 18: relSize = val; break;                       // DT_RELSZ
            case 23: jmprel = val; break;                        // DT_JMPREL
            case 25: initArray = val; break;                     // DT_INIT_ARRAY
            case 26: finiArray = val; break;                     // DT_FINI_ARRAY
            case 27: initArraySz = val; break;
            case 28: finiArraySz = val; break;
            default: break;
        }
    }
    if (!strtab || !symtab) throw std::runtime_error("ELF: no dynamic symbol table");
    auto str = [&](uint32_t o) {
        std::string s;
        for (uint32_t a = strtab + bias + o; s.size() < 4096; ++a) {
            const char c = char(mem.read<uint8_t>(a));
            if (!c) break;
            s.push_back(c);
        }
        return s;
    };
    for (uint32_t o : neededOffs) out.needed.push_back(str(o));
    if (soname) out.soname = str(soname);

    // Number of dynamic symbols: nchain of DT_HASH (every Android .so of this era has one).
    const uint32_t nsyms = hash ? rd(hash + 4) : 0;
    struct DSym { uint32_t name, value; uint8_t info; uint16_t shndx; };
    auto sym = [&](uint32_t i) {
        const uint32_t a = symtab + i * 16;
        return DSym{rd(a), rd(a + 4), uint8_t(rd(a + 12) & 0xFF), uint16_t(rd(a + 12) >> 16)};
    };
    for (uint32_t i = 1; i < nsyms; ++i) {
        const DSym s = sym(i);
        if (!s.shndx || !s.name) continue;
        const uint8_t type = s.info & 0xF;
        if (type != 1 /*OBJECT*/ && type != 2 /*FUNC*/ && type != 0 /*NOTYPE*/) continue;
        img.symbols[str(s.name)] = s.value + bias;
    }

    auto relocate = [&](uint32_t tableVa, uint32_t size) {
        for (uint32_t off = 0; off + 8 <= size; off += 8) {
            const uint32_t where = rd(tableVa + off) + bias, info = rd(tableVa + off + 4);
            const uint32_t type = info & 0xFF, symIdx = info >> 8;
            switch (type) {
                case 0: break;                                    // R_ARM_NONE
                case 23:                                          // R_ARM_RELATIVE
                    mem.write<uint32_t>(where, mem.read<uint32_t>(where) + bias);
                    break;
                case 2: case 21: case 22: {                       // ABS32, GLOB_DAT, JUMP_SLOT
                    const DSym s = sym(symIdx);
                    if (s.shndx) {
                        // Defined here: bind to our own definition (no symbol interposition).
                        const uint32_t v = s.value + bias;
                        if (type == 2) mem.write<uint32_t>(where, mem.read<uint32_t>(where) + v);
                        else mem.write<uint32_t>(where, v);
                    } else {
                        out.imports.push_back({where, type, str(s.name), (s.info >> 4) == 2 /*STB_WEAK*/});
                    }
                    break;
                }
                default:
                    throw std::runtime_error("ELF: unsupported relocation type " + std::to_string(type));
            }
        }
    };
    if (rel) relocate(rel, relSize);
    if (jmprel) relocate(jmprel, pltSize);

    if (init) out.initializers.push_back(init + bias);
    for (uint32_t i = 0; i < initArraySz / 4; ++i) {
        const uint32_t fn = mem.read<uint32_t>(initArray + bias + i * 4);
        if (fn && fn != 0xFFFFFFFFu) out.initializers.push_back(fn);
    }
    for (uint32_t i = finiArraySz / 4; i-- > 0;) {
        const uint32_t fn = mem.read<uint32_t>(finiArray + bias + i * 4);
        if (fn && fn != 0xFFFFFFFFu) out.finalizers.push_back(fn);
    }
    if (fini) out.finalizers.push_back(fini + bias);

    for (const auto& s : segs) mem.protect(s.va, s.size, s.prot | Memory::kProtRead);
    // Writable segments that share a page with a read-only one (relro next to data) keep RW.
    for (const auto& s : segs) if (s.prot & Memory::kProtWrite) mem.protect(s.va, s.size, s.prot | Memory::kProtRead);
    return out;
}

} // namespace Hydrogen::Loader
