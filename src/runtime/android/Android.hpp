// HydrogenVM — Android games (armeabi / armeabi-v7a native code).
//
// An Android game here is its native library (lib*.so) and its APK's assets, nothing more:
// the Java side of the app is not run. HydrogenVM plays the Activity, the GLSurfaceView's
// renderer thread and the few Java helper classes the native code calls through JNI.
//
//   Bionic.cpp  — Android's C library where it differs from iOS's (struct layouts, flag and
//                 errno values, the old pthread and stdio ABIs); every other import uses the
//                 iOS implementation of the same function.
//   Jni.cpp     — a JavaVM / JNIEnv whose objects and methods are host code.
//   App.cpp     — loading the library, and the activity lifecycle and frame loop.
#pragma once
#include "loader/ElfLoader.hpp"
#include "runtime/Abi.hpp"
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace Hydrogen::Runtime {
class Process;
}

namespace Hydrogen::Runtime::Android {

// Where the first library is loaded in guest memory; the others follow it.
constexpr uint32_t kLibraryBase = 0x04000000u;

// One loaded native library of the game.
struct LoadedLib {
    std::string name;            // file name, e.g. libunity.so
    std::string path;            // host path
    Loader::ElfShared so;
    bool bound = false, initialized = false;
};

// The game's native libraries (Process::service): every lib*.so of the APK's ABI folder, loaded
// in dependency order, as System.loadLibrary and the dynamic linker would have them. Linker.cpp.
struct Library {
    explicit Library(Process&) {}
    std::vector<std::unique_ptr<LoadedLib>> libs;   // load order: dependencies first
    LoadedLib* main = nullptr;                      // the one the activity runs (most Java natives)
    std::string name;                               // main's file name
    std::string libDir;                             // host folder of the libraries
    uint32_t nextBase = kLibraryBase;
    std::recursive_mutex mu;

    // A symbol any of the game's libraries defines, in load order.
    std::optional<uint32_t> find(const std::string& symbol) const;
    // The library whose mapping holds `addr`.
    const LoadedLib* containing(uint32_t addr) const;
    // Whether any library links against `soname` (DT_NEEDED), e.g. "libGLESv2.so".
    bool needs(const std::string& soname) const;
};

// Loads the main library (`mainPath`) and every other library of its folder, binds their
// imports and returns the names of the imports nothing provides.
std::vector<std::string> loadLibraries(Process& p, const std::string& mainPath);
// Runs every library's initializers that hasn't run them yet, dependencies first.
void runInitializers(Process& p, CPU::Cpu& c);

// A host path the game is about to open or stat: links the APK's assets into an SD-card data
// folder (Android/data/<package>/files) the first time the game looks in one (App.cpp).
void noteExternalPath(Process& p, const std::string& hostPath);
// The GLSurfaceView's framebuffer 0 (App.cpp).
void registerSurfaceFunctions(Process& p);
// Registers the bionic functions (under "android:<name>") on a process that runs an Android game.
void registerBionic(Process& p);
// The guest address an import of the library binds to: an "android:" definition, else the iOS
// function of the same name, else a missing-function stub (0 for weak imports nothing provides).
uint32_t resolveImport(Process& p, const std::string& name, bool weak);

// The JNI layer: a JavaVM and a JNIEnv per guest thread.
uint32_t javaVM(Process& p);
uint32_t jniEnv(Process& p);   // for the calling guest thread

// Runs the game: JNI_OnLoad, the activity's NativeOnCreate, the surface callbacks, then
// NativeOnDrawFrame every frame until the process stops. Called on the main guest thread.
int runActivity(Process& p, CPU::Cpu& c);

} // namespace Hydrogen::Runtime::Android
