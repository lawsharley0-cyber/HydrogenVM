// HydrogenVM — one emulated iOS process: guest memory, CPU engine, loaded
// image, host-implemented system libraries, guest threads, logging.
#pragma once
#include "Abi.hpp"
#include "FrameHistogram.hpp"
#include "Heap.hpp"
#include "loader/MachO.hpp"
#include "loader/Plist.hpp"
#include <atomic>
#include <condition_variable>
#include <functional>
#include <memory>
#include <map>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <thread>
#include <typeindex>
#include <unordered_map>
#include <vector>
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <utility>

namespace Hydrogen::Runtime {

enum class LogLevel { Debug, Info, Warning, Error, GuestStdout, GuestStderr };

struct ProcessConfig {
    std::string bundlePath;            // …/Foo.app
    std::string homePath;              // sandbox home (Documents/, Library/, tmp/ live here); default: bundle's parent
    std::vector<std::string> extraArgs;
    std::function<void(LogLevel, const std::string&)> log;   // called from any thread
    bool stopOnMissingFunction = true; // false: log and return 0
    bool traceCalls = false;           // log every HLE call (very slow, for debugging)
    bool loopbackNetwork = false;     // opt-in TCP/IPv4 diagnostics; only 127/8 and localhost
    std::vector<std::string> loopbackAliases; // explicitly route these names to 127.0.0.1
    std::string systemVersion = "6.1"; // reported by UIDevice / sysctl
    std::string deviceModel = "iPhone5,2";
    uint32_t screenWidth = 320, screenHeight = 568;   // points
    float screenScale = 2.0f;
    // Called on the emulated main thread with each new frame (premultiplied
    // RGBA, screenWidth*scale x screenHeight*scale, portrait). Optional.
    std::function<void(const uint8_t* rgba, int width, int height)> present;
    // Audio: the platform pulls mixed sound from Process::mixer() (see audio/Audio.hpp).
};

// Input from the host (screen points, portrait orientation of the emulated device).
enum class TouchPhase { Began = 0, Moved = 1, Ended = 3, Cancelled = 4 };

class Process;
namespace Audio { class Mixer; }

struct GuestThread {
    explicit GuestThread(Process& p) : proc(p) {}
    Process& proc;
    CPU::Cpu cpu;
    uint32_t id = 0;
    uint32_t self = 0;            // guest pthread_t (address of a guest-side block)
    uint32_t stackBase = 0, stackSize = 0;
    uint32_t errnoAddr = 0;
    std::unordered_map<uint32_t, uint32_t> tsd;   // pthread keys
    std::thread host;
    std::atomic<bool> finished{false};
    bool detached = false;
    bool pthreadExited = false;   // left via pthread_exit()
    uint32_t retval = 0;
    std::mutex m;
    std::condition_variable cv;
    // SJLJ exception unwinding (libc/Unwind.cpp): registered function contexts
    // (top of the list, and whether each registrant was Thumb code) and the
    // stack of exceptions currently being handled by catch blocks.
    uint32_t sjljTop = 0;
    std::unordered_map<uint32_t, bool> sjljThumb;
    std::vector<uint32_t> caughtExceptions;
    void* objcState = nullptr;    // per-thread ObjC runtime data (autorelease pools)
    void (*objcStateFree)(void*) = nullptr;
    ~GuestThread() { if (objcState && objcStateFree) objcStateFree(objcState); }
};

// Thrown by HLE functions to end the process (exit(), abort(), fatal errors).
struct ProcessExit : std::runtime_error {
    int code;
    explicit ProcessExit(int c, const std::string& why = "process exited") : std::runtime_error(why), code(c) {}
};

// Thrown for a call to an import nobody implemented (when stopOnMissingFunction).
struct MissingFunction : std::runtime_error { using std::runtime_error::runtime_error; };

class Process {
public:
    explicit Process(ProcessConfig cfg);
    ~Process();
    Process(const Process&) = delete;
    Process& operator=(const Process&) = delete;

    // Load the app's executable, bind imports, register ObjC metadata.
    void load();
    // Run the app on the calling host thread until it exits or crashes.
    // Returns the exit code (or -1 after a crash; see crashReport()).
    int run();
    // From any thread: stop all guest threads soon.
    void requestStop();

