#include "Apk.hpp"
#include "Zip.hpp"
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <cctype>

namespace fs = std::filesystem;

namespace Hydrogen::Loader {
namespace {

struct Reader {
    const std::vector<uint8_t>& d;
    uint32_t u8(size_t o) const { return o < d.size() ? d[o] : 0; }
    uint32_t u16(size_t o) const { return o + 2 <= d.size() ? uint32_t(d[o] | d[o + 1] << 8) : 0; }
    uint32_t u32(size_t o) const {
        return o + 4 <= d.size() ? uint32_t(d[o] | d[o + 1] << 8 | d[o + 2] << 16 | uint32_t(d[o + 3]) << 24) : 0;
    }
};

// A ResStringPool chunk at `at`.
std::vector<std::string> stringPool(const Reader& r, size_t at) {
    std::vector<std::string> out;
    const uint32_t count = r.u32(at + 8), flags = r.u32(at + 16), start = r.u32(at + 20);
    const bool utf8 = flags & 0x100;
    for (uint32_t i = 0; i < count && i < 200000; ++i) {
        size_t o = at + start + r.u32(at + 28 + size_t(i) * 4);
        std::string s;
        if (utf8) {
            o += (r.u8(o) & 0x80) ? 2 : 1;   // length in characters
            size_t n = r.u8(o);
            if (n & 0x80) { n = ((n & 0x7F) << 8) | r.u8(o + 1); ++o; }
            ++o;
            if (o + n <= r.d.size()) s.assign(reinterpret_cast<const char*>(&r.d[o]), n);
        } else {
            size_t n = r.u16(o);
            o += 2;
            if (n & 0x8000) { n = ((n & 0x7FFF) << 16) | r.u16(o); o += 2; }
            for (size_t k = 0; k < n && o + k * 2 + 1 < r.d.size(); ++k) {
                const uint32_t c = r.u16(o + k * 2);
                if (c < 0x80) s += char(c);
                else if (c < 0x800) { s += char(0xC0 | (c >> 6)); s += char(0x80 | (c & 63)); }
                else { s += char(0xE0 | (c >> 12)); s += char(0x80 | ((c >> 6) & 63)); s += char(0x80 | (c & 63)); }
            }
        }
        out.push_back(std::move(s));
    }
    return out;
}

std::string xmlEscape(const std::string& s) {
    std::string o;
    for (char c : s) {
        if (c == '&') o += "&amp;";
        else if (c == '<') o += "&lt;";
        else if (c == '>') o += "&gt;";
        else o += c;
    }
    return o;
}

} // namespace

ApkManifest parseManifest(const std::vector<uint8_t>& d) {
    ApkManifest m;
    Reader r{d};
    std::vector<std::string> strings;
    bool inApplication = false, sawActivity = false;
    for (size_t at = 8; at + 8 <= d.size();) {
        const uint32_t type = r.u16(at), size = r.u32(at + 4);
        if (size < 8 || at + size > d.size()) break;
        if (type == 0x0001) strings = stringPool(r, at);
        else if (type == 0x0102) {
            const uint32_t nameIdx = r.u32(at + 20);
            const std::string name = nameIdx < strings.size() ? strings[nameIdx] : "";
            const uint32_t attrStart = r.u16(at + 24), attrSize = r.u16(at + 26), attrCount = r.u16(at + 28);
            if (name == "application") inApplication = true;
            for (uint32_t i = 0; i < attrCount; ++i) {
                const size_t a = at + 16 + attrStart + size_t(i) * attrSize;
                const uint32_t an = r.u32(a + 4), raw = r.u32(a + 8), dataType = r.u8(a + 15), data = r.u32(a + 16);
                const std::string attr = an < strings.size() ? strings[an] : "";
                const std::string rawStr = raw < strings.size() ? strings[raw] : "";
                if (name == "manifest" && attr == "package") m.package = rawStr;
                if (name == "manifest" && attr == "versionName") m.versionName = rawStr;
                if (name == "application" && attr == "label") { if (dataType == 1) m.labelRes = data; else m.label = rawStr; }
                if (name == "application" && attr == "icon" && dataType == 1) m.iconRes = data;
                if (name == "activity" && !sawActivity && attr == "screenOrientation") {
                    // android:screenOrientation is an enum: 0 landscape, 1 portrait, 6 sensorLandscape, ...
                    static const char* kNames[] = {"landscape", "portrait", "user", "behind", "sensor", "nosensor",
                                                   "sensorLandscape", "sensorPortrait", "reverseLandscape", "reversePortrait"};
                    m.screenOrientation = dataType == 3 ? rawStr : data < 10 ? kNames[data] : "";
                }
            }
            if (name == "activity") sawActivity = true;
            (void)inApplication;
        }
        at += size;
    }
    return m;
}

std::vector<ResValue> resolveStrings(const std::vector<uint8_t>& d, uint32_t id) {
    std::vector<ResValue> out;
    Reader r{d};
    if (r.u16(0) != 0x0002) return out;
    std::vector<std::string> global;
    const uint32_t pkgId = id >> 24, typeId = (id >> 16) & 0xFF, entry = id & 0xFFFF;
    for (size_t at = r.u16(2); at + 8 <= d.size();) {
        const uint32_t type = r.u16(at), size = r.u32(at + 4);
        if (size < 8 || at + size > d.size()) break;
        if (type == 0x0001) global = stringPool(r, at);
        else if (type == 0x0200 && r.u32(at + 8) == pkgId) {
            for (size_t c = at + r.u16(at + 2); c + 8 <= at + size;) {
                const uint32_t ct = r.u16(c), cs = r.u32(c + 4);
                if (cs < 8 || c + cs > at + size) break;
                if (ct == 0x0201 && r.u8(c + 8) == typeId) {
                    const uint32_t entryCount = r.u32(c + 12), entriesStart = r.u32(c + 16);
                    const size_t config = c + 20;
                    if (entry < entryCount) {
                        const uint32_t off = r.u32(c + r.u16(c + 2) + size_t(entry) * 4);
                        if (off != 0xFFFFFFFFu) {
                            const size_t e = c + entriesStart + off;
                            const uint32_t flags = r.u16(e + 2);
                            if (!(flags & 1)) {   // a simple value
                                const size_t v = e + r.u16(e);
                                if (r.u8(v + 3) == 3 && r.u32(v + 4) < global.size())
                                    out.push_back({global[r.u32(v + 4)], uint16_t(r.u16(config + 14)), r.u16(config + 8) != 0});
                            }
                        }
                    }
                }
                c += cs;
            }
        }
        at += size;
    }
    return out;
}

AndroidEngine detectAndroidEngine(const std::vector<std::string>& libs, const std::string& dex) {
    auto has = [&](const char* n) { return std::find(libs.begin(), libs.end(), n) != libs.end(); };
    auto any = [&](auto pred) { return std::any_of(libs.begin(), libs.end(), pred); };
    auto in = [&](const char* s) { return dex.find(s) != std::string::npos; };
    if (has("libil2cpp.so")) return {"Unity (IL2CPP)", "none"};
    if (has("libunity.so") && has("libmono.so")) return {"Unity (Mono)", "driver"};
    if (has("libunity.so")) return {"Unity", "driver"};
    if (has("libUE4.so")) return {"Unreal Engine 4", "none"};
    if (has("libUE3.so") || any([](const std::string& n) { return n.rfind("libUnrealEngine3", 0) == 0; })) return {"Unreal Engine 3", "none"};
    if (in("Lcom/ea/blast/")) return {"EA Blast", "driver"};
    if (has("libuaf.so")) return {"Ubisoft UAF", "driver"};
    if (in("Lorg/cocos2dx/") || any([](const std::string& n) {
            std::string l = n; for (char& c : l) c = char(std::tolower(static_cast<unsigned char>(c)));
            return l.find("cocos") != std::string::npos; }))
        return {"Cocos2d-x", "none"};
    if (has("libyoyo.so")) return {"GameMaker", "none"};
    if (has("libgdx.so") || in("Lcom/badlogic/gdx/")) return {"libGDX", "none"};
    if (has("libCore.so") && in("Lcom/adobe/air/")) return {"Adobe AIR", "none"};
    if (has("libcorona.so")) return {"Corona", "none"};
    if (any([](const std::string& n) { return n.rfind("libs3e", 0) == 0; })) return {"Marmalade", "none"};
    if (in("Landroid/app/NativeActivity;")) return {"NativeActivity", "planned"};
    if (libs.empty()) return {"Java only", "none"};
    return {"Java + native", "none"};
}

AppInfo importApk(const std::string& apkPath, const std::string& appsRoot) {
    ZipReader zip(apkPath);
    const auto* manifestEntry = zip.find("AndroidManifest.xml");
    if (!manifestEntry) throw ZipError("This file isn't an Android app (.apk has no AndroidManifest.xml).");
    const auto manifestBytes = zip.read(*manifestEntry);
    ApkManifest m = parseManifest(manifestBytes);
    std::vector<uint8_t> arsc;
    if (const auto* e = zip.find("resources.arsc")) arsc = zip.read(*e);

    std::string lib;
    for (const char* abi : {"lib/armeabi-v7a/", "lib/armeabi/"}) {
        const std::string prefix = abi;
        // Several libraries: the largest is the game (the others are helpers it loads).
        uint64_t best = 0;
        for (const auto& e : zip.entries())
            if (e.name.rfind(prefix, 0) == 0 && e.name.find('/', prefix.size()) == std::string::npos &&
                e.name.size() > 3 && e.name.compare(e.name.size() - 3, 3, ".so") == 0 && e.size > best) { lib = e.name; best = e.size; }
        if (!lib.empty()) break;
    }

    std::string label = m.label;
    if (label.empty() && m.labelRes) {
        const auto values = resolveStrings(arsc, m.labelRes);
        for (const auto& v : values) if (!v.localized) { label = v.value; break; }
        if (label.empty() && !values.empty()) label = values.front().value;
    }
    if (m.package.empty()) m.package = fs::path(apkPath).stem().string();
    if (label.empty()) label = m.package;
    std::string folderName = label;
    for (char& c : folderName) if (c == '/' || c == '\\' || c == ':' || c == '\0') c = '_';
    std::string pkg = m.package;
    for (char& c : pkg) if (c == '/' || c == '\\' || c == ':') c = '_';

    const fs::path container = fs::path(appsRoot) / pkg;
    const fs::path staging = fs::path(appsRoot) / (pkg + ".importing");
    const fs::path app = staging / (folderName + ".app");
    std::error_code ec;
    fs::remove_all(staging, ec);
    fs::create_directories(app);
    if (!lib.empty()) zip.extract("lib/", (app / "lib").string());
    zip.extract("assets/", (app / "assets").string());
    // Unity reads its data from inside the APK (Context.getPackageCodePath): keep a copy.
    bool unity = false;
    for (const auto& e : zip.entries())
        if (e.name.size() > 12 && e.name.compare(e.name.size() - 12, 12, "/libunity.so") == 0) unity = true;
    if (unity) fs::copy_file(apkPath, app / "base.apk", fs::copy_options::overwrite_existing);
    std::ofstream(app / "AndroidManifest.xml", std::ios::binary)
        .write(reinterpret_cast<const char*>(manifestBytes.data()), std::streamsize(manifestBytes.size()));

    // The icon: the densest PNG the icon resource names.
    bool haveIcon = false;
    if (m.iconRes) {
        auto values = resolveStrings(arsc, m.iconRes);
        std::sort(values.begin(), values.end(), [](const ResValue& a, const ResValue& b) {
            const auto rank = [](uint16_t d) { return d == 0xFFFF || d == 0xFFFE ? 0 : int(d); };   // any / none densities last
            return rank(a.density) > rank(b.density);
        });
        for (const auto& v : values) {
            if (v.value.size() < 4 || v.value.compare(v.value.size() - 4, 4, ".png") != 0) continue;
            if (const auto* e = zip.find(v.value)) {
                const auto png = zip.read(*e);
                std::ofstream(app / "icon.png", std::ios::binary).write(reinterpret_cast<const char*>(png.data()), std::streamsize(png.size()));
                haveIcon = true;
                break;
            }
        }
    }

    // Which engine: native library names (the chosen ABI) and the class names the DEX holds.
    std::vector<std::string> libNames;
    if (!lib.empty()) {
        const std::string dir = lib.substr(0, lib.rfind('/') + 1);
        for (const auto& e : zip.entries())
            if (e.name.rfind(dir, 0) == 0 && e.name.find('/', dir.size()) == std::string::npos) libNames.push_back(e.name.substr(dir.size()));
    }
    std::string dexText;
    for (const auto& e : zip.entries())
        if (e.name.rfind("classes", 0) == 0 && e.name.size() > 4 && e.name.compare(e.name.size() - 4, 4, ".dex") == 0 && e.size < (256u << 20)) {
            const auto d = zip.read(e);
            dexText.append(reinterpret_cast<const char*>(d.data()), d.size());
        }
    const AndroidEngine engine = detectAndroidEngine(libNames, dexText);
    dexText.clear();

    const bool portrait = m.screenOrientation.find("ortrait") != std::string::npos;
    std::ofstream plist(app / "Info.plist");
    plist << "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
             "<!DOCTYPE plist PUBLIC \"-//Apple//DTD PLIST 1.0//EN\" \"http://www.apple.com/DTDs/PropertyList-1.0.dtd\">\n"
             "<plist version=\"1.0\">\n<dict>\n"
          << "\t<key>CFBundleIdentifier</key><string>" << xmlEscape(m.package) << "</string>\n"
          << "\t<key>CFBundleName</key><string>" << xmlEscape(label) << "</string>\n"
          << "\t<key>CFBundleDisplayName</key><string>" << xmlEscape(label) << "</string>\n"
          << "\t<key>CFBundleShortVersionString</key><string>" << xmlEscape(m.versionName) << "</string>\n"
          << "\t<key>CFBundleExecutable</key><string>" << xmlEscape(lib) << "</string>\n"
          << "\t<key>HydrogenVMAndroidLibrary</key><string>" << xmlEscape(lib) << "</string>\n"
          << "\t<key>HydrogenVMAndroidEngine</key><string>" << xmlEscape(engine.name) << "</string>\n"
          << "\t<key>HydrogenVMAndroidEngineSupport</key><string>" << xmlEscape(engine.support) << "</string>\n"
          << "\t<key>MinimumOSVersion</key><string>Android</string>\n"
          << (haveIcon ? "\t<key>CFBundleIconFile</key><string>icon.png</string>\n" : "")
          << "\t<key>UISupportedInterfaceOrientations</key><array><string>"
          << (portrait ? "UIInterfaceOrientationPortrait" : "UIInterfaceOrientationLandscapeRight") << "</string></array>\n"
          << "</dict>\n</plist>\n";
    plist.close();

    // An update keeps the game's data.
    if (fs::is_directory(container, ec)) {
        for (const auto& entry : fs::directory_iterator(container, ec)) {
            const std::string name = entry.path().filename().string();
            if (entry.is_directory(ec) && name.size() > 4 && name.compare(name.size() - 4, 4, ".app") == 0) continue;
            fs::rename(entry.path(), staging / entry.path().filename(), ec);
        }
    }
    fs::remove_all(container, ec);
    fs::rename(staging, container);
    AppInfo info = readBundle((container / (folderName + ".app")).string());
    if (lib.empty() && info.problem.empty()) {
        info.runnable = false;
        info.problem = "This Android app has no 32-bit ARM native code (lib/armeabi or lib/armeabi-v7a); Java-only apps can't run.";
    }
    return info;
}

} // namespace Hydrogen::Loader
