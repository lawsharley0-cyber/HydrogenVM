#include "ElfLoader.hpp"
#include <cstring>
#include <fstream>
#include <stdexcept>
#include <algorithm>

namespace Argon::Loader {
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

} // namespace Argon::Loader