    // App lifecycle, from any host thread (the iOS app calls these when the user leaves a
    // game or HydrogenVM goes to the background). UIKit delivers the matching delegate
    // calls and notifications on the guest's main thread, so the game can save.
    //  * requestGracefulStop: resign active -> enter background -> will terminate, flush
    //    NSUserDefaults and stdio, then stop. Forces requestStop() after `timeout` seconds.
    //  * enterBackground: resign active -> enter background (game saves), then the guest is
    //    paused: main thread parked, other guest threads suspended, audio silenced.
    //  * enterForeground: resume, then will-enter-foreground -> did-become-active.
    enum class Lifecycle { Background, Foreground, Terminate };
    void setLifecycleHandler(std::function<void(Lifecycle)> h) { lifecycleHandler_ = std::move(h); }
    void requestGracefulStop(double timeout = 5.0);
    void enterBackground();
    void enterForeground();
    bool backgrounded() const { return backgrounded_.load(); }
    bool gracefulStopRequested() const { return gracefulStop_.load(); }
    // Used by the lifecycle handler while the app is in the background (parks the caller).
    void waitWhileBackgrounded();
    void setBackgrounded(bool b);
    // Everything the app plays is mixed here; the platform renders it on its
    // audio thread through mixerPtr() (safe to keep after the Process ends).
    Audio::Mixer& mixer() { return *mixer_; }
    std::shared_ptr<Audio::Mixer> mixerPtr() const { return mixer_; }
    bool stopping() const { return engine_->stopRequested(); }

    // ---- performance counters (read any time; reported by `hvm --report`)
    struct Perf {
        std::atomic<uint64_t> frames{0};          // frame hooks run (60 Hz main run loop)
        std::atomic<uint64_t> framesPresented{0}; // frames that changed and were composited
        std::atomic<uint64_t> frameNs{0};         // host time inside frame hooks (input, links, layout, display, composite)
        std::atomic<uint64_t> compositeNs{0};     // of which compositing
        std::atomic<uint64_t> maxFrameNs{0};
        std::atomic<uint64_t> slowFrames{0};      // frame hooks over 16.7 ms
        // ~0 until the first presented frame. 0 ns is a real measurement.
        std::atomic<uint64_t> timeToFirstFrameNs{~uint64_t{0}};
        FrameHistogram frameTimes;
        // Set when this process object starts, before load() and run().
        std::chrono::steady_clock::time_point origin{std::chrono::steady_clock::now()};

        // Same updates the frame hook used to do inline, plus one bounded histogram sample.
        void recordFrame(uint64_t ns) noexcept {
            frames++;
            frameNs += ns;
            if (ns > 16'700'000) slowFrames++;
            uint64_t m = maxFrameNs.load();
            while (ns > m && !maxFrameNs.compare_exchange_weak(m, ns)) {}
            frameTimes.add(ns);
        }
        // Clock read only until the first presented frame is recorded.
        void notePresented() noexcept {
            framesPresented++;
            if (timeToFirstFrameNs.load(std::memory_order_relaxed) != ~uint64_t{0}) return;
            const auto delta = std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::steady_clock::now() - origin);
            const uint64_t since = delta.count() > 0 ? uint64_t(delta.count()) : 0;
            uint64_t expected = ~uint64_t{0};
            timeToFirstFrameNs.compare_exchange_strong(expected, since);
        }
        // Test seam: `sinceStartNs` is nanoseconds since `origin`. The first call wins.
        void notePresentedAt(uint64_t sinceStartNs) noexcept {
            framesPresented++;
            uint64_t expected = ~uint64_t{0};
            timeToFirstFrameNs.compare_exchange_strong(expected, sinceStartNs);
        }
    };
    Perf& perf() { return perf_; }
    // Guest instructions retired by every thread so far (approximate while running).
    uint64_t guestInstructions();

    // ---- results
    bool crashed() const { return crashed_; }
    const std::string& crashReport() const { return crashReport_; }
    std::vector<std::string> missingSymbols() const;
    // Imports bound to "missing" stubs at load (functions), in file order — the static gap list.
    const std::vector<std::string>& unimplementedImports() const { return unimplementedImports_; }
    // Lazily created guest objects/buffers that must not outlive this process. HLE code
    // must never cache guest addresses in C++ statics: the iOS app runs one Process per
    // launch, and a static would hand the next launch an address from the previous one.
    //   static const char key = 0; uint32_t& v = x.proc.processSlot(&key); if (!v) v = ...;
    uint32_t& processSlot(const void* key);
    uint32_t& threadSlot(const void* key);   // per guest thread (strtok, inet_ntoa, asctime buffers)
    int exitCode() const { return exitCode_; }

    // ---- subsystems
    const ProcessConfig& config() const { return cfg_; }
    Memory::GuestArena& mem() { return *mem_; }
    CPU::Engine& engine() { return *engine_; }
    Heap& heap() { return *heap_; }
    const Loader::Image& image() const { return image_; }
    const Plist::Value* infoPlist() const { return infoPlist_.get(); }

    // Type-indexed services (ObjC runtime, Foundation state, ...).
    template <typename T> T& service() {
        std::lock_guard<std::mutex> lk(servicesMu_);
        auto& p = services_[std::type_index(typeid(T))];
        if (!p) p = std::make_shared<T>(*this);
        return *static_cast<T*>(p.get());
    }

