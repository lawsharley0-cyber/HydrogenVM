// HydrogenVM — HostGL platform on EGL (Linux, Mesa). Loaded with dlopen, so the
// build needs no EGL or GLES headers or libraries.
//
// Surfaceless display (EGL_MESA_platform_surfaceless): the guest renders into
// framebuffer objects, and presentRenderbuffer: reads the drawable back for the
// compositor. ES 2.0 guests get an ES 3 context (a superset that also has VAOs,
// MapBufferRange and instancing). ES 1.1 guests get an ES 1 context when the
// driver has one, otherwise a desktop GL compatibility context: Ubuntu's Mesa is
// built without ES 1.1, and desktop GL keeps the whole fixed-function pipeline.
#include "runtime/gl/HostGL.hpp"

#if !defined(__APPLE__)
#include <dlfcn.h>
#include <initializer_list>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

namespace Hydrogen::Runtime::HostGL {
namespace {

using EGLDisplay = void*;
using EGLConfig = void*;
using EGLContext = void*;
using EGLSurface = void*;
using EGLint = int32_t;
using EGLBoolean = unsigned;
using EGLenum = unsigned;

constexpr EGLenum kPlatformSurfacelessMesa = 0x31DD;
constexpr EGLint kNone = 0x3038, kRenderableType = 0x3040, kSurfaceType = 0x3033;
constexpr EGLint kOpenGLES2Bit = 0x0004, kOpenGLES3Bit = 0x0040, kOpenGLESBit = 0x0001, kOpenGLBit = 0x0008;
constexpr EGLint kContextMajorVersion = 0x3098, kContextMinorVersion = 0x30FB, kContextProfileMask = 0x30FD;
constexpr EGLint kContextCompatibilityProfileBit = 0x0002;
constexpr EGLenum kOpenGLESApi = 0x30A0, kOpenGLApi = 0x30A2;

struct Egl {
    EGLDisplay (*getPlatformDisplay)(EGLenum, void*, const intptr_t*) = nullptr;
    EGLDisplay (*getDisplay)(void*) = nullptr;
    EGLSurface (*createPbufferSurface)(EGLDisplay, EGLConfig, const EGLint*) = nullptr;
    EGLBoolean (*destroySurface)(EGLDisplay, EGLSurface) = nullptr;
    EGLBoolean (*initialize)(EGLDisplay, EGLint*, EGLint*) = nullptr;
    EGLBoolean (*chooseConfig)(EGLDisplay, const EGLint*, EGLConfig*, EGLint, EGLint*) = nullptr;
    EGLBoolean (*bindAPI)(EGLenum) = nullptr;
    EGLContext (*createContext)(EGLDisplay, EGLConfig, EGLContext, const EGLint*) = nullptr;
    EGLBoolean (*destroyContext)(EGLDisplay, EGLContext) = nullptr;
    EGLBoolean (*makeCurrent)(EGLDisplay, EGLSurface, EGLSurface, EGLContext) = nullptr;
    void* (*getProcAddress)(const char*) = nullptr;
    EGLint (*getError)() = nullptr;
};

struct EglContext {
    EGLContext ctx = nullptr;
    bool desktop = false;
    EGLSurface surface = nullptr;   // Android: a 1x1 pbuffer (not every driver has surfaceless contexts)
};

class EglPlatform final : public Platform {
public:
    bool init(std::string* why) {
#if defined(__ANDROID__)
        lib_ = dlopen("libEGL.so", RTLD_NOW | RTLD_LOCAL);
#else
        lib_ = dlopen("libEGL.so.1", RTLD_NOW | RTLD_LOCAL);
#endif
        if (!lib_) { if (why) *why = "libEGL not found"; return false; }
#define LOAD(field, sym) egl_.field = reinterpret_cast<decltype(egl_.field)>(dlsym(lib_, sym)); if (!egl_.field) { if (why) *why = std::string("libEGL lacks ") + sym; return false; }
#if defined(__ANDROID__)
        LOAD(getDisplay, "eglGetDisplay")
        LOAD(createPbufferSurface, "eglCreatePbufferSurface")
        LOAD(destroySurface, "eglDestroySurface")
#else
        LOAD(getPlatformDisplay, "eglGetPlatformDisplay")
#endif
        LOAD(initialize, "eglInitialize")
        LOAD(chooseConfig, "eglChooseConfig")
        LOAD(bindAPI, "eglBindAPI")
        LOAD(createContext, "eglCreateContext")
        LOAD(destroyContext, "eglDestroyContext")
        LOAD(makeCurrent, "eglMakeCurrent")
        LOAD(getProcAddress, "eglGetProcAddress")
        LOAD(getError, "eglGetError")
#undef LOAD
#if defined(__ANDROID__)
        dpy_ = egl_.getDisplay(nullptr);   // EGL_DEFAULT_DISPLAY
#else
        dpy_ = egl_.getPlatformDisplay(kPlatformSurfacelessMesa, nullptr, nullptr);
#endif
        EGLint major = 0, minor = 0;
        if (!dpy_ || !egl_.initialize(dpy_, &major, &minor)) {
            if (why) *why = "eglInitialize failed on the surfaceless platform";
            return false;
        }
        return true;
    }

#if defined(__ANDROID__)
    const char* name() const override { return "EGL (Android)"; }
#else
    const char* name() const override { return "EGL (Mesa)"; }
#endif

