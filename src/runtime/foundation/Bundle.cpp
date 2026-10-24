// HydrogenVM — NSBundle, NSURL, NSFileManager, NSFileHandle, search paths,
// and the CFBundle / CFURL functions over the same objects.
//
// Guest code sees real host paths (the app bundle and its sandbox home live
// in the HydrogenVM container), exactly as an iOS app sees its own sandbox.
#include "Foundation.hpp"
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <sys/stat.h>
#include <unistd.h>

namespace fs = std::filesystem;

namespace Hydrogen::Runtime::Foundation {

namespace {

// ------------------------------------------------------------- helpers ---

std::string percentEncode(const std::string& s, const char* keep) {
    static const char* hex = "0123456789ABCDEF";
    std::string out;
    for (unsigned char c : s) {
        if (std::isalnum(c) || std::strchr(keep, c)) out += char(c);
        else { out += '%'; out += hex[c >> 4]; out += hex[c & 15]; }
    }
    return out;
}
std::string percentDecode(const std::string& s) {
    std::string out;
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '%' && i + 2 < s.size() && std::isxdigit(uint8_t(s[i + 1])) && std::isxdigit(uint8_t(s[i + 2]))) {
            out += char(std::stoi(s.substr(i + 1, 2), nullptr, 16));
            i += 2;
        } else out += s[i];
    }
    return out;
}
std::string standardize(const std::string& p) {
    if (p.empty()) return p;
    std::string s = fs::path(p).lexically_normal().string();
    while (s.size() > 1 && s.back() == '/') s.pop_back();
    return s;
}

struct ErrorOut {
    static void set(Ctx& x, GuestPtr out, const std::string& domain, int32_t code, const std::string& desc, const std::string& path = {}) {
        if (!out) return;
        std::vector<std::pair<id, id>> info;
        info.emplace_back(makeString(x, "NSLocalizedDescription"), makeString(x, desc));
        if (!path.empty()) info.emplace_back(makeString(x, "NSFilePath"), makeString(x, path));
        const id err = id(rt(x).send(x.cpu, rt(x).getClass("NSError"), "errorWithDomain:code:userInfo:",
                                     {makeString(x, domain), uint32_t(code), makeDict(x, info)}));
        x.mem.write<uint32_t>(out, err);
    }
    static void posix(Ctx& x, GuestPtr out, int err, const std::string& path) {
        // Map to the closest Cocoa error code, like Foundation does.
        int32_t code = 512;   // NSFileWriteUnknownError
        if (err == ENOENT) code = 4;        // NSFileNoSuchFileError
        else if (err == EEXIST) code = 516; // NSFileWriteFileExistsError
        else if (err == EACCES || err == EPERM) code = 513;   // NSFileWriteNoPermissionError
        else if (err == ENOSPC) code = 640; // NSFileWriteOutOfSpaceError
        set(x, out, "NSCocoaErrorDomain", code, std::strerror(err), path);
    }
};

// ------------------------------------------------------------------ NSURL ---

struct UrlData : ObjC::HostObject {
    std::string str;      // as given / built (relative part when base != 0)
    id base = 0;
    bool isFile = false;
};

struct Bundles {
    explicit Bundles(Process&) {}
    Class nsBundle = 0, nsURL = 0, fileManager = 0, fileHandle = 0, dirEnum = 0;
    id mainBundle = 0, defaultManager = 0;
    std::mutex mu;
    std::unordered_map<std::string, id> bundlesByPath;
    std::unordered_map<std::string, std::shared_ptr<Plist::Value>> stringTables;   // path -> parsed table
    std::string language;                                                          // chosen .lproj
};
Bundles& B(Process& p) { return p.service<Bundles>(); }
Bundles& B(Ctx& x) { return B(x.proc); }

UrlData* ud(Ctx& x, id u) { return u ? rt(x).hostIf<UrlData>(u) : nullptr; }

struct UrlParts { std::string scheme, user, password, host, port, path, query, fragment, params; bool hasAuthority = false; };
UrlParts parseUrl(const std::string& s) {
    UrlParts p;
    std::string rest = s;
    const size_t hash = rest.find('#');
    if (hash != std::string::npos) { p.fragment = rest.substr(hash + 1); rest.resize(hash); }
    const size_t colon = rest.find(':');
    const size_t firstSlash = rest.find('/');
    if (colon != std::string::npos && colon > 0 && (firstSlash == std::string::npos || colon < firstSlash)) {
        bool ok = std::isalpha(uint8_t(rest[0]));
        for (size_t i = 0; i < colon && ok; ++i) ok = std::isalnum(uint8_t(rest[i])) || std::strchr("+-.", rest[i]);
        if (ok) { p.scheme = rest.substr(0, colon); rest = rest.substr(colon + 1); }
    }
    const size_t q = rest.find('?');
    if (q != std::string::npos) { p.query = rest.substr(q + 1); rest.resize(q); }
    if (rest.rfind("//", 0) == 0) {
        p.hasAuthority = true;
        rest = rest.substr(2);
        const size_t slash = rest.find('/');
        std::string auth = rest.substr(0, slash);
        rest = slash == std::string::npos ? "" : rest.substr(slash);
        const size_t at = auth.rfind('@');
        if (at != std::string::npos) {
            const std::string ui = auth.substr(0, at);
            auth = auth.substr(at + 1);
            const size_t c = ui.find(':');
            p.user = ui.substr(0, c);
            if (c != std::string::npos) p.password = ui.substr(c + 1);
        }
        const size_t pc = auth.rfind(':');
        if (pc != std::string::npos && auth.find(']', pc) == std::string::npos) { p.port = auth.substr(pc + 1); auth.resize(pc); }
        p.host = auth;
    }
    const size_t semi = rest.find(';');
    if (semi != std::string::npos) { p.params = rest.substr(semi + 1); rest.resize(semi); }
    p.path = rest;
    return p;
}

std::string absoluteString(Ctx& x, id u);
std::string resolveRelative(const std::string& base, const std::string& rel) {
    const UrlParts r = parseUrl(rel);
    if (!r.scheme.empty()) return rel;
    UrlParts b = parseUrl(base);
    std::string out = b.scheme + ":";
    if (r.hasAuthority) return out + rel;
    if (b.hasAuthority) out += "//" + (b.user.empty() ? "" : b.user + (b.password.empty() ? "" : ":" + b.password) + "@") + b.host + (b.port.empty() ? "" : ":" + b.port);
    std::string path;
    if (r.path.empty()) path = b.path;
    else if (r.path[0] == '/') path = r.path;
    else {
        const size_t slash = b.path.rfind('/');
        path = (slash == std::string::npos ? std::string("/") : b.path.substr(0, slash + 1)) + r.path;
    }
    // remove dot segments
    std::vector<std::string> segs;
    size_t i = 0;
    const bool trailing = !path.empty() && (path.back() == '/' || (path.size() >= 2 && path.substr(path.size() - 2) == "/.") || (path.size() >= 3 && path.substr(path.size() - 3) == "/.."));
    while (i <= path.size()) {
        const size_t j = path.find('/', i);
        const std::string seg = path.substr(i, j == std::string::npos ? std::string::npos : j - i);
        if (seg == "..") { if (!segs.empty()) segs.pop_back(); }
        else if (seg != "." && !seg.empty()) segs.push_back(seg);
        if (j == std::string::npos) break;
        i = j + 1;
    }
    std::string np;
    for (auto& s : segs) np += "/" + s;
    if (trailing || np.empty()) np += "/";
    out += np;
    if (!r.query.empty() || (r.path.empty() && !b.query.empty())) out += "?" + (r.query.empty() && r.path.empty() ? b.query : r.query);
    if (!r.fragment.empty()) out += "#" + r.fragment;
    return out;
}
std::string absoluteString(Ctx& x, id u) {
    UrlData* d = ud(x, u);
    if (!d) return u ? utf8(x, id(rt(x).send(x.cpu, u, "absoluteString"))) : std::string();
    return d->base ? resolveRelative(absoluteString(x, d->base), d->str) : d->str;
}
std::string filePath(Ctx& x, id u) {
    const UrlParts p = parseUrl(absoluteString(x, u));
    std::string path = percentDecode(p.path);
    while (path.size() > 1 && path.back() == '/') path.pop_back();
    return path;
}

id newURL(Ctx& x, const std::string& s, id base, bool owned = false) {
    const id u = rt(x).createInstance(B(x).nsURL);
    auto h = std::make_unique<UrlData>();
    h->str = s;
    h->base = rt(x).retain(x.cpu, base);
    rt(x).setHost(u, std::move(h));
    ud(x, u)->isFile = parseUrl(absoluteString(x, u)).scheme == "file";
    return owned ? u : autoreleased(x, u);
}
std::string fileURLString(const std::string& path, bool dir) {
    std::string p = path;
    if (p.empty() || p[0] != '/') p = (fs::current_path() / p).string();
    std::string s = "file://" + percentEncode(p, "/-._~!$&'()*+,;=:@");
    if (dir && s.back() != '/') s += '/';
    return s;
}
bool isDirPath(const std::string& p) { std::error_code ec; return fs::is_directory(p, ec); }

void initURL(Ctx& x, id self, const std::string& s, id base) {
    auto h = std::make_unique<UrlData>();
    h->str = s;
    h->base = rt(x).retain(x.cpu, base);
    rt(x).setHost(self, std::move(h));
    ud(x, self)->isFile = parseUrl(absoluteString(x, self)).scheme == "file";
}
bool validURLString(const std::string& s) {
    if (s.empty()) return true;
    for (unsigned char c : s) if (c <= ' ' || c >= 0x7F || std::strchr("\"<>\\^`{|}", c)) return false;
    return true;
}

id url_initFile(Ctx& x, id self, SEL, id path) { const std::string p = utf8(x, path); initURL(x, self, fileURLString(p, isDirPath(p)), 0); return self; }
id url_initFileDir(Ctx& x, id self, SEL, id path, bool dir) { initURL(x, self, fileURLString(utf8(x, path), dir), 0); return self; }
id url_initString(Ctx& x, id self, SEL, id str) {
    if (!str) { rt(x).release(x.cpu, self); return 0; }
    const std::string s = utf8(x, str);
    if (!validURLString(s)) { rt(x).release(x.cpu, self); return 0; }
    initURL(x, self, s, 0);
    return self;
}
id url_initStringRel(Ctx& x, id self, SEL, id str, id base) {
    if (!str) { rt(x).release(x.cpu, self); return 0; }
    const std::string s = utf8(x, str);
    if (!validURLString(s)) { rt(x).release(x.cpu, self); return 0; }
    initURL(x, self, s, base);
    return self;
}
id urlc_fileURL(Ctx& x, id self, SEL, id path) { return autoreleased(x, url_initFile(x, rt(x).createInstance(self), 0, path)); }
id urlc_fileURLDir(Ctx& x, id self, SEL, id path, bool dir) { return autoreleased(x, url_initFileDir(x, rt(x).createInstance(self), 0, path, dir)); }
id urlc_string(Ctx& x, id self, SEL, id s) { const id u = url_initString(x, rt(x).createInstance(self), 0, s); return u ? autoreleased(x, u) : 0; }
id urlc_stringRel(Ctx& x, id self, SEL, id s, id base) { const id u = url_initStringRel(x, rt(x).createInstance(self), 0, s, base); return u ? autoreleased(x, u) : 0; }

