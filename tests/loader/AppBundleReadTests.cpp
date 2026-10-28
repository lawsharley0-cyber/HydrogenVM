#include "loader/AppBundle.hpp"
#include "loader/Plist.hpp"
#include <filesystem>
#include <fstream>
#include <cstdio>
#include <string>
#include <vector>

namespace fs = std::filesystem;
using Hydrogen::Loader::AppInfo;
using Hydrogen::Loader::readBundle;
using Hydrogen::Plist::Value;

// Helper to write a simple executable file that will fail probeMachO or just pass enough to hit the error.
void writeDummyFile(const fs::path& path, const std::string& content = "dummy") {
    std::ofstream out(path, std::ios::binary);
    out.write(content.data(), content.size());
}

int main(int argc, char** argv) {
    (void)argc;
    (void)argv;
    const fs::path root = "build/appbundle-test-fixtures";
    fs::remove_all(root);
    fs::create_directories(root);

    int passed = 0, failed = 0;

    // helper macro to test things
    auto check = [&](const std::string& name, bool ok) {
        std::printf("%s %s\n", ok ? "PASS" : "FAIL", name.c_str());
        ok ? ++passed : ++failed;
    };

    // 1. Missing Info.plist
    {
        const fs::path appPath = root / "MissingPlist.app";
        fs::create_directories(appPath);
        AppInfo info = readBundle(appPath.string());
        check("missing-plist-reports-error", info.problem == "The app bundle has no Info.plist.");
    }

    // 2. Corrupt Info.plist
    {
        const fs::path appPath = root / "CorruptPlist.app";
        fs::create_directories(appPath);
        writeDummyFile(appPath / "Info.plist", "This is not a valid plist");

        AppInfo info = readBundle(appPath.string());
        check("corrupt-plist-reports-error", info.problem.find("Couldn't read Info.plist:") != std::string::npos);
    }

    // 3. Valid Info.plist but missing executable
    {
        const fs::path appPath = root / "ValidPlistMissingExe.app";
        fs::create_directories(appPath);

        auto dict = Value::makeDict();
        dict->set("CFBundleExecutable", Value::makeString("MyApp"));
        dict->set("CFBundleIdentifier", Value::makeString("com.example.myapp"));
        Hydrogen::Plist::writeXmlFile(*dict, (appPath / "Info.plist").string());

        AppInfo info = readBundle(appPath.string());
        check("missing-exe-reports-error", info.problem == "The app's executable (MyApp) is missing.");
        check("parses-bundle-id", info.bundleId == "com.example.myapp");
    }

    // 4. Test missing CFBundleExecutable falls back to app folder name
    {
        const fs::path appPath = root / "FallbackExeName.app";
        fs::create_directories(appPath);

        auto dict = Value::makeDict();
        dict->set("CFBundleIdentifier", Value::makeString("com.example.fallback"));
        Hydrogen::Plist::writeXmlFile(*dict, (appPath / "Info.plist").string());

        AppInfo info = readBundle(appPath.string());
        check("fallback-exe-name", info.problem == "The app's executable (FallbackExeName) is missing.");
        check("fallback-exe-path", info.executablePath == (appPath / "FallbackExeName").string());
        check("fallback-bundle-name", info.name == "FallbackExeName");
        check("fallback-display-name", info.displayName == "FallbackExeName");
    }

    // 5. Valid Info.plist with explicit executable that is present but not Mach-O
    {
        const fs::path appPath = root / "ValidExe.app";
        fs::create_directories(appPath);

        auto dict = Value::makeDict();
        dict->set("CFBundleExecutable", Value::makeString("MyAppBin"));
        dict->set("CFBundleName", Value::makeString("My App Name"));
        dict->set("CFBundleDisplayName", Value::makeString("My App Display Name"));
        dict->set("CFBundleShortVersionString", Value::makeString("1.2.3"));
        dict->set("MinimumOSVersion", Value::makeString("8.0"));

        auto fam = Value::makeArray();
        fam->array.push_back(Value::makeInt(1)); // iPhone
        fam->array.push_back(Value::makeString("2")); // iPad
        dict->set("UIDeviceFamily", fam);

        Hydrogen::Plist::writeXmlFile(*dict, (appPath / "Info.plist").string());

        writeDummyFile(appPath / "MyAppBin", "dummy_executable_content");

        AppInfo info = readBundle(appPath.string());

        // Since it's not a real MachO, probeMachO will fail
        check("invalid-macho-reports-error", info.problem.find("This file isn't a Mach-O binary") != std::string::npos || info.problem.find("not a Mach-O") != std::string::npos || info.problem.find("too small") != std::string::npos || info.problem != "");

        check("parses-name", info.name == "My App Name");
        check("parses-display-name", info.displayName == "My App Display Name");
        check("parses-version", info.version == "1.2.3");
        check("parses-minimum-os", info.minimumOS == "8.0");
        check("parses-device-family", info.deviceFamily.size() == 2 && info.deviceFamily[0] == 1 && info.deviceFamily[1] == 2);
    }

    // 6. Test parsing icons
    {
        const fs::path appPath = root / "Icons.app";
        fs::create_directories(appPath);

        auto dict = Value::makeDict();

        auto primaryIcons = Value::makeArray();
        primaryIcons->array.push_back(Value::makeString("AppIcon29x29"));
        primaryIcons->array.push_back(Value::makeString("AppIcon40x40"));

        auto primaryIconDict = Value::makeDict();
        primaryIconDict->set("CFBundleIconFiles", primaryIcons);

        auto iconsDict = Value::makeDict();
        iconsDict->set("CFBundlePrimaryIcon", primaryIconDict);

        dict->set("CFBundleIcons", iconsDict);
        dict->set("CFBundleIconFile", Value::makeString("OldIcon.png"));

        Hydrogen::Plist::writeXmlFile(*dict, (appPath / "Info.plist").string());

        // Create dummy files for icons to make them 'exist'
        writeDummyFile(appPath / "AppIcon29x29.png"); // The loader appends .png if no extension
        writeDummyFile(appPath / "AppIcon40x40@2x.png"); // Should check @2x
        writeDummyFile(appPath / "OldIcon.png");
        writeDummyFile(appPath / "Icon-72.png");

        AppInfo info = readBundle(appPath.string());

        check("finds-primary-icons", std::find(info.iconPaths.begin(), info.iconPaths.end(), (appPath / "AppIcon29x29.png").string()) != info.iconPaths.end());
        check("finds-primary-icons-2x", std::find(info.iconPaths.begin(), info.iconPaths.end(), (appPath / "AppIcon40x40@2x.png").string()) != info.iconPaths.end());
        check("finds-old-icon", std::find(info.iconPaths.begin(), info.iconPaths.end(), (appPath / "OldIcon.png").string()) != info.iconPaths.end());
        check("finds-default-icon-72", std::find(info.iconPaths.begin(), info.iconPaths.end(), (appPath / "Icon-72.png").string()) != info.iconPaths.end());
    }

    std::printf("AppBundle: %d passed, %d failed\n", passed, failed);
    return failed ? 1 : 0;
}