    void* createContext(int api, void* share, bool& desktop) override {
        std::lock_guard<std::mutex> lk(mu_);
        desktop = false;
        EGLContext shareCtx = share ? static_cast<EglContext*>(share)->ctx : nullptr;
        const bool shareDesktop = share && static_cast<EglContext*>(share)->desktop;
        EGLContext c = nullptr;
        if (api == 1) {
            if (!shareDesktop) c = create(kOpenGLESApi, kOpenGLESBit, {kContextMajorVersion, 1, kNone}, shareCtx);
            if (!c) {
                c = create(kOpenGLApi, kOpenGLBit,
                           {kContextProfileMask, kContextCompatibilityProfileBit, kContextMajorVersion, 2, kContextMinorVersion, 1, kNone},
                           shareDesktop ? shareCtx : nullptr);
                desktop = c != nullptr;
            }
        } else if (!shareDesktop) {
            c = create(kOpenGLESApi, kOpenGLES3Bit, {kContextMajorVersion, 3, kNone}, shareCtx);
            if (!c) c = create(kOpenGLESApi, kOpenGLES2Bit, {kContextMajorVersion, 2, kNone}, shareCtx);
        }
        if (!c) return nullptr;
        auto* out = new EglContext{c, desktop};
#if defined(__ANDROID__)
        const EGLint pbAttrs[] = {0x3057 /*EGL_WIDTH*/, 1, 0x3056 /*EGL_HEIGHT*/, 1, kNone};
        out->surface = egl_.createPbufferSurface(dpy_, lastConfig_, pbAttrs);
#endif
        return out;
    }

    void destroyContext(void* ctx) override {
        auto* c = static_cast<EglContext*>(ctx);
        if (!c) return;
        egl_.destroyContext(dpy_, c->ctx);   // deferred by EGL while current elsewhere
#if defined(__ANDROID__)
        if (c->surface) egl_.destroySurface(dpy_, c->surface);
#endif
        delete c;
    }

    bool makeCurrent(void* ctx) override {
        auto* c = static_cast<EglContext*>(ctx);
        if (!c) return egl_.makeCurrent(dpy_, nullptr, nullptr, nullptr) != 0;
        egl_.bindAPI(c->desktop ? kOpenGLApi : kOpenGLESApi);
        return egl_.makeCurrent(dpy_, c->surface, c->surface, c->ctx) != 0;
    }

    void* proc(void*, const char* name) override {
        // eglGetProcAddress hands out a stub for any name, so a missing Apple
        // extension would look present and do nothing. Mesa has none of the
        // APPLE entry points; HostGL then falls back to the ES 3 equivalents.
        const size_t n = std::strlen(name);
        if (n > 5 && std::strcmp(name + n - 5, "APPLE") == 0) return nullptr;
        return egl_.getProcAddress(name);
    }

private:
    EGLContext create(EGLenum api, EGLint renderable, std::initializer_list<EGLint> attrs, EGLContext share) {
        egl_.bindAPI(api);
#if defined(__ANDROID__)
        const EGLint cfgAttrs[] = {kRenderableType, renderable, kSurfaceType, 0x0001 /*EGL_PBUFFER_BIT*/,
                                   0x3024, 8, 0x3023, 8, 0x3022, 8, 0x3021, 8, kNone};   // RGBA8
#else
        const EGLint cfgAttrs[] = {kRenderableType, renderable, kSurfaceType, 0, kNone};
#endif
        EGLConfig cfg = nullptr;
        EGLint n = 0;
        if (!egl_.chooseConfig(dpy_, cfgAttrs, &cfg, 1, &n) || n < 1) cfg = nullptr;
        std::vector<EGLint> a(attrs);
        EGLContext c = egl_.createContext(dpy_, cfg, share, a.data());
        if (c) lastConfig_ = cfg;
        return c;
    }

    void* lib_ = nullptr;
    Egl egl_;
    EGLDisplay dpy_ = nullptr;
    EGLConfig lastConfig_ = nullptr;   // the config of the context just created (createContext holds mu_)
    std::mutex mu_;
};

} // namespace

std::shared_ptr<Platform> createEglPlatform(std::string* why) {
    auto p = std::make_shared<EglPlatform>();
    if (!p->init(why)) return nullptr;
    return p;
}

} // namespace Hydrogen::Runtime::HostGL

#else

namespace Hydrogen::Runtime::HostGL {
std::shared_ptr<Platform> createEglPlatform(std::string* why) {
    if (why) *why = "EGL isn't used on Apple platforms (the app shell provides an EAGL platform)";
    return nullptr;
}
} // namespace Hydrogen::Runtime::HostGL

#endif
