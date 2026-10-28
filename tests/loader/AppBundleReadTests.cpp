#include "loader/AppBundle.hpp"
#include "loader/Plist.hpp"
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;
using Hydrogen::Loader::AppInfo;
using Hydrogen::Loader::readBundle;
using Hydrogen::Plist::Value;

namespace {

int gPassed = 0;
int gFailed = 0;

void check(const std::string& name, bool ok, const std::string& detail) {
    std::printf("%s %s\n", ok ? "PASS" : "FAIL", name.c_str());
    if (!ok) {
        std::printf("  %s\n", detail.c_str());
        ++gFailed;
    } else {
        ++gPassed;
    }
}

void writeBytes(const fs::path& path, const std::vector<uint8_t>& bytes) {
    std::ofstream out(path, std::ios::binary);
    out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
}

void writeText(const fs::path& path, const std::string& text) {
    writeBytes(path, std::vector<uint8_t>(text.begin(), text.end()));
}

void put32(std::vector<uint8_t>& b, uint32_t v) {
    b.push_back(static_cast<uint8_t>(v));
    b.push_back(static_cast<uint8_t>(v >> 8));
    b.push_back(static_cast<uint8_t>(v >> 16));
    b.push_back(static_cast<uint8_t>(v >> 24));
}

// Thin little-endian Mach-O with no load commands. probeMachO accepts an
// armv7 image and rejects other CPU types from the header alone.
std::vector<uint8_t> thinMachO(uint32_t magic, uint32_t cpuType, uint32_t cpuSubtype) {
    std::vector<uint8_t> b;
    put32(b, magic);
    put32(b, cpuType);
    put32(b, cpuSubtype);
    put32(b, 2); // MH_EXECUTE
    put32(b, 0); // ncmds
    put32(b, 0); // sizeofcmds
    put32(b, 0); // flags
    return b;
}

// armv7 executable whose only load command is LC_ENCRYPTION_INFO with cryptid 1.
std::vector<uint8_t> encryptedArmv7() {
    std::vector<uint8_t> b = thinMachO(0xFEEDFACE, 12, 9);
    b[16] = 1; // ncmds
    b[20] = 20; // sizeofcmds
    put32(b, 0x21); // LC_ENCRYPTION_INFO
    put32(b, 20);   // cmdsize
    put32(b, 0);    // cryptoff
    put32(b, 0);    // cryptsize
    put32(b, 1);    // cryptid
    return b;
}

std::string joined(const std::vector<std::string>& paths) {
    std::string out;
    for (const auto& p : paths) {
        if (!out.empty()) out += ", ";
        out += p;
    }
    return out;
}

} // namespace

