// HydrogenVM — helpers shared by the bionic files of runtime/android (not a public API).
#pragma once
#include "runtime/Abi.hpp"
#include "runtime/Process.hpp"
#include <cerrno>
#include <initializer_list>

namespace Hydrogen::Runtime::Android {

// Darwin errno value -> Linux (bionic) errno value.
int linuxErrno(int darwin);
// The host's errno as a Linux value.
int hostErrnoLinux(int hostErrno);
// Sets the game's errno (a Linux value).
void setLinuxErrno(Ctx& x, int e);
template <typename R> R failHost(Ctx& x, R r = R(-1)) { setLinuxErrno(x, hostErrnoLinux(errno)); return r; }
// Calls the iOS host function bound to `name` (r0-r3, then the stack) and returns r1:r0.
uint64_t callIos(Ctx& x, const char* name, std::initializer_list<uint32_t> words);

// Registration of the other bionic files.
void registerBionicExtra(Process& p);
void registerLibAndroid(Process& p);
void registerOpenSLES(Process& p);
void registerLibdl(Process& p);
void registerEgl(Process& p);
void registerSyscalls(Process& p);
void registerSignals(Process& p);
void registerFmodBridge(Process& p);
void registerLooper(Process& p);
// Generated files for guest paths under /proc (ProcFs.cpp).
void installProcFs(Process& p);
// Placeholders from tools/android_gaps.py (Placeholders.cpp); registered after everything real.
void registerNativePlaceholders(Process& p);
// The AInputQueue a NativeActivity gets, and a touch delivered through it (Looper.cpp).
uint32_t inputQueue(Process& p);
void pushTouchEvent(Process& p, TouchPhase phase, float x, float y, uint32_t id, uint32_t pointersDown);
// Sends a Linux signal to the guest thread with Linux thread id `tid` (tkill). 0 or a Linux errno.
int sendSignalToTid(Process& p, CPU::Cpu& self, uint32_t tid, int sig);
// The screen's ANativeWindow, for drivers that hand one to the game (Egl.cpp).
uint32_t screenNativeWindow(Process& p);

} // namespace Hydrogen::Runtime::Android