    // ---- symbols
    using HostFn = std::function<void(Ctx&)>;
    uint32_t defineFunction(const std::string& name, HostFn fn);
    template <auto F> uint32_t def(const std::string& name) {
        return defineFunction(name, [](Ctx& x) { callWrapped<F>(x); });
    }
    void defineData(const std::string& name, uint32_t addr);
    // Allocate `size` bytes of zeroed guest data and export it as `name`.
    uint32_t defineDataBlock(const std::string& name, uint32_t size);
    std::optional<uint32_t> lookupSymbol(const std::string& name) const;
    // Hooks consulted (in order) for imports that nothing defines. Return 0
    // to decline. Used for e.g. auto-creating stub ObjC classes.
    void addMissingSymbolResolver(std::function<uint32_t(const std::string&)> fn);
    // Register a host function without exporting a symbol (method IMPs, …).
    uint32_t registerHostFunction(const std::string& debugName, HostFn fn);

    // Calls `fn` on every registration of a module; used by service objects
    // that need the process fully constructed.
    void onLoaded(std::function<void()> fn) { loadedHooks_.push_back(std::move(fn)); }
    // Called on the main thread before the image's initializers run (ObjC +load).
    void onBeforeInitializers(std::function<void(CPU::Cpu&)> fn) { preInitHooks_.push_back(std::move(fn)); }
    // Host-side cleanup when run() finishes (flush preferences, ...).
    void onExit(std::function<void()> fn) { exitHooks_.push_back(std::move(fn)); }
    // Consulted when a host function throws: returns true if it turned the
    // error into a guest exception that the app catches (Foundation installs
    // one that raises NSExceptions for Cocoa-style errors).
    void setHostErrorHook(std::function<bool(Ctx&, const std::exception&)> fn) { hostErrorHook_ = std::move(fn); }

    // ---- calling guest code
    uint64_t callGuest(CPU::Cpu& cpu, uint32_t fn, std::initializer_list<uint32_t> args) {
        return engine_->callGuest(cpu, fn, args);
    }
    uint64_t callGuest(CPU::Cpu& cpu, uint32_t fn, const std::vector<uint32_t>& args) {
        return engine_->callGuest(cpu, fn, args.data(), args.size());
    }

    // ---- threads
    GuestThread& currentThread();
    GuestThread* findThread(uint32_t self);
    GuestThread* findThreadById(uint32_t id);
    std::vector<uint32_t> liveThreadIds();   // ids of the main thread and all running threads
    // Start a guest thread running fn(arg). Returns the guest pthread_t.
    uint32_t startThread(uint32_t fn, uint32_t arg, uint32_t stackSize, bool detached);
    bool joinThread(uint32_t self, uint32_t* retval);
    void detachThread(uint32_t self);
    GuestThread& mainThread() { return *mainThread_; }

    // ---- exit
    [[noreturn]] void exit(int code, const std::string& why = {});
    void addAtExit(uint32_t fn, uint32_t arg, bool hasArg);
    void runAtExitHandlers(CPU::Cpu& cpu);

    // ---- guest memory helpers
    std::string readCString(uint32_t addr, size_t max = 1u << 20) const;
    uint32_t allocCString(const std::string& s);           // heap, caller frees
    uint32_t internCString(const std::string& s);          // never freed, deduplicated
    uint32_t allocStatic(uint32_t size, uint32_t align = 16);   // never freed

    // ---- paths
    const std::string& bundlePath() const { return cfg_.bundlePath; }
    const std::string& homePath() const { return homePath_; }
    const std::string& executablePath() const { return executablePath_; }

    // ---- logging / diagnostics
    void log(LogLevel lvl, const std::string& msg) const;
    void logf(LogLevel lvl, const char* fmt, ...) const __attribute__((format(printf, 3, 4)));
    void reportMissing(const std::string& what);   // logs once per name
    std::string describeAddress(uint32_t addr) const;
    std::string backtrace(const CPU::Cpu& c, int maxFrames = 24) const;

    // ---- host input (any thread); UIKit installs the handler
    void injectTouch(TouchPhase phase, float x, float y, uint32_t fingerId) { if (touchHandler_) touchHandler_(phase, x, y, fingerId); }
    void setAcceleration(float x, float y, float z) { if (accelHandler_) accelHandler_(x, y, z); }
    void setTouchHandler(std::function<void(TouchPhase, float, float, uint32_t)> h) { touchHandler_ = std::move(h); }
    void setAccelerationHandler(std::function<void(float, float, float)> h) { accelHandler_ = std::move(h); }
    // Screen actually used by the app (UIKit picks 3.5" vs 4" from the bundle).
    void setScreenSize(uint32_t w, uint32_t h) { cfg_.screenWidth = w; cfg_.screenHeight = h; }