id url_absoluteString(Ctx& x, id self, SEL) { return makeString(x, absoluteString(x, self)); }
id url_relativeString(Ctx& x, id self, SEL) { UrlData* d = ud(x, self); return makeString(x, d ? d->str : std::string()); }
id url_baseURL(Ctx& x, id self, SEL) { UrlData* d = ud(x, self); return d ? d->base : 0; }
id url_absoluteURL(Ctx& x, id self, SEL) { UrlData* d = ud(x, self); return d && d->base ? newURL(x, absoluteString(x, self), 0) : self; }
id url_path(Ctx& x, id self, SEL) {
    const UrlParts p = parseUrl(absoluteString(x, self));
    if (p.path.empty() && !p.hasAuthority && !p.scheme.empty()) return 0;
    return makeString(x, filePath(x, self));
}
id url_relativePath(Ctx& x, id self, SEL s) {
    UrlData* d = ud(x, self);
    if (!d || !d->base) return url_path(x, self, s);
    std::string p = percentDecode(parseUrl(d->str).path);
    while (p.size() > 1 && p.back() == '/') p.pop_back();
    return makeString(x, p);
}
bool url_isFile(Ctx& x, id self, SEL) { UrlData* d = ud(x, self); return d && d->isFile; }
id part(Ctx& x, id self, std::string UrlParts::*m, bool emptyIsNil = true) {
    const UrlParts p = parseUrl(absoluteString(x, self));
    const std::string& v = p.*m;
    if (v.empty() && emptyIsNil) return 0;
    return makeString(x, v);
}
id url_scheme(Ctx& x, id self, SEL) { return part(x, self, &UrlParts::scheme); }
id url_host(Ctx& x, id self, SEL) { return part(x, self, &UrlParts::host); }
id url_query(Ctx& x, id self, SEL) { return part(x, self, &UrlParts::query); }
id url_fragment(Ctx& x, id self, SEL) { return part(x, self, &UrlParts::fragment); }
id url_user(Ctx& x, id self, SEL) { return part(x, self, &UrlParts::user); }
id url_password(Ctx& x, id self, SEL) { return part(x, self, &UrlParts::password); }
id url_params(Ctx& x, id self, SEL) { return part(x, self, &UrlParts::params); }
id url_port(Ctx& x, id self, SEL) {
    const UrlParts p = parseUrl(absoluteString(x, self));
    return p.port.empty() ? 0 : makeNumberInt(x, std::atoi(p.port.c_str()));
}
id url_resourceSpecifier(Ctx& x, id self, SEL) {
    const std::string s = absoluteString(x, self);
    const size_t c = s.find(':');
    return makeString(x, c == std::string::npos ? s : s.substr(c + 1));
}
id url_lastPathComponent(Ctx& x, id self, SEL) {
    const std::string p = filePath(x, self);
    if (p == "/") return makeString(x, "/");
    return makeString(x, p.substr(p.rfind('/') + 1));
}
id url_pathExtension(Ctx& x, id self, SEL) {
    const std::string p = filePath(x, self);
    const std::string last = p.substr(p.rfind('/') + 1);
    const size_t dot = last.rfind('.');
    return makeString(x, dot == std::string::npos || dot == 0 ? std::string() : last.substr(dot + 1));
}
id url_pathComponents(Ctx& x, id self, SEL) {
    const std::string p = filePath(x, self);
    std::vector<id> parts;
    if (!p.empty() && p[0] == '/') parts.push_back(makeString(x, "/"));
    size_t i = 0;
    while (i < p.size()) {
        const size_t j = p.find('/', i);
        const std::string seg = p.substr(i, j == std::string::npos ? std::string::npos : j - i);
        if (!seg.empty()) parts.push_back(makeString(x, seg));
        if (j == std::string::npos) break;
        i = j + 1;
    }
    return makeArray(x, parts);
}
// Rebuild a URL with a new path, keeping scheme/authority/query.
id withPath(Ctx& x, id self, const std::string& newPath) {
    UrlParts p = parseUrl(absoluteString(x, self));
    std::string s = p.scheme.empty() ? "" : p.scheme + ":";
    if (p.hasAuthority) s += "//" + (p.user.empty() ? "" : p.user + (p.password.empty() ? "" : ":" + p.password) + "@") + p.host + (p.port.empty() ? "" : ":" + p.port);
    s += percentEncode(newPath, "/-._~!$&'()*+,;=:@");
    if (!p.query.empty()) s += "?" + p.query;
    if (!p.fragment.empty()) s += "#" + p.fragment;
    return newURL(x, s, 0);
}
id url_appendingComponent(Ctx& x, id self, SEL, id comp) {
    std::string base = percentDecode(parseUrl(absoluteString(x, self)).path);
    const std::string c = utf8(x, comp);
    if (base.empty() || base.back() != '/') base += '/';
    std::string np = base + c;
    UrlData* d = ud(x, self);
    if (d && d->isFile && isDirPath(np) && np.back() != '/') np += '/';
    return withPath(x, self, np);
}
id url_appendingComponentDir(Ctx& x, id self, SEL s, id comp, bool dir) {
    const id u = url_appendingComponent(x, self, s, comp);
    std::string p = percentDecode(parseUrl(absoluteString(x, u)).path);
    if (dir && (p.empty() || p.back() != '/')) return withPath(x, self, p + "/");
    if (!dir && !p.empty() && p.back() == '/') { p.pop_back(); return withPath(x, self, p); }
    return u;
}
id url_appendingExtension(Ctx& x, id self, SEL, id ext) {
    std::string p = percentDecode(parseUrl(absoluteString(x, self)).path);
    const bool dir = !p.empty() && p.back() == '/';
    if (dir) p.pop_back();
    return withPath(x, self, p + "." + utf8(x, ext) + (dir ? "/" : ""));
}
id url_deletingLast(Ctx& x, id self, SEL) {
    std::string p = percentDecode(parseUrl(absoluteString(x, self)).path);
    while (p.size() > 1 && p.back() == '/') p.pop_back();
    const size_t slash = p.rfind('/');
    p = slash == std::string::npos ? std::string("./") : p.substr(0, slash + 1);
    return withPath(x, self, p);
}
id url_deletingExtension(Ctx& x, id self, SEL) {
    std::string p = percentDecode(parseUrl(absoluteString(x, self)).path);
    const bool dir = !p.empty() && p.back() == '/';
    if (dir) p.pop_back();
    const size_t slash = p.rfind('/'), dot = p.rfind('.');
    if (dot != std::string::npos && (slash == std::string::npos || dot > slash + 1)) p.resize(dot);
    return withPath(x, self, p + (dir ? "/" : ""));
}
id url_standardized(Ctx& x, id self, SEL) { return newURL(x, resolveRelative(absoluteString(x, self), ""), 0); }
bool url_isEqual(Ctx& x, id self, SEL, id o) {
    if (!o || !ud(x, o)) return false;
    return absoluteString(x, self) == absoluteString(x, o);
}
uint32_t url_hash(Ctx& x, id self, SEL) { return uint32_t(std::hash<std::string>()(absoluteString(x, self))); }
id url_description(Ctx& x, id self, SEL) {
    UrlData* d = ud(x, self);
    if (d && d->base) return makeString(x, d->str + " -- " + absoluteString(x, d->base));
    return makeString(x, absoluteString(x, self));
}
uint32_t url_fsRep(Ctx& x, id self, SEL) { return id(rt(x).send(x.cpu, makeString(x, filePath(x, self)), "fileSystemRepresentation")); }
bool url_reachable(Ctx& x, id self, SEL, GuestPtr err) {
    std::error_code ec;
    if (fs::exists(x.proc.hostPath(filePath(x, self)), ec)) return true;
    ErrorOut::posix(x, err, ENOENT, filePath(x, self));
    return false;
}
bool url_setResourceValue(Ctx&, id, SEL, id, id, GuestPtr) { return true; }
bool url_getResourceValue(Ctx& x, id self, SEL, GuestPtr out, id key, GuestPtr err) {
    const std::string k = utf8(x, key), p = x.proc.hostPath(filePath(x, self));
    std::error_code ec;
    id v = 0;
    if (k == "NSURLIsDirectoryKey") v = makeBool(x, fs::is_directory(p, ec));
    else if (k == "NSURLIsRegularFileKey") v = makeBool(x, fs::is_regular_file(p, ec));
    else if (k == "NSURLFileSizeKey") v = makeNumberInt(x, fs::is_regular_file(p, ec) ? int64_t(fs::file_size(p, ec)) : 0);
    else if (k == "NSURLNameKey") v = makeString(x, p.substr(p.rfind('/') + 1));
    else if (k == "NSURLIsExcludedFromBackupKey") v = makeBool(x, false);
    else if (k == "NSURLContentModificationDateKey" || k == "NSURLCreationDateKey") {
        struct stat st;
        if (::stat(p.c_str(), &st) == 0) v = makeDate(x, double(st.st_mtime) - 978307200.0);
    }
    (void)err;
    if (out) x.mem.write<uint32_t>(out, v);
    return true;
}
id url_copy(Ctx& x, id self, SEL, uint32_t) { return rt(x).retain(x.cpu, self); }
void url_dealloc(Ctx& x, id self, SEL) {
    if (UrlData* d = ud(x, self)) { const id b = d->base; d->base = 0; rt(x).release(x.cpu, b); }
    rt(x).disposeInstance(x.cpu, self);
}
id url_initWithScheme(Ctx& x, id self, SEL, id scheme, id host, id path) {
    initURL(x, self, utf8(x, scheme) + "://" + (host ? utf8(x, host) : std::string()) + percentEncode(utf8(x, path), "/-._~!$&'()*+,;=:@"), 0);
    return self;
}

// --------------------------------------------------------------- NSBundle ---

struct BundleData : ObjC::HostObject {
    std::string path;
    Plist::ValuePtr info;
    id infoDict = 0;
    bool isMain = false;
};
BundleData* bd(Ctx& x, id b) { return b ? rt(x).hostIf<BundleData>(b) : nullptr; }

std::string chooseLanguage(const std::string& bundle) {
    std::error_code ec;
    for (const char* l : {"en", "English", "en_US", "en-US", "Base"})
        if (fs::is_directory(fs::path(bundle) / (std::string(l) + ".lproj"), ec)) return l;
    // fall back to whatever localization exists
    for (auto& e : fs::directory_iterator(bundle, ec)) {
        const std::string n = e.path().filename().string();
        if (n.size() > 6 && n.substr(n.size() - 6) == ".lproj") return n.substr(0, n.size() - 6);
    }
    return "en";
}

id bundleFor(Ctx& x, const std::string& rawPath, bool main = false) {
    auto& b = B(x);
    const std::string path = standardize(rawPath);
    {
        std::lock_guard<std::mutex> lk(b.mu);
        auto it = b.bundlesByPath.find(path);
        if (it != b.bundlesByPath.end()) return it->second;
    }
    std::error_code ec;
    if (!fs::is_directory(path, ec)) return 0;
    const id o = rt(x).createInstance(b.nsBundle);
    auto h = std::make_unique<BundleData>();
    h->path = path;
    h->isMain = main;
    if (main && x.proc.infoPlist()) h->info = std::make_shared<Plist::Value>(*x.proc.infoPlist());
    else {
        try { if (fs::exists(fs::path(path) / "Info.plist", ec)) h->info = Plist::parseFile((fs::path(path) / "Info.plist").string()); }
        catch (const std::exception&) {}
    }
    if (!h->info) h->info = Plist::Value::makeDict();
    rt(x).setHost(o, std::move(h));
    rt(x).makeImmortal(o);
    std::lock_guard<std::mutex> lk(b.mu);
    b.bundlesByPath[path] = o;
    return o;
}
id mainBundle(Ctx& x) {
    auto& b = B(x);
    if (!b.mainBundle) {
        b.mainBundle = bundleFor(x, x.proc.bundlePath(), true);
        b.language = chooseLanguage(x.proc.bundlePath());
    }
    return b.mainBundle;
}

