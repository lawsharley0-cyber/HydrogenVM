#include "AppBundle.hpp"
#include "Apk.hpp"
#include "ElfLoader.hpp"
#include "MachO.hpp"
#include "Zip.hpp"
#include <filesystem>
#include <fstream>
#include <set>
#include <vector>

namespace fs = std::filesystem;

namespace Hydrogen::Loader {
namespace {

void addIcon(AppInfo& info, std::set<std::string>& seen, const std::string& bundle, std::string name) {
    if (name.empty()) return;
    std::vector<std::string> candidates;
    const bool hasExt = fs::path(name).has_extension();
    for (const char* suffix : {"@2x", ""}) {
        if (hasExt) {
            fs::path p(name);
            candidates.push_back((p.parent_path() / (p.stem().string() + suffix + p.extension().string())).string());
        } else {
            candidates.push_back(name + suffix + ".png");
        }
    }
    if (hasExt) candidates.push_back(name);
    for (const auto& c : candidates) {
        const std::string full = (fs::path(bundle) / c).string();
        if (!seen.count(full) && fs::is_regular_file(full)) { seen.insert(full); info.iconPaths.push_back(full); }
    }
}

void collectIcons(AppInfo& info, const Plist::Value& plist) {
    std::set<std::string> seen;
    auto fromArray = [&](const Plist::Value* a) {
        if (!a || a->type != Plist::Value::Type::Array) return;
        for (const auto& v : a->array) if (v->type == Plist::Value::Type::String) addIcon(info, seen, info.bundlePath, v->s);
    };
    if (const auto* icons = plist.get("CFBundleIcons")) {
        if (const auto* primary = icons->get("CFBundlePrimaryIcon")) fromArray(primary->get("CFBundleIconFiles"));
    }
    fromArray(plist.get("CFBundleIconFiles"));
    addIcon(info, seen, info.bundlePath, plist.getString("CFBundleIconFile"));
    for (const char* n : {"Icon@2x.png", "Icon.png", "icon@2x.png", "icon.png", "Icon-72.png"})
        addIcon(info, seen, info.bundlePath, n);
}

} // namespace

AppInfo readBundle(const std::string& appPath) {
    AppInfo info;
    info.bundlePath = appPath;
    const fs::path plistPath = fs::path(appPath) / "Info.plist";
    if (!fs::exists(plistPath)) { info.problem = "The app bundle has no Info.plist."; return info; }
    try {
        info.infoPlist = Plist::parseFile(plistPath.string());
    } catch (const std::exception& e) {
        info.problem = std::string("Couldn't read Info.plist: ") + e.what();
        return info;
    }
    const Plist::Value& p = *info.infoPlist;
    std::string exe = p.getString("CFBundleExecutable");
    if (exe.empty()) exe = fs::path(appPath).stem().string();
    info.executablePath = (fs::path(appPath) / exe).string();
    info.bundleId = p.getString("CFBundleIdentifier", "unknown." + exe);
    info.name = p.getString("CFBundleName", exe);
    info.displayName = p.getString("CFBundleDisplayName", info.name);
    if (info.displayName.empty()) info.displayName = exe;
    info.version = p.getString("CFBundleShortVersionString", p.getString("CFBundleVersion"));
    info.minimumOS = p.getString("MinimumOSVersion");
    if (const auto* fam = p.get("UIDeviceFamily")) {
        if (fam->type == Plist::Value::Type::Array)
            for (const auto& v : fam->array) info.deviceFamily.push_back(int(v->type == Plist::Value::Type::String ? std::atoi(v->s.c_str()) : v->i));
        else if (fam->type == Plist::Value::Type::Integer) info.deviceFamily.push_back(int(fam->i));
    }
    collectIcons(info, p);

    if (const std::string lib = p.getString("HydrogenVMAndroidLibrary"); !lib.empty()) {
        // An imported Android app (Apk.cpp): its native library must be 32-bit ARM.
        info.androidLibrary = (fs::path(appPath) / lib).string();
        info.engine = p.getString("HydrogenVMAndroidEngine");
        info.engineSupport = p.getString("HydrogenVMAndroidEngineSupport");
        if (info.engine.empty()) {
            // Imported before engines were recorded: the library names alone.
            std::vector<std::string> names;
            std::error_code lec;
            for (const auto& e : fs::directory_iterator(fs::path(info.androidLibrary).parent_path(), lec))
                names.push_back(e.path().filename().string());
            const AndroidEngine guess = detectAndroidEngine(names, "");
            if (guess.name != "Java + native") { info.engine = guess.name; info.engineSupport = guess.support; }
        }
        std::ifstream in(info.androidLibrary, std::ios::binary);
        unsigned char h[20] = {};
        in.read(reinterpret_cast<char*>(h), sizeof h);
        if (!in) { info.problem = "The Android app's native library (" + lib + ") is missing."; return info; }
        if (h[0] != 0x7F || h[1] != 'E' || h[2] != 'L' || h[3] != 'F' || h[4] != 1 || h[18] != 40) {
            info.problem = "The Android app's native library isn't 32-bit ARM.";
            return info;
        }
        info.runnable = true;
        return info;
    }

    if (!fs::is_regular_file(info.executablePath)) { info.problem = "The app's executable (" + exe + ") is missing."; return info; }
    const auto bin = readFile(info.executablePath);
    const auto probe = probeMachO(bin);
    if (!probe.isMachO) { info.problem = probe.error; return info; }
    info.cpuSubtype = probe.cpuSubtype;
    info.encrypted = probe.encrypted;
    if (probe.encrypted) {
        info.problem = "This app is encrypted (App Store FairPlay DRM). HydrogenVM needs a decrypted copy of the .ipa.";
        return info;
    }
    info.runnable = true;
    return info;
}

AppInfo importIpa(const std::string& ipaPath, const std::string& appsRoot) {
    ZipReader zip(ipaPath);
    std::string appDir;   // "Payload/Foo.app/"
    for (const auto& e : zip.entries()) {
        const std::string& n = e.name;
        if (n.compare(0, 8, "Payload/") != 0) continue;
        const size_t dot = n.find(".app/", 8);
        if (dot == std::string::npos) continue;
        const std::string candidate = n.substr(0, dot + 5);
        if (candidate.find('/', 8) != dot + 4) continue;    // must be directly under Payload/
        appDir = candidate;
        break;
    }
    if (appDir.empty()) throw ZipError("This file isn't an iOS app (.ipa has no Payload/*.app).");
    const std::string appName = appDir.substr(8, appDir.size() - 9);   // "Foo.app"

    // Read Info.plist from the archive first so we know where to put it.
    std::string bundleId = fs::path(appName).stem().string();
    if (const auto* e = zip.find(appDir + "Info.plist")) {
        try {
            auto v = Plist::parse(zip.read(*e));
            bundleId = v->getString("CFBundleIdentifier", bundleId);
        } catch (...) {}
    }
    for (char& c : bundleId) if (c == '/' || c == '\\' || c == ':') c = '_';
    const fs::path container = fs::path(appsRoot) / bundleId;
    const fs::path staging = fs::path(appsRoot) / (bundleId + ".importing");
    std::error_code ec;
    fs::remove_all(staging, ec);
    fs::create_directories(staging / appName);
    zip.extract(appDir, (staging / appName).string());
    // Bundled iTunesMetadata / artwork are outside Payload; ignore them.
    // Re-importing an app (an update) replaces the bundle but must keep the app's data
    // (Documents/, Library/, tmp/) so saves and preferences survive the update.
    if (fs::is_directory(container, ec)) {
        std::vector<fs::path> keep;
        for (const auto& entry : fs::directory_iterator(container, ec)) {
            const std::string name = entry.path().filename().string();
            if (entry.is_directory(ec) && name.size() > 4 && name.compare(name.size() - 4, 4, ".app") == 0) continue;
            keep.push_back(entry.path());
        }
        for (const auto& p : keep) fs::rename(p, staging / p.filename(), ec);
    }
    // An IPA made from an installed app with its data ("with data" / cache-inclusive IPAs, as
    // IPA Installer and backup tools write them) carries the app's sandbox next to Payload/ as
    // Container/ (Container/Library/Caches/..., Container/Documents/...). It goes into the app's
    // home the way it was on the device. Files the app already has here (saves kept from an
    // earlier install) are not overwritten.
    const size_t restored = zip.extract("Container/", staging.string(), /*overwrite=*/false);
    fs::remove_all(container, ec);
    fs::rename(staging, container);
    AppInfo info = readBundle((container / appName).string());
    info.restoredDataFiles = restored;
    return info;
}

} // namespace Hydrogen::Loader
