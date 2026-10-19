// HydrogenVM — minimal zip reader for .ipa files (stored + deflate, zip64).
#pragma once
#include <cstdint>
#include <stdexcept>
#include <string>
#include <vector>

namespace Hydrogen::Loader {

struct ZipError : std::runtime_error { using std::runtime_error::runtime_error; };

struct ZipEntry {
    std::string name;
    uint64_t compressedSize = 0, size = 0, localHeaderOffset = 0;
    uint16_t method = 0;
    uint32_t crc32 = 0;
    uint32_t unixMode = 0;     // from external attributes, 0 if not a Unix archive
    bool isDirectory() const { return !name.empty() && name.back() == '/'; }
    bool isSymlink() const { return (unixMode & 0170000) == 0120000; }
};

class ZipReader {
public:
    explicit ZipReader(const std::string& path);
    ~ZipReader();
    ZipReader(const ZipReader&) = delete;
    ZipReader& operator=(const ZipReader&) = delete;

    const std::vector<ZipEntry>& entries() const { return entries_; }
    const ZipEntry* find(const std::string& name) const;
    std::vector<uint8_t> read(const ZipEntry& e) const;

    // Extract entries whose name starts with `prefix` into `destDir`,
    // stripping the prefix. Rejects absolute paths and ".." components.
    // Returns the number of files written.
    size_t extract(const std::string& prefix, const std::string& destDir) const;

private:
    void readRaw(uint64_t off, void* dst, size_t n) const;
    std::FILE* f_ = nullptr;
    uint64_t size_ = 0;
    std::vector<ZipEntry> entries_;
};

} // namespace Hydrogen::Loader