// Resource lookup in the order NSBundle uses: <dir>/ then <dir>/<lang>.lproj,
// trying the ~iphone device modifier first.
std::string findResource(Ctx& x, const std::string& bundle, std::string name, std::string type, const std::string& subdir,
                         const std::string& localization = {}) {
    if (name.empty() && type.empty()) return {};
    if (!type.empty() && type[0] == '.') type = type.substr(1);
    if (type.empty()) {
        const size_t dot = name.rfind('.');
        if (dot != std::string::npos && dot > 0 && name.find('/', dot) == std::string::npos) { type = name.substr(dot + 1); name.resize(dot); }
    }
    std::error_code ec;
    const fs::path root = subdir.empty() ? fs::path(bundle) : (fs::path(subdir).is_absolute() ? fs::path(subdir) : fs::path(bundle) / subdir);
    const std::string lang = localization.empty() ? (B(x).language.empty() ? chooseLanguage(bundle) : B(x).language) : localization;
    std::vector<fs::path> dirs = {root, root / (lang + ".lproj"), root / "Base.lproj", root / "en.lproj", root / "English.lproj"};
    auto toLower = [](std::string s) {
        for (char& c : s) c = char(std::tolower(uint8_t(c)));
        return s;
    };
    for (const auto& d : dirs) {
        if (name.empty()) {   // first file of that type
            if (!fs::is_directory(d, ec)) continue;
            for (auto& e : fs::directory_iterator(d, ec)) if (e.path().extension() == "." + type) return e.path().string();
            continue;
        }
        const std::string ext = type.empty() ? "" : "." + type;
        for (const std::string& candidate : {name + "~iphone" + ext, name + ext}) {
            const fs::path p = d / candidate;
            if (fs::exists(p, ec)) return p.string();
        }
        // Case-insensitive fallback for Linux
        if (fs::is_directory(d, ec)) {
            for (const std::string& candidate : {name + "~iphone" + ext, name + ext}) {
                const std::string targetLower = toLower(candidate);
                for (auto& e : fs::directory_iterator(d, ec)) {
                    if (toLower(e.path().filename().string()) == targetLower) return e.path().string();
                }
            }
        }
    }
    return {};
}

std::string resourcePathFor(Ctx& x, id bundle, id name, id type, id dir, id loc = 0) {
    BundleData* d = bd(x, bundle);
    if (!d) d = bd(x, mainBundle(x));
    return findResource(x, d->path, name ? utf8(x, name) : "", type ? utf8(x, type) : "", dir ? utf8(x, dir) : "", loc ? utf8(x, loc) : "");
}

id b_main(Ctx& x, id, SEL) { return mainBundle(x); }
id b_withPath(Ctx& x, id, SEL, id path) { mainBundle(x); return path ? bundleFor(x, x.proc.hostPath(utf8(x, path))) : 0; }
id b_withURL(Ctx& x, id, SEL, id url) { mainBundle(x); return url ? bundleFor(x, x.proc.hostPath(filePath(x, url))) : 0; }
id b_initWithPath(Ctx& x, id self, SEL, id path) { rt(x).disposeInstance(x.cpu, self); return rt(x).retain(x.cpu, b_withPath(x, 0, 0, path)); }
id b_forClass(Ctx& x, id, SEL, Class) { return mainBundle(x); }
id b_withIdentifier(Ctx& x, id, SEL, id ident) {
    const id m = mainBundle(x);
    if (ident && bd(x, m)->info->getString("CFBundleIdentifier") == utf8(x, ident)) return m;
    return 0;
}
id b_allBundles(Ctx& x, id, SEL) { return makeArray(x, {mainBundle(x)}); }
id b_allFrameworks(Ctx& x, id, SEL) { return makeArray(x, {}); }
id b_bundlePath(Ctx& x, id self, SEL) { return makeString(x, x.proc.guestPath(bd(x, self)->path)); }
id b_resourcePath(Ctx& x, id self, SEL) { return makeString(x, x.proc.guestPath(bd(x, self)->path)); }
id b_executablePath(Ctx& x, id self, SEL) {
    BundleData* d = bd(x, self);
    if (d->isMain) return makeString(x, x.proc.guestExecutablePath());
    const std::string e = d->info->getString("CFBundleExecutable");
    return e.empty() ? 0 : makeString(x, x.proc.guestPath((fs::path(d->path) / e).string()));
}
id b_privateFrameworksPath(Ctx& x, id self, SEL) { return makeString(x, bd(x, self)->path + "/Frameworks"); }
id b_builtInPlugInsPath(Ctx& x, id self, SEL) { return makeString(x, bd(x, self)->path + "/PlugIns"); }
id b_bundleURL(Ctx& x, id self, SEL) { return newURL(x, fileURLString(x.proc.guestPath(bd(x, self)->path), true), 0); }
id b_executableURL(Ctx& x, id self, SEL s) { const id p = b_executablePath(x, self, s); return p ? newURL(x, fileURLString(utf8(x, p), false), 0) : 0; }
id b_identifier(Ctx& x, id self, SEL) { const std::string v = bd(x, self)->info->getString("CFBundleIdentifier"); return v.empty() ? 0 : makeString(x, v); }
id b_infoDictionary(Ctx& x, id self, SEL) {
    BundleData* d = bd(x, self);
    if (!d->infoDict) { d->infoDict = rt(x).retain(x.cpu, plistToObject(x, *d->info)); rt(x).makeImmortal(d->infoDict); }
    return d->infoDict;
}
id b_objectForInfoKey(Ctx& x, id self, SEL, id key) {
    BundleData* d = bd(x, self);
    const std::string k = utf8(x, key);
    // InfoPlist.strings localizes display name etc.
    const std::string loc = findResource(x, d->path, "InfoPlist", "strings", "");
    if (!loc.empty()) {
        try { if (auto t = Plist::parseFile(loc)) if (const Plist::Value* v = t->get(k)) return plistToObject(x, *v); } catch (const std::exception&) {}
    }
    return dictGet(x, b_infoDictionary(x, self, 0), key);
}
id b_localizedInfo(Ctx& x, id self, SEL) { return b_infoDictionary(x, self, 0); }
id b_pathForResource(Ctx& x, id self, SEL, id name, id type) {
    const std::string p = resourcePathFor(x, self, name, type, 0);
    return p.empty() ? 0 : makeString(x, x.proc.guestPath(p));
}
id b_pathForResourceDir(Ctx& x, id self, SEL, id name, id type, id dir) {
    const std::string p = resourcePathFor(x, self, name, type, dir);
    return p.empty() ? 0 : makeString(x, x.proc.guestPath(p));
}
id b_pathForResourceDirLoc(Ctx& x, id self, SEL, id name, id type, id dir, id loc) {
    const std::string p = resourcePathFor(x, self, name, type, dir, loc);
    return p.empty() ? 0 : makeString(x, x.proc.guestPath(p));
}
id bc_pathForResourceInDir(Ctx& x, id, SEL, id name, id type, id dirPath) {
    std::string n = name ? utf8(x, name) : "", t = type ? utf8(x, type) : "";
    const std::string p = findResource(x, x.proc.hostPath(utf8(x, dirPath)), n, t, "");
    return p.empty() ? 0 : makeString(x, x.proc.guestPath(p));
}
id b_pathsForResources(Ctx& x, id self, SEL, id type, id dir) {
    BundleData* d = bd(x, self);
    std::string t = type ? utf8(x, type) : "";
    if (!t.empty() && t[0] == '.') t = t.substr(1);
    const fs::path root = dir ? fs::path(d->path) / utf8(x, dir) : fs::path(d->path);
    std::vector<id> out;
    std::error_code ec;
    for (const fs::path& dd : {root, root / (B(x).language + ".lproj")}) {
        if (!fs::is_directory(dd, ec)) continue;
        for (auto& e : fs::directory_iterator(dd, ec))
            if (t.empty() || e.path().extension() == "." + t) out.push_back(makeString(x, x.proc.guestPath(e.path().string())));
    }
    return makeArray(x, out);
}
id b_urlForResource(Ctx& x, id self, SEL, id name, id ext) {
    const std::string p = resourcePathFor(x, self, name, ext, 0);
    return p.empty() ? 0 : newURL(x, fileURLString(x.proc.guestPath(p), isDirPath(p)), 0);
}
id b_urlForResourceDir(Ctx& x, id self, SEL, id name, id ext, id dir) {
    const std::string p = resourcePathFor(x, self, name, ext, dir);
    return p.empty() ? 0 : newURL(x, fileURLString(x.proc.guestPath(p), isDirPath(p)), 0);
}
id b_urlsForResources(Ctx& x, id self, SEL s, id ext, id dir) {
    std::vector<id> out;
    for (id p : arrayItems(x, b_pathsForResources(x, self, s, ext, dir))) out.push_back(newURL(x, fileURLString(utf8(x, p), false), 0));
    return makeArray(x, out);
}

std::shared_ptr<Plist::Value> stringTable(Ctx& x, const std::string& bundle, const std::string& table) {
    const std::string path = findResource(x, bundle, table.empty() ? "Localizable" : table, "strings", "");
    auto& b = B(x);
    std::lock_guard<std::mutex> lk(b.mu);
    auto it = b.stringTables.find(bundle + "|" + table);
    if (it != b.stringTables.end()) return it->second;
    std::shared_ptr<Plist::Value> t;
    if (!path.empty()) {
        try { t = Plist::parseFile(path); } catch (const std::exception& e) { x.proc.logf(LogLevel::Warning, "can't parse %s: %s", path.c_str(), e.what()); }
    }
    b.stringTables[bundle + "|" + table] = t;
    return t;
}
id localized(Ctx& x, id bundle, id key, id value, id table) {
    BundleData* d = bd(x, bundle);
    if (!d) d = bd(x, mainBundle(x));
    if (!key) return value ? value : makeString(x, "");
    auto t = stringTable(x, d->path, table ? utf8(x, table) : "");
    if (t) if (const Plist::Value* v = t->get(utf8(x, key)); v && v->type == Plist::Value::Type::String) return makeString(x, v->s);
    if (value && uint32_t(rt(x).send(x.cpu, value, "length"))) return value;
    return key;
}
id b_localizedString(Ctx& x, id self, SEL, id key, id value, id table) { return localized(x, self, key, value, table); }
id b_localizations(Ctx& x, id self, SEL) {
    std::vector<id> out;
    std::error_code ec;
    for (auto& e : fs::directory_iterator(bd(x, self)->path, ec)) {
        const std::string n = e.path().filename().string();
        if (n.size() > 6 && n.substr(n.size() - 6) == ".lproj") out.push_back(makeString(x, n.substr(0, n.size() - 6)));
    }
    return makeArray(x, out);
}
id b_preferredLocalizations(Ctx& x, id, SEL) { mainBundle(x); return makeArray(x, {makeString(x, B(x).language)}); }
id b_devLocalization(Ctx& x, id self, SEL) { const std::string v = bd(x, self)->info->getString("CFBundleDevelopmentRegion", "en"); return makeString(x, v); }
bool b_true(Ctx&, id, SEL) { return true; }
Class b_principalClass(Ctx& x, id self, SEL) {
    const std::string n = bd(x, self)->info->getString("NSPrincipalClass");
    return n.empty() ? 0 : rt(x).getClass(n);
}
Class b_classNamed(Ctx& x, id, SEL, id name) { return rt(x).getClass(utf8(x, name)); }
id b_description(Ctx& x, id self, SEL) {
    char buf[64];
    std::snprintf(buf, sizeof buf, "NSBundle <%s> (loaded)", "");
    return makeString(x, "NSBundle <" + bd(x, self)->path + "> (loaded)");
}

