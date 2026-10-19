#include "AppBundle.hpp"
#include "ElfLoader.hpp"
#include "MachO.hpp"
#include "Zip.hpp"
#include <filesystem>
#include <set>

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
    fs::remove_all(container, ec);
    fs::rename(staging, container);
    return readBundle((container / appName).string());
}

} // namespace Hydrogen::Loader
