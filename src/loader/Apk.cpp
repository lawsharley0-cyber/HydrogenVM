#include "Apk.hpp"
#include "Zip.hpp"
#include "runtime/android/dex/DexName.hpp"
#include <map>
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
    struct Element {
        std::string tag;
        std::string activity;
        bool enabled = true;
        bool main = false;
        bool launcher = false;
    };
    std::vector<Element> elements;
    bool sawActivity = false;
    for (size_t at = 8; at + 8 <= d.size();) {
        const uint32_t type = r.u16(at), size = r.u32(at + 4);
        if (size < 8 || at + size > d.size()) break;
        if (type == 0x0001) strings = stringPool(r, at);
        else if (type == 0x0102) {
            const uint32_t nameIdx = r.u32(at + 20);
            const std::string name = nameIdx < strings.size() ? strings[nameIdx] : "";
            const uint32_t attrStart = r.u16(at + 24), attrSize = r.u16(at + 26), attrCount = r.u16(at + 28);
            if (size < 36 || attrStart < 20 || attrSize < 20 ||
                uint64_t(16) + attrStart + uint64_t(attrCount) * attrSize > size) break;
            Element element;
            element.tag = name;
            for (uint32_t i = 0; i < attrCount; ++i) {
                const size_t a = at + 16 + attrStart + size_t(i) * attrSize;
                const uint32_t an = r.u32(a + 4), raw = r.u32(a + 8), dataType = r.u8(a + 15), data = r.u32(a + 16);
                const std::string attr = an < strings.size() ? strings[an] : "";
                const auto value = raw < strings.size() ? raw : dataType == 3 ? data : UINT32_MAX;
                const std::string rawStr = value < strings.size() ? strings[value] : "";
                if (attr == "enabled" && (dataType == 0x12 || rawStr == "false"))
                    element.enabled = dataType == 0x12 ? data != 0 : false;
                if ((name == "activity" && attr == "name") ||
                    (name == "activity-alias" && attr == "targetActivity"))
                    element.activity = rawStr;
                if (!elements.empty() && elements.back().tag == "intent-filter" && attr == "name") {
                    if (name == "action" && rawStr == "android.intent.action.MAIN") elements.back().main = true;
                    if (name == "category" && rawStr == "android.intent.category.LAUNCHER") elements.back().launcher = true;
                }
                if (name == "manifest" && attr == "package") m.package = rawStr;
                if (name == "manifest" && attr == "versionName") m.versionName = rawStr;
                if (name == "uses-sdk" && attr == "minSdkVersion") {
                    if (dataType >= 0x10 && dataType <= 0x1F) m.minSdkVersion = int(data);
                    else if (!rawStr.empty()) m.minSdkVersion = std::atoi(rawStr.c_str());
                }
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
            elements.push_back(std::move(element));
        } else if (type == 0x0103 && size >= 24 && !elements.empty()) {
            const uint32_t nameIdx = r.u32(at + 20);
            if (nameIdx >= strings.size() || elements.back().tag != strings[nameIdx]) break;
            const auto& ended = elements.back();
            if (m.launcherActivity.empty() && ended.tag == "intent-filter" && ended.main && ended.launcher &&
                elements.size() == 4 && elements[0].tag == "manifest" && elements[1].tag == "application") {
                const auto& candidate = elements[2];
                if (elements[1].enabled && candidate.enabled && !candidate.activity.empty()) {
                    m.launcherActivity = candidate.activity;
                }
            }
            elements.pop_back();
        }
        at += size;
    }
    if (!m.launcherActivity.empty() && !m.package.empty()) {
        if (m.launcherActivity.front() == '.') m.launcherActivity = m.package + m.launcherActivity;
        else if (m.launcherActivity.find('.') == std::string::npos)
            m.launcherActivity = m.package + "." + m.launcherActivity;
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

namespace {

std::string lowerExt(const std::string& path) {
    std::string ext = fs::path(path).extension().string();
    for (char& c : ext) c = char(std::tolower(static_cast<unsigned char>(c)));
    return ext;
}

// Parses main.<version>.<package>.obb or patch.<version>.<package>.obb -> <package>.
std::string packageFromObbName(const std::string& filename) {
    if (lowerExt(filename) != ".obb") return {};
    const std::string stem = filename.substr(0, filename.size() - 4);
    const size_t first = stem.find('.');
    if (first == std::string::npos) return {};
    const size_t second = stem.find('.', first + 1);
    if (second == std::string::npos || second + 1 >= stem.size()) return {};
    const std::string kind = stem.substr(0, first);
    if (kind != "main" && kind != "patch") return {};
    return stem.substr(second + 1);
}

bool isRelevantSplitApk(const std::string& entryName, const ZipReader& z) {
    const std::string base = fs::path(entryName).filename().string();
    std::string lower = base;
    for (char& c : lower) c = char(std::tolower(static_cast<unsigned char>(c)));
    if (lower.find("arm64") != std::string::npos || lower.find("x86") != std::string::npos ||
        lower.find("mips") != std::string::npos) {
        // Only reject if it doesn't also carry 32-bit ARM libs.
        bool hasArm32 = false;
        for (const auto& e : z.entries())
            if (e.name.rfind("lib/armeabi-v7a/", 0) == 0 || e.name.rfind("lib/armeabi/", 0) == 0) { hasArm32 = true; break; }
        if (!hasArm32) return false;
    }
    // Skip non-English language splits (config.fr.apk, split_config.de.apk, etc.).
    for (const char* pre : {"config.", "split_config."}) {
        const std::string p = pre;
        if (lower.rfind(p, 0) == 0 && lower.size() == p.size() + 6 && lower.compare(p.size() + 2, 4, ".apk") == 0) {
            const std::string lang = lower.substr(p.size(), 2);
            if (lang != "en") return false;
        }
    }
    return true;
}

AppInfo importStandaloneObb(const std::string& obbPath, const std::string& appsRoot) {
    const std::string filename = fs::path(obbPath).filename().string();
    std::string pkg = packageFromObbName(filename);
    if (pkg.empty()) throw ZipError("Expansion file name must be main.<version>.<package>.obb or patch.<version>.<package>.obb.");
    for (char& c : pkg) if (c == '/' || c == '\\' || c == ':') c = '_';

    const fs::path container = fs::path(appsRoot) / pkg;
    const fs::path obbDir = container / "Library/Caches/sdcard/Android/obb" / pkg;
    std::error_code ec;
    fs::create_directories(obbDir, ec);
    fs::copy_file(obbPath, obbDir / filename, fs::copy_options::overwrite_existing);

    if (fs::is_directory(container, ec)) {
        for (const auto& entry : fs::directory_iterator(container, ec)) {
            const std::string name = entry.path().filename().string();
            if (entry.is_directory(ec) && name.size() > 4 && name.compare(name.size() - 4, 4, ".app") == 0)
                return readBundle(entry.path().string());
        }
    }
    AppInfo info;
    info.bundleId = pkg;
    info.displayName = filename;
    info.runnable = false;
    info.problem = "Expansion file installed for " + pkg + ". Import the game's .apk to play.";
    return info;
}

} // namespace

AppInfo importApk(const std::string& apkPath, const std::string& appsRoot) {
    if (lowerExt(apkPath) == ".obb") return importStandaloneObb(apkPath, appsRoot);

    ZipReader outerZip(apkPath);
    std::vector<std::string> splitPaths;
    std::vector<std::string> outerObbs;
    std::string baseApkPath = apkPath;
    fs::path tempSplitsDir;
    struct ScopedRemove {
        fs::path* target;
        ~ScopedRemove() {
            if (target && !target->empty()) {
                std::error_code e;
                fs::remove_all(*target, e);
            }
        }
    };
    ScopedRemove cleanSplits{&tempSplitsDir};

    if (!outerZip.find("AndroidManifest.xml")) {
        // Split APK archive (.xapk, .apks, .apkm): find nested .apk and .obb entries.
        std::vector<const ZipEntry*> apkEntries;
        for (const auto& e : outerZip.entries()) {
            const std::string ext = lowerExt(e.name);
            if (ext == ".apk") apkEntries.push_back(&e);
            else if (ext == ".obb") outerObbs.push_back(e.name);
        }
        if (apkEntries.empty())
            throw ZipError("This file isn't an Android app (.apk has no AndroidManifest.xml).");

        tempSplitsDir = fs::path(appsRoot) / ("_hvm_splits_" + fs::path(apkPath).stem().string());
        std::error_code ec;
        fs::remove_all(tempSplitsDir, ec);
        fs::create_directories(tempSplitsDir, ec);

        std::string chosenBase;
        for (size_t i = 0; i < apkEntries.size(); ++i) {
            const auto* e = apkEntries[i];
            const std::string fname = fs::path(e->name).filename().string();
            const fs::path dst = tempSplitsDir / (std::to_string(i) + "_" + fname);
            const auto bytes = outerZip.read(*e);
            std::ofstream(dst, std::ios::binary).write(reinterpret_cast<const char*>(bytes.data()), std::streamsize(bytes.size()));
            try {
                ZipReader sz(dst.string());
                if (!isRelevantSplitApk(fname, sz)) continue;
                splitPaths.push_back(dst.string());
                std::string lower = fname;
                for (char& c : lower) c = char(std::tolower(static_cast<unsigned char>(c)));
                if (sz.find("AndroidManifest.xml") &&
                    (chosenBase.empty() || lower == "base.apk" || lower == "base-master.apk"))
                    chosenBase = dst.string();
            } catch (const std::exception&) {}
        }
        if (chosenBase.empty()) {
            fs::remove_all(tempSplitsDir, ec);
            throw ZipError("Split archive has no base .apk with AndroidManifest.xml.");
        }
        baseApkPath = chosenBase;
    }

    ZipReader zip(baseApkPath);
    const auto* manifestEntry = zip.find("AndroidManifest.xml");
    if (!manifestEntry) {
        std::error_code ec;
        if (!tempSplitsDir.empty()) fs::remove_all(tempSplitsDir, ec);
        throw ZipError("This file isn't an Android app (.apk has no AndroidManifest.xml).");
    }
    const auto manifestBytes = zip.read(*manifestEntry);
    ApkManifest m = parseManifest(manifestBytes);
    std::vector<uint8_t> arsc;
    if (const auto* e = zip.find("resources.arsc")) arsc = zip.read(*e);

    auto findLibInZip = [](const ZipReader& z) -> std::string {
        for (const char* abi : {"lib/armeabi-v7a/", "lib/armeabi/"}) {
            const std::string prefix = abi;
            uint64_t best = 0;
            std::string found;
            for (const auto& e : z.entries())
                if (e.name.rfind(prefix, 0) == 0 && e.name.find('/', prefix.size()) == std::string::npos &&
                    e.name.size() > 3 && e.name.compare(e.name.size() - 3, 3, ".so") == 0 && e.size > best) {
                    found = e.name;
                    best = e.size;
                }
            if (!found.empty()) return found;
        }
        return {};
    };
    std::string lib = findLibInZip(zip);
    for (const auto& sp : splitPaths) {
        if (!lib.empty()) break;
        ZipReader sz(sp);
        lib = findLibInZip(sz);
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
    fs::path staging = fs::path(appsRoot) / (pkg + ".importing");
    const fs::path app = staging / (folderName + ".app");
    std::error_code ec;
    fs::remove_all(staging, ec);
    ScopedRemove cleanStaging{&staging};
    fs::create_directories(app);
    if (!lib.empty()) zip.extract("lib/", (app / "lib").string());
    zip.extract("assets/", (app / "assets").string());
    for (const auto& sp : splitPaths) {
        if (sp == baseApkPath) continue;
        ZipReader sz(sp);
        sz.extract("lib/", (app / "lib").string());
        sz.extract("assets/", (app / "assets").string());
    }
    // Extract any .obb expansion files from the outer .xapk/.apks/.apkm or from assets/ into sdcard/Android/obb/<pkg>/.
    const fs::path obbDir = staging / "Library/Caches/sdcard/Android/obb" / pkg;
    for (const auto& obbEntry : outerObbs) {
        if (const auto* e = outerZip.find(obbEntry)) {
            fs::create_directories(obbDir, ec);
            const auto bytes = outerZip.read(*e);
            std::ofstream(obbDir / fs::path(obbEntry).filename(), std::ios::binary)
                .write(reinterpret_cast<const char*>(bytes.data()), std::streamsize(bytes.size()));
        }
    }
    for (const auto& e : fs::directory_iterator(app / "assets", ec)) {
        if (lowerExt(e.path().string()) == ".obb") {
            fs::create_directories(obbDir, ec);
            fs::copy_file(e.path(), obbDir / e.path().filename(), fs::copy_options::overwrite_existing, ec);
        }
    }

    // Unity reads its data from inside the APK (Context.getPackageCodePath): keep a copy.
    bool unity = false;
    for (const auto& e : zip.entries())
        if (e.name.size() > 12 && e.name.compare(e.name.size() - 12, 12, "/libunity.so") == 0) unity = true;
    if (unity) fs::copy_file(baseApkPath, app / "base.apk", fs::copy_options::overwrite_existing);
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
        auto addLibs = [&](const ZipReader& z) {
            for (const auto& e : z.entries())
                if (e.name.rfind(dir, 0) == 0 && e.name.find('/', dir.size()) == std::string::npos)
                    libNames.push_back(e.name.substr(dir.size()));
        };
        addLibs(zip);
        for (const auto& sp : splitPaths) if (sp != baseApkPath) addLibs(ZipReader(sp));
    }
    std::string dexText;
    constexpr uint64_t dexLimit = 256u << 20;
    uint64_t lastOrdinal = 0;
    auto retainDex = [&](const ZipReader& archive, bool base) {
        std::map<uint32_t, const ZipEntry*> ordered;
        for (const auto& e : archive.entries()) {
            const auto ordinal = Runtime::Android::Dex::dexOrdinal(e.name);
            if (!ordinal || e.isSymlink()) continue;
            if (!ordered.emplace(*ordinal, &e).second)
                throw ZipError("Duplicate DEX archive entry: " + e.name);
        }
        for (const auto& [ordinal, entry] : ordered) {
            if (entry->size >= dexLimit || entry->compressedSize >= dexLimit ||
                entry->size > dexLimit - dexText.size())
                throw ZipError("APK bytecode exceeds the DEX import limit.");
            const auto bytes = archive.read(*entry);
            if (bytes.size() != entry->size) throw ZipError("Invalid DEX entry length.");
            // Keep base names. Append each split in its own numeric class-path order.
            lastOrdinal = base ? ordinal : lastOrdinal + 1;
            if (lastOrdinal > UINT32_MAX) throw ZipError("DEX class-path index overflow.");
            const auto name = lastOrdinal == 1 ? "classes.dex" : "classes" + std::to_string(lastOrdinal) + ".dex";
            std::ofstream out(app / name, std::ios::binary);
            if (!bytes.empty()) {
                const auto* data = reinterpret_cast<const char*>(bytes.data());
                out.write(data, std::streamsize(bytes.size()));
                dexText.append(data, bytes.size());
            }
            out.close();
            if (!out) throw ZipError("Could not write imported DEX: " + name);
        }
    };
    retainDex(zip, true);
    for (const auto& sp : splitPaths)
        if (sp != baseApkPath) retainDex(ZipReader(sp), false);
    const AndroidEngine engine = detectAndroidEngine(libNames, dexText);
    dexText.clear();
    if (!tempSplitsDir.empty()) fs::remove_all(tempSplitsDir, ec);

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
          << (m.launcherActivity.empty() ? "" :
              "\t<key>HydrogenVMAndroidLauncherActivity</key><string>" + xmlEscape(m.launcherActivity) + "</string>\n")
          << "\t<key>HydrogenVMAndroidEngine</key><string>" << xmlEscape(engine.name) << "</string>\n"
          << "\t<key>HydrogenVMAndroidEngineSupport</key><string>" << xmlEscape(engine.support) << "</string>\n"
          << "\t<key>MinimumOSVersion</key><string>Android</string>\n"
          << (haveIcon ? "\t<key>CFBundleIconFile</key><string>icon.png</string>\n" : "")
          << "\t<key>UISupportedInterfaceOrientations</key><array><string>"
          << (portrait ? "UIInterfaceOrientationPortrait" : "UIInterfaceOrientationLandscapeRight") << "</string></array>\n"
          << "</dict>\n</plist>\n";
    plist.close();

    // An update keeps the game's data (merge Library/Caches/sdcard/Android/obb/ if new OBBs were extracted).
    if (fs::is_directory(container, ec)) {
        for (const auto& entry : fs::directory_iterator(container, ec)) {
            const std::string name = entry.path().filename().string();
            if (entry.is_directory(ec) && name.size() > 4 && name.compare(name.size() - 4, 4, ".app") == 0) continue;
            if (fs::exists(staging / entry.path().filename(), ec)) continue;
            fs::rename(entry.path(), staging / entry.path().filename(), ec);
        }
    }
    fs::remove_all(container, ec);
    fs::rename(staging, container);
    staging.clear();
    AppInfo info = readBundle((container / (folderName + ".app")).string());
    if (lib.empty() && !info.runnable && info.problem.empty()) {
        info.problem = "This Android app has no 32-bit ARM native code (lib/armeabi or lib/armeabi-v7a) and no runnable DEX launcher activity.";
    }
    return info;
}

} // namespace Hydrogen::Loader