// --------------------------------------------------------- search paths ---

enum : uint32_t {
    kNSApplicationDirectory = 1, kNSDemoApplicationDirectory, kNSDeveloperApplicationDirectory, kNSAdminApplicationDirectory,
    kNSLibraryDirectory, kNSDeveloperDirectory, kNSUserDirectory, kNSDocumentationDirectory, kNSDocumentDirectory,
    kNSCoreServiceDirectory, kNSAutosavedInformationDirectory, kNSDesktopDirectory, kNSCachesDirectory,
    kNSApplicationSupportDirectory, kNSDownloadsDirectory, kNSInputMethodsDirectory, kNSMoviesDirectory, kNSMusicDirectory,
    kNSPicturesDirectory, kNSPrinterDescriptionDirectory, kNSSharedPublicDirectory, kNSPreferencePanesDirectory,
    kNSItemReplacementDirectory = 99, kNSAllApplicationsDirectory = 100, kNSAllLibrariesDirectory = 101,
};
std::vector<std::string> searchPaths(Ctx& x, uint32_t dir, uint32_t mask, bool expand) {
    std::string rel;
    switch (dir) {
        case kNSDocumentDirectory: rel = "Documents"; break;
        case kNSLibraryDirectory: case kNSAllLibrariesDirectory: rel = "Library"; break;
        case kNSCachesDirectory: rel = "Library/Caches"; break;
        case kNSApplicationSupportDirectory: rel = "Library/Application Support"; break;
        case kNSApplicationDirectory: case kNSAllApplicationsDirectory: rel = "Applications"; break;
        case kNSDemoApplicationDirectory: rel = "Applications/Demos"; break;
        case kNSDeveloperApplicationDirectory: rel = "Developer/Applications"; break;
        case kNSAdminApplicationDirectory: rel = "Applications/Utilities"; break;
        case kNSDeveloperDirectory: rel = "Developer"; break;
        case kNSDocumentationDirectory: rel = "Library/Documentation"; break;
        case kNSAutosavedInformationDirectory: rel = "Library/Autosave Information"; break;
        case kNSDesktopDirectory: rel = "Desktop"; break;
        case kNSDownloadsDirectory: rel = "Downloads"; break;
        case kNSInputMethodsDirectory: rel = "Library/Input Methods"; break;
        case kNSMoviesDirectory: rel = "Movies"; break;
        case kNSMusicDirectory: rel = "Music"; break;
        case kNSPicturesDirectory: rel = "Pictures"; break;
        case kNSPreferencePanesDirectory: rel = "Library/PreferencePanes"; break;
        case kNSItemReplacementDirectory: rel = "tmp"; break;
        default: return {};
    }
    std::vector<std::string> paths;
    if (mask & 1) paths.push_back((expand ? x.proc.guestHomePath() : std::string("~")) + "/" + rel);
    // There is no shared guest filesystem yet. Local-domain locations have a
    // separate per-process home-backed root, rather than exposing host /Library.
    // Personal folders (Documents, Desktop, etc.) do not have a local domain.
    const bool localDirectory =
        dir == kNSLibraryDirectory || dir == kNSAllLibrariesDirectory ||
        dir == kNSCachesDirectory || dir == kNSApplicationSupportDirectory ||
        dir == kNSApplicationDirectory || dir == kNSAllApplicationsDirectory ||
        dir == kNSDemoApplicationDirectory || dir == kNSAdminApplicationDirectory ||
        dir == kNSDeveloperApplicationDirectory || dir == kNSDeveloperDirectory ||
        dir == kNSDocumentationDirectory || dir == kNSInputMethodsDirectory ||
        dir == kNSPreferencePanesDirectory;
    if ((mask & 2) && localDirectory)
        paths.push_back(x.proc.guestHomePath() + "/LocalDomain/" + rel);
    return paths;
}
id f_searchPaths(Ctx& x, uint32_t dir, uint32_t mask, bool expand) {
    std::vector<id> out;
    for (auto& s : searchPaths(x, dir, mask, expand)) out.push_back(makeString(x, s));
    if (x.proc.config().traceCalls)
        x.proc.logf(LogLevel::Debug, "NSSearchPathForDirectoriesInDomains(dir=%u, mask=0x%x, expand=%u) -> %zu paths",
                    dir, mask, unsigned(expand), out.size());
    return makeArray(x, out);
}
id f_homeDirectory(Ctx& x) { return makeString(x, x.proc.guestHomePath()); }
id f_homeDirectoryForUser(Ctx& x, id) { return makeString(x, x.proc.guestHomePath()); }
id f_temporaryDirectory(Ctx& x) { return makeString(x, x.proc.guestHomePath() + "/tmp/"); }
id f_openStepRoot(Ctx& x) { return makeString(x, "/"); }

// ----------------------------------------------------------- NSFileManager ---

id fm_default(Ctx& x, id, SEL) {
    auto& b = B(x);
    if (!b.defaultManager) { b.defaultManager = rt(x).createInstance(b.fileManager); rt(x).makeImmortal(b.defaultManager); }
    return b.defaultManager;
}
id fm_init(Ctx&, id self, SEL) { return self; }
std::string P(Ctx& x, id path) { return path ? x.proc.hostPath(utf8(x, path)) : std::string(); }
bool fm_exists(Ctx& x, id, SEL, id path) { struct stat st; return path && ::stat(P(x, path).c_str(), &st) == 0; }
bool fm_existsDir(Ctx& x, id, SEL, id path, GuestPtr isDir) {
    struct stat st;
    const bool ok = path && ::stat(P(x, path).c_str(), &st) == 0;
    if (isDir) x.mem.write<uint8_t>(isDir, ok && S_ISDIR(st.st_mode) ? 1 : 0);
    return ok;
}
bool fm_createDir(Ctx& x, id, SEL, id path, bool intermediate, id, GuestPtr err) {
    const std::string p = P(x, path);
    std::error_code ec;
    if (intermediate) {
        fs::create_directories(p, ec);
        if (ec || !fs::is_directory(p)) { ErrorOut::posix(x, err, ec ? ec.value() : ENOTDIR, p); return false; }
        return true;
    }
    if (::mkdir(p.c_str(), 0755) != 0) { ErrorOut::posix(x, err, errno, p); return false; }
    return true;
}
bool fm_createDirOld(Ctx& x, id self, SEL s, id path, id attrs) { return fm_createDir(x, self, s, path, false, attrs, 0); }
bool fm_createDirURL(Ctx& x, id self, SEL s, id url, bool inter, id attrs, GuestPtr err) {
    return fm_createDir(x, self, s, makeString(x, filePath(x, url)), inter, attrs, err);
}
bool fm_createFile(Ctx& x, id, SEL, id path, id data, id) {
    std::ofstream out(P(x, path), std::ios::binary | std::ios::trunc);
    if (!out) return false;
    if (data) { const auto b = dataBytes(x, data); out.write(reinterpret_cast<const char*>(b.data()), std::streamsize(b.size())); }
    return bool(out);
}
bool fm_remove(Ctx& x, id, SEL, id path, GuestPtr err) {
    const std::string p = P(x, path);
    std::error_code ec;
    if (!fs::exists(fs::symlink_status(p, ec))) { ErrorOut::posix(x, err, ENOENT, p); return false; }
    fs::remove_all(p, ec);
    if (ec) { ErrorOut::posix(x, err, ec.value(), p); return false; }
    return true;
}
bool fm_removeOld(Ctx& x, id self, SEL s, id path, id) { return fm_remove(x, self, s, path, 0); }
bool fm_removeURL(Ctx& x, id self, SEL s, id url, GuestPtr err) { return fm_remove(x, self, s, makeString(x, filePath(x, url)), err); }
bool fm_copy(Ctx& x, id, SEL, id src, id dst, GuestPtr err) {
    const std::string a = P(x, src), b = P(x, dst);
    std::error_code ec;
    if (fs::exists(b, ec)) { ErrorOut::posix(x, err, EEXIST, b); return false; }
    fs::copy(a, b, fs::copy_options::recursive | fs::copy_options::copy_symlinks, ec);
    if (ec) { ErrorOut::posix(x, err, ec.value(), a); return false; }
    return true;
}
bool fm_copyURL(Ctx& x, id self, SEL s, id src, id dst, GuestPtr err) {
    return fm_copy(x, self, s, makeString(x, filePath(x, src)), makeString(x, filePath(x, dst)), err);
}
bool fm_move(Ctx& x, id, SEL, id src, id dst, GuestPtr err) {
    const std::string a = P(x, src), b = P(x, dst);
    std::error_code ec;
    if (fs::exists(b, ec)) { ErrorOut::posix(x, err, EEXIST, b); return false; }
    fs::rename(a, b, ec);
    if (ec) {
        fs::copy(a, b, fs::copy_options::recursive, ec);
        if (!ec) fs::remove_all(a, ec);
    }
    if (ec) { ErrorOut::posix(x, err, ec.value(), a); return false; }
    return true;
}
bool fm_moveURL(Ctx& x, id self, SEL s, id src, id dst, GuestPtr err) {
    return fm_move(x, self, s, makeString(x, filePath(x, src)), makeString(x, filePath(x, dst)), err);
}
bool fm_link(Ctx& x, id, SEL, id src, id dst, GuestPtr err) {
    std::error_code ec;
    fs::create_hard_link(P(x, src), P(x, dst), ec);
    if (ec) { ErrorOut::posix(x, err, ec.value(), P(x, src)); return false; }
    return true;
}
bool fm_symlink(Ctx& x, id, SEL, id path, id dest, GuestPtr err) {
    std::error_code ec;
    fs::create_symlink(P(x, dest), P(x, path), ec);
    if (ec) { ErrorOut::posix(x, err, ec.value(), P(x, path)); return false; }
    return true;
}
id fm_destOfSymlink(Ctx& x, id, SEL, id path, GuestPtr err) {
    std::error_code ec;
    const fs::path t = fs::read_symlink(P(x, path), ec);
    if (ec) { ErrorOut::posix(x, err, ec.value(), P(x, path)); return 0; }
    return makeString(x, t.string());
}
id fm_contents(Ctx& x, id, SEL, id path, GuestPtr err) {
    const std::string p = P(x, path);
    std::error_code ec;
    std::vector<std::string> names;
    for (auto& e : fs::directory_iterator(p, ec)) names.push_back(e.path().filename().string());
    if (ec) { ErrorOut::posix(x, err, ec.value(), p); return 0; }
    std::sort(names.begin(), names.end());
    std::vector<id> out;
    for (auto& n : names) out.push_back(makeString(x, n));
    return makeArray(x, out);
}
id fm_contentsOld(Ctx& x, id self, SEL s, id path) { return fm_contents(x, self, s, path, 0); }
id fm_contentsURL(Ctx& x, id self, SEL s, id url, id, uint32_t opts, GuestPtr err) {
    const std::string dir = filePath(x, url);
    const id names = fm_contents(x, self, s, makeString(x, dir), err);
    if (!names) return 0;
    std::vector<id> out;
    for (id n : arrayItems(x, names)) {
        const std::string name = utf8(x, n);
        if ((opts & 4) && !name.empty() && name[0] == '.') continue;   // NSDirectoryEnumerationSkipsHiddenFiles
        const std::string full = dir + "/" + name;
        out.push_back(newURL(x, fileURLString(full, isDirPath(full)), 0));
    }
    return makeArray(x, out);
}
id fm_subpaths(Ctx& x, id, SEL, id path, GuestPtr err) {
    const std::string p = P(x, path);
    std::error_code ec;
    if (!fs::is_directory(p, ec)) { ErrorOut::posix(x, err, ENOENT, p); return 0; }
    std::vector<std::string> names;
    for (auto it = fs::recursive_directory_iterator(p, ec); !ec && it != fs::recursive_directory_iterator(); it.increment(ec))
        names.push_back(fs::relative(it->path(), p, ec).string());
    std::sort(names.begin(), names.end());
    std::vector<id> out;
    for (auto& n : names) out.push_back(makeString(x, n));
    return makeArray(x, out);
}
id fm_subpathsOld(Ctx& x, id self, SEL s, id path) { return fm_subpaths(x, self, s, path, 0); }
id fm_contentsAtPath(Ctx& x, id, SEL, id path) {
    std::ifstream in(P(x, path), std::ios::binary);
    if (!in) return 0;
    std::vector<uint8_t> b((std::istreambuf_iterator<char>(in)), {});
    return makeData(x, b.data(), uint32_t(b.size()));
}
bool fm_contentsEqual(Ctx& x, id self, SEL s, id a, id b) {
    const id da = fm_contentsAtPath(x, self, s, a), db = fm_contentsAtPath(x, self, s, b);
    return da && db && dataBytes(x, da) == dataBytes(x, db);
}
// NSDictionary (NSFileAttributes) accessors.
constexpr const char* kObjKeys[] = {"NSFileModificationDate", "NSFileCreationDate", "NSFileType", "NSFileOwnerAccountName", "NSFileGroupOwnerAccountName"};
constexpr const char* kNumKeys[] = {"NSFilePosixPermissions", "NSFileSystemNumber", "NSFileSystemFileNumber", "NSFileOwnerAccountID", "NSFileGroupOwnerAccountID"};
constexpr const char* kBoolKeys[] = {"NSFileExtensionHidden", "NSFileImmutable", "NSFileAppendOnly"};
uint64_t fa_size(Ctx& x, id self, SEL) { const id n = dictGet(x, self, "NSFileSize"); return n ? uint64_t(numberInt(x, n)) : 0; }
template <int I> id fa_obj(Ctx& x, id self, SEL) { return dictGet(x, self, kObjKeys[I]); }
template <int I> uint32_t fa_num(Ctx& x, id self, SEL) {
    const id n = dictGet(x, self, kNumKeys[I]);
    if (I >= 3) return n;   // the account IDs are returned as NSNumber objects
    return n ? uint32_t(numberInt(x, n)) : 0;
}
template <int I> bool fa_bool(Ctx& x, id self, SEL) { const id n = dictGet(x, self, kBoolKeys[I]); return n && numberInt(x, n); }

