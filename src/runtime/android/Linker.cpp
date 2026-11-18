// HydrogenVM — the dynamic linker for Android games: every native library of the APK, bound to
// HydrogenVM's system libraries and to each other, and libdl (dlopen, dlsym, dladdr, ...).
// See Android.hpp.
//
// Symbol lookup follows Android's linker for a library's imports: the system libraries (here
// the "android:" definitions, then the iOS functions of the same name) come before the app's
// own libraries, because the zygote loaded them first; among the app's libraries, load order.
// A library's references to its own symbols were bound when it was loaded (no interposition).
#include "runtime/android/Android.hpp"
#include "runtime/android/BionicInternal.hpp"
#include "runtime/Support.hpp"
#include "core/cpu/Engine.hpp"
#include <algorithm>
#include <cxxabi.h>
#include <filesystem>
#include <functional>
#include <set>
#include <unordered_map>

namespace fs = std::filesystem;

namespace Hydrogen::Runtime::Android {

std::optional<uint32_t> Library::find(const std::string& symbol) const {
    for (const auto& l : libs) {
        auto it = l->so.image.symbols.find(symbol);
        if (it != l->so.image.symbols.end()) return it->second;
    }
    return std::nullopt;
}
const LoadedLib* Library::containing(uint32_t addr) const {
    for (const auto& l : libs)
        if (addr >= l->so.image.lowest && addr < l->so.image.highest) return l.get();
    return nullptr;
}
bool Library::needs(const std::string& soname) const {
    for (const auto& l : libs)
        if (std::find(l->so.needed.begin(), l->so.needed.end(), soname) != l->so.needed.end()) return true;
    return false;
}

namespace {

std::string demangled(const std::string& s) {
    int st = 0;
    char* d = abi::__cxa_demangle(s.c_str(), nullptr, nullptr, &st);
    std::string out = st == 0 && d ? d : s;
    std::free(d);
    return out;
}

// The libraries Android itself provides: never looked for in the APK.
bool isSystemLibrary(const std::string& n) {
    static const std::set<std::string> kSystem = {
        "libc.so", "libm.so", "libdl.so", "liblog.so", "libz.so", "libstdc++.so", "libandroid.so",
        "libEGL.so", "libGLESv1_CM.so", "libGLESv2.so", "libGLESv3.so", "libOpenSLES.so", "libOpenMAXAL.so",
        "libjnigraphics.so", "libmediandk.so", "libvulkan.so", "libc++.so", "libutils.so", "libcutils.so",
        "libbinder.so", "libui.so", "libgui.so", "libmedia.so", "libicuuc.so", "libicui18n.so", "libssl.so",
        "libcrypto.so", "libsqlite.so", "libnativehelper.so",
    };
    return kSystem.count(n) != 0;
}

// Maps one library file at the next free base.
LoadedLib* mapLibrary(Process& p, Library& L, const std::string& path) {
    const std::string name = fs::path(path).filename().string();
    for (const auto& l : L.libs) if (l->name == name) return l.get();
    auto lib = std::make_unique<LoadedLib>();
    lib->name = name;
    lib->path = path;
    lib->so = Loader::loadElfShared(p.mem(), Loader::readFile(path), L.nextBase);
    L.nextBase = (lib->so.image.highest + 0xFFFFFu) & ~0xFFFFFu;   // next 1 MB boundary
    p.logf(LogLevel::Info, "Loaded %s at 0x%08x: %zu imports, %zu initializers", name.c_str(), lib->so.image.lowest,
           lib->so.imports.size(), lib->so.initializers.size());
    L.libs.push_back(std::move(lib));
    return L.libs.back().get();
}

// Maps `path` after the libraries of the APK it needs (depth first), so load order is
// dependency order. A needed library that is neither a system one nor in the APK is reported.
LoadedLib* mapWithDependencies(Process& p, Library& L, const std::string& path, std::set<std::string>& visiting) {
    const std::string name = fs::path(path).filename().string();
    for (const auto& l : L.libs) if (l->name == name) return l.get();
    if (!visiting.insert(name).second) return nullptr;   // a cycle: the other side maps it
    // Read DT_NEEDED without mapping: map into scratch space first would waste the address; the
    // ELF loader only gives `needed` after mapping, so map now and order the list afterwards.
    LoadedLib* lib = mapLibrary(p, L, path);
    const size_t at = L.libs.size() - 1;
    for (const auto& n : lib->so.needed) {
        if (isSystemLibrary(n)) continue;
        const fs::path dep = fs::path(L.libDir) / n;
        if (!fs::exists(dep)) {
            p.logf(LogLevel::Warning, "Android: %s needs %s, which the APK doesn't have", name.c_str(), n.c_str());
            continue;
        }
        mapWithDependencies(p, L, dep.string(), visiting);
    }
    // Dependencies mapped above go before this library in the load order.
    auto it = std::find_if(L.libs.begin(), L.libs.end(), [&](const auto& l) { return l.get() == lib; });
    if (size_t(it - L.libs.begin()) == at && at + 1 < L.libs.size()) {
        auto owned = std::move(*it);
        L.libs.erase(it);
        L.libs.push_back(std::move(owned));
    }
    return lib;
}

// Binds a library's imports. Returns the demangled names nothing provides.
std::vector<std::string> bind(Process& p, Library& L, LoadedLib& lib) {
    std::vector<std::string> missing;
    std::unordered_map<std::string, uint32_t> cache;
    for (const auto& imp : lib.so.imports) {
        uint32_t target;
        if (auto it = cache.find(imp.symbol); it != cache.end()) target = it->second;
        else {
            const bool host = p.lookupSymbol("android:" + imp.symbol) || p.lookupSymbol("_" + imp.symbol);
            std::optional<uint32_t> fromLib;
            if (!host)
                for (const auto& other : L.libs) {
                    if (other.get() == &lib) continue;
                    auto s = other->so.image.symbols.find(imp.symbol);
                    if (s != other->so.image.symbols.end()) { fromLib = s->second; break; }
                }
            if (host || !fromLib) target = resolveImport(p, imp.symbol, imp.weak);
            else target = *fromLib;
            cache.emplace(imp.symbol, target);
            if (!host && !fromLib && target) missing.push_back(demangled(imp.symbol));
        }
        if (imp.type == 2) p.mem().write<uint32_t>(imp.address, p.mem().read<uint32_t>(imp.address) + target);
        else p.mem().write<uint32_t>(imp.address, target);
    }
    lib.bound = true;
    return missing;
}

void initialize(Process& p, CPU::Cpu& c, LoadedLib& lib) {
    if (lib.initialized) return;
    lib.initialized = true;   // first: an initializer that dlopens itself doesn't run twice
    for (const uint32_t fn : lib.so.initializers) p.callGuest(c, fn, {});
}

} // namespace

std::vector<std::string> loadLibraries(Process& p, const std::string& mainPath) {
    auto& L = p.service<Library>();
    std::lock_guard<std::recursive_mutex> lk(L.mu);
    L.libDir = fs::path(mainPath).parent_path().string();
    std::set<std::string> visiting;
    L.main = mapWithDependencies(p, L, mainPath, visiting);
    L.name = L.main->name;
    // The other libraries of the folder: Java loads them with System.loadLibrary.
    std::vector<fs::path> others;
    std::error_code ec;
    for (const auto& e : fs::directory_iterator(L.libDir, ec))
        if (e.path().extension() == ".so") others.push_back(e.path());
    std::sort(others.begin(), others.end());
    for (const auto& o : others) mapWithDependencies(p, L, o.string(), visiting);
    std::vector<std::string> missing;
    std::set<std::string> seen;
    for (const auto& l : L.libs)
        for (auto& m : bind(p, L, *l))
            if (seen.insert(m).second) missing.push_back(m);
    return missing;
}

void runInitializers(Process& p, CPU::Cpu& c) {
    auto& L = p.service<Library>();
    std::vector<LoadedLib*> order;
    {
        std::lock_guard<std::recursive_mutex> lk(L.mu);
        for (auto& l : L.libs) order.push_back(l.get());
    }
    for (LoadedLib* l : order) initialize(p, c, *l);
}

// ------------------------------------------------------------------- libdl ---

namespace {

// Handles: a game library is 0x10000 + its index; a system library is 0x20000 + an index into
// the names opened so far. RTLD_DEFAULT and RTLD_NEXT are bionic's 32-bit values.
constexpr uint32_t kGameHandle = 0x10000, kSystemHandle = 0x20000, kRtldDefault = 0xFFFFFFFFu, kRtldNext = 0xFFFFFFFEu;

struct Dl {
    explicit Dl(Process&) {}
    std::mutex mu;
    std::vector<std::string> systemNames;
    std::string error;
    uint32_t errorBuf = 0;
    std::unordered_map<uint32_t, uint32_t> names;   // dladdr: library -> its guest file name
};
Dl& D(Ctx& x) { return x.proc.service<Dl>(); }
void setError(Ctx& x, std::string e) { auto& d = D(x); std::lock_guard<std::mutex> lk(d.mu); d.error = std::move(e); }

uint32_t dl_open(Ctx& x, GuestPtr fileP, int32_t) {
    auto& L = x.proc.service<Library>();
    if (!fileP) return kRtldDefault;   // the main program: global lookup
    std::string file = x.proc.readCString(fileP, 1024);
    const std::string name = fs::path(file).filename().string();
    std::lock_guard<std::recursive_mutex> lk(L.mu);
    for (size_t i = 0; i < L.libs.size(); ++i)
        if (L.libs[i]->name == name) {
            if (!L.libs[i]->initialized) initialize(x.proc, x.cpu, *L.libs[i]);
            return kGameHandle + uint32_t(i);
        }
    if (isSystemLibrary(name)) {
        auto& d = D(x);
        std::lock_guard<std::mutex> dk(d.mu);
        for (size_t i = 0; i < d.systemNames.size(); ++i) if (d.systemNames[i] == name) return kSystemHandle + uint32_t(i);
        d.systemNames.push_back(name);
        return kSystemHandle + uint32_t(d.systemNames.size() - 1);
    }
    // A library of the APK that wasn't in the ABI folder (a path in the app's data, say).
    const std::string host = x.proc.hostPath(file);
    std::error_code ec;
    if (!fs::is_regular_file(host, ec)) {
        setError(x, "dlopen failed: library \"" + file + "\" not found");
        return 0;
    }
    try {
        std::set<std::string> visiting;
        LoadedLib* lib = mapWithDependencies(x.proc, L, host, visiting);
        for (auto& l : L.libs) if (!l->bound) for (auto& m : bind(x.proc, L, *l)) x.proc.logf(LogLevel::Info, "%s: %s isn't implemented yet", l->name.c_str(), m.c_str());
        for (auto& l : L.libs) if (!l->initialized) initialize(x.proc, x.cpu, *l);
        for (size_t i = 0; i < L.libs.size(); ++i) if (L.libs[i].get() == lib) return kGameHandle + uint32_t(i);
    } catch (const std::exception& e) {
        setError(x, std::string("dlopen failed: ") + e.what());
    }
    return 0;
}

uint32_t dl_sym(Ctx& x, uint32_t handle, GuestPtr nameP) {
    const std::string name = x.proc.readCString(nameP, 1024);
    auto& L = x.proc.service<Library>();
    std::lock_guard<std::recursive_mutex> lk(L.mu);
    auto inGame = [&]() -> uint32_t { auto a = L.find(name); return a ? *a : 0; };
    auto inSystem = [&]() -> uint32_t {
        if (auto a = x.proc.lookupSymbol("android:" + name)) return *a;
        if (auto a = x.proc.lookupSymbol("_" + name)) return *a;
        return 0;
    };
    uint32_t r = 0;
    if (handle >= kGameHandle && handle < kGameHandle + L.libs.size()) {
        const auto& syms = L.libs[handle - kGameHandle]->so.image.symbols;
        auto it = syms.find(name);
        r = it != syms.end() ? it->second : inGame();   // then its dependencies: the global scope
        if (!r) r = inSystem();
    } else if (handle >= kSystemHandle && handle < kSystemHandle + 0x10000) {
        r = inSystem();
    } else if (handle == kRtldDefault || handle == kRtldNext) {
        r = inSystem();
        if (!r) r = inGame();
    }
    if (!r) setError(x, "undefined symbol: " + name);
    return r;
}
int32_t dl_close(Ctx&, uint32_t) { return 0; }   // libraries stay mapped, as most of Android's do
uint32_t dl_error(Ctx& x) {
    auto& d = D(x);
    std::lock_guard<std::mutex> lk(d.mu);
    if (d.error.empty()) return 0;
    if (d.errorBuf) x.proc.heap().free(d.errorBuf);
    d.errorBuf = x.proc.allocCString(d.error);
    d.error.clear();
    return d.errorBuf;
}
// dladdr(addr, Dl_info*): Dl_info is {dli_fname, dli_fbase, dli_sname, dli_saddr}.
int32_t dl_addr(Ctx& x, uint32_t addr, GuestPtr info) {
    auto& L = x.proc.service<Library>();
    std::lock_guard<std::recursive_mutex> lk(L.mu);
    const LoadedLib* lib = L.containing(addr & ~1u);
    if (!lib || !info) return 0;
    auto& d = D(x);
    uint32_t fname;
    {
        std::lock_guard<std::mutex> dk(d.mu);
        uint32_t& cell = d.names[lib->so.image.lowest];
        if (!cell) cell = x.proc.internCString(x.proc.guestPath(lib->path));
        fname = cell;
    }
    uint32_t best = 0;
    std::string bestName;
    for (const auto& [n, a] : lib->so.image.symbols)
        if ((a & ~1u) <= (addr & ~1u) && (a & ~1u) >= (best & ~1u)) { best = a; bestName = n; }
    x.mem.write<uint32_t>(info, fname);
    x.mem.write<uint32_t>(info + 4, lib->so.image.lowest);
    x.mem.write<uint32_t>(info + 8, best ? x.proc.internCString(bestName) : 0);
    x.mem.write<uint32_t>(info + 12, best);
    return 1;
}
// dl_iterate_phdr isn't offered: games of this era don't call it (their unwinders use
// __gnu_Unwind_Find_exidx / dl_unwind_find_exidx).

} // namespace

void registerLibdl(Process& p) {
    supportDef<&dl_open>(p, "android:dlopen", SupportStatus::Real, "the APK's libraries, loaded and initialized; system libraries are HydrogenVM's");
    supportDef<&dl_sym>(p, "android:dlsym", SupportStatus::Real, "game library symbols, then HydrogenVM's");
    supportDef<&dl_close>(p, "android:dlclose", SupportStatus::Approx, "libraries stay loaded");
    supportDef<&dl_error>(p, "android:dlerror", SupportStatus::Real, "the last dlopen/dlsym failure");
    supportDef<&dl_addr>(p, "android:dladdr", SupportStatus::Real, "the library and nearest symbol");
}

} // namespace Hydrogen::Runtime::Android