    // Guest process environment.
    uint32_t argc() const { return argc_; }
    uint32_t argvAddr() const { return argv_; }
    uint32_t envpAddr() const { return envp_; }
    uint32_t appleAddr() const { return apple_; }
    std::string getenv(const std::string& name) const;
    uint32_t getenvAddr(const std::string& name) const;   // guest pointer or 0
    void setenv(const std::string& name, const std::string& value);

private:
    void bindImports();
    uint32_t resolveImport(const Loader::BindRecord& b);
    uint32_t makeMissingStub(const std::string& name);
    void setupMainThread();
    void setupEnvironment();
    void initThreadState(GuestThread& t, uint32_t stackSize);
    void threadMain(GuestThread* t, uint32_t fn, uint32_t arg);
    void finishCrash(const CPU::Cpu& c, CPU::Exit e);

    ProcessConfig cfg_;
    std::unique_ptr<Memory::GuestArena> mem_;
    std::unique_ptr<CPU::Engine> engine_;
    std::unique_ptr<Heap> heap_;
    Loader::Image image_;
    Plist::ValuePtr infoPlist_;
    std::string homePath_, executablePath_;

    std::unordered_map<std::string, uint32_t> symbols_;
    std::vector<std::function<uint32_t(const std::string&)>> missingResolvers_;
    mutable std::mutex symMu_;
    std::unordered_map<std::type_index, std::shared_ptr<void>> services_;
    std::mutex servicesMu_;
    std::vector<std::function<void()>> loadedHooks_;
    std::vector<std::function<void(CPU::Cpu&)>> preInitHooks_;
    std::vector<std::function<void()>> exitHooks_;

    // threads
    std::unique_ptr<GuestThread> mainThreadOwner_;
    GuestThread* mainThread_ = nullptr;
    std::unordered_map<uint32_t, std::unique_ptr<GuestThread>> threads_;
    std::vector<std::string> unimplementedImports_;
    std::function<void(Lifecycle)> lifecycleHandler_;
    struct Liveness { std::mutex mu; bool alive = true; };
    std::shared_ptr<Liveness> liveness_ = std::make_shared<Liveness>();   // for helper threads that outlive us
    std::atomic<bool> backgrounded_{false}, gracefulStop_{false};
    std::mutex lifeMu_;
    std::condition_variable lifeCv_;
    std::mutex slotMu_;
    std::unordered_map<const void*, uint32_t> slots_;
    std::map<std::pair<const void*, uint32_t>, uint32_t> threadSlots_;
    std::mutex threadsMu_;
    uint32_t nextThreadId_ = 1;
    uint32_t stackTop_ = 0xBF000000u;

    // static data region
    uint32_t staticTop_ = 0, staticEnd_ = 0;
    std::unordered_map<std::string, uint32_t> interned_;
    std::mutex staticMu_;

    // environment
    uint32_t argc_ = 0, argv_ = 0, envp_ = 0, apple_ = 0;
    std::vector<std::pair<std::string, std::string>> env_;
    std::unordered_map<std::string, uint32_t> envAddrs_;
    mutable std::mutex envMu_;

    // exit
    struct AtExit { uint32_t fn, arg; bool hasArg; };
    std::vector<AtExit> atExit_;
    std::mutex exitMu_;
    std::atomic<bool> exiting_{false};
    int exitCode_ = 0;
    bool crashed_ = false;
    std::function<bool(Ctx&, const std::exception&)> hostErrorHook_;
    std::shared_ptr<Audio::Mixer> mixer_;
    std::string crashReport_;

    std::function<void(TouchPhase, float, float, uint32_t)> touchHandler_;
    std::function<void(float, float, float)> accelHandler_;

    // diagnostics
    Perf perf_;
    mutable std::mutex missingMu_;
    std::set<std::string> missing_;
    std::set<std::string> calledImports_;   // symbols the app reaches through lazy (call) stubs
};

// Module registration (each lives in its own .cpp).
void registerLibcString(Process& p);
void registerCompilerRt(Process& p);
void registerLibcStdio(Process& p);
void registerLibcStdlib(Process& p);
void registerLibcMath(Process& p);
void registerLibcTime(Process& p);
void registerLibcPthread(Process& p);
void registerLibcSystem(Process& p);
void registerCxxRuntime(Process& p);
void registerUnwinder(Process& p);
void registerLibStdcxx(Process& p);
void registerLibcPosix(Process& p);
void registerZlib(Process& p);
void registerLibcNet(Process& p);
void registerCommonCrypto(Process& p);
void registerSqlite(Process& p);
void registerObjCRuntime(Process& p);
void registerFoundation(Process& p);
void registerUIKit(Process& p);

// Inner lines of the --report "perf" object. Legacy keys keep their formulas.
std::string perfJson(const Process::Perf& perf, double wallSeconds, uint64_t guestInstructions, uint64_t blocksDecoded);

} // namespace Hydrogen::Runtime