id attributes(Ctx& x, const std::string& p, bool follow, GuestPtr err) {
    struct stat st;
    if ((follow ? ::stat(p.c_str(), &st) : ::lstat(p.c_str(), &st)) != 0) { ErrorOut::posix(x, err, errno, p); return 0; }
    const char* type = S_ISDIR(st.st_mode) ? "NSFileTypeDirectory" : S_ISLNK(st.st_mode) ? "NSFileTypeSymbolicLink" :
                       S_ISREG(st.st_mode) ? "NSFileTypeRegular" : S_ISCHR(st.st_mode) ? "NSFileTypeCharacterSpecial" :
                       S_ISBLK(st.st_mode) ? "NSFileTypeBlockSpecial" : S_ISSOCK(st.st_mode) ? "NSFileTypeSocket" : "NSFileTypeUnknown";
    std::vector<std::pair<id, id>> e = {
        {makeString(x, "NSFileSize"), makeNumberInt(x, int64_t(st.st_size))},
        {makeString(x, "NSFileType"), makeString(x, type)},
        {makeString(x, "NSFileModificationDate"), makeDate(x, double(st.st_mtime) - 978307200.0)},
        {makeString(x, "NSFileCreationDate"), makeDate(x, double(st.st_ctime) - 978307200.0)},
        {makeString(x, "NSFilePosixPermissions"), makeNumberInt(x, st.st_mode & 07777)},
        {makeString(x, "NSFileReferenceCount"), makeNumberInt(x, int64_t(st.st_nlink))},
        {makeString(x, "NSFileSystemNumber"), makeNumberInt(x, int64_t(st.st_dev))},
        {makeString(x, "NSFileSystemFileNumber"), makeNumberInt(x, int64_t(st.st_ino))},
        {makeString(x, "NSFileOwnerAccountName"), makeString(x, "mobile")},
        {makeString(x, "NSFileGroupOwnerAccountName"), makeString(x, "mobile")},
        {makeString(x, "NSFileOwnerAccountID"), makeNumberInt(x, 501)},
        {makeString(x, "NSFileGroupOwnerAccountID"), makeNumberInt(x, 501)},
        {makeString(x, "NSFileExtensionHidden"), makeBool(x, false)},
        {makeString(x, "NSFileProtectionKey"), makeString(x, "NSFileProtectionNone")},
    };
    return makeDict(x, e);
}
id fm_attributes(Ctx& x, id, SEL, id path, GuestPtr err) { return attributes(x, P(x, path), false, err); }
id fm_attributesOld(Ctx& x, id, SEL, id path, bool traverse) { return attributes(x, P(x, path), traverse, 0); }
bool fm_setAttributes(Ctx& x, id, SEL, id attrs, id path, GuestPtr) {
    if (const id perm = dictGet(x, attrs, "NSFilePosixPermissions")) ::chmod(P(x, path).c_str(), mode_t(numberInt(x, perm)));
    return true;
}
id fm_fsAttributes(Ctx& x, id, SEL, id, GuestPtr) {
    std::error_code ec;
    const fs::space_info si = fs::space(x.proc.homePath(), ec);
    return makeDict(x, {
        {makeString(x, "NSFileSystemSize"), makeNumberInt(x, ec ? int64_t(16) << 30 : int64_t(si.capacity))},
        {makeString(x, "NSFileSystemFreeSize"), makeNumberInt(x, ec ? int64_t(8) << 30 : int64_t(si.available))},
        {makeString(x, "NSFileSystemNodes"), makeNumberInt(x, 1000000)},
        {makeString(x, "NSFileSystemFreeNodes"), makeNumberInt(x, 500000)},
        {makeString(x, "NSFileSystemNumber"), makeNumberInt(x, 1)},
    });
}
bool fm_access(Ctx& x, id path, int mode) { return path && ::access(P(x, path).c_str(), mode) == 0; }
bool fm_readable(Ctx& x, id, SEL, id p) { return fm_access(x, p, R_OK); }
bool fm_writable(Ctx& x, id, SEL, id p) { return fm_access(x, p, W_OK); }
bool fm_executable(Ctx& x, id, SEL, id p) { return fm_access(x, p, X_OK); }
bool fm_deletable(Ctx& x, id, SEL, id p) {
    if (!p) return false;
    const std::string parent = fs::path(P(x, p)).parent_path().string();
    return ::access(parent.empty() ? "." : parent.c_str(), W_OK) == 0;
}
id fm_currentDir(Ctx& x, id, SEL) { std::error_code ec; return makeString(x, x.proc.guestPath(fs::current_path(ec).string())); }
bool fm_changeDir(Ctx& x, id, SEL, id p) { return ::chdir(P(x, p).c_str()) == 0; }
id fm_urlsForDirectory(Ctx& x, id, SEL, uint32_t dir, uint32_t mask) {
    std::vector<id> out;
    for (auto& s : searchPaths(x, dir, mask, true)) out.push_back(newURL(x, fileURLString(s, true), 0));
    return makeArray(x, out);
}
id fm_urlForDirectory(Ctx& x, id, SEL, uint32_t dir, uint32_t mask, id, bool create, GuestPtr err) {
    auto paths = searchPaths(x, dir, mask ? mask : 1, true);
    if (paths.empty()) { ErrorOut::set(x, err, "NSCocoaErrorDomain", 4, "No such directory"); return 0; }
    std::error_code ec;
    if (create) fs::create_directories(paths[0], ec);
    return newURL(x, fileURLString(paths[0], true), 0);
}
id fm_displayName(Ctx& x, id, SEL, id p) { const std::string s = P(x, p); return makeString(x, s.substr(s.rfind('/') + 1)); }
uint32_t fm_fsRep(Ctx& x, id, SEL, id p) { return id(rt(x).send(x.cpu, p, "fileSystemRepresentation")); }
id fm_stringWithFsRep(Ctx& x, id, SEL, GuestPtr s, uint32_t len) {
    return makeString(x, std::string(reinterpret_cast<const char*>(x.mem.host(s)), len));
}
id fm_nil(Ctx&, id, SEL) { return 0; }
id fm_nil1(Ctx&, id, SEL, id) { return 0; }
void fm_setDelegate(Ctx&, id, SEL, id) {}

// Directory enumerator (snapshot of the recursive listing).
struct DirEnumData : ObjC::HostObject {
    std::vector<std::string> items;
    size_t pos = 0;
    std::string root;
    bool isURL = false;
    uint32_t mutationCell = 0;
};
id fm_enumerator(Ctx& x, id, SEL, id path) {
    const std::string p = P(x, path);
    std::error_code ec;
    if (!fs::is_directory(p, ec)) return 0;
    auto h = std::make_unique<DirEnumData>();
    h->root = p;
    h->isURL = false;
    for (auto it = fs::recursive_directory_iterator(p, ec); !ec && it != fs::recursive_directory_iterator(); it.increment(ec))
        h->items.push_back(fs::relative(it->path(), p, ec).string());
    std::sort(h->items.begin(), h->items.end());
    const id e = rt(x).createInstance(B(x).dirEnum);
    rt(x).setHost(e, std::move(h));
    return autoreleased(x, e);
}
id fm_enumeratorURL(Ctx& x, id self, SEL s, id url, id, uint32_t, uint32_t) {
    const std::string p = filePath(x, url), hp = x.proc.hostPath(p);
    std::error_code ec;
    if (!fs::is_directory(hp, ec)) return 0;
    auto h = std::make_unique<DirEnumData>();
    h->root = p;   // URLs handed back keep the guest's form of the path
    h->isURL = true;
    for (auto it = fs::recursive_directory_iterator(hp, ec); !ec && it != fs::recursive_directory_iterator(); it.increment(ec))
        h->items.push_back(fs::relative(it->path(), hp, ec).string());
    std::sort(h->items.begin(), h->items.end());
    const id e = rt(x).createInstance(B(x).dirEnum);
    rt(x).setHost(e, std::move(h));
    return autoreleased(x, e);
}
id de_next(Ctx& x, id self, SEL) {
    DirEnumData* d = rt(x).hostIf<DirEnumData>(self);
    if (!d || d->pos >= d->items.size()) return 0;
    const std::string item = d->items[d->pos++];
    return d->isURL ? makeFileURL(x, d->root + "/" + item) : makeString(x, item);
}
id de_all(Ctx& x, id self, SEL) {
    DirEnumData* d = rt(x).hostIf<DirEnumData>(self);
    std::vector<id> out;
    if (d) {
        while (d->pos < d->items.size()) {
            const std::string item = d->items[d->pos++];
            out.push_back(d->isURL ? makeFileURL(x, d->root + "/" + item) : makeString(x, item));
        }
    }
    return makeArray(x, out);
}
void de_skip(Ctx& x, id self, SEL) {
    DirEnumData* d = rt(x).hostIf<DirEnumData>(self);
    if (!d || d->pos == 0) return;
    const std::string prefix = d->items[d->pos - 1] + "/";
    while (d->pos < d->items.size() && d->items[d->pos].rfind(prefix, 0) == 0) ++d->pos;
}
id de_fileAttributes(Ctx& x, id self, SEL) {
    DirEnumData* d = rt(x).hostIf<DirEnumData>(self);
    if (!d || d->pos == 0) return 0;
    return attributes(x, d->root + "/" + d->items[d->pos - 1], false, 0);
}
uint32_t de_countByEnumerating(Ctx& x, id self, SEL, GuestPtr state, GuestPtr buf, uint32_t len) {
    DirEnumData* d = rt(x).hostIf<DirEnumData>(self);
    if (!d || !state) return 0;
    uint32_t pos = x.mem.read<uint32_t>(state);
    if (pos == 0) {
        if (!d->mutationCell) d->mutationCell = x.proc.heap().allocZeroed(4);
        x.mem.write<uint32_t>(state + 8, d->mutationCell);
    }
    if (pos >= d->items.size()) return 0;
    const uint32_t n = std::min<uint32_t>(len, uint32_t(d->items.size()) - pos);
    for (uint32_t i = 0; i < n; ++i) {
        id obj = d->isURL ? makeFileURL(x, d->root + "/" + d->items[pos + i])
                          : makeString(x, d->items[pos + i]);
        x.mem.write<uint32_t>(buf + i * 4, obj);
    }
    x.mem.write<uint32_t>(state + 4, buf);
    x.mem.write<uint32_t>(state, pos + n);
    return n;
}
void de_dealloc(Ctx& x, id self, SEL) {
    if (DirEnumData* d = rt(x).hostIf<DirEnumData>(self)) {
        if (d->mutationCell) x.proc.heap().free(d->mutationCell);
    }
    rt(x).disposeInstance(x.cpu, self);
}
void obj_dealloc(Ctx& x, id self, SEL) { rt(x).disposeInstance(x.cpu, self); }

