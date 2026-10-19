// HydrogenVM — .ipa import and .app bundle inspection.
#pragma once
#include "Plist.hpp"
#include <string>
#include <vector>

namespace Hydrogen::Loader {

struct AppInfo {
    std::string bundlePath;        // …/Foo.app
    std::string executablePath;    // …/Foo.app/Foo
    std::string bundleId;
    std::string name;              // CFBundleName / executable
    std::string displayName;       // CFBundleDisplayName, falls back to name
    std::string version;
    std::string minimumOS;
    std::vector<std::string> iconPaths;   // existing icon files, best first (may be CgBI PNGs)
    std::vector<int> deviceFamily;        // UIDeviceFamily: 1 iPhone, 2 iPad
    uint32_t cpuSubtype = 0;
    bool encrypted = false;
    bool runnable = false;         // 32-bit ARM, not encrypted
    std::string problem;           // why it can't run, for the UI
    Plist::ValuePtr infoPlist;
};

// Read Info.plist and the executable header of an extracted bundle.
AppInfo readBundle(const std::string& appPath);

// Extract Payload/<X>.app from an .ipa into `appsRoot/<bundle id>/<X>.app`
// (replacing any previous copy) and return its info. Throws on bad archives.
AppInfo importIpa(const std::string& ipaPath, const std::string& appsRoot);

} // namespace Hydrogen::Loader
