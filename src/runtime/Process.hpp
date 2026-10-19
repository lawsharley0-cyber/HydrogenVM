// HydrogenVM — one emulated iOS process: guest memory, CPU engine, loaded
// image, host-implemented system libraries, guest threads, logging.
#pragma once
#include "Abi.hpp"
#include "Heap.hpp"
#include "loader/MachO.hpp"
#include "loader/Plist.hpp"
#include <atomic>
#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <thread>
#include <typeindex>
#include <unordered_map>
#include <vector>

namespace Hydrogen::Runtime {

enum class LogLevel { Debug, Info, Warning, Error, GuestStdout, GuestStderr };

struct ProcessConfig {
    std::string bundlePath;            // …/Foo.app
    std::string homePath;              // sandbox home (Documents/, Library/, tmp/ live here); default: bundle's parent
    std::vector<std::string> extraArgs;
    std::function<void(LogLevel, const std::string&)> log;   // called from any thread
    bool stopOnMissingFunction = true; // false: log and return 0
    bool traceCalls = false;           // log every HLE call (very slow, for debugging)
    std::string systemVersion = "6.1"; // reported by UIDevice / sysctl
    std::string deviceModel = "iPhone5,2";
    uint32_t screenWidth = 320, screenHeight = 568;   // points
    float screenScale = 2.0f;
    // Called on the emulated main thread with each new frame (premultiplied
    // RGBA, screenWidth*scale x screenHeight*scale, portrait). Optional.
    std::function<void(const uint8_t* rgba, int width, int height)> present;
};

// Input from the host (screen points, portrait orientation of the emulated device).
enum class TouchPhase { Began = 0, Moved = 1, Ended = 3, Cancelled = 4 };

class Process;

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
    bool stopping() const { return engine_->stopRequested(); }

    // ---- results
    bool crashed() const { return crashed_; }
    const std::string& crashReport() const { return crashReport_; }
    std::vector<std::string> missingSymbols() const;
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
    std::string crashReport_;

    std::function<void(TouchPhase, float, float, uint32_t)> touchHandler_;
    std::function<void(float, float, float)> accelHandler_;

    // diagnostics
    mutable std::mutex missingMu_;
    std::set<std::string> missing_;
};

// Module registration (each lives in its own .cpp).
void registerLibcString(Process& p);
void registerLibcStdio(Process& p);
void registerLibcStdlib(Process& p);
void registerLibcMath(Process& p);
void registerLibcTime(Process& p);
void registerLibcPthread(Process& p);
void registerLibcSystem(Process& p);
void registerCxxRuntime(Process& p);
void registerObjCRuntime(Process& p);
void registerFoundation(Process& p);
void registerUIKit(Process& p);

} // namespace Hydrogen::Runtime