// ------------------------------------------------------------ NSFileHandle ---

struct FileHandleData : ObjC::HostObject {
    FILE* f = nullptr;
    int stdStream = -1;   // 0 in, 1 out, 2 err
    bool closeOnDealloc = true;
    uint64_t fileHandle = 0;
    int64_t bytesRead = 0;
    Process* proc = nullptr;
    ~FileHandleData() override {
        if (fileHandle && proc) proc->recordFileClose(fileHandle);
        if (f && closeOnDealloc) std::fclose(f);
    }
};
id newHandle(Ctx& x, FILE* f, int stdStream) {
    if (!f && stdStream < 0) return 0;
    const id h = rt(x).createInstance(B(x).fileHandle);
    auto d = std::make_unique<FileHandleData>();
    d->f = f;
    d->stdStream = stdStream;
    rt(x).setHost(h, std::move(d));
    return autoreleased(x, h);
}
FileHandleData* fhd(Ctx& x, id self) { return rt(x).hostIf<FileHandleData>(self); }
id fh_reading(Ctx& x, id, SEL, id p) {
    const std::string path = P(x, p);
    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) {
        x.proc.recordFileOpen(path, "NSFileHandle", errno ? errno : ENOENT, -1, 0, false);
        return 0;
    }
    int64_t sz = -1;
    struct stat st;
    if (::fstat(fileno(f), &st) == 0) sz = st.st_size;
    const uint64_t h = x.proc.recordFileOpen(path, "NSFileHandle", 0, sz, 0, true);
    const id handleObj = newHandle(x, f, -1);
    if (FileHandleData* d = fhd(x, handleObj)) {
        d->fileHandle = h;
        d->proc = &x.proc;
    }
    return handleObj;
}
id fh_writing(Ctx& x, id, SEL, id p) {
    // NSFileHandle doesn't create or truncate: the file must exist
    return newHandle(x, std::fopen(P(x, p).c_str(), "r+b"), -1);
}
id fh_updating(Ctx& x, id, SEL, id p) { return newHandle(x, std::fopen(P(x, p).c_str(), "r+b"), -1); }
id fh_readingURL(Ctx& x, id, SEL, id u, GuestPtr err) {
    const std::string p = x.proc.hostPath(filePath(x, u));
    FILE* f = std::fopen(p.c_str(), "rb");
    if (!f) { ErrorOut::posix(x, err, errno, p); return 0; }
    return newHandle(x, f, -1);
}
id fh_stdout(Ctx& x, id, SEL) { return newHandle(x, nullptr, 1); }
id fh_stderr(Ctx& x, id, SEL) { return newHandle(x, nullptr, 2); }
id fh_stdin(Ctx& x, id, SEL) { return newHandle(x, nullptr, 0); }
id fh_null(Ctx& x, id, SEL) { return newHandle(x, std::fopen("/dev/null", "r+b"), -1); }
id fh_readToEnd(Ctx& x, id self, SEL) {
    FileHandleData* d = fhd(x, self);
    std::vector<uint8_t> out;
    if (d && d->f) {
        uint8_t buf[65536];
        size_t n;
        while ((n = std::fread(buf, 1, sizeof buf, d->f)) > 0) {
            out.insert(out.end(), buf, buf + n);
            d->bytesRead += n;
            if (d->proc) d->proc->updateFileRead(d->fileHandle, d->bytesRead);
        }
    }
    return makeData(x, out.data(), uint32_t(out.size()));
}
id fh_readLength(Ctx& x, id self, SEL, uint32_t len) {
    FileHandleData* d = fhd(x, self);
    std::vector<uint8_t> out(len);
    size_t n = d && d->f ? std::fread(out.data(), 1, len, d->f) : 0;
    if (d && n > 0) {
        d->bytesRead += n;
        if (d->proc) d->proc->updateFileRead(d->fileHandle, d->bytesRead);
    }
    return makeData(x, out.data(), uint32_t(n));
}
id fh_available(Ctx& x, id self, SEL s) { return fh_readToEnd(x, self, s); }
void fh_write(Ctx& x, id self, SEL, id data) {
    FileHandleData* d = fhd(x, self);
    if (!d) return;
    const auto b = dataBytes(x, data);
    if (d->stdStream == 1 || d->stdStream == 2) {
        x.proc.log(d->stdStream == 1 ? LogLevel::GuestStdout : LogLevel::GuestStderr, std::string(b.begin(), b.end()));
        return;
    }
    if (d->f) { std::fwrite(b.data(), 1, b.size(), d->f); }
}
uint64_t fh_seekToEnd(Ctx& x, id self, SEL) {
    FileHandleData* d = fhd(x, self);
    if (!d || !d->f) return 0;
    std::fseek(d->f, 0, SEEK_END);
    return uint64_t(std::ftell(d->f));
}
void fh_seekTo(Ctx& x, id self, SEL, uint64_t off) { if (FileHandleData* d = fhd(x, self); d && d->f) std::fseek(d->f, long(off), SEEK_SET); }
uint64_t fh_offset(Ctx& x, id self, SEL) { FileHandleData* d = fhd(x, self); return d && d->f ? uint64_t(std::ftell(d->f)) : 0; }
void fh_truncate(Ctx& x, id self, SEL, uint64_t off) {
    FileHandleData* d = fhd(x, self);
    if (!d || !d->f) return;
    std::fflush(d->f);
    if (::ftruncate(fileno(d->f), off_t(off)) == 0) std::fseek(d->f, long(off), SEEK_SET);
}
void fh_sync(Ctx& x, id self, SEL) { if (FileHandleData* d = fhd(x, self); d && d->f) std::fflush(d->f); }
void fh_close(Ctx& x, id self, SEL) {
    if (FileHandleData* d = fhd(x, self); d) {
        if (d->fileHandle && d->proc) { d->proc->recordFileClose(d->fileHandle); d->fileHandle = 0; }
        if (d->f) { std::fclose(d->f); d->f = nullptr; }
    }
}
int32_t fh_fd(Ctx& x, id self, SEL) { FileHandleData* d = fhd(x, self); return d ? d->stdStream : -1; }

// -------------------------------------------------------- CFBundle / CFURL ---

uint32_t cfb_main(Ctx& x) { return mainBundle(x); }
uint32_t cfb_withIdentifier(Ctx& x, id ident) { return b_withIdentifier(x, 0, 0, ident); }
uint32_t cfb_create(Ctx& x, uint32_t, id url) { mainBundle(x); return rt(x).retain(x.cpu, bundleFor(x, x.proc.hostPath(filePath(x, url)))); }
uint32_t cfb_copyResourceURL(Ctx& x, id bundle, id name, id type, id dir) {
    const std::string p = resourcePathFor(x, bundle, name, type, dir);
    return p.empty() ? 0 : newURL(x, fileURLString(x.proc.guestPath(p), isDirPath(p)), 0, true);
}
uint32_t cfb_copyResourceURLsOfType(Ctx& x, id bundle, id type, id dir) {
    return rt(x).retain(x.cpu, b_urlsForResources(x, bundle, 0, type, dir));
}
uint32_t cfb_copyBundleURL(Ctx& x, id b) { return rt(x).retain(x.cpu, b_bundleURL(x, b, 0)); }
uint32_t cfb_copyResourcesDirURL(Ctx& x, id b) { return rt(x).retain(x.cpu, b_bundleURL(x, b, 0)); }
uint32_t cfb_copyExecutableURL(Ctx& x, id b) { return rt(x).retain(x.cpu, b_executableURL(x, b, 0)); }
uint32_t cfb_identifier(Ctx& x, id b) { const id s = b_identifier(x, b, 0); return s ? constString(x.proc, utf8(x, s)) : 0; }
uint32_t cfb_infoDict(Ctx& x, id b) { return b_infoDictionary(x, b, 0); }
uint32_t cfb_localInfoDict(Ctx& x, id b) { return b_infoDictionary(x, b, 0); }
uint32_t cfb_valueForKey(Ctx& x, id b, id key) { return dictGet(x, b_infoDictionary(x, b, 0), key); }
uint32_t cfb_copyLocalizedString(Ctx& x, id b, id key, id value, id table) { return rt(x).retain(x.cpu, localized(x, b, key, value, table)); }
uint32_t cfb_versionNumber(Ctx&, id) { return 0x01008000; }
uint32_t cfb_copyLocalizations(Ctx& x, id b) { return rt(x).retain(x.cpu, b_localizations(x, b, 0)); }
uint32_t cfb_copyPreferredLocalizations(Ctx& x, id) { return rt(x).retain(x.cpu, b_preferredLocalizations(x, 0, 0)); }
uint32_t cfb_copyInfoDictForURL(Ctx& x, id url) {
    try {
        auto v = Plist::parseFile(x.proc.hostPath(filePath(x, url)) + "/Info.plist");
        return v ? rt(x).retain(x.cpu, plistToObject(x, *v)) : 0;
    } catch (const std::exception&) { return 0; }
}

