#include "loader/Zip.hpp"
#include <zlib.h>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <vector>

using Bytes = std::vector<uint8_t>;
namespace fs = std::filesystem;
using Hydrogen::Loader::ZipReader;
using Hydrogen::Loader::ZipError;

void num(Bytes& b, uint64_t v, int n) { for (int i=0;i<n;++i) b.push_back(uint8_t(v>>(8*i))); }
void set(Bytes& b, size_t off, uint64_t v, int n) { for(int i=0;i<n;++i)b[off+i]=uint8_t(v>>(8*i)); }
void append(Bytes& b, const Bytes& a) { b.insert(b.end(),a.begin(),a.end()); }

struct TestFile {
    std::string name;
    std::string content;
    bool is_symlink;
};

Bytes simple_archive(const std::vector<TestFile>& files) {
    Bytes b;
    std::vector<Bytes> cds;
    size_t offset = 0;
    for (const auto& f : files) {
        const std::string name = f.name;
        const Bytes comp(f.content.begin(), f.content.end());
        const uint32_t crc = crc32(0, comp.data(), comp.size());

        Bytes local(30);
        set(local, 0, 0x04034b50, 4);
        set(local, 4, 20, 2);
        set(local, 8, 0, 2);
        set(local, 14, crc, 4);
        set(local, 18, comp.size(), 4);
        set(local, 22, comp.size(), 4);
        set(local, 26, name.size(), 2);
        local.insert(local.end(), name.begin(), name.end());
        append(local, comp);

        Bytes cd(46);
        set(cd, 0, 0x02014b50, 4);
        set(cd, 4, 3 << 8, 2); // made by Unix
        set(cd, 6, 45, 2);
        set(cd, 10, 0, 2);
        set(cd, 16, crc, 4);
        set(cd, 20, comp.size(), 4);
        set(cd, 24, comp.size(), 4);
        set(cd, 28, name.size(), 2);

        uint32_t ext = f.is_symlink ? 0120000 : 0100644;
        ext <<= 16;
        set(cd, 38, ext, 4);

        set(cd, 42, offset, 4);
        cd.insert(cd.end(), name.begin(), name.end());

        cds.push_back(cd);
        append(b, local);
        offset += local.size();
    }

    size_t cdOffset = b.size();
    size_t cdSize = 0;
    for (const auto& cd : cds) {
        append(b, cd);
        cdSize += cd.size();
    }

    Bytes end(22);
    set(end, 0, 0x06054b50, 4);
    set(end, 8, files.size(), 2);
    set(end, 10, files.size(), 2);
    set(end, 12, cdSize, 4);
    set(end, 16, cdOffset, 4);
    append(b, end);
    return b;
}

int main(int argc, char** argv) {
    const fs::path root = argc > 1 ? argv[1] : "build/zip-extract-fixtures";
    fs::create_directories(root);
    int passed = 0, failed = 0;

    auto check = [&](const std::string& name, const Bytes& bytes, const std::string& prefix, bool valid, size_t expected_files) {
        const fs::path path = root / (name + ".zip");
        const fs::path dest = root / ("out_" + name);
        { std::ofstream file(path, std::ios::binary); file.write(reinterpret_cast<const char*>(bytes.data()), bytes.size()); }
        bool ok = false;
        try {
            ZipReader z(path.string());
            fs::create_directories(dest);
            size_t n = z.extract(prefix, dest.string());
            ok = (valid && n == expected_files);
        } catch(const ZipError&) {
            ok = !valid;
        } catch(const std::exception& e) {
            std::printf("unexpected %s: %s\n", name.c_str(), e.what());
        }

        if (fs::exists(dest)) fs::remove_all(dest);
        fs::remove(path);

        std::printf("%s %s\n", ok ? "PASS" : "FAIL", name.c_str());
        ok ? ++passed : ++failed;
    };

    check("basic_extract", simple_archive({{"test.txt", "hello", false}}), "", true, 1);
    check("prefix_extract", simple_archive({{"prefix/test.txt", "hello", false}, {"other.txt", "ignore", false}}), "prefix/", true, 1);
    check("unsafe_path", simple_archive({{"../unsafe.txt", "bad", false}}), "", false, 0);
    check("unsafe_path_nested", simple_archive({{"dir/../../unsafe.txt", "bad", false}}), "", false, 0);
    check("absolute_path", simple_archive({{"/etc/passwd", "bad", false}}), "", false, 0);
    check("extract_dir", simple_archive({{"dir/", "", false}, {"dir/file.txt", "test", false}}), "", true, 1);

    // Symlink tests
    check("valid_symlink", simple_archive({{"link", "target.txt", true}, {"target.txt", "hello", false}}), "", true, 2);
    check("unsafe_symlink_absolute", simple_archive({{"link", "/etc/passwd", true}}), "", true, 0);
    check("unsafe_symlink_relative", simple_archive({{"link", "../passwd", true}}), "", true, 0);

    std::printf("ZIP extraction: %d passed, %d failed\n", passed, failed);
    return failed ? 1 : 0;
}
