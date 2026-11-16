// HydrogenVM — Android .apk import: binary AndroidManifest.xml, resources.arsc lookups, and
// turning an APK into an app folder the library can list (see AppBundle.hpp).
#pragma once
#include "AppBundle.hpp"
#include <cstdint>
#include <string>
#include <vector>

namespace Hydrogen::Loader {

struct ApkManifest {
    std::string package;
    std::string versionName;
    std::string label;          // literal text, or "" when it's a resource (see labelRes)
    uint32_t labelRes = 0;      // @string/... id of the application label
    uint32_t iconRes = 0;       // @drawable/... id of the application icon
    std::string screenOrientation;   // of the first activity: "landscape", "portrait", "sensorLandscape", ... or ""
};

// Parses a compiled (binary XML) AndroidManifest.xml. Missing fields stay empty.
ApkManifest parseManifest(const std::vector<uint8_t>& axml);

// The string values of resource `id` in resources.arsc, one per configuration, with each
// configuration's screen density (0 = default) and whether it names a language.
struct ResValue { std::string value; uint16_t density = 0; bool localized = false; };
std::vector<ResValue> resolveStrings(const std::vector<uint8_t>& arsc, uint32_t id);

// Extracts an .apk into `appsRoot/<package>/<label>.app`: the native library (lib/), the
// assets (assets/), AndroidManifest.xml, the icon, and a generated Info.plist that marks the
// folder as an Android app (key HydrogenVMAndroidLibrary: the library, relative to the folder).
// Data beside an earlier copy (Documents/, Library/, tmp/) is kept. Throws on bad archives.
AppInfo importApk(const std::string& apkPath, const std::string& appsRoot);

} // namespace Hydrogen::Loader