uint32_t cfu_createWithFsPath(Ctx& x, uint32_t, id path, int32_t, bool isDir) {
    return path ? newURL(x, fileURLString(utf8(x, path), isDir), 0, true) : 0;
}
uint32_t cfu_createFromFsRep(Ctx& x, uint32_t, GuestPtr buf, int32_t len, bool isDir) {
    return newURL(x, fileURLString(std::string(reinterpret_cast<const char*>(x.mem.host(buf)), uint32_t(len)), isDir), 0, true);
}
uint32_t cfu_createWithString(Ctx& x, uint32_t, id str, id base) {
    if (!str) return 0;
    const std::string s = utf8(x, str);
    return validURLString(s) ? newURL(x, s, base, true) : 0;
}
uint32_t cfu_createWithBytes(Ctx& x, uint32_t, GuestPtr p, int32_t n, uint32_t, id base) {
    return newURL(x, std::string(reinterpret_cast<const char*>(x.mem.host(p)), uint32_t(n)), base, true);
}
// CFURLCreateAbsoluteURLWithBytes(alloc, bytes, length, encoding, baseURL, useCompatibilityMode):
// the URL resolved against its base (Army of Darkness opens its sounds this way).
uint32_t cfu_createAbsoluteWithBytes(Ctx& x, uint32_t, GuestPtr p, int32_t n, uint32_t, id base, uint32_t) {
    if (!p || n < 0) return 0;
    const std::string s(reinterpret_cast<const char*>(x.mem.host(p)), uint32_t(n));
    if (!base) return newURL(x, s, 0, true);
    const id relative = newURL(x, s, base);
    return newURL(x, absoluteString(x, relative), 0, true);
}
bool cfu_getFsRep(Ctx& x, id url, bool, GuestPtr buf, int32_t max) {
    const std::string p = filePath(x, url);
    if (int32_t(p.size()) + 1 > max) return false;
    std::memcpy(x.mem.host(buf), p.c_str(), p.size() + 1);
    return true;
}
uint32_t cfu_copyFsPath(Ctx& x, id url, int32_t) { return makeString(x, filePath(x, url), true); }
uint32_t cfu_getString(Ctx& x, id url) { UrlData* d = ud(x, url); return d ? constString(x.proc, d->str) : 0; }
uint32_t cfu_copyAbsoluteURL(Ctx& x, id url) { return rt(x).retain(x.cpu, url_absoluteURL(x, url, 0)); }
uint32_t cfu_getBaseURL(Ctx& x, id url) { return url_baseURL(x, url, 0); }
uint32_t cfu_copyPathExtension(Ctx& x, id url) { return rt(x).retain(x.cpu, url_pathExtension(x, url, 0)); }
uint32_t cfu_copyLastPathComponent(Ctx& x, id url) { return rt(x).retain(x.cpu, url_lastPathComponent(x, url, 0)); }
uint32_t cfu_copyScheme(Ctx& x, id url) { return rt(x).retain(x.cpu, url_scheme(x, url, 0)); }
uint32_t cfu_copyHost(Ctx& x, id url) { return rt(x).retain(x.cpu, url_host(x, url, 0)); }
uint32_t cfu_copyPath(Ctx& x, id url) { return rt(x).retain(x.cpu, url_path(x, url, 0)); }
uint32_t cfu_copyQuery(Ctx& x, id url, id) { return rt(x).retain(x.cpu, url_query(x, url, 0)); }
uint32_t cfu_appendComponent(Ctx& x, uint32_t, id url, id comp, bool dir) { return rt(x).retain(x.cpu, url_appendingComponentDir(x, url, 0, comp, dir)); }
uint32_t cfu_appendExtension(Ctx& x, uint32_t, id url, id ext) { return rt(x).retain(x.cpu, url_appendingExtension(x, url, 0, ext)); }
uint32_t cfu_deleteLast(Ctx& x, uint32_t, id url) { return rt(x).retain(x.cpu, url_deletingLast(x, url, 0)); }
uint32_t cfu_deleteExtension(Ctx& x, uint32_t, id url) { return rt(x).retain(x.cpu, url_deletingExtension(x, url, 0)); }
uint32_t cfu_createStringByReplacingPercentEscapes(Ctx& x, uint32_t, id s, id) { return makeString(x, percentDecode(utf8(x, s)), true); }
uint32_t cfu_createStringByAddingPercentEscapes(Ctx& x, uint32_t, id s, id leave, id force, uint32_t) {
    std::string keep = "-._~!$&'()*+,;=:@/?#[]";
    if (force) for (char c : utf8(x, force)) keep.erase(std::remove(keep.begin(), keep.end(), c), keep.end());
    if (leave) keep += utf8(x, leave);
    return makeString(x, percentEncode(utf8(x, s), keep.c_str()), true);
}

} // namespace

// Shared with UIKit: resource lookup in the main bundle, file URLs.
std::string bundleResource(Ctx& x, const std::string& name, const std::string& type, const std::string& subdir) {
    mainBundle(x);
    return findResource(x, x.proc.bundlePath(), name, type, subdir);
}
id makeFileURL(Ctx& x, const std::string& path, bool isDir) { return newURL(x, fileURLString(path, isDir), 0); }
std::string urlFilePath(Ctx& x, id url) { return x.proc.hostPath(filePath(x, url)); }
std::string urlString(Ctx& x, id url) { return absoluteString(x, url); }
bool isURL(Ctx& x, id obj) { return ud(x, obj) != nullptr; }
id localizedString(Ctx& x, id key, id value, id table) { return localized(x, mainBundle(x), key, value, table); }

