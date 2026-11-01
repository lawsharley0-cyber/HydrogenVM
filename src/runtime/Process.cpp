#include "Process.hpp"
#include "libc/Unwind.hpp"
#include "runtime/objc/ObjC.hpp"
#include <sstream>
#include "audio/Audio.hpp"
#include "loader/ElfLoader.hpp"
#include <cstdarg>
#include <cstdio>
#include <cxxabi.h>
#include <filesystem>
#include <pthread.h>
#include <unistd.h>
#include <iomanip>
#if defined(__APPLE__)
#include <CommonCrypto/CommonDigest.h>
#elif defined(HVM_HAVE_OPENSSL)
#include <openssl/sha.h>
#endif
#ifndef HVM_COMMIT
#define HVM_COMMIT "unknown"
#endif

namespace fs = std::filesystem;

namespace Hydrogen::Runtime {

std::vector<uint8_t> hvmDigest(int alg, const std::string& s);
void flushGuestStdio(Process& p);   // libc/Stdio.cpp

namespace {
thread_local GuestThread* t_thread = nullptr;

constexpr uint32_t kHeapBase = 0x10000000u, kHeapLimit = 0x80000000u;
constexpr uint32_t kStaticBase = 0x0E000000u, kStaticLimit = 0x10000000u;
constexpr uint32_t kMainStackSize = 1u << 20, kThreadStackSize = 512u << 10, kGuard = 64u << 10;

std::string demangle(const std::string& sym) {
    std::string s = sym;
    if (!s.empty() && s[0] == '_') s.erase(0, 1);
    if (s.compare(0, 2, "_Z") == 0) {
        int st = 0;
        char* d = abi::__cxa_demangle(s.c_str(), nullptr, nullptr, &st);
        if (d && st == 0) { std::string r = d; std::free(d); return r; }
        std::free(d);
    }
    return s;
}

// CoreFoundation-style function names: a verb (Create, Copy, Get, ...) followed by
// another word or the end ("CFHostCreateWithName", "SecTrustCopyPublicKey").
bool looksLikeFunction(const std::string& s) {
    if (s.size() < 3 || s[0] != '_' || s[1] == 'k' || !std::isupper(uint8_t(s[1]))) return false;
    for (const char* v : {"Create", "Copy", "Get", "Set", "Open", "Close", "Release", "Retain", "Read", "Write", "Dispose", "Invalidate"}) {
        const size_t n = std::strlen(v);
        for (size_t at = s.find(v, 2); at != std::string::npos; at = s.find(v, at + 1)) {
            const size_t after = at + n;
            if (std::islower(uint8_t(s[at - 1])) || std::isupper(uint8_t(s[at - 1])))
                if (after == s.size() || std::isupper(uint8_t(s[after]))) return true;
        }
    }
    return false;
}

bool looksLikeData(const std::string& s) {
    if (looksLikeFunction(s)) return false;
    auto starts = [&](const char* p) { return s.compare(0, std::strlen(p), p) == 0; };
    auto ends = [&](const char* p) { const size_t n = std::strlen(p); return s.size() >= n && s.compare(s.size() - n, n, p) == 0; };
    if (starts("__ZTV") || starts("__ZTI") || starts("__ZTS") || starts("__ZTT") ||
        starts("__ZSt4cout") || starts("__ZSt4cerr") || starts("__ZSt4clog") || starts("__ZSt3cin") ||
        starts("__NSConcrete") || starts("_OBJC_")) return true;
    if (ends("_S_empty_rep_storageE") || ends("_S_max_sizeE") || ends("_S_terminalE")) return true;
    if (s.size() > 2 && s[0] == '_' && s[1] == 'k' && std::isupper(uint8_t(s[2]))) return true;
    if (s.size() > 2 && s[0] == '_' && std::isupper(uint8_t(s[1])) &&
        (ends("Notification") || ends("Key") || ends("Mode") || ends("Domain") || ends("Name") || ends("Exception") ||
         ends("AttributeName") || ends("Identity") || ends("Callbacks") || ends("CallBacks") || ends("Preset") ||
         starts("_AVCaptureSessionPreset") || starts("_AVMediaType") || starts("_AVLayerVideoGravity") ||
         starts("_AVAudioSession") || starts("_AVAssetExportPreset") || starts("_AVFileType") ||
         starts("_AVVideo") || starts("_AVMetadata") || starts("_UIActivityType") || starts("_UITransitionContext") || starts("_UIFontTextStyle") ||
         starts("_ADBannerContentSizeIdentifier") || starts("_SLServiceType") ||
         starts("_ACAccountTypeIdentifier") || starts("_SKStoreProductParameter")))
        return true;
    return false;
}

// Host threads for guest threads get a generous stack: nested host↔guest
// calls (ObjC dispatch from host code, callbacks) recurse on it.
void spawnHostThread(std::function<void()> fn) {
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, 8u << 20);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    auto* heapFn = new std::function<void()>(std::move(fn));
    pthread_t th;
    const int r = pthread_create(&th, &attr, [](void* p) -> void* {
        std::unique_ptr<std::function<void()>> f(static_cast<std::function<void()>*>(p));
        (*f)();
        return nullptr;
    }, heapFn);
    pthread_attr_destroy(&attr);
    if (r != 0) { delete heapFn; throw std::runtime_error("couldn't create host thread"); }
}

} // namespace

// ----------------------------------------------------------- lifecycle ---

Process::Process(ProcessConfig cfg) : cfg_(std::move(cfg)), mixer_(std::make_shared<Audio::Mixer>()) {
    mem_ = std::make_unique<Memory::GuestArena>();
    engine_ = std::make_unique<CPU::Engine>(*mem_);
    heap_ = std::make_unique<Heap>(*mem_, kHeapBase, kHeapLimit);
    mem_->map(kStaticBase, 1u << 20, Memory::kProtRW);
    staticTop_ = kStaticBase;
    staticEnd_ = kStaticBase + (1u << 20);

    // The app passes directory URLs' paths, which end in '/'. Path mapping joins
    // prefixes with the rest of a path, so keep both without a trailing slash
    // (a trailing slash here turned ".../Documents" into "<UUID>Documents").
    auto trimSlash = [](std::string s) { while (s.size() > 1 && s.back() == '/') s.pop_back(); return s; };
    cfg_.bundlePath = trimSlash(cfg_.bundlePath);
    homePath_ = trimSlash(cfg_.homePath.empty() ? fs::path(cfg_.bundlePath).parent_path().string() : cfg_.homePath);
    std::error_code ec;
    for (const char* d : {"Documents", "Library", "Library/Caches", "Library/Preferences", "tmp"})
        fs::create_directories(fs::path(homePath_) / d, ec);

    const auto digest = hvmDigest(1, homePath_);
    char uuidBuf[37];
    std::snprintf(uuidBuf, sizeof(uuidBuf),
                  "%02X%02X%02X%02X-%02X%02X-%02X%02X-%02X%02X-%02X%02X%02X%02X%02X%02X",
                  digest[0], digest[1], digest[2], digest[3],
                  digest[4], digest[5], digest[6], digest[7],
                  digest[8], digest[9], digest[10], digest[11],
                  digest[12], digest[13], digest[14], digest[15]);
    guestHomePath_ = std::string("/var/mobile/Applications/") + uuidBuf;
    const std::string bundleName = fs::path(cfg_.bundlePath).filename().string();
    guestBundlePath_ = guestHomePath_ + "/" + (bundleName.empty() ? "App.app" : bundleName);

    mainThreadOwner_ = std::make_unique<GuestThread>(*this);
    mainThread_ = mainThreadOwner_.get();
    mainThread_->id = nextThreadId_++;
    t_thread = mainThread_;

    registerLibcString(*this);
    registerLibcStdio(*this);
    registerLibcStdlib(*this);
    registerLibcMath(*this);
    registerCompilerRt(*this);
    registerLibcTime(*this);
    registerLibcPthread(*this);
    registerLibcSystem(*this);
    registerCxxRuntime(*this);
    registerObjCRuntime(*this);
    registerUnwinder(*this);
    registerLibStdcxx(*this);
    registerLibcPosix(*this);
    registerZlib(*this);
    registerLibcNet(*this);
    registerCommonCrypto(*this);
    registerSqlite(*this);
    registerFoundation(*this);
    registerUIKit(*this);
}

