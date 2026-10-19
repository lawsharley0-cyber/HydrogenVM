#include "Zip.hpp"
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <zlib.h>

namespace fs = std::filesystem;

namespace Hydrogen::Loader {
namespace {

template <typename T> T le(const uint8_t* p) { T v; std::memcpy(&v, p, sizeof v); return v; }

bool safeRelative(const std::string& p) {
    if (p.empty() || p[0] == '/' || p.find('\\') != std::string::npos) return false;
    size_t i = 0;
    while (i <= p.size()) {
        size_t j = p.find('/', i);
        if (j == std::string::npos) j = p.size();
        const std::string comp = p.substr(i, j - i);
        if (comp == "..") return false;
        i = j + 1;
    }
    return true;
}

} // namespace

ZipReader::ZipReader(const std::string& path) {
    f_ = std::fopen(path.c_str(), "rb");
    if (!f_) throw ZipError("can't open " + path);
    std::fseek(f_, 0, SEEK_END);
    size_ = uint64_t(ftello(f_));
    if (size_ < 22) throw ZipError("not a zip file (too small)");

    // Find the end-of-central-directory record (it may be followed by a comment).
    const size_t tail = size_t(std::min<uint64_t>(size_, 65557));
    std::vector<uint8_t> buf(tail);
    readRaw(size_ - tail, buf.data(), tail);
    size_t eocd = SIZE_MAX;
    for (size_t i = tail - 22 + 1; i-- > 0;)
        if (le<uint32_t>(&buf[i]) == 0x06054B50u) { eocd = i; break; }
    if (eocd == SIZE_MAX) throw ZipError("not a zip file (no end of central directory)");
    uint64_t count = le<uint16_t>(&buf[eocd + 10]);
    uint64_t cdSize = le<uint32_t>(&buf[eocd + 12]);
    uint64_t cdOff = le<uint32_t>(&buf[eocd + 16]);

    // Zip64 locator immediately precedes the EOCD.
    if ((count == 0xFFFF || cdOff == 0xFFFFFFFFu) && eocd >= 20 && le<uint32_t>(&buf[eocd - 20]) == 0x07064B50u) {
        const uint64_t z64 = le<uint64_t>(&buf[eocd - 20 + 8]);
        uint8_t rec[56];
        readRaw(z64, rec, sizeof rec);
        if (le<uint32_t>(rec) != 0x06064B50u) throw ZipError("bad zip64 record");
        count = le<uint64_t>(rec + 32);
        cdSize = le<uint64_t>(rec + 40);
        cdOff = le<uint64_t>(rec + 48);
    }
    if (cdOff + cdSize > size_) throw ZipError("central directory out of range");
    std::vector<uint8_t> cd(static_cast<size_t>(cdSize));
    readRaw(cdOff, cd.data(), cd.size());
    size_t p = 0;
    for (uint64_t i = 0; i < count; ++i) {
        if (p + 46 > cd.size() || le<uint32_t>(&cd[p]) != 0x02014B50u) throw ZipError("corrupt central directory");
        ZipEntry e;
        const uint16_t madeBy = le<uint16_t>(&cd[p + 4]);
        e.method = le<uint16_t>(&cd[p + 10]);
        e.crc32 = le<uint32_t>(&cd[p + 16]);
        e.compressedSize = le<uint32_t>(&cd[p + 20]);
        e.size = le<uint32_t>(&cd[p + 24]);
        const uint16_t nameLen = le<uint16_t>(&cd[p + 28]), extraLen = le<uint16_t>(&cd[p + 30]), commentLen = le<uint16_t>(&cd[p + 32]);
        const uint32_t ext = le<uint32_t>(&cd[p + 38]);
        e.localHeaderOffset = le<uint32_t>(&cd[p + 42]);
        if (p + 46 + nameLen + extraLen + commentLen > cd.size()) throw ZipError("corrupt central directory entry");
        e.name.assign(reinterpret_cast<const char*>(&cd[p + 46]), nameLen);
        if ((madeBy >> 8) == 3) e.unixMode = ext >> 16;
        // Zip64 extended information.
        size_t x = p + 46 + nameLen;
        const size_t xend = x + extraLen;
        while (x + 4 <= xend) {
            const uint16_t id = le<uint16_t>(&cd[x]), len = le<uint16_t>(&cd[x + 2]);
            if (id == 1) {
                size_t q = x + 4;
                if (e.size == 0xFFFFFFFFu && q + 8 <= x + 4 + len) { e.size = le<uint64_t>(&cd[q]); q += 8; }
                if (e.compressedSize == 0xFFFFFFFFu && q + 8 <= x + 4 + len) { e.compressedSize = le<uint64_t>(&cd[q]); q += 8; }
                if (e.localHeaderOffset == 0xFFFFFFFFu && q + 8 <= x + 4 + len) { e.localHeaderOffset = le<uint64_t>(&cd[q]); q += 8; }
            }
            x += 4 + len;
        }
        entries_.push_back(std::move(e));
        p += 46 + nameLen + extraLen + commentLen;
    }
}

ZipReader::~ZipReader() { if (f_) std::fclose(f_); }

void ZipReader::readRaw(uint64_t off, void* dst, size_t n) const {
    if (off + n > size_) throw ZipError("read past end of zip");
    if (fseeko(f_, off_t(off), SEEK_SET) != 0 || std::fread(dst, 1, n, f_) != n) throw ZipError("zip read failed");
}

const ZipEntry* ZipReader::find(const std::string& name) const {
    for (const auto& e : entries_) if (e.name == name) return &e;
    return nullptr;
}

std::vector<uint8_t> ZipReader::read(const ZipEntry& e) const {
    uint8_t lh[30];
    readRaw(e.localHeaderOffset, lh, sizeof lh);
    if (le<uint32_t>(lh) != 0x04034B50u) throw ZipError("bad local header for " + e.name);
    const uint64_t dataOff = e.localHeaderOffset + 30 + le<uint16_t>(lh + 26) + le<uint16_t>(lh + 28);
    if (e.size > (uint64_t(1) << 31)) throw ZipError(e.name + " is too large");
    std::vector<uint8_t> comp(size_t(e.compressedSize));
    readRaw(dataOff, comp.data(), comp.size());
    std::vector<uint8_t> out;
    if (e.method == 0) {
        out = std::move(comp);
    } else if (e.method == 8) {
        out.resize(size_t(e.size));
        z_stream zs{};
        if (inflateInit2(&zs, -MAX_WBITS) != Z_OK) throw ZipError("inflateInit failed");
        zs.next_in = comp.data();
        zs.avail_in = uInt(comp.size());
        zs.next_out = out.data();
        zs.avail_out = uInt(out.size());
        const int r = inflate(&zs, Z_FINISH);
        inflateEnd(&zs);
        if (r != Z_STREAM_END || zs.total_out != e.size) throw ZipError("corrupt deflate data in " + e.name);
    } else {
        throw ZipError(e.name + ": unsupported compression method " + std::to_string(e.method));
    }
    if (crc32(0, out.data(), uInt(out.size())) != e.crc32) throw ZipError("CRC mismatch in " + e.name);
    return out;
}

size_t ZipReader::extract(const std::string& prefix, const std::string& destDir) const {
    size_t n = 0;
    for (const auto& e : entries_) {
        if (e.name.compare(0, prefix.size(), prefix) != 0) continue;
        const std::string rel = e.name.substr(prefix.size());
        if (rel.empty()) continue;
        if (!safeRelative(rel)) throw ZipError("unsafe path in archive: " + e.name);
        const fs::path dst = fs::path(destDir) / rel;
        if (e.isDirectory()) { fs::create_directories(dst); continue; }
        fs::create_directories(dst.parent_path());
        const auto data = read(e);
        if (e.isSymlink()) {
            const std::string target(data.begin(), data.end());
            if (!target.empty() && target[0] != '/' && safeRelative(target)) {
                std::error_code ec;
                fs::remove(dst, ec);
                fs::create_symlink(target, dst, ec);
                if (!ec) { ++n; continue; }
            }
            continue;   // skip symlinks pointing outside the bundle
        }
        std::FILE* out = std::fopen(dst.c_str(), "wb");
        if (!out) throw ZipError("can't write " + dst.string());
        if (!data.empty() && std::fwrite(data.data(), 1, data.size(), out) != data.size()) {
            std::fclose(out);
            throw ZipError("write failed: " + dst.string());
        }
        std::fclose(out);
        ++n;
    }
    return n;
}

} // namespace Hydrogen::Loader