void registerBundle(Process& p, ObjC::Runtime& r) {
    auto& b = B(p);
    ObjC::ClassBuilder url = r.defineClass("NSURL", "NSObject");
    b.nsURL = url.cls();
    url.cm<&urlc_fileURL>("fileURLWithPath:").cm<&urlc_fileURLDir>("fileURLWithPath:isDirectory:")
       .cm<&urlc_string>("URLWithString:").cm<&urlc_stringRel>("URLWithString:relativeToURL:");
    url.m<&url_initFile>("initFileURLWithPath:").m<&url_initFileDir>("initFileURLWithPath:isDirectory:")
       .m<&url_initString>("initWithString:").m<&url_initStringRel>("initWithString:relativeToURL:")
       .m<&url_initWithScheme>("initWithScheme:host:path:")
       .m<&url_absoluteString>("absoluteString").m<&url_relativeString>("relativeString").m<&url_baseURL>("baseURL")
       .m<&url_absoluteURL>("absoluteURL").m<&url_path>("path").m<&url_relativePath>("relativePath")
       .m<&url_isFile>("isFileURL", "c@:").m<&url_scheme>("scheme").m<&url_host>("host").m<&url_port>("port")
       .m<&url_query>("query").m<&url_fragment>("fragment").m<&url_user>("user").m<&url_password>("password")
       .m<&url_params>("parameterString").m<&url_resourceSpecifier>("resourceSpecifier")
       .m<&url_lastPathComponent>("lastPathComponent").m<&url_pathExtension>("pathExtension")
       .m<&url_pathComponents>("pathComponents").m<&url_appendingComponent>("URLByAppendingPathComponent:")
       .m<&url_appendingComponentDir>("URLByAppendingPathComponent:isDirectory:")
       .m<&url_appendingExtension>("URLByAppendingPathExtension:").m<&url_deletingLast>("URLByDeletingLastPathComponent")
       .m<&url_deletingExtension>("URLByDeletingPathExtension").m<&url_standardized>("standardizedURL")
       .m<&url_standardized>("URLByStandardizingPath").m<&url_standardized>("URLByResolvingSymlinksInPath")
       .m<&url_isEqual>("isEqual:", "c@:@").m<&url_hash>("hash", "I@:").m<&url_description>("description")
       .m<&url_fsRep>("fileSystemRepresentation", "r*@:").m<&url_reachable>("checkResourceIsReachableAndReturnError:", "c@:^@")
       .m<&url_setResourceValue>("setResourceValue:forKey:error:", "c@:@@^@")
       .m<&url_getResourceValue>("getResourceValue:forKey:error:", "c@:^@@^@")
       .m<&url_copy>("copyWithZone:").m<&url_dealloc>("dealloc", "v@:");

    ObjC::ClassBuilder bun = r.defineClass("NSBundle", "NSObject");
    b.nsBundle = bun.cls();
    bun.cm<&b_main>("mainBundle").cm<&b_withPath>("bundleWithPath:").cm<&b_withURL>("bundleWithURL:")
       .cm<&b_forClass>("bundleForClass:").cm<&b_withIdentifier>("bundleWithIdentifier:").cm<&b_allBundles>("allBundles")
       .cm<&b_allFrameworks>("allFrameworks").cm<&bc_pathForResourceInDir>("pathForResource:ofType:inDirectory:")
       .cm<&b_preferredLocalizations>("preferredLocalizationsFromArray:");
    bun.m<&b_initWithPath>("initWithPath:").m<&b_bundlePath>("bundlePath").m<&b_resourcePath>("resourcePath")
       .m<&b_executablePath>("executablePath").m<&b_privateFrameworksPath>("privateFrameworksPath")
       .m<&b_builtInPlugInsPath>("builtInPlugInsPath").m<&b_bundleURL>("bundleURL").m<&b_bundleURL>("resourceURL")
       .m<&b_executableURL>("executableURL").m<&b_identifier>("bundleIdentifier").m<&b_infoDictionary>("infoDictionary")
       .m<&b_localizedInfo>("localizedInfoDictionary").m<&b_objectForInfoKey>("objectForInfoDictionaryKey:")
       .m<&b_pathForResource>("pathForResource:ofType:").m<&b_pathForResourceDir>("pathForResource:ofType:inDirectory:")
       .m<&b_pathForResourceDirLoc>("pathForResource:ofType:inDirectory:forLocalization:")
       .m<&b_pathsForResources>("pathsForResourcesOfType:inDirectory:")
       .m<&b_urlForResource>("URLForResource:withExtension:").m<&b_urlForResourceDir>("URLForResource:withExtension:subdirectory:")
       .m<&b_urlsForResources>("URLsForResourcesWithExtension:subdirectory:")
       .m<&b_localizedString>("localizedStringForKey:value:table:").m<&b_localizations>("localizations")
       .m<&b_preferredLocalizations>("preferredLocalizations").m<&b_devLocalization>("developmentLocalization")
       .m<&b_true>("load", "c@:").m<&b_true>("isLoaded", "c@:").m<&b_true>("unload", "c@:")
       .m<&b_principalClass>("principalClass", "#@:").m<&b_classNamed>("classNamed:", "#@:@")
       .m<&b_description>("description");

    p.def<&f_searchPaths>("_NSSearchPathForDirectoriesInDomains");
    p.def<&f_homeDirectory>("_NSHomeDirectory");
    p.def<&f_homeDirectoryForUser>("_NSHomeDirectoryForUser");
    p.def<&f_temporaryDirectory>("_NSTemporaryDirectory");
    p.def<&f_openStepRoot>("_NSOpenStepRootDirectory");

    ObjC::ClassBuilder fm = r.defineClass("NSFileManager", "NSObject");
    b.fileManager = fm.cls();
    fm.cm<&fm_default>("defaultManager");
    fm.m<&fm_init>("init").m<&fm_exists>("fileExistsAtPath:", "c@:@").m<&fm_existsDir>("fileExistsAtPath:isDirectory:", "c@:@^c")
      .m<&fm_createDir>("createDirectoryAtPath:withIntermediateDirectories:attributes:error:", "c@:@c@^@")
      .m<&fm_createDirOld>("createDirectoryAtPath:attributes:", "c@:@@")
      .m<&fm_createDirURL>("createDirectoryAtURL:withIntermediateDirectories:attributes:error:", "c@:@c@^@")
      .m<&fm_createFile>("createFileAtPath:contents:attributes:", "c@:@@@")
      .m<&fm_remove>("removeItemAtPath:error:", "c@:@^@").m<&fm_removeOld>("removeFileAtPath:handler:", "c@:@@")
      .m<&fm_removeURL>("removeItemAtURL:error:", "c@:@^@")
      .m<&fm_copy>("copyItemAtPath:toPath:error:", "c@:@@^@").m<&fm_copyURL>("copyItemAtURL:toURL:error:", "c@:@@^@")
      .m<&fm_move>("moveItemAtPath:toPath:error:", "c@:@@^@").m<&fm_moveURL>("moveItemAtURL:toURL:error:", "c@:@@^@")
      .m<&fm_link>("linkItemAtPath:toPath:error:", "c@:@@^@")
      .m<&fm_symlink>("createSymbolicLinkAtPath:withDestinationPath:error:", "c@:@@^@")
      .m<&fm_destOfSymlink>("destinationOfSymbolicLinkAtPath:error:")
      .m<&fm_contents>("contentsOfDirectoryAtPath:error:").m<&fm_contentsOld>("directoryContentsAtPath:")
      .m<&fm_contentsURL>("contentsOfDirectoryAtURL:includingPropertiesForKeys:options:error:")
      .m<&fm_subpaths>("subpathsOfDirectoryAtPath:error:").m<&fm_subpathsOld>("subpathsAtPath:")
      .m<&fm_contentsAtPath>("contentsAtPath:").m<&fm_contentsEqual>("contentsEqualAtPath:andPath:", "c@:@@")
      .m<&fm_attributes>("attributesOfItemAtPath:error:").m<&fm_attributesOld>("fileAttributesAtPath:traverseLink:")
      .m<&fm_setAttributes>("setAttributes:ofItemAtPath:error:", "c@:@@^@")
      .m<&fm_fsAttributes>("attributesOfFileSystemForPath:error:")
      .m<&fm_readable>("isReadableFileAtPath:", "c@:@").m<&fm_writable>("isWritableFileAtPath:", "c@:@")
      .m<&fm_executable>("isExecutableFileAtPath:", "c@:@").m<&fm_deletable>("isDeletableFileAtPath:", "c@:@")
      .m<&fm_currentDir>("currentDirectoryPath").m<&fm_changeDir>("changeCurrentDirectoryPath:", "c@:@")
      .m<&fm_urlsForDirectory>("URLsForDirectory:inDomains:")
      .m<&fm_urlForDirectory>("URLForDirectory:inDomain:appropriateForURL:create:error:")
      .m<&fm_displayName>("displayNameAtPath:").m<&fm_fsRep>("fileSystemRepresentationWithPath:", "r*@:@")
      .m<&fm_stringWithFsRep>("stringWithFileSystemRepresentation:length:")
      .m<&fm_enumerator>("enumeratorAtPath:")
      .m<&fm_enumeratorURL>("enumeratorAtURL:includingPropertiesForKeys:options:errorHandler:")
      .m<&fm_nil>("ubiquityIdentityToken").m<&fm_nil1>("URLForUbiquityContainerIdentifier:")
      .m<&fm_setDelegate>("setDelegate:", "v@:@").m<&fm_nil>("delegate");

    ObjC::ClassBuilder de = r.defineClass("NSDirectoryEnumerator", "NSEnumerator");
    b.dirEnum = de.cls();
    de.m<&de_next>("nextObject").m<&de_all>("allObjects").m<&de_skip>("skipDescendents", "v@:")
      .m<&de_skip>("skipDescendants", "v@:").m<&de_fileAttributes>("fileAttributes")
      .m<&de_countByEnumerating>("countByEnumeratingWithState:objects:count:", "I@:^v^@I")
      .m<&de_dealloc>("dealloc", "v@:");

    ObjC::ClassBuilder fh = r.defineClass("NSFileHandle", "NSObject");
    b.fileHandle = fh.cls();
    fh.cm<&fh_reading>("fileHandleForReadingAtPath:").cm<&fh_writing>("fileHandleForWritingAtPath:")
      .cm<&fh_updating>("fileHandleForUpdatingAtPath:").cm<&fh_readingURL>("fileHandleForReadingFromURL:error:")
      .cm<&fh_stdout>("fileHandleWithStandardOutput").cm<&fh_stderr>("fileHandleWithStandardError")
      .cm<&fh_stdin>("fileHandleWithStandardInput").cm<&fh_null>("fileHandleWithNullDevice");
    fh.m<&fh_readToEnd>("readDataToEndOfFile").m<&fh_readLength>("readDataOfLength:").m<&fh_available>("availableData")
      .m<&fh_write>("writeData:", "v@:@").m<&fh_seekToEnd>("seekToEndOfFile", "Q@:").m<&fh_seekTo>("seekToFileOffset:", "v@:Q")
      .m<&fh_offset>("offsetInFile", "Q@:").m<&fh_truncate>("truncateFileAtOffset:", "v@:Q").m<&fh_sync>("synchronizeFile", "v@:")
      .m<&fh_close>("closeFile", "v@:").m<&fh_fd>("fileDescriptor", "i@:").m<&obj_dealloc>("dealloc", "v@:");

    // NSDictionary (NSFileAttributes)
    r.builder(r.getClass("NSDictionary"))
        .m<&fa_size>("fileSize", "Q@:").m<&fa_obj<0>>("fileModificationDate").m<&fa_obj<1>>("fileCreationDate")
        .m<&fa_obj<2>>("fileType").m<&fa_obj<3>>("fileOwnerAccountName").m<&fa_obj<4>>("fileGroupOwnerAccountName")
        .m<&fa_num<0>>("filePosixPermissions", "I@:").m<&fa_num<1>>("fileSystemNumber", "I@:")
        .m<&fa_num<2>>("fileSystemFileNumber", "I@:").m<&fa_num<3>>("fileOwnerAccountID")
        .m<&fa_num<4>>("fileGroupOwnerAccountID").m<&fa_bool<0>>("fileExtensionHidden", "c@:")
        .m<&fa_bool<1>>("fileIsImmutable", "c@:").m<&fa_bool<2>>("fileIsAppendOnly", "c@:");

    for (const char* n : {"NSFileSize", "NSFileType", "NSFileModificationDate", "NSFileCreationDate", "NSFilePosixPermissions",
                          "NSFileReferenceCount", "NSFileSystemNumber", "NSFileSystemFileNumber", "NSFileOwnerAccountName",
                          "NSFileGroupOwnerAccountName", "NSFileOwnerAccountID", "NSFileGroupOwnerAccountID", "NSFileExtensionHidden",
                          "NSFileProtectionKey", "NSFileProtectionNone", "NSFileProtectionComplete",
                          "NSFileProtectionCompleteUnlessOpen", "NSFileProtectionCompleteUntilFirstUserAuthentication",
                          "NSFileTypeDirectory", "NSFileTypeRegular", "NSFileTypeSymbolicLink", "NSFileTypeSocket",
                          "NSFileTypeCharacterSpecial", "NSFileTypeBlockSpecial", "NSFileTypeUnknown", "NSFileSystemSize",
                          "NSFileSystemFreeSize", "NSFileSystemNodes", "NSFileSystemFreeNodes", "NSFileImmutable", "NSFileAppendOnly",
                          "NSFileBusy", "NSFileDeviceIdentifier",
                          "NSURLIsDirectoryKey", "NSURLIsRegularFileKey", "NSURLFileSizeKey", "NSURLNameKey",
                          "NSURLIsExcludedFromBackupKey", "NSURLContentModificationDateKey", "NSURLCreationDateKey",
                          "NSURLLocalizedNameKey", "NSURLIsHiddenKey", "NSURLFileResourceTypeKey", "NSURLTotalFileSizeKey",
                          "NSURLFileAllocatedSizeKey", "NSURLIsSymbolicLinkKey", "NSURLIsPackageKey", "NSURLTypeIdentifierKey"}) {
        const uint32_t cell = p.defineDataBlock(std::string("_") + n, 4);
        p.mem().write<uint32_t>(cell, constString(p, n));
    }

    p.def<&cfb_main>("_CFBundleGetMainBundle");
    p.def<&cfb_withIdentifier>("_CFBundleGetBundleWithIdentifier");
    p.def<&cfb_create>("_CFBundleCreate");
    p.def<&cfb_copyResourceURL>("_CFBundleCopyResourceURL");
    p.def<&cfb_copyResourceURLsOfType>("_CFBundleCopyResourceURLsOfType");
    p.def<&cfb_copyBundleURL>("_CFBundleCopyBundleURL");
    p.def<&cfb_copyResourcesDirURL>("_CFBundleCopyResourcesDirectoryURL");
    p.def<&cfb_copyExecutableURL>("_CFBundleCopyExecutableURL");
    p.def<&cfb_identifier>("_CFBundleGetIdentifier");
    p.def<&cfb_infoDict>("_CFBundleGetInfoDictionary");
    p.def<&cfb_localInfoDict>("_CFBundleGetLocalInfoDictionary");
    p.def<&cfb_valueForKey>("_CFBundleGetValueForInfoDictionaryKey");
    p.def<&cfb_copyLocalizedString>("_CFBundleCopyLocalizedString");
    p.def<&cfb_versionNumber>("_CFBundleGetVersionNumber");
    p.def<&cfb_copyLocalizations>("_CFBundleCopyBundleLocalizations");
    p.def<&cfb_copyPreferredLocalizations>("_CFBundleCopyPreferredLocalizationsFromArray");
    p.def<&cfb_copyInfoDictForURL>("_CFBundleCopyInfoDictionaryForURL");
    p.def<&cfb_copyInfoDictForURL>("_CFBundleCopyInfoDictionaryInDirectory");

    p.def<&cfu_createWithFsPath>("_CFURLCreateWithFileSystemPath");
    p.def<&cfu_createFromFsRep>("_CFURLCreateFromFileSystemRepresentation");
    p.def<&cfu_createWithString>("_CFURLCreateWithString");
    p.def<&cfu_createWithBytes>("_CFURLCreateWithBytes");
    p.def<&cfu_createAbsoluteWithBytes>("_CFURLCreateAbsoluteURLWithBytes");
    p.def<&cfu_getFsRep>("_CFURLGetFileSystemRepresentation");
    p.def<&cfu_copyFsPath>("_CFURLCopyFileSystemPath");
    p.def<&cfu_getString>("_CFURLGetString");
    p.def<&cfu_copyAbsoluteURL>("_CFURLCopyAbsoluteURL");
    p.def<&cfu_getBaseURL>("_CFURLGetBaseURL");
    p.def<&cfu_copyPathExtension>("_CFURLCopyPathExtension");
    p.def<&cfu_copyLastPathComponent>("_CFURLCopyLastPathComponent");
    p.def<&cfu_copyScheme>("_CFURLCopyScheme");
    p.def<&cfu_copyHost>("_CFURLCopyHostName");
    p.def<&cfu_copyPath>("_CFURLCopyPath");
    p.def<&cfu_copyQuery>("_CFURLCopyQueryString");
    p.def<&cfu_appendComponent>("_CFURLCreateCopyAppendingPathComponent");
    p.def<&cfu_appendExtension>("_CFURLCreateCopyAppendingPathExtension");
    p.def<&cfu_deleteLast>("_CFURLCreateCopyDeletingLastPathComponent");
    p.def<&cfu_deleteExtension>("_CFURLCreateCopyDeletingPathExtension");
    p.def<&cfu_createStringByReplacingPercentEscapes>("_CFURLCreateStringByReplacingPercentEscapes");
    p.def<&cfu_createStringByReplacingPercentEscapes>("_CFURLCreateStringByReplacingPercentEscapesUsingEncoding");
    p.def<&cfu_createStringByAddingPercentEscapes>("_CFURLCreateStringByAddingPercentEscapes");
}

} // namespace Hydrogen::Runtime::Foundation
