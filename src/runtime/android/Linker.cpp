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
#include <cctype>
#include <cstring>
#include <cxxabi.h>
#include <filesystem>
#include <functional>
#include <map>
#include <set>
#include <unordered_map>
#include <unordered_set>

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
    if (fs::path(mainPath).extension() == ".dex") {
        L.libDir = (fs::path(mainPath).parent_path() / "lib").string();
        L.name = fs::path(mainPath).filename().string();
        return {};
    }
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

// Handles: a soinfo record (below) for every library, as Android before 7 returns. The older
// small handles (a game library 0x10000 + its index, a system library 0x20000 + an index) are
// still accepted. RTLD_DEFAULT and RTLD_NEXT are bionic's 32-bit values.
constexpr uint32_t kGameHandle = 0x10000, kSystemHandle = 0x20000, kRtldDefault = 0xFFFFFFFFu, kRtldNext = 0xFFFFFFFEu;

struct Dl {
    explicit Dl(Process&) {}
    std::mutex mu;
    std::string error;
    uint32_t errorBuf = 0;
    std::unordered_map<uint32_t, uint32_t> names;   // dladdr: library -> its guest file name
    // Libraries as bionic soinfo records, which Android before 7 returns from dlopen. System
    // libraries share one set of ELF symbol tables; each game library has its own.
    struct Tables { uint32_t strtab = 0, symtab = 0, nbucket = 0, nchain = 0, bucket = 0, chain = 0; };
    Tables system;
    std::unordered_map<std::string, uint32_t> systemSoinfo;   // name -> soinfo
    std::unordered_set<uint32_t> systemSoinfos;
    std::unordered_map<size_t, uint32_t> gameSoinfo;          // library index -> soinfo
    std::unordered_map<uint32_t, size_t> gameIndex;           // soinfo -> library index
};
Dl& D(Ctx& x) { return x.proc.service<Dl>(); }
void setError(Ctx& x, std::string e) { auto& d = D(x); std::lock_guard<std::mutex> lk(d.mu); d.error = std::move(e); }

// The System V ABI ELF hash, as bionic's linker computes it.
uint32_t elfHash(const std::string& s) {
    uint32_t h = 0;
    for (unsigned char c : s) {
        h = (h << 4) + c;
        const uint32_t g = h & 0xF0000000u;
        h ^= g;
        h ^= g >> 24;
    }
    return h;
}

// An ELF string table, symbol table and SysV hash table in guest memory over `exported`
// (name -> address). A symbol's st_value is its address minus `base`, as bionic's linker adds
// the soinfo's base back.
Dl::Tables placeTables(Ctx& x, const std::map<std::string, uint32_t>& exported, uint32_t base) {
    Dl::Tables t;
    std::string strtab(1, '\0');
    t.nchain = uint32_t(exported.size()) + 1;                 // entry 0 is the null symbol
    t.nbucket = std::max<uint32_t>(1, t.nchain / 2) | 1;
    std::vector<uint8_t> symtab(size_t(t.nchain) * 16, 0);
    std::vector<uint32_t> bucket(t.nbucket, 0), chain(t.nchain, 0);
    uint32_t index = 1;
    for (const auto& [name, address] : exported) {
        const uint32_t nameOffset = uint32_t(strtab.size()), value = address - base;
        strtab += name;
        strtab += '\0';
        uint8_t* s = symtab.data() + size_t(index) * 16;   // Elf32_Sym
        std::memcpy(s + 0, &nameOffset, 4);
        std::memcpy(s + 4, &value, 4);
        s[12] = 0x12;                                       // STB_GLOBAL, STT_FUNC
        const uint16_t section = 1;                         // defined (not SHN_UNDEF)
        std::memcpy(s + 14, &section, 2);
        const uint32_t h = elfHash(name) % t.nbucket;
        chain[index] = bucket[h];
        bucket[h] = index;
        ++index;
    }
    auto place = [&](const void* data, size_t n) {
        const uint32_t at = x.proc.heap().allocZeroed(uint32_t(std::max<size_t>(n, 4)));
        x.mem.copyIn(at, static_cast<const uint8_t*>(data), n);
        return at;
    };
    t.strtab = place(strtab.data(), strtab.size());
    t.symtab = place(symtab.data(), symtab.size());
    t.bucket = place(bucket.data(), bucket.size() * 4);
    t.chain = place(chain.data(), chain.size() * 4);
    return t;
}

