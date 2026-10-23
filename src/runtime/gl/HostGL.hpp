// HydrogenVM — OpenGL ES on the host GPU.
//
// With a HostGL::Platform in ProcessConfig::hostGL, the guest's GLES 1.1 / 2.0
// calls are forwarded to a real driver instead of the software rasterizer:
// OpenGLES.framework on iOS (the platform object lives in the app shell),
// Mesa EGL on Linux (HostGLEgl.cpp; used by tests and the desktop CLI).
//
// What stays in the emulator:
//   * EAGLContext / EAGLSharegroup / CAEAGLLayer (OpenGLES.cpp) — each guest
//     context gets one host context, created in the guest sharegroup's share
//     group and made current on the same host thread as the guest thread;
//   * guest-memory translation: client-side arrays, texture data, shader
//     source arrays, mapped buffers and sync objects;
//   * the drawable: the renderbuffer bound with renderbufferStorage:fromDrawable:
//     gets platform storage, and presentRenderbuffer: either presents it
//     directly (iOS, CAEAGLLayer) or reads it back for the software compositor.
//
// Nothing here compiles shaders or rasterizes: the driver does.
#pragma once
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace Hydrogen::Runtime {
class Process;
namespace ObjC { class Runtime; }
}

namespace Hydrogen::Runtime::HostGL {

class Platform {
public:
    virtual ~Platform() = default;
    virtual const char* name() const = 0;

    // Create a host context for guest API `api` (1 = ES 1.1, 2 = ES 2.0, 3 = ES 3.0)
    // sharing objects with `share` (another context from this platform, or null).
    // `desktopFixedFunction` is set when an ES 1.1 guest runs on a desktop GL
    // compatibility context (Linux Mesa builds without ES 1.1): the backend then
    // maps the few ES-only entry points (glOrthof, glFrustumf, OES suffixes).
    virtual void* createContext(int api, void* share, bool& desktopFixedFunction) = 0;
    virtual void destroyContext(void* ctx) = 0;
    // Make `ctx` current on the calling thread (null: release the current one).
    virtual bool makeCurrent(void* ctx) = 0;
    virtual void* proc(void* ctx, const char* name) = 0;

    // Give the currently bound GL_RENDERBUFFER storage for the drawable `layer`
    // (an opaque key for the guest CAEAGLLayer) of `width` x `height` pixels.
    // The default allocates plain RGBA8 storage.
    virtual bool drawableStorage(void* ctx, uint32_t layer, int width, int height, bool& handled) {
        handled = false;
        return false;
    }
    // Present the currently bound renderbuffer of `layer` directly (e.g. EAGL
    // presentRenderbuffer: on a host CAEAGLLayer). Return false to have the
    // backend read the pixels back for the software compositor instead.
    virtual bool presentDirect(void* ctx, uint32_t layer) { return false; }
    // The guest layer went away.
    virtual void releaseLayer(uint32_t layer) {}
};

// Linux / any EGL with Mesa: surfaceless display, ES 2/3 contexts, and a desktop
// GL compatibility context for ES 1.1 when the driver has no ES 1.1. Null when
// libEGL can't be loaded or initialized.
std::shared_ptr<Platform> createEglPlatform(std::string* why = nullptr);

// --- used by OpenGLES.cpp -------------------------------------------------

bool enabled(Process& p);
// Called from the EAGLContext methods when enabled().
void* contextCreate(Process& p, uint32_t guestContext, int api, uint32_t guestSharegroup);
void contextDestroy(Process& p, uint32_t guestContext);
void contextMakeCurrent(Process& p, uint32_t guestContext);   // 0: none
// True when this guest EAGLContext has a host context (false: it renders in software).
bool isHostContext(Process& p, uint32_t guestContext);
bool renderbufferStorageFromDrawable(Process& p, uint32_t guestContext, uint32_t layer, int width, int height);
// Presents the bound renderbuffer. When the platform can't present directly,
// fills `rgbaBottomLeft` (straight RGBA, GL row order) and returns true.
bool presentRenderbuffer(Process& p, uint32_t guestContext, uint32_t& layer, int& width, int& height,
                         std::vector<uint8_t>& rgbaBottomLeft, bool& direct);
void layerGone(Process& p, uint32_t layer);

// Registers the forwarded `_gl*` symbols (overriding the software ones).
void registerFunctions(Process& p);

} // namespace Hydrogen::Runtime::HostGL