int main() {
    const fs::path root = "build/appbundle-test-fixtures";
    fs::remove_all(root);
    fs::create_directories(root);

    {
        const fs::path appPath = root / "MissingPlist.app";
        fs::create_directories(appPath);
        const AppInfo info = readBundle(appPath.string());
        check("missing-plist-reports-error",
              info.problem == "The app bundle has no Info.plist." && !info.runnable && info.executablePath.empty(),
              "problem='" + info.problem + "' runnable=" + std::to_string(info.runnable));
    }

    {
        const fs::path appPath = root / "CorruptPlist.app";
        fs::create_directories(appPath);
        writeText(appPath / "Info.plist", "bplist00");
        const AppInfo info = readBundle(appPath.string());
        const bool ok = info.problem.rfind("Couldn't read Info.plist:", 0) == 0 &&
                        info.problem.find("bplist: too small") != std::string::npos && !info.runnable;
        check("corrupt-plist-reports-parser-error", ok, "problem='" + info.problem + "'");
    }

    {
        const fs::path appPath = root / "ValidPlistMissingExe.app";
        fs::create_directories(appPath);
        auto dict = Value::makeDict();
        dict->set("CFBundleExecutable", Value::makeString("MyApp"));
        dict->set("CFBundleIdentifier", Value::makeString("com.example.myapp"));
        dict->set("CFBundleVersion", Value::makeString("45"));
        Hydrogen::Plist::writeXmlFile(*dict, (appPath / "Info.plist").string());
        const AppInfo info = readBundle(appPath.string());
        check("missing-exe-reports-error",
              info.problem == "The app's executable (MyApp) is missing." && !info.runnable,
              "problem='" + info.problem + "'");
        check("parses-bundle-id", info.bundleId == "com.example.myapp", "bundleId='" + info.bundleId + "'");
        check("version-falls-back-to-bundle-version", info.version == "45", "version='" + info.version + "'");
        check("missing-exe-path", info.executablePath == (appPath / "MyApp").string(),
              "executablePath='" + info.executablePath + "'");
    }

    {
        const fs::path appPath = root / "FallbackExeName.app";
        fs::create_directories(appPath);
        auto dict = Value::makeDict();
        Hydrogen::Plist::writeXmlFile(*dict, (appPath / "Info.plist").string());
        const AppInfo info = readBundle(appPath.string());
        check("fallback-exe-name", info.problem == "The app's executable (FallbackExeName) is missing.",
              "problem='" + info.problem + "'");
        check("fallback-exe-path", info.executablePath == (appPath / "FallbackExeName").string(),
              "executablePath='" + info.executablePath + "'");
        check("fallback-bundle-name", info.name == "FallbackExeName" && info.displayName == "FallbackExeName",
              "name='" + info.name + "' display='" + info.displayName + "'");
        check("fallback-bundle-id", info.bundleId == "unknown.FallbackExeName", "bundleId='" + info.bundleId + "'");
    }

    {
        const fs::path appPath = root / "ValidExe.app";
        fs::create_directories(appPath);
        auto dict = Value::makeDict();
        dict->set("CFBundleExecutable", Value::makeString("MyAppBin"));
        dict->set("CFBundleIdentifier", Value::makeString("com.example.valid"));
        dict->set("CFBundleName", Value::makeString("My App Name"));
        dict->set("CFBundleDisplayName", Value::makeString("My App Display Name"));
        dict->set("CFBundleShortVersionString", Value::makeString("1.2.3"));
        dict->set("MinimumOSVersion", Value::makeString("8.0"));
        auto fam = Value::makeArray();
        fam->array.push_back(Value::makeInt(1));
        fam->array.push_back(Value::makeString("2"));
        dict->set("UIDeviceFamily", fam);
        Hydrogen::Plist::writeXmlFile(*dict, (appPath / "Info.plist").string());
        writeBytes(appPath / "MyAppBin", thinMachO(0xFEEDFACE, 12, 9));

        const AppInfo info = readBundle(appPath.string());
        check("armv7-macho-is-runnable",
              info.problem.empty() && info.runnable && !info.encrypted && info.cpuSubtype == 9,
              "problem='" + info.problem + "' runnable=" + std::to_string(info.runnable) +
                  " encrypted=" + std::to_string(info.encrypted) + " subtype=" + std::to_string(info.cpuSubtype));
        check("parses-name", info.name == "My App Name" && info.displayName == "My App Display Name",
              "name='" + info.name + "' display='" + info.displayName + "'");
        check("parses-version", info.version == "1.2.3" && info.minimumOS == "8.0",
              "version='" + info.version + "' min='" + info.minimumOS + "'");
        check("parses-device-family", info.deviceFamily.size() == 2 && info.deviceFamily[0] == 1 && info.deviceFamily[1] == 2,
              "family size=" + std::to_string(info.deviceFamily.size()));
    }

    {
        const fs::path appPath = root / "NotMachO.app";
        fs::create_directories(appPath);
        auto dict = Value::makeDict();
        dict->set("CFBundleExecutable", Value::makeString("MyAppBin"));
        Hydrogen::Plist::writeXmlFile(*dict, (appPath / "Info.plist").string());
        writeText(appPath / "MyAppBin", "dummy_executable_content");
        const AppInfo info = readBundle(appPath.string());
        check("non-macho-reports-probe-error",
              info.problem == "not a Mach-O file" && !info.runnable && !info.encrypted,
              "problem='" + info.problem + "'");
    }

    {
        const fs::path appPath = root / "Arm64.app";
        fs::create_directories(appPath);
        auto dict = Value::makeDict();
        dict->set("CFBundleExecutable", Value::makeString("MyAppBin"));
        Hydrogen::Plist::writeXmlFile(*dict, (appPath / "Info.plist").string());
        writeBytes(appPath / "MyAppBin", thinMachO(0xFEEDFACF, 0x0100000C, 0));
        const AppInfo info = readBundle(appPath.string());
        check("arm64-is-rejected",
              info.problem == "This app is 64-bit only. HydrogenVM runs 32-bit (armv6/armv7) apps." && !info.runnable,
              "problem='" + info.problem + "'");
    }

    {
        const fs::path appPath = root / "NotArm.app";
        fs::create_directories(appPath);
        auto dict = Value::makeDict();
        dict->set("CFBundleExecutable", Value::makeString("MyAppBin"));
        Hydrogen::Plist::writeXmlFile(*dict, (appPath / "Info.plist").string());
        writeBytes(appPath / "MyAppBin", thinMachO(0xFEEDFACE, 7, 3)); // CPU_TYPE_X86
        const AppInfo info = readBundle(appPath.string());
        check("non-arm-is-rejected",
              info.problem == "Mach-O: not a 32-bit ARM binary" && !info.runnable,
              "problem='" + info.problem + "'");
    }

    {
        const fs::path appPath = root / "Encrypted.app";
        fs::create_directories(appPath);
        auto dict = Value::makeDict();
        dict->set("CFBundleExecutable", Value::makeString("MyAppBin"));
        Hydrogen::Plist::writeXmlFile(*dict, (appPath / "Info.plist").string());
        writeBytes(appPath / "MyAppBin", encryptedArmv7());
        const AppInfo info = readBundle(appPath.string());
        check("encrypted-is-not-runnable",
              info.encrypted && !info.runnable && info.cpuSubtype == 9 &&
                  info.problem == "This app is encrypted (App Store FairPlay DRM). HydrogenVM needs a decrypted copy of the .ipa.",
              "problem='" + info.problem + "' encrypted=" + std::to_string(info.encrypted) +
                  " subtype=" + std::to_string(info.cpuSubtype));
    }

    {
        const fs::path appPath = root / "IntegerFamily.app";
        fs::create_directories(appPath);
        auto dict = Value::makeDict();
        dict->set("UIDeviceFamily", Value::makeInt(2));
        Hydrogen::Plist::writeXmlFile(*dict, (appPath / "Info.plist").string());
        const AppInfo info = readBundle(appPath.string());
        check("integer-device-family", info.deviceFamily.size() == 1 && info.deviceFamily[0] == 2,
              "family size=" + std::to_string(info.deviceFamily.size()));
    }

    {
        const fs::path appPath = root / "Icons.app";
        fs::create_directories(appPath);
        auto primaryIcons = Value::makeArray();
        primaryIcons->array.push_back(Value::makeString("AppIcon29x29"));
        auto primaryIconDict = Value::makeDict();
        primaryIconDict->set("CFBundleIconFiles", primaryIcons);
        auto iconsDict = Value::makeDict();
        iconsDict->set("CFBundlePrimaryIcon", primaryIconDict);
        auto topIcons = Value::makeArray();
        topIcons->array.push_back(Value::makeString("TopIcon.png"));
        auto dict = Value::makeDict();
        dict->set("CFBundleIcons", iconsDict);
        dict->set("CFBundleIconFiles", topIcons);
        dict->set("CFBundleIconFile", Value::makeString("OldIcon.png"));
        Hydrogen::Plist::writeXmlFile(*dict, (appPath / "Info.plist").string());

        writeText(appPath / "AppIcon29x29.png", "png");
        writeText(appPath / "AppIcon29x29@2x.png", "png");
        writeText(appPath / "TopIcon.png", "png");
        writeText(appPath / "TopIcon@2x.png", "png");
        writeText(appPath / "OldIcon.png", "png");
        writeText(appPath / "OldIcon@2x.png", "png");
        writeText(appPath / "Icon-72.png", "png");

        const AppInfo info = readBundle(appPath.string());
        const std::vector<std::string> expected = {
            (appPath / "AppIcon29x29@2x.png").string(),
            (appPath / "AppIcon29x29.png").string(),
            (appPath / "TopIcon@2x.png").string(),
            (appPath / "TopIcon.png").string(),
            (appPath / "OldIcon@2x.png").string(),
            (appPath / "OldIcon.png").string(),
            (appPath / "Icon-72.png").string(),
        };
        check("icon-order-and-existing-files-only", info.iconPaths == expected,
              "got [" + joined(info.iconPaths) + "]");
    }

    std::printf("AppBundle: %d passed, %d failed\n", gPassed, gFailed);
    return gFailed ? 1 : 0;
}