// A soinfo record (bionic linker.h, 32-bit ARM, Android 2.3 to 6): name[128], base 0x8c,
// size 0x90, flags 0xa8, strtab 0xac, symtab 0xb0, nbucket 0xb4, nchain 0xb8, bucket 0xbc,
// chain 0xc0, refcount 0x100.
uint32_t writeSoinfo(Ctx& x, const std::string& name, const Dl::Tables& t, uint32_t base, uint32_t size) {
    constexpr uint32_t kSoinfoSize = 0x140;
    const uint32_t si = x.proc.heap().allocZeroed(kSoinfoSize);
    const std::string shortName = name.substr(0, 127);
    x.mem.copyIn(si, reinterpret_cast<const uint8_t*>(shortName.c_str()), shortName.size() + 1);
    x.mem.write<uint32_t>(si + 0x8C, base);
    x.mem.write<uint32_t>(si + 0x90, size);
    x.mem.write<uint32_t>(si + 0xA8, 1);          // flags: FLAG_LINKED
    x.mem.write<uint32_t>(si + 0xAC, t.strtab);
    x.mem.write<uint32_t>(si + 0xB0, t.symtab);
    x.mem.write<uint32_t>(si + 0xB4, t.nbucket);
    x.mem.write<uint32_t>(si + 0xB8, t.nchain);
    x.mem.write<uint32_t>(si + 0xBC, t.bucket);
    x.mem.write<uint32_t>(si + 0xC0, t.chain);
    x.mem.write<uint32_t>(si + 0x100, 1);         // refcount
    return si;
}

// A system library: every system function and variable HydrogenVM provides, under the names
// dlsym answers ("android:" entries first, then the iOS ones), in one table shared by all of
// them, so a game's own linker finds a name in whichever system dependency it searches first.
uint32_t systemSoinfo(Ctx& x, const std::string& name) {
    auto& d = D(x);
    std::lock_guard<std::mutex> lk(d.mu);
    if (auto it = d.systemSoinfo.find(name); it != d.systemSoinfo.end()) return it->second;
    if (!d.system.symtab) {
        std::map<std::string, uint32_t> exported;   // ordered: a stable layout from run to run
        const auto table = x.proc.symbolTable();
        auto plainName = [](const std::string& n) {
            if (n.empty()) return false;
            for (char c : n) if (!(std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '$' || c == '.')) return false;
            return true;
        };
        for (const auto& [n, a] : table)
            if (n.rfind("android:", 0) == 0 && plainName(n.substr(8))) exported[n.substr(8)] = a;
        for (const auto& [n, a] : table)
            if (n.size() > 1 && n[0] == '_' && plainName(n.substr(1))) exported.emplace(n.substr(1), a);
        d.system = placeTables(x, exported, 0);
    }
    const uint32_t si = writeSoinfo(x, name, d.system, 0, 0);
    d.systemSoinfo[name] = si;
    d.systemSoinfos.insert(si);
    return si;
}
// A game library: its own dynamic symbols, at its load bias. Caller holds L.mu.
uint32_t gameSoinfo(Ctx& x, Library& L, size_t index) {
    auto& d = D(x);
    std::lock_guard<std::mutex> lk(d.mu);
    if (auto it = d.gameSoinfo.find(index); it != d.gameSoinfo.end()) return it->second;
    const LoadedLib& lib = *L.libs[index];
    const auto& image = lib.so.image;
    const std::map<std::string, uint32_t> exported(image.symbols.begin(), image.symbols.end());
    const uint32_t size = image.highest > image.lowest ? image.highest - image.lowest : 0;
    const uint32_t si = writeSoinfo(x, lib.name, placeTables(x, exported, image.load_bias), image.load_bias, size);
    d.gameSoinfo[index] = si;
    d.gameIndex[si] = index;
    return si;
}
bool isSystemSoinfo(Ctx& x, uint32_t handle) {
    auto& d = D(x);
    std::lock_guard<std::mutex> lk(d.mu);
    return d.systemSoinfos.count(handle) != 0;
}
// The library index of a game handle (the soinfo, or the older 0x10000 + index), or -1.
long gameHandleIndex(Ctx& x, Library& L, uint32_t handle) {
    if (handle >= kGameHandle && handle < kGameHandle + L.libs.size()) return long(handle - kGameHandle);
    auto& d = D(x);
    std::lock_guard<std::mutex> lk(d.mu);
    auto it = d.gameIndex.find(handle);
    return it == d.gameIndex.end() ? -1 : long(it->second);
}

uint32_t dl_open(Ctx& x, GuestPtr fileP, int32_t) {
    auto& L = x.proc.service<Library>();
    if (!fileP) return kRtldDefault;   // the main program: global lookup
    std::string file = x.proc.readCString(fileP, 1024);
    const std::string name = fs::path(file).filename().string();
    std::lock_guard<std::recursive_mutex> lk(L.mu);
    for (size_t i = 0; i < L.libs.size(); ++i)
        if (L.libs[i]->name == name) {
            if (!L.libs[i]->initialized) initialize(x.proc, x.cpu, *L.libs[i]);
            return gameSoinfo(x, L, i);
        }
    if (isSystemLibrary(name)) return systemSoinfo(x, name);
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
        for (size_t i = 0; i < L.libs.size(); ++i) if (L.libs[i].get() == lib) return gameSoinfo(x, L, i);
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
    if (const long index = gameHandleIndex(x, L, handle); index >= 0) {
        const auto& syms = L.libs[size_t(index)]->so.image.symbols;
        auto it = syms.find(name);
        r = it != syms.end() ? it->second : inGame();   // then its dependencies: the global scope
        if (!r) r = inSystem();
    } else if ((handle >= kSystemHandle && handle < kSystemHandle + 0x10000) || isSystemSoinfo(x, handle)) {
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
