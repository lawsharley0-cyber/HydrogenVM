#include "loader/AppBundle.hpp"
#include "loader/Zip.hpp"
#include <filesystem>
#include <fstream>
#include <cstdio>
#include <string>
#include <vector>
#include <zlib.h>

namespace fs = std::filesystem;
using Hydrogen::Loader::AppInfo;
using Hydrogen::Loader::importIpa;

class ZipBuilder {
public:
    void add(const std::string& name, const std::string& content) {
        entries_.push_back({name, content});
    }

    std::vector<uint8_t> build() const {
        std::vector<uint8_t> out;
        std::vector<uint8_t> cd;
        for (const auto& e : entries_) {
            uint32_t crc = crc32(0, reinterpret_cast<const uint8_t*>(e.content.data()), e.content.size());
            uint32_t size = e.content.size();
            uint32_t offset = out.size();

            // Local file header
            add32(out, 0x04034b50);
            add16(out, 20); // version
            add16(out, 0); // flags
            add16(out, 0); // compression
            add16(out, 0); // mod time
            add16(out, 0); // mod date
            add32(out, crc);
            add32(out, size);
            add32(out, size);
            add16(out, e.name.size());
            add16(out, 0); // extra length
            out.insert(out.end(), e.name.begin(), e.name.end());
            out.insert(out.end(), e.content.begin(), e.content.end());

            // Central directory
            add32(cd, 0x02014b50);
            add16(cd, 20); // version made by
            add16(cd, 20); // version needed
            add16(cd, 0); // flags
            add16(cd, 0); // compression
            add16(cd, 0); // time
            add16(cd, 0); // date
            add32(cd, crc);
            add32(cd, size);
            add32(cd, size);
            add16(cd, e.name.size());
            add16(cd, 0); // extra
            add16(cd, 0); // comment
            add16(cd, 0); // disk start
            add16(cd, 0); // internal attr
            add32(cd, 0); // external attr
            add32(cd, offset);
            cd.insert(cd.end(), e.name.begin(), e.name.end());
        }

        uint32_t cdOffset = out.size();
        uint32_t cdSize = cd.size();
        out.insert(out.end(), cd.begin(), cd.end());

        // End of central directory
        add32(out, 0x06054b50);
        add16(out, 0); // disk number
        add16(out, 0); // cd disk
        add16(out, entries_.size());
        add16(out, entries_.size());
        add32(out, cdSize);
        add32(out, cdOffset);
        add16(out, 0); // comment length

        return out;
    }

private:
    struct Entry { std::string name, content; };
    std::vector<Entry> entries_;

    static void add16(std::vector<uint8_t>& b, uint16_t v) {
        b.push_back(v & 0xff);
        b.push_back((v >> 8) & 0xff);
    }
    static void add32(std::vector<uint8_t>& b, uint32_t v) {
        b.push_back(v & 0xff);
        b.push_back((v >> 8) & 0xff);
        b.push_back((v >> 16) & 0xff);
        b.push_back((v >> 24) & 0xff);
    }
};

int main(int argc, char** argv) {
    const fs::path root = argc > 1 ? argv[1] : "build/app-bundle-test-fixtures";
    const fs::path appsRoot = root / "apps";
    std::error_code ec;
    fs::create_directories(root, ec);
    int passed = 0, failed = 0;

    auto check = [&](const std::string& name, const std::vector<uint8_t>& zipData, bool valid, const std::string& expectedBundleId) {
        const fs::path ipaPath = root / (name + ".ipa");
        fs::remove_all(appsRoot, ec);
        fs::create_directories(appsRoot, ec);

        {
            std::ofstream file(ipaPath, std::ios::binary);
            file.write(reinterpret_cast<const char*>(zipData.data()), zipData.size());
        }

        bool ok = false;
        try {
            AppInfo info = importIpa(ipaPath.string(), appsRoot.string());
            if (valid) {
                ok = (info.bundleId == expectedBundleId);
                if (!ok) std::printf("  expected bundle id %s, got %s\n", expectedBundleId.c_str(), info.bundleId.c_str());

                // verify extraction output
                const fs::path expectedAppDir = appsRoot / expectedBundleId / "MyApp.app";
                if (!fs::exists(expectedAppDir, ec) || !fs::is_directory(expectedAppDir, ec)) {
                    ok = false;
                    std::printf("  expected extraction output %s to exist\n", expectedAppDir.c_str());
                } else if (!fs::exists(expectedAppDir / "Info.plist", ec)) {
                    ok = false;
                    std::printf("  expected Info.plist to be extracted\n");
                }
            }
        } catch (const std::exception& e) {
            ok = !valid;
            if (valid) std::printf("unexpected exception for %s: %s\n", name.c_str(), e.what());
        }

        fs::remove(ipaPath, ec);
        fs::remove_all(appsRoot, ec);
        std::printf("%s %s\n", ok ? "PASS" : "FAIL", name.c_str());
        ok ? ++passed : ++failed;
    };

    {
        ZipBuilder z;
        z.add("Payload/MyApp.app/Info.plist",
              "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
              "<!DOCTYPE plist PUBLIC \"-//Apple//DTD PLIST 1.0//EN\" \"http://www.apple.com/DTDs/PropertyList-1.0.dtd\">\n"
              "<plist version=\"1.0\">\n"
              "<dict>\n"
              "    <key>CFBundleIdentifier</key>\n"
              "    <string>com.example.myapp</string>\n"
              "    <key>CFBundleExecutable</key>\n"
              "    <string>MyApp</string>\n"
              "</dict>\n"
              "</plist>");
        z.add("Payload/MyApp.app/MyApp", "not a real macho but it doesn't matter for this test because importIpa fails gracefully if probeMachO fails on an existing file, wait, readBundle fails if it doesn't exist. So we add it.");
        check("valid_ipa", z.build(), true, "com.example.myapp");
    }

    {
        ZipBuilder z;
        z.add("Payload/NotAnApp.txt", "hello");
        check("no_app_dir", z.build(), false, "");
    }

    {
        ZipBuilder z;
        z.add("Payload/Nested/MyApp.app/Info.plist", "bad");
        check("nested_app_dir", z.build(), false, "");
    }

    std::printf("AppBundle tests: %d passed, %d failed\n", passed, failed);
    return failed ? 1 : 0;
}
