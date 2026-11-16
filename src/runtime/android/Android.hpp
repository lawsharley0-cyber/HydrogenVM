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
#include <string>
#include <vector>

namespace Hydrogen::Runtime {
class Process;
}

namespace Hydrogen::Runtime::Android {

// Where the library is loaded in guest memory.
constexpr uint32_t kLibraryBase = 0x04000000u;

// The loaded library (Process::service).
struct Library {
    explicit Library(Process&) {}
    Loader::ElfShared so;
    std::string name;   // file name, e.g. libDeadSpace.so
};

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
