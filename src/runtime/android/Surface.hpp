// HydrogenVM — Android window surfaces (internal to runtime/android): the emulated screen as an
// Android window, and the GL drawables on it. Used by the activity drivers (App.cpp) and EGL.
#pragma once
#include "runtime/Process.hpp"
#include <string>

namespace Hydrogen::Runtime::Android {

// The window: the emulated screen in the game's orientation, in pixels.
struct Surface {
    int width = 0, height = 0;
    float scale = 2.0f;
    bool portrait = false;
};
Surface surfaceFor(Process& p);

// A host GL context, and when it draws to the window, a framebuffer whose colour buffer is the
// drawable HydrogenVM presents. Framebuffer 0 of the game is this framebuffer.
struct GlSurface {
    int api = 1;
    uint32_t context = 0, sharegroup = 0, layer = 0;
    uint32_t fbo = 0, color = 0, depth = 0;
    int width = 0, height = 0;
    bool placed = false;
};

uint32_t glFn(Process& p, const char* name);
uint32_t glFn(Process& p, const std::string& name);
// Framebuffer functions: the OES extension names in ES 1.1, the core names in ES 2.
std::string fbName(int api, const char* core);
// The ES version the game links against: 2 for libGLESv2/v3, else 1.
int linkedGlesApi(Process& p);

// A new host context of `api` sharing objects with `shareWith` (another GlSurface's context, or 0).
void createContext(Process& p, GlSurface& g, int api, uint32_t shareSharegroup = 0);
// Gives the context (current on this thread) a window framebuffer of `w` x `h`.
void attachDrawable(Process& p, CPU::Cpu& c, GlSurface& g, int w, int h, bool depthStencil = true);
// The window surface current on this thread (its framebuffer stands for framebuffer 0), or null.
void setCurrentSurface(GlSurface* g);
GlSurface* currentSurface();
// Presents the drawable (opaque), as an Android window shows a frame.
void present(Process& p, CPU::Cpu& c, GlSurface& g, const Surface& s);

} // namespace Hydrogen::Runtime::Android