Process::~Process() {
    {
        std::lock_guard<std::mutex> lk(liveness_->mu);
        liveness_->alive = false;
    }
    requestStop();
    // Wait (bounded) for guest threads to notice.
    for (int i = 0; i < 200; ++i) {
        bool alive = false;
        {
            std::lock_guard<std::mutex> lk(threadsMu_);
            for (auto& [k, t] : threads_) alive |= !t->finished.load();
        }
        if (!alive) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    if (t_thread && &t_thread->proc == this) t_thread = nullptr;
}

void Process::requestStop() {
    engine_->requestStop();
    mixer_->stopAll();
    pthreadsBroadcastStop(*this);
    lifeCv_.notify_all();   // a backgrounded main thread is parked in waitWhileBackgrounded()
}

void Process::requestGracefulStop(double timeout) {
    if (stopping() || gracefulStop_.exchange(true)) return;
    if (!lifecycleHandler_) { requestStop(); return; }
    lifecycleHandler_(Lifecycle::Terminate);
    // Watchdog: an app that doesn't get through its shutdown (or never runs its main loop) is stopped anyway.
    std::thread([this, timeout, live = liveness_] {
        const auto end = std::chrono::steady_clock::now() + std::chrono::duration<double>(timeout);
        for (;;) {
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
            std::lock_guard<std::mutex> lk(live->mu);
            if (!live->alive || stopping()) return;
            if (std::chrono::steady_clock::now() >= end) {
                log(LogLevel::Warning, "The app didn't finish saving in time; stopping it");
                {
                    std::lock_guard<std::mutex> lk(exitMu_);
                    crashed_ = true;
                    crashReport_ = "The game did not finish saving before the watchdog stopped it.";
                    CPU::Cpu empty{}; // The guest is running: do not race its register writes.
                    captureCrash(empty, "graceful-stop-watchdog", "registers unavailable while running");
                }
                requestStop();
                return;
            }
        }
    }).detach();
}
void Process::enterBackground() {
    if (stopping() || backgroundRequested_.load() || !lifecycleHandler_) return;
    backgroundRequested_ = true;
    lifecycleHandler_(Lifecycle::Background);
}
void Process::enterForeground() {
    if (stopping() || !lifecycleHandler_) return;
    backgroundRequested_ = false;
    setBackgrounded(false);
    lifecycleHandler_(Lifecycle::Foreground);
}
void Process::setBackgrounded(bool b) {
    {
        std::lock_guard<std::mutex> lk(lifeMu_);
        backgrounded_ = b;
    }
    mixer_->setAllPaused(b);
    lifeCv_.notify_all();
}
void Process::waitWhileBackgrounded() {
    std::unique_lock<std::mutex> lk(lifeMu_);
    lifeCv_.wait(lk, [&] { return !backgrounded_.load() || !backgroundRequested_.load() || stopping() || gracefulStop_.load(); });
}

void Process::load() {
    const fs::path plist = fs::path(cfg_.bundlePath) / "Info.plist";
    if (fs::exists(plist)) infoPlist_ = Plist::parseFile(plist.string());
    std::string exe = infoPlist_ ? infoPlist_->getString("CFBundleExecutable") : "";
    if (exe.empty()) exe = fs::path(cfg_.bundlePath).stem().string();
    executablePath_ = (fs::path(cfg_.bundlePath) / exe).string();
    guestExecutablePath_ = guestBundlePath_ + "/" + exe;

    const auto file = Loader::readFile(executablePath_);
    unsigned char digest[32];
#if defined(__APPLE__)
    CC_SHA256(file.data(), CC_LONG(file.size()), digest);
#elif defined(HVM_HAVE_OPENSSL)
    SHA256(file.data(), file.size(), digest);
#else
    const auto bytes = hvmDigest(2, std::string(file.begin(), file.end()));
    std::copy(bytes.begin(), bytes.end(), digest);
#endif
    std::ostringstream hash;
    for (auto b : digest) hash << std::hex << std::setw(2) << std::setfill('0') << unsigned(b);
    executableHash_ = hash.str();
    image_ = Loader::loadMachO(*mem_, file, executablePath_);
    logf(LogLevel::Info, "Loaded %s: %s, %zu imports, %zu initializers, entry %s",
         exe.c_str(), image_.cpuSubtype == Loader::kCpuArmV7S ? "armv7s" : image_.cpuSubtype == Loader::kCpuArmV6 ? "armv6" : "armv7",
         image_.binds.size(), image_.initializers.size(), describeAddress(image_.entry).c_str());
    defineData("__mh_execute_header", image_.headerAddr);
    setupEnvironment();
    setupMainThread();
    bindImports();

    // Old (pre-dyld-info) binaries call through __DATA,__dyld.
    if (const auto* s = image_.findSection("__DATA", "__dyld"); s && s->size >= 8) {
        mem_->write<uint32_t>(s->addr, *lookupSymbol("__hvm_dyld_lazy_entry"));
        mem_->write<uint32_t>(s->addr + 4, *lookupSymbol("__hvm_dyld_func_lookup"));
    }
    for (auto& h : loadedHooks_) h();
}

// -------------------------------------------------------------- symbols ---

uint32_t Process::registerHostFunction(const std::string& debugName, HostFn fn) {
    return engine_->registerHle(debugName, [this, fn = std::move(fn), debugName](CPU::Cpu& c) {
        if (cfg_.traceCalls) log(LogLevel::Debug, "→ " + debugName + " (from " + describeAddress(c.r[CPU::kLR]) + ")");
        Ctx x{*this, c, *mem_};
        try {
            fn(x);
        } catch (const GuestThrow& g) {
            landGuestException(x, g.ue);   // e.g. -[NSException raise] inside an app's @try
        } catch (const ProcessExit&) {
            // This is intentional process control, not an HLE exception.
            // Let nested guest calls propagate Halt instead of manufacturing
            // HostError("stopping") in Engine::callHle.
            c.exit = CPU::Exit::Halt;
        } catch (const std::runtime_error& e) {
            if (!hostErrorHook_ || !hostErrorHook_(x, e)) throw;
        }
    });
}

uint32_t Process::defineFunction(const std::string& name, HostFn fn) {
    const uint32_t addr = registerHostFunction(name, std::move(fn));
    std::lock_guard<std::mutex> lk(symMu_);
    symbols_[name] = addr;
    return addr;
}

void Process::defineData(const std::string& name, uint32_t addr) {
    std::lock_guard<std::mutex> lk(symMu_);
    symbols_[name] = addr;
}

uint32_t Process::defineDataBlock(const std::string& name, uint32_t size) {
    const uint32_t a = allocStatic(size);
    defineData(name, a);
    return a;
}

std::optional<uint32_t> Process::lookupSymbol(const std::string& name) const {
    {
        std::lock_guard<std::mutex> lk(symMu_);
        auto it = symbols_.find(name);
        if (it != symbols_.end()) return it->second;
    }
    auto it = image_.exports.find(name);
    if (it != image_.exports.end()) return it->second;
    return std::nullopt;
}

void Process::addMissingSymbolResolver(std::function<uint32_t(const std::string&)> fn) {
    missingResolvers_.push_back(std::move(fn));
}

uint32_t Process::makeMissingStub(const std::string& name) {
    // Data, by name, or because the app only takes its address (never calls it
    // through a lazy stub) and it has a Cocoa-style constant name (NSFooKey,
    // UIBackgroundTaskInvalid, NSGregorianCalendar, ...).
    const bool cocoaName = name.size() > 2 && name[0] == '_' && std::isupper(uint8_t(name[1]));
    // Missing C++ vtables and VTTs: instead of zeros (a virtual call then jumps to
    // address 0 with nothing to say why), fill the slots with stubs that name the
    // class and slot, so the crash report says what the app tried to call.
    const bool vtable = name.compare(0, 5, "__ZTV") == 0, vtt = name.compare(0, 5, "__ZTT") == 0;
    if (vtable || vtt) {
        const std::string pretty = demangle(name);
        auto trapTable = [&](uint32_t block, uint32_t firstSlotWord, uint32_t words) {
            for (uint32_t i = firstSlotWord; i < words; ++i) {
                const uint32_t slot = i - firstSlotWord;
                const uint32_t fn = defineFunction(name + "$hvm_slot" + std::to_string(slot), [this, pretty, slot](Ctx& x) {
                    const std::string what = "virtual function #" + std::to_string(slot) + " of " + pretty;
                    reportMissing(what);
                    if (!cfg_.stopOnMissingFunction) { x.cpu.r[0] = x.cpu.r[1] = 0; return; }   // keep going: like other missing functions
                    throw MissingFunction("Missing " + what + " (HydrogenVM doesn't provide this C++ class; called from " +
                                          describeAddress(x.cpu.r[CPU::kLR]) + ")");
                });
                mem_->write<uint32_t>(block + 4 * i, fn);
            }
        };
        if (vtable) {
            // [offset-to-top][typeinfo][slot 0...]: objects point at +8.
            const uint32_t a = defineDataBlock(name, 64);
            trapTable(a, 2, 16);
            log(LogLevel::Warning, "Missing data symbol " + pretty + " (virtual calls through it report the slot)");
            return a;
        }
        // VTT: every entry points at a stand-in vtable whose slots report the call;
        // the words before the address point (vbase/top offsets) stay 0.
        const uint32_t table = allocStatic(80);
        trapTable(table, 4, 20);
        const uint32_t a = defineDataBlock(name, 64);
        for (uint32_t i = 0; i < 16; ++i) mem_->write<uint32_t>(a + 4 * i, table + 16);
        log(LogLevel::Warning, "Missing data symbol " + pretty + " (virtual calls through it report the slot)");
        return a;
    }
    if (looksLikeData(name) || (cocoaName && !calledImports_.count(name) && !looksLikeFunction(name))) {
        const uint32_t a = defineDataBlock(name, 64);
        log(LogLevel::Warning, "Missing data symbol " + demangle(name) + " (using zeros)");
        return a;
    }
    const std::string pretty = demangle(name);
    return defineFunction(name, [this, pretty](Ctx& x) {
        reportMissing(pretty);
        if (cfg_.stopOnMissingFunction)
            throw MissingFunction("Missing function: " + pretty + " (called from " + describeAddress(x.cpu.r[CPU::kLR]) + ")");
        x.cpu.r[0] = x.cpu.r[1] = 0;
    });
}

uint32_t Process::resolveImport(const Loader::BindRecord& b) {
    if (auto a = lookupSymbol(b.symbol)) return *a;
    // A weak import that nothing provides is 0, as dyld does: apps weak-link
    // frameworks newer than their minimum OS and test for nil before use.
    // Calls through a lazy stub still get a stub that reports the missing
    // function (tests for availability use the address, not the stub).
    // Framework constants (strings, numbers) are still provided: they exist on every real device,
    // and apps that weak-link a framework use them without checking.
    if (b.weakImport && !b.lazy) {
        if (b.symbol.compare(0, 6, "_OBJC_") == 0) return 0;
        for (auto& r : missingResolvers_)
            if (uint32_t a = r(b.symbol)) { defineData(b.symbol, a); return a; }
        return 0;
    }
    for (auto& r : missingResolvers_)
        if (uint32_t a = r(b.symbol)) { defineData(b.symbol, a); return a; }
    return makeMissingStub(b.symbol);
}

void Process::bindImports() {
    std::unordered_map<std::string, uint32_t> cache;
    size_t unresolvedBefore;
    {
        std::lock_guard<std::mutex> lk(symMu_);
        unresolvedBefore = symbols_.size();
    }
    std::vector<std::string> missingFns;
    for (const auto& b : image_.binds) if (b.lazy) calledImports_.insert(b.symbol);
    // Class and metaclass pointers inside the app's own class structures
    // (superclass, isa) always need a class, even when the import is weak:
    // dyld would refuse to load an app whose superclass is missing, and a nil
    // superclass would silently turn the subclass into a root class.
    const Loader::Section* objcData = image_.findSection("__DATA", "__objc_data");
    auto inObjcData = [&](uint32_t a) { return objcData && a >= objcData->addr && a < objcData->addr + objcData->size; };
    for (const auto& b0 : image_.binds) {
        Loader::BindRecord strong;
        const bool needsClass = b0.weakImport && inObjcData(b0.address) &&
            (b0.symbol.rfind("_OBJC_CLASS_$_", 0) == 0 || b0.symbol.rfind("_OBJC_METACLASS_$_", 0) == 0);
        if (needsClass) { strong = b0; strong.weakImport = false; }
        const Loader::BindRecord& b = needsClass ? strong : b0;
        const std::string key = needsClass ? b.symbol + "#class" : (b.weakImport && !b.lazy) ? b.symbol + "#weak" : b.symbol;
        uint32_t target;
        auto it = cache.find(key);
        if (it != cache.end()) target = it->second;
        else {
            const bool known = lookupSymbol(b.symbol).has_value();
            target = resolveImport(b);
            cache.emplace(key, target);
            if (!known && target && engine_->isHleAddress(target) &&
                engine_->hleName(engine_->hleIndex(target)) == b.symbol && !looksLikeData(b.symbol))
                missingFns.push_back(demangle(b.symbol));
        }
        switch (b.kind) {
            case Loader::BindKind::Pointer:
            case Loader::BindKind::TextAbsolute32:
                mem_->write<uint32_t>(b.address, target + uint32_t(b.addend));
                break;
            case Loader::BindKind::ExternalReloc:
                mem_->write<uint32_t>(b.address, mem_->read<uint32_t>(b.address) + target);
                break;
            case Loader::BindKind::TextPcrel32:
                mem_->write<uint32_t>(b.address, target + uint32_t(b.addend) - (b.address + 4));
                break;
        }
    }
    // Weak binds (coalesced template instantiations, inline functions, operator
    // new/delete). The app may define these itself (a rebase already put its own
    // definition in the slot); we only override when HydrogenVM or another image
    // actually exports the symbol, and we never replace a real value with a
    // missing-function stub — the app's own weak definition stays.
    for (const auto& b : image_.weakBinds) {
        const auto found = lookupSymbol(b.symbol);
        if (!found) continue;
        const uint32_t target = *found;
        switch (b.kind) {
            case Loader::BindKind::Pointer:
            case Loader::BindKind::TextAbsolute32:
                mem_->write<uint32_t>(b.address, target + uint32_t(b.addend));
                break;
            case Loader::BindKind::ExternalReloc:
                mem_->write<uint32_t>(b.address, mem_->read<uint32_t>(b.address) + target);
                break;
            case Loader::BindKind::TextPcrel32:
                mem_->write<uint32_t>(b.address, target + uint32_t(b.addend) - (b.address + 4));
                break;
        }
    }
    (void)unresolvedBefore;
    unimplementedImports_ = missingFns;
    if (!missingFns.empty()) {
        std::string list;
        for (size_t i = 0; i < missingFns.size(); ++i) list += (i ? ", " : "") + missingFns[i];
        logf(LogLevel::Info, "%zu imported functions aren't implemented yet (the app may still run until it calls one): %s",
             missingFns.size(), list.c_str());
    }
}

// ----------------------------------------------------------- environment ---

uint32_t Process::allocStatic(uint32_t size, uint32_t align) {
    std::lock_guard<std::mutex> lk(staticMu_);
    if (align < 4) align = 4;
    uint32_t a = (staticTop_ + align - 1) & ~(align - 1);
    if (a + size > staticEnd_) {
        const uint32_t newEnd = std::min<uint64_t>(kStaticLimit, (uint64_t(a) + size + (1u << 20) - 1) & ~uint64_t((1u << 20) - 1));
        if (a + size > newEnd) throw std::runtime_error("static guest data region exhausted");
        mem_->map(staticEnd_, newEnd - staticEnd_, Memory::kProtRW);
        staticEnd_ = newEnd;
    }
    staticTop_ = a + size;
    return a;
}

uint32_t Process::internCString(const std::string& s) {
    {
        std::lock_guard<std::mutex> lk(staticMu_);
        auto it = interned_.find(s);
        if (it != interned_.end()) return it->second;
    }
    const uint32_t a = allocStatic(uint32_t(s.size() + 1), 1);
    mem_->copyIn(a, s.c_str(), s.size() + 1);
    std::lock_guard<std::mutex> lk(staticMu_);
    return interned_.emplace(s, a).first->second;
}

uint32_t Process::allocCString(const std::string& s) {
    const uint32_t a = heap_->alloc(uint32_t(s.size() + 1));
    if (a) mem_->copyIn(a, s.c_str(), s.size() + 1);
    return a;
}

std::string Process::readCString(uint32_t addr, size_t max) const {
    if (!addr) return {};
    std::string out;
    while (out.size() < max) {
        const char ch = char(mem_->read<uint8_t>(addr + uint32_t(out.size())));
        if (!ch) break;
        out += ch;
    }
    return out;
}

void Process::setupEnvironment() {
    const std::string guestTmp = guestHomePath_ + "/tmp/";
    env_ = {
        {"HOME", guestHomePath_}, {"CFFIXED_USER_HOME", guestHomePath_}, {"TMPDIR", guestTmp},
        {"PATH", "/usr/bin:/bin:/usr/sbin:/sbin"}, {"USER", "mobile"}, {"LOGNAME", "mobile"},
        {"SHELL", "/bin/sh"}, {"__CF_USER_TEXT_ENCODING", "0x1F5:0:0"},
    };
    std::vector<std::string> args{guestExecutablePath_.empty() ? executablePath_ : guestExecutablePath_};
    for (auto& a : cfg_.extraArgs) args.push_back(a);
    argc_ = uint32_t(args.size());
    argv_ = allocStatic(uint32_t(args.size() + 1) * 4);
    for (size_t i = 0; i < args.size(); ++i) mem_->write<uint32_t>(argv_ + uint32_t(i) * 4, internCString(args[i]));
    mem_->write<uint32_t>(argv_ + uint32_t(args.size()) * 4, 0);
    envp_ = allocStatic(uint32_t(env_.size() + 1 + 32) * 4);   // room for setenv additions
    for (size_t i = 0; i < env_.size(); ++i) {
        const uint32_t s = internCString(env_[i].first + "=" + env_[i].second);
        envAddrs_[env_[i].first] = s + uint32_t(env_[i].first.size()) + 1;
        mem_->write<uint32_t>(envp_ + uint32_t(i) * 4, s);
    }
    mem_->write<uint32_t>(envp_ + uint32_t(env_.size()) * 4, 0);
    apple_ = allocStatic(8);
    mem_->write<uint32_t>(apple_, internCString(guestExecutablePath_.empty() ? executablePath_ : guestExecutablePath_));
    mem_->write<uint32_t>(apple_ + 4, 0);
}

namespace {
bool matchesPathPrefix(const std::string& path, const std::string& prefix) {
    if (prefix.empty() || path.compare(0, prefix.size(), prefix) != 0) return false;
    return path.size() == prefix.size() || path[prefix.size()] == '/';
}
// iOS reaches the same files as /var/... and /private/var/...; compare in the
// /var form so either spelling of a host or guest path matches.
std::string canonVar(const std::string& p) { return p.compare(0, 13, "/private/var/") == 0 ? p.substr(8) : p; }
} // namespace

std::string Process::hostPath(const std::string& guestPath) const {
    if (guestPath.empty()) return {};
    std::string p = guestPath;
    if (p == "~") p = guestHomePath_;
    else if (p.compare(0, 2, "~/") == 0) p = guestHomePath_ + p.substr(1);
    const std::string norm = canonVar(p);
    // Already a host path: leave it alone.
    if (matchesPathPrefix(norm, canonVar(bundlePath())) || matchesPathPrefix(norm, canonVar(homePath_))) return p;
    // Guest paths, most specific first (the bundle sits inside the home).
    if (matchesPathPrefix(norm, guestBundlePath_)) return bundlePath() + norm.substr(guestBundlePath_.size());
    if (matchesPathPrefix(norm, guestHomePath_)) return homePath_ + norm.substr(guestHomePath_.size());
    const std::string hostTmp = homePath_ + "/tmp";
    if (matchesPathPrefix(norm, "/tmp")) return hostTmp + norm.substr(4);
    if (matchesPathPrefix(norm, "/var/tmp")) return hostTmp + norm.substr(8);
    return p;
}

std::string Process::guestPath(const std::string& hostPath) const {
    if (hostPath.empty()) return {};
    const std::string norm = canonVar(hostPath);
    if (matchesPathPrefix(norm, "/var/mobile/Applications")) return norm;
    // Longest host prefix first: the bundle, its home's tmp, then the home.
    const std::pair<std::string, std::string> mappings[] = {
        {canonVar(bundlePath()), guestBundlePath_},
        {canonVar(homePath_ + "/tmp"), guestHomePath_ + "/tmp"},
        {canonVar(homePath_), guestHomePath_},
    };
    const std::pair<std::string, std::string>* best = nullptr;
    for (const auto& m : mappings)
        if (!m.first.empty() && !m.second.empty() && matchesPathPrefix(norm, m.first) &&
            (!best || m.first.size() > best->first.size()))
            best = &m;
    return best ? best->second + norm.substr(best->first.size()) : hostPath;
}

std::string Process::getenv(const std::string& name) const {
    std::lock_guard<std::mutex> lk(envMu_);
    for (auto& [k, v] : env_) if (k == name) return v;
    return {};
}

uint32_t Process::getenvAddr(const std::string& name) const {
    std::lock_guard<std::mutex> lk(envMu_);
    auto it = envAddrs_.find(name);
    return it == envAddrs_.end() ? 0 : it->second;
}

void Process::setenv(const std::string& name, const std::string& value) {
    std::lock_guard<std::mutex> lk(envMu_);
    bool found = false;
    for (auto& [k, v] : env_) if (k == name) { v = value; found = true; }
    if (!found) env_.emplace_back(name, value);
    const uint32_t s = internCString(name + "=" + value);
    envAddrs_[name] = s + uint32_t(name.size()) + 1;
    if (env_.size() < 40) {
        for (size_t i = 0; i < env_.size(); ++i) {
            const auto& [k, v] = env_[i];
            mem_->write<uint32_t>(envp_ + uint32_t(i) * 4, envAddrs_[k] - uint32_t(k.size()) - 1);
        }
        mem_->write<uint32_t>(envp_ + uint32_t(env_.size()) * 4, 0);
    }
}

// --------------------------------------------------------------- threads ---

void Process::initThreadState(GuestThread& t, uint32_t stackSize) {
    {
        std::lock_guard<std::mutex> lk(threadsMu_);
        stackTop_ -= stackSize + kGuard;
        t.stackBase = stackTop_ + kGuard;
    }
    t.stackSize = stackSize;
    mem_->map(t.stackBase, stackSize, Memory::kProtRW);
    // A guest-side block standing in for struct _pthread: TSD slots at the
    // start (TPIDRURO points here, slot 0 = self), errno after them.
    t.self = allocStatic(1024, 64);
    t.errnoAddr = t.self + 0x200;
    mem_->write<uint32_t>(t.self, t.self);
    t.cpu.tpidruro = t.self;
    t.cpu.r[CPU::kSP] = t.stackBase + stackSize - 64;
    t.cpu.fpscr = 0x03000000u;     // RunFast (flush-to-zero + default NaN), as iOS sets up
    t.cpu.user = &t;
    t.cpu.engine = engine_.get();
}

void Process::setupMainThread() {
    const uint32_t stack = image_.stackSize ? std::max(image_.stackSize, kMainStackSize) : kMainStackSize;
    initThreadState(*mainThread_, stack);
}

GuestThread& Process::currentThread() {
    if (t_thread && &t_thread->proc == this) return *t_thread;
    // A host thread that isn't a guest thread (e.g. a UI callback) acts as main.
    return *mainThread_;
}

GuestThread* Process::findThread(uint32_t self) {
    if (mainThread_->self == self) return mainThread_;
    std::lock_guard<std::mutex> lk(threadsMu_);
    for (auto& [k, t] : threads_) if (t->self == self) return t.get();
    return nullptr;
}

uint32_t& Process::processSlot(const void* key) {
    std::lock_guard<std::mutex> lk(slotMu_);
    return slots_[key];   // node-based: the reference stays valid
}
uint32_t& Process::threadSlot(const void* key) {
    const uint32_t tid = currentThread().id;
    std::lock_guard<std::mutex> lk(slotMu_);
    return threadSlots_[{key, tid}];
}

std::vector<uint32_t> Process::liveThreadIds() {
    std::vector<uint32_t> out{mainThread_->id};
    std::lock_guard<std::mutex> lk(threadsMu_);
    for (auto& [k, t] : threads_) if (!t->finished) out.push_back(t->id);
    return out;
}
GuestThread* Process::findThreadById(uint32_t id) {
    if (mainThread_->id == id) return mainThread_;
    std::lock_guard<std::mutex> lk(threadsMu_);
    for (auto& [k, t] : threads_) if (t->id == id) return t.get();
    return nullptr;
}

uint32_t Process::startThread(uint32_t fn, uint32_t arg, uint32_t stackSize, bool detached) {
    auto t = std::make_unique<GuestThread>(*this);
    GuestThread* raw = t.get();
    {
        std::lock_guard<std::mutex> lk(threadsMu_);
        raw->id = nextThreadId_++;
    }
    initThreadState(*raw, std::max<uint32_t>(stackSize ? (stackSize + 4095) & ~4095u : kThreadStackSize, 64u << 10));
    raw->detached = detached;
    {
        std::lock_guard<std::mutex> lk(threadsMu_);
        threads_[raw->self] = std::move(t);
    }
    spawnHostThread([this, raw, fn, arg] { threadMain(raw, fn, arg); });
    return raw->self;
}

void Process::threadMain(GuestThread* t, uint32_t fn, uint32_t arg) {
    t_thread = t;
    CPU::Engine::lastFault() = {};
    try {
        t->retval = uint32_t(engine_->callGuest(t->cpu, fn, {arg}));
    } catch (const CPU::GuestFault& f) {
        if (!t->pthreadExited && !exiting_ && !stopping()) finishCrash(t->cpu, f.exit);
    } catch (const std::exception& e) {
        if (!exiting_ && !stopping()) {
            std::lock_guard<std::mutex> lk(exitMu_);
            if (!crashed_) {
                crashed_ = true;
                crashReport_ = std::string("HydrogenVM error: ") + e.what();
                captureCrash(t->cpu, "hle-exception", e.what());
                log(LogLevel::Error, crashReport_);
                engine_->requestStop();
            }
        }
    }

    if (t->objcState) {
        // hostFunctionFor returns std::function<void(Cpu&)>. Its target is the
        // HLE adapter, not void(*)(Cpu&, uint32_t), so a target<> cast of
        // _objc_autoreleasePoolPop is null and never drains. Call the runtime.
        try {
            ObjC::rt(*this).drainPools(t->cpu);
        } catch (const std::exception& e) {
            log(LogLevel::Warning, std::string("autorelease pool drain on thread exit: ") + e.what());
        } catch (...) {
            log(LogLevel::Warning, "autorelease pool drain on thread exit failed");
        }
        if (t->objcStateFree) t->objcStateFree(t->objcState);
        t->objcState = nullptr;
    }
    {
        std::lock_guard<std::mutex> lk(t->m);
        t->finished = true;
    }
    t->cv.notify_all();
    t_thread = nullptr;
}

bool Process::joinThread(uint32_t self, uint32_t* retval) {
    GuestThread* t = findThread(self);
    if (!t || t == mainThread_) return false;
    std::unique_lock<std::mutex> lk(t->m);
    while (!t->finished) {
        if (stopping()) throw ProcessExit(exitCode_, "stopping");
        t->cv.wait_for(lk, std::chrono::milliseconds(50));
    }
    if (retval) *retval = t->retval;
    return true;
}

void Process::detachThread(uint32_t self) {
    if (GuestThread* t = findThread(self)) t->detached = true;
}

// ------------------------------------------------------------------ exit ---

void Process::exit(int code, const std::string& why) {
    if (!exiting_.exchange(true)) {
        exitCode_ = code;
        logf(LogLevel::Info, "App exited with code %d%s%s", code, why.empty() ? "" : ": ", why.c_str());
    }
    requestStop();
    throw ProcessExit(code, why.empty() ? "exit" : why);
}

void Process::addAtExit(uint32_t fn, uint32_t arg, bool hasArg) {
    std::lock_guard<std::mutex> lk(exitMu_);
    atExit_.push_back({fn, arg, hasArg});
}

void Process::runAtExitHandlers(CPU::Cpu& cpu) {
    for (;;) {
        AtExit h;
        {
            std::lock_guard<std::mutex> lk(exitMu_);
            if (atExit_.empty()) return;
            h = atExit_.back();
            atExit_.pop_back();
        }
        if (h.hasArg) engine_->callGuest(cpu, h.fn, {h.arg});
        else engine_->callGuest(cpu, h.fn, {});
    }
}

// ------------------------------------------------------------------- run ---

// HVM_GUEST_PROFILE=<file>: sample every guest thread's current block about once
// a millisecond and write the hottest guest functions (and the host calls the
// guest is waiting in) to <file> when the app ends. Finds the guest code worth a
// native fast path.
namespace {
struct GuestSampler {
    Process& p;
    std::string path;
    std::atomic<bool> stop{false};
    std::thread th;
    std::mutex mu;
    std::unordered_map<uint32_t, uint64_t> guestBlocks, hostCallers, hostFns;
    uint64_t total = 0;
    GuestSampler(Process& proc, std::string out) : p(proc), path(std::move(out)) {
        th = std::thread([this] {
            while (!stop.load()) {
                std::this_thread::sleep_for(std::chrono::microseconds(1000));
                for (uint32_t id : p.liveThreadIds()) {
                    GuestThread* t = p.findThreadById(id);
                    if (!t) continue;
                    const bool inGuest = t->cpu.in_guest.load(std::memory_order_relaxed);
                    const uint32_t pc = inGuest ? t->cpu.block_pc : t->cpu.r[CPU::kLR];
                    if (!pc) continue;
                    const uint32_t hle = t->cpu.current_hle.load(std::memory_order_relaxed);
                    std::lock_guard<std::mutex> lk(mu);
                    ++(inGuest ? guestBlocks : hostCallers)[pc & ~1u];
                    if (!inGuest) ++hostFns[hle];
                    ++total;
                }
            }
        });
    }
    ~GuestSampler() {
        stop = true;
        if (th.joinable()) th.join();
        auto fold = [&](const std::unordered_map<uint32_t, uint64_t>& m) {
            std::map<std::string, uint64_t> bySym;
            for (auto& [pc, n] : m) {
                std::string d = p.describeAddress(pc);
                if (const size_t plus = d.rfind(" + "); plus != std::string::npos) d.resize(plus);
                bySym[d] += n;
            }
            std::vector<std::pair<uint64_t, std::string>> v;
            for (auto& [k, n] : bySym) v.emplace_back(n, k);
            std::sort(v.rbegin(), v.rend());
            return v;
        };
        const auto g = fold(guestBlocks), h = fold(hostCallers);
        uint64_t gsum = 0, hsum = 0;
        for (auto& e : g) gsum += e.first;
        for (auto& e : h) hsum += e.first;
        std::FILE* f = std::fopen(path.c_str(), "w");
        if (!f) return;
        std::fprintf(f, "samples %llu: guest code %.1f%%, host (HLE) calls %.1f%%\n\n", (unsigned long long)total,
                     total ? 100.0 * double(gsum) / double(total) : 0.0, total ? 100.0 * double(hsum) / double(total) : 0.0);
        std::fprintf(f, "## hottest guest functions (nearest symbol; stripped binaries map to the preceding export)\n");
        for (size_t i = 0; i < g.size() && i < 60; ++i)
            std::fprintf(f, "%6.2f%%  %s\n", 100.0 * double(g[i].first) / double(std::max<uint64_t>(total, 1)), g[i].second.c_str());
        std::vector<std::pair<uint64_t, uint32_t>> raw;
        for (auto& [pc, n] : guestBlocks) raw.emplace_back(n, pc);
        std::sort(raw.rbegin(), raw.rend());
        std::fprintf(f, "\n## hottest guest blocks\n");
        for (size_t i = 0; i < raw.size() && i < 40; ++i)
            std::fprintf(f, "%6.2f%%  0x%08x  %s\n", 100.0 * double(raw[i].first) / double(std::max<uint64_t>(total, 1)), raw[i].second,
                         p.describeAddress(raw[i].second).c_str());
        std::vector<std::pair<uint64_t, uint32_t>> fns;
        for (auto& [idx, n] : hostFns) fns.emplace_back(n, idx);
        std::sort(fns.rbegin(), fns.rend());
        std::fprintf(f, "\n## host functions the guest threads are in (blocking waits included)\n");
        for (size_t i = 0; i < fns.size() && i < 40; ++i)
            std::fprintf(f, "%6.2f%%  %s\n", 100.0 * double(fns[i].first) / double(std::max<uint64_t>(total, 1)),
                         fns[i].second == 0xFFFFFFFFu ? "(none: parked or between calls)" : p.hleFunctionName(fns[i].second).c_str());
        std::fprintf(f, "\n## guest code waiting in host calls (by caller)\n");
        for (size_t i = 0; i < h.size() && i < 40; ++i)
            std::fprintf(f, "%6.2f%%  %s\n", 100.0 * double(h[i].first) / double(std::max<uint64_t>(total, 1)), h[i].second.c_str());
        std::fclose(f);
    }
};
} // namespace

int Process::run() {
    std::unique_ptr<GuestSampler> sampler;
    if (const char* prof = std::getenv("HVM_GUEST_PROFILE"); prof && *prof) sampler = std::make_unique<GuestSampler>(*this, prof);
    t_thread = mainThread_;
    CPU::Cpu& c = mainThread_->cpu;
    CPU::Engine::lastFault() = {};
    try {
        // dyld order: libSystem/libobjc setup (+load), then the image's initializers.
        for (auto& h : preInitHooks_) h(c);
        const uint32_t pvars = allocStatic(20);
        mem_->write<uint32_t>(pvars, image_.headerAddr);
        for (const uint32_t fn : image_.initializers)
            engine_->callGuest(c, fn, {argc_, argv_, envp_, apple_, pvars});

        if (image_.entryIsMain) {
            const int r = int(engine_->callGuest(c, image_.entry, {argc_, argv_, envp_, apple_}));
            // Returning from main() == exit(main()).
            const uint32_t exitFn = *lookupSymbol("_exit");
            engine_->callGuest(c, exitFn, {uint32_t(r)});
        } else {
            // crt1 `start` expects the kernel's stack: argc, argv[], 0, envp[], 0, apple[], 0.
            std::vector<uint32_t> words{argc_};
            for (uint32_t a = argv_;; a += 4) { const uint32_t v = mem_->read<uint32_t>(a); words.push_back(v); if (!v) break; }
            for (uint32_t a = envp_;; a += 4) { const uint32_t v = mem_->read<uint32_t>(a); words.push_back(v); if (!v) break; }
            for (uint32_t a = apple_;; a += 4) { const uint32_t v = mem_->read<uint32_t>(a); words.push_back(v); if (!v) break; }
            uint32_t sp = (c.r[CPU::kSP] - uint32_t(words.size()) * 4) & ~15u;
            for (size_t i = 0; i < words.size(); ++i) mem_->write<uint32_t>(sp + uint32_t(i) * 4, words[i]);
            c.r[CPU::kSP] = sp;
            c.r[CPU::kLR] = 0;
            c.r[CPU::kPC] = image_.entry & ~1u;
            c.thumb = image_.entry & 1;
            const CPU::Exit e = engine_->runToExit(c);
            if (!exiting_ && !(e == CPU::Exit::Halt && stopping())) {
                CPU::Engine::captureFault(c, e);
                finishCrash(c, e);
            }
        }
    } catch (const CPU::GuestFault& f) {
        if (!exiting_ && !(f.exit == CPU::Exit::Halt && stopping())) finishCrash(c, f.exit);
    } catch (const ProcessExit&) {
        // exit() called directly from host code
    } catch (const std::exception& e) {
        if (!exiting_) {
            try { flushGuestStdio(*this); } catch (...) {}
            crashed_ = true;
            crashReport_ = std::string("HydrogenVM error: ") + e.what();
            captureCrash(c, "hle-exception", e.what());
            log(LogLevel::Error, crashReport_);
        }
    }
    for (auto& h : exitHooks_) {
        try { h(); } catch (const std::exception& e) { logf(LogLevel::Warning, "exit hook failed: %s", e.what()); }
    }
    requestStop();
    sampler.reset();
    return crashed_ ? -1 : exitCode_;
}

void Process::finishCrash(const CPU::Cpu& c, CPU::Exit e) {
    try { flushGuestStdio(*this); } catch (...) {}   // what the app printed before dying is the most useful clue
    std::lock_guard<std::mutex> lk(exitMu_);
    if (crashed_ || exiting_) return;
    if (terminating_.load()) {
        log(LogLevel::Info, "The app saved and closed");
        requestStop();
        return;
    }
    crashed_ = true;
    const CPU::FaultSnapshot& f = CPU::Engine::lastFault();
    uint32_t r[16];
    if (f.valid) std::memcpy(r, f.r, sizeof r); else std::memcpy(r, c.r, sizeof r);
    const CPU::Exit why = f.valid ? f.exit : e;
    const uint32_t info = f.valid ? f.info : c.exit_info;
    std::string head;
    char buf[256];
    switch (why) {
        case CPU::Exit::MemFault:
            std::snprintf(buf, sizeof buf, "Crash: bad memory access at 0x%08x", info);
            head = buf;
            if (engine_->isHleAddress(info & ~3u))
                head += " (inside missing data symbol " + engine_->hleName(engine_->hleIndex(info & ~3u)) + ")";
            else if (info < 0x1000) head += " (NULL pointer)";
            break;
        case CPU::Exit::Undefined: {
            const uint32_t insn = (f.valid ? f.thumb : c.thumb) ? (uint32_t(mem_->read<uint16_t>(r[15])) << 16 | mem_->read<uint16_t>(r[15] + 2))
                                                                : mem_->read<uint32_t>(r[15]);
            std::snprintf(buf, sizeof buf, "Crash: unsupported instruction 0x%08x at %s", insn, describeAddress(r[15]).c_str());
            head = buf;
            break;
        }
        case CPU::Exit::Breakpoint: head = "Crash: breakpoint / trap instruction (often __builtin_trap or an assertion)"; break;
        case CPU::Exit::Svc: std::snprintf(buf, sizeof buf, "Crash: direct system call (svc #%u) isn't supported", info); head = buf; break;
        case CPU::Exit::HostError: head = "Stopped: " + CPU::Engine::lastHostError(); break;
        case CPU::Exit::Halt: head = "Stopped"; break;
        default: head = "Crash"; break;
    }
    std::string rep = head + "\n";
    std::snprintf(buf, sizeof buf, "  pc %s\n  lr %s\n", describeAddress(r[15]).c_str(), describeAddress(r[14]).c_str());
    rep += buf;
    for (int i = 0; i < 16; i += 4) {
        std::snprintf(buf, sizeof buf, "  r%-2d %08x  r%-2d %08x  r%-2d %08x  r%-2d %08x\n", i, r[i], i + 1, r[i + 1], i + 2, r[i + 2], i + 3, r[i + 3]);
        rep += buf;
    }
    CPU::Cpu tmp;
    std::memcpy(tmp.r, r, sizeof r);
    rep += "  backtrace:\n" + backtrace(tmp);
    crashReport_ = rep;
    CPU::Cpu snapshot;
    std::memcpy(snapshot.r, r, sizeof r);
    captureCrash(snapshot, std::to_string(int(why)), why == CPU::Exit::HostError ? CPU::Engine::lastHostError() : "");
    log(LogLevel::Error, rep);
    engine_->requestStop();
}

namespace {
std::string quote(const std::string& value) {
    std::ostringstream out;
    out << '"';
    for (unsigned char c : value) {
        if (c == '"' || c == '\\') out << '\\' << char(c);
        else if (c < 32 || c >= 127) out << "\\u00" << std::hex << std::setw(2) << std::setfill('0') << unsigned(c);
        else out << char(c);
    }
    return out.str() + '"';
}
}

void Process::captureCrash(const CPU::Cpu& c, const std::string& kind, const std::string& detail) {
    std::copy(c.r, c.r + 16, crashRegisters_.begin());
    crashKind_ = kind;
    crashDetail_ = detail;
    // HLE PCs are host-dispatch slots, not stable image offsets. Use the guest caller.
    const uint32_t pc = engine_->isHleAddress(c.r[15] & ~1u) ? c.r[14] : c.r[15];
    crashLocation_ = fs::path(executablePath_).filename().string() + "+" + (pc ? std::to_string((pc & ~1u) - image_.headerAddr) : "unavailable");
    // Do not hash caller descriptions: their absolute addresses vary with image placement.
    const auto end = detail.find(" (called from ");
    const std::string key = kind + "\n" + crashLocation_ + "\n" + detail.substr(0, end);
    unsigned char digest[20];
#if defined(__APPLE__)
    CC_SHA1(key.data(), CC_LONG(key.size()), digest);
#elif defined(HVM_HAVE_OPENSSL)
    SHA1(reinterpret_cast<const unsigned char*>(key.data()), key.size(), digest);
#else
    const auto bytes = hvmDigest(1, key);
    std::copy(bytes.begin(), bytes.end(), digest);
#endif
    std::ostringstream hash;
    for (int i = 0; i < 6; ++i) hash << std::hex << std::setw(2) << std::setfill('0') << unsigned(digest[i]);
    crashSignature_ = hash.str();
}

std::string Process::crashBundle() {
    if (!crashed_) return "null";
    std::ostringstream out;
    std::string summary = crashReport_.substr(0, crashReport_.find('\n'));
    const auto missing = crashDetail_.find("Missing function: ");
    if (missing != std::string::npos) {
        auto name = crashDetail_.substr(missing + 18);
        name = name.substr(0, name.find(" (called from "));
        summary = "This game needs " + name + ", which HydrogenVM does not support yet.";
    }
    out << "{\"signature\":" << quote(crashSignature_) << ",\"exceptionKind\":" << quote(crashKind_)
        << ",\"location\":" << quote(crashLocation_) << ",\"detail\":" << quote(crashDetail_)
        << ",\"summary\":" << quote(summary)
        << ",\"report\":" << quote(crashReport_) << ",\"bundleId\":" << quote(infoPlist_ ? infoPlist_->getString("CFBundleIdentifier") : "")
        << ",\"appVersion\":" << quote(infoPlist_ ? infoPlist_->getString("CFBundleShortVersionString") : "")
        << ",\"executableSHA256\":" << quote(executableHash_) << ",\"host\":{\"pageSize\":" << sysconf(_SC_PAGESIZE)
        << ",\"emulatorCommit\":" << quote(HVM_COMMIT) << "},\"registers\":[";
    for (size_t i = 0; i < crashRegisters_.size(); ++i) out << (i ? "," : "") << crashRegisters_[i];
    out << "],\"backtrace\":[";
    // Walk only once execution has stopped; the watchdog has no safe register snapshot.
    CPU::Cpu c{};
    std::copy(crashRegisters_.begin(), crashRegisters_.end(), c.r);
    std::istringstream trace(backtrace(c));
    bool first = true;
    for (std::string line; std::getline(trace, line);) {
        out << (first ? "" : ",") << quote(line); first = false;
    }
    out << "],\"unimplementedImports\":[";
    for (size_t i = 0; i < unimplementedImports_.size(); ++i) out << (i ? "," : "") << quote(unimplementedImports_[i]);
    out << "],\"logs\":[";
    {
        std::lock_guard<std::mutex> lk(logRingMu_);
        for (size_t i = 0; i < logRing_.size(); ++i) out << (i ? "," : "") << quote(logRing_[i]);
    }
    out << "],\"fileHistory\":[";
    {
        std::lock_guard<std::mutex> lk(fileRingMu_);
        for (size_t i = 0; i < fileRing_.size(); ++i) {
            const auto& f = fileRing_[i];
            out << (i ? "," : "")
                << "{\"path\":" << quote(f.path)
                << ",\"api\":" << quote(f.api)
                << ",\"hostResult\":" << f.hostResult
                << ",\"size\":" << f.size
                << ",\"bytesRead\":" << f.bytesRead
                << ",\"isOpen\":" << (f.isOpen ? "true" : "false") << "}";
        }
    }
    const double wall = std::chrono::duration<double>(std::chrono::steady_clock::now() - perf_.origin).count();
    out << "],\"perf\":{" << perfJson(perf_, wall, guestInstructions(), engine_->stats().blocks_decoded) << "}}";
    return out.str();
}

// ------------------------------------------------------------- microphone ---

void Process::pushMicrophone(const float* mono, size_t frames, double rate) {
    if (!mono || !frames || !(rate > 0)) return;
    std::lock_guard<std::mutex> lk(micMu_);
    micRate_ = rate;
    mic_.insert(mic_.end(), mono, mono + frames);
    // Keep at most a second: a recording queue that fell behind starts from "now".
    const size_t cap = size_t(rate);
    if (mic_.size() > cap) { mic_.erase(mic_.begin(), mic_.begin() + std::ptrdiff_t(mic_.size() - cap)); micPos_ = 0; }
}

void Process::pullMicrophone(float* out, size_t frames, double rate) {
    std::lock_guard<std::mutex> lk(micMu_);
    const double step = rate > 0 ? micRate_ / rate : 1;
    for (size_t i = 0; i < frames; ++i) {
        const size_t at = size_t(micPos_);
        if (at + 1 < mic_.size()) {
            const double t = micPos_ - double(at);
            out[i] = float(mic_[at] * (1 - t) + mic_[at + 1] * t);
            micPos_ += step;
        } else {
            out[i] = 0;   // not captured (yet): silence
        }
    }
    const size_t used = std::min(mic_.size(), size_t(micPos_));
    mic_.erase(mic_.begin(), mic_.begin() + std::ptrdiff_t(used));
    micPos_ -= double(used);
}

void Process::microphoneUser(bool start) {
    bool notify = false, on = false;
    {
        std::lock_guard<std::mutex> lk(micMu_);
        const int before = micUsers_;
        micUsers_ = std::max(0, micUsers_ + (start ? 1 : -1));
        notify = (before == 0) != (micUsers_ == 0);
        on = micUsers_ > 0;
        if (notify && !on) { mic_.clear(); micPos_ = 0; }
    }
    if (notify) {
        log(LogLevel::Info, on ? "Microphone: the app started recording" : "Microphone: recording stopped");
        if (cfg_.microphone) cfg_.microphone(on);
    }
}

// ------------------------------------------------------------ diagnostics ---

uint64_t Process::recordFileOpen(const std::string& path, const std::string& api, int hostResult, int64_t size, int64_t bytesRead, bool isOpen) {
    std::lock_guard<std::mutex> lk(fileRingMu_);
    if (fileRing_.size() >= 32) fileRing_.pop_front();
    const uint64_t h = ++nextFileHandle_;
    fileRing_.push_back({h, path, api, hostResult, size, bytesRead, isOpen});
    return h;
}

void Process::updateFileRead(uint64_t handle, int64_t bytesRead) {
    if (!handle) return;
    std::lock_guard<std::mutex> lk(fileRingMu_);
    for (auto& r : fileRing_) {
        if (r.handle == handle) { r.bytesRead = bytesRead; break; }
    }
}

void Process::recordFileClose(uint64_t handle) {
    if (!handle) return;
    std::lock_guard<std::mutex> lk(fileRingMu_);
    for (auto& r : fileRing_) {
        if (r.handle == handle) { r.isOpen = false; break; }
    }
}

std::vector<Process::FileRecord> Process::fileRing() const {
    std::lock_guard<std::mutex> lk(fileRingMu_);
    return std::vector<FileRecord>(fileRing_.begin(), fileRing_.end());
}

void Process::log(LogLevel lvl, const std::string& msg) const {
    {
        std::lock_guard<std::mutex> lk(logRingMu_);
        std::istringstream lines(msg);
        for (std::string line; std::getline(lines, line);) {
            if (logRing_.size() == 2000) logRing_.pop_front();
            logRing_.push_back(line.substr(0, 4096));
            CPU::Engine::recordLogLineAsyncSafe(line.data(), line.size());   // for host-fault diagnostics
        }
    }
    if (cfg_.log) { cfg_.log(lvl, msg); return; }
    const char* tag = lvl == LogLevel::Error ? "[error] " : lvl == LogLevel::Warning ? "[warn] " : lvl == LogLevel::Debug ? "[debug] " : "";
    if (lvl == LogLevel::GuestStdout) { std::fwrite(msg.data(), 1, msg.size(), stdout); std::fflush(stdout); return; }
    if (lvl == LogLevel::GuestStderr) { std::fwrite(msg.data(), 1, msg.size(), stderr); return; }
    std::fprintf(stderr, "%s%s\n", tag, msg.c_str());
}

void Process::logf(LogLevel lvl, const char* fmt, ...) const {
    char buf[2048];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    log(lvl, buf);
}

void Process::reportMissing(const std::string& what) {
    {
        std::lock_guard<std::mutex> lk(missingMu_);
        if (!missing_.insert(what).second) return;
    }
    log(LogLevel::Warning, "Missing: " + what);
}

uint64_t Process::guestInstructions() {
    std::lock_guard<std::mutex> lk(threadsMu_);
    uint64_t n = 0;
    bool sawMain = false;
    for (auto& [k, t] : threads_) { n += t->cpu.insn_count; sawMain |= t.get() == mainThread_; }
    if (mainThread_ && !sawMain) n += mainThread_->cpu.insn_count;
    return n;
}

std::vector<std::string> Process::missingSymbols() const {
    std::lock_guard<std::mutex> lk(missingMu_);
    return {missing_.begin(), missing_.end()};
}

std::string Process::hleFunctionName(uint32_t index) const { return demangle(engine_->hleName(index)); }
std::function<void(CPU::Cpu&)> Process::hostFunctionFor(const std::string& symbol) const {
    const auto addr = lookupSymbol(symbol);
    if (!addr || !engine_->isHleAddress(*addr & ~1u)) return {};
    return engine_->hleFunction(engine_->hleIndex(*addr & ~1u));
}

std::string Process::describeAddress(uint32_t addr) const {
    char buf[32];
    if (engine_->isHleAddress(addr & ~1u)) return "[HydrogenVM] " + demangle(engine_->hleName(engine_->hleIndex(addr & ~1u)));
    std::snprintf(buf, sizeof buf, "0x%08x", addr);
    std::string s = image_.symbolicate(addr & ~1u);
    for (const auto& segment : image_.segments) {
        if ((addr & ~1u) >= segment.vmaddr && (addr & ~1u) - segment.vmaddr < segment.vmsize) {
            s = fs::path(executablePath_).filename().string() + "+" + std::to_string((addr & ~1u) - image_.headerAddr) + " " + s;
            break;
        }
    }
    return s.empty() ? std::string(buf) : std::string(buf) + " " + demangle(s);
}

std::string Process::backtrace(const CPU::Cpu& c, int maxFrames) const {
    std::string out = "    " + describeAddress(c.r[15]) + "\n";
    if (!engine_->isHleAddress(c.r[14] & ~1u)) out += "    " + describeAddress(c.r[14]) + "\n";
    uint32_t fp = c.r[7];
    for (int i = 0; i < maxFrames && fp && (fp & 3) == 0; ++i) {
        if (!mem_->isMapped(fp) || !mem_->isMapped(fp + 4)) break;
        const uint32_t next = mem_->read<uint32_t>(fp), lr = mem_->read<uint32_t>(fp + 4);
        if (!lr) break;
        out += "    " + describeAddress(lr) + "\n";
        if (next <= fp) break;
        fp = next;
    }
    return out;
}

std::string perfJson(const Process::Perf& pf, double wall, uint64_t insns, uint64_t blocksDecoded) {
    const uint64_t frames = pf.frames.load();
    const uint64_t presented = pf.framesPresented.load();
    const uint64_t ttff = pf.timeToFirstFrameNs.load();
    std::ostringstream f;
    f << "    \"wallSeconds\": " << wall << ",\n";
    f << "    \"guestInstructions\": " << insns << ",\n";
    f << "    \"guestMIPS\": " << (wall > 0 ? double(insns) / 1e6 / wall : 0) << ",\n";
    f << "    \"blocksDecoded\": " << blocksDecoded << ",\n";
    f << "    \"frames\": " << frames << ",\n";
    f << "    \"framesPresented\": " << presented << ",\n";
    f << "    \"avgFrameMs\": " << (frames ? double(pf.frameNs.load()) / 1e6 / double(frames) : 0) << ",\n";
    f << "    \"avgCompositeMs\": " << (presented ? double(pf.compositeNs.load()) / 1e6 / double(presented) : 0) << ",\n";
    f << "    \"maxFrameMs\": " << double(pf.maxFrameNs.load()) / 1e6 << ",\n";
    f << "    \"slowFrames\": " << pf.slowFrames.load() << ",\n";
    f << "    \"p50FrameMs\": " << double(pf.frameTimes.percentile(50)) / 1e6 << ",\n";
    f << "    \"p95FrameMs\": " << double(pf.frameTimes.percentile(95)) / 1e6 << ",\n";
    f << "    \"p99FrameMs\": " << double(pf.frameTimes.percentile(99)) / 1e6 << ",\n";
    f << "    \"timeToFirstFrameMs\": " << (ttff == ~uint64_t{0} ? 0.0 : double(ttff) / 1e6) << "\n";
    return f.str();
}

} // namespace Hydrogen::Runtime
