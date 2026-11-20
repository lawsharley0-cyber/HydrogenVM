// HydrogenVM — libEGL and ANativeWindow for Android games. See Android.hpp.
//
// Games that set up GL themselves (NativeActivity games, Unity, Unreal, Cocos2d-x on its own
// thread) get one window: the emulated screen. EGL contexts are HydrogenVM host GL contexts;
// a window surface is a framebuffer whose colour buffer is the drawable that is presented, and it
// stands for framebuffer 0 while current. eglSwapBuffers presents it as GLSurfaceView would.
#include "runtime/android/Android.hpp"
#include "runtime/android/BionicInternal.hpp"
#include "runtime/android/Jni.hpp"
#include "runtime/android/Surface.hpp"
#include "runtime/gl/HostGL.hpp"
#include "runtime/Support.hpp"
#include <map>
#include <tuple>
#include <mutex>
#include <unordered_map>

namespace Hydrogen::Runtime::Android {
namespace {

constexpr uint32_t EGL_SUCCESS = 0x3000, EGL_BAD_ATTRIBUTE = 0x3004, EGL_BAD_CONFIG = 0x3005, EGL_BAD_CONTEXT = 0x3006,
                   EGL_BAD_DISPLAY = 0x3008, EGL_BAD_MATCH = 0x3009, EGL_BAD_NATIVE_WINDOW = 0x300B, EGL_BAD_PARAMETER = 0x300C,
                   EGL_BAD_SURFACE = 0x300D;
constexpr uint32_t EGL_NONE = 0x3038, kDisplay = 1;
constexpr uint32_t EGL_WIDTH = 0x3057, EGL_HEIGHT = 0x3056, EGL_CONFIG_ID = 0x3028;

// ------------------------------------------------------------------ windows ---

struct Window {
    int bufferW = 0, bufferH = 0, format = 1;   // setBuffersGeometry; 0 = the screen's size
    int refs = 1;
};

struct Config {
    uint32_t id, r, g, b, a, depth, stencil, nativeFormat;
};
// RGBA8888 + D24S8, RGB888 + D24S8, RGB565 + D16, RGBA8888 without depth.
const Config kConfigs[] = {
    {1, 8, 8, 8, 8, 24, 8, 1},
    {2, 8, 8, 8, 0, 24, 8, 2},
    {3, 5, 6, 5, 0, 16, 0, 4},
    {4, 8, 8, 8, 8, 0, 0, 1},
};
const Config* configById(uint32_t id) {
    for (const auto& c : kConfigs) if (c.id == id) return &c;
    return nullptr;
}

struct EglSurface {
    bool window = false;
    uint32_t nativeWindow = 0, config = 0;
    int width = 0, height = 0;               // pbuffers
    GlSurface g;                             // the drawable, made for the context it was first current with
    bool hasDrawable = false;
};
struct EglContext {
    GlSurface g;                             // context fields only
    uint32_t config = 0;
    int version = 1;
};

struct Egl {
    explicit Egl(Process&) {}
    std::recursive_mutex mu;
    std::unordered_map<uint32_t, Window> windows;
    std::unordered_map<uint32_t, EglSurface> surfaces;
    std::unordered_map<uint32_t, EglContext> contexts;
};
Egl& E(Ctx& x) { return x.proc.service<Egl>(); }

struct Current { uint32_t ctx = 0, draw = 0, read = 0; uint32_t error = EGL_SUCCESS; };
thread_local Current tCur;
uint32_t fail(uint32_t err) { tCur.error = err; return 0; }
uint32_t ok(uint32_t v = 1) { tCur.error = EGL_SUCCESS; return v; }

// The size a window's buffers have: what the game asked for, else the screen in the game's
// orientation.
std::pair<int, int> windowSize(Ctx& x, const Window& w) {
    if (w.bufferW > 0 && w.bufferH > 0) return {w.bufferW, w.bufferH};
    const Surface s = surfaceFor(x.proc);
    return {s.width, s.height};
}

uint32_t w_fromSurface(Ctx& x, GuestPtr, GuestPtr jsurface) {
    auto& e = E(x);
    std::lock_guard<std::recursive_mutex> lk(e.mu);
    // One window per Java Surface: kept in the Surface object.
    Jni& j = jni(x.proc);
    if (JObject* o = j.get(jsurface)) {
        auto it = o->fields.find("hvm.window");
        if (it != o->fields.end()) {
            if (auto w = e.windows.find(uint32_t(it->second)); w != e.windows.end()) { ++w->second.refs; return uint32_t(it->second); }
        }
        const uint32_t h = x.proc.allocStatic(64);
        x.mem.write<uint32_t>(h, 0x5F776E64);   // '_wnd', ANativeWindow's magic
        e.windows[h] = Window{};
        o->fields["hvm.window"] = h;
        return h;
    }
    return 0;
}
// The window for the screen, for drivers that give a game an ANativeWindow* directly.
uint32_t screenWindow(Process& p) {
    auto& e = p.service<Egl>();
    std::lock_guard<std::recursive_mutex> lk(e.mu);
    for (const auto& [h, w] : e.windows) return h;
    const uint32_t h = p.allocStatic(64);
    p.mem().write<uint32_t>(h, 0x5F776E64);
    e.windows[h] = Window{};
    return h;
}
Window* windowOf(Egl& e, uint32_t h) { auto it = e.windows.find(h); return it == e.windows.end() ? nullptr : &it->second; }
void w_acquire(Ctx& x, GuestPtr h) { auto& e = E(x); std::lock_guard<std::recursive_mutex> lk(e.mu); if (Window* w = windowOf(e, h)) ++w->refs; }
void w_release(Ctx& x, GuestPtr h) { auto& e = E(x); std::lock_guard<std::recursive_mutex> lk(e.mu); if (Window* w = windowOf(e, h)) w->refs = std::max(0, w->refs - 1); }
int32_t w_width(Ctx& x, GuestPtr h) {
    auto& e = E(x); std::lock_guard<std::recursive_mutex> lk(e.mu);
    Window* w = windowOf(e, h);
    return w ? windowSize(x, *w).first : -22;
}
int32_t w_height(Ctx& x, GuestPtr h) {
    auto& e = E(x); std::lock_guard<std::recursive_mutex> lk(e.mu);
    Window* w = windowOf(e, h);
    return w ? windowSize(x, *w).second : -22;
}
int32_t w_format(Ctx& x, GuestPtr h) {
    auto& e = E(x); std::lock_guard<std::recursive_mutex> lk(e.mu);
    Window* w = windowOf(e, h);
    return w ? w->format : -22;
}
int32_t w_setGeometry(Ctx& x, GuestPtr h, int32_t width, int32_t height, int32_t format) {
    auto& e = E(x); std::lock_guard<std::recursive_mutex> lk(e.mu);
    Window* w = windowOf(e, h);
    if (!w) return -22;
    w->bufferW = width > 0 ? width : 0;
    w->bufferH = height > 0 ? height : 0;
    if (format > 0) w->format = format;
    return 0;
}
// Software drawing into the window (ANativeWindow_lock) isn't offered: games of the GL era
// don't use it, and the error makes the rest fall back to GL.
int32_t w_lock(Ctx& x, GuestPtr, GuestPtr, GuestPtr) {
    x.proc.reportDefaulted("ANativeWindow_lock (software window drawing) -> error");
    return -22;
}
int32_t w_unlockAndPost(Ctx&, GuestPtr) { return -22; }

// --------------------------------------------------------------------- EGL ---

uint32_t e_getDisplay(Ctx&, uint32_t) { return ok(kDisplay); }
uint32_t e_initialize(Ctx& x, uint32_t dpy, GuestPtr major, GuestPtr minor) {
    if (dpy != kDisplay) return fail(EGL_BAD_DISPLAY);
    if (major) x.mem.write<int32_t>(major, 1);
    if (minor) x.mem.write<int32_t>(minor, 4);
    return ok();
}
uint32_t e_terminate(Ctx&, uint32_t) { return ok(); }
uint32_t e_getError(Ctx&) { const uint32_t e = tCur.error; tCur.error = EGL_SUCCESS; return e; }
uint32_t e_queryString(Ctx& x, uint32_t, int32_t name) {
    switch (name) {
        case 0x3053: return x.proc.internCString("HydrogenVM");
        case 0x3054: return x.proc.internCString("1.4 HydrogenVM");
        case 0x3055: return x.proc.internCString("EGL_KHR_surfaceless_context EGL_ANDROID_image_native_buffer");
        case 0x308D: return x.proc.internCString("OpenGL_ES");
        default: fail(EGL_BAD_PARAMETER); return 0;
    }
}

uint32_t attrOf(const Config& c, int32_t attr, bool& known) {
    known = true;
    switch (attr) {
        case 0x3020: return c.r + c.g + c.b + c.a;   // BUFFER_SIZE
        case 0x3021: return c.a; case 0x3022: return c.b; case 0x3023: return c.g; case 0x3024: return c.r;
        case 0x3025: return c.depth; case 0x3026: return c.stencil;
        case 0x3027: return EGL_NONE;                // CONFIG_CAVEAT
        case 0x3028: return c.id;
        case 0x3029: return 0;                       // LEVEL
        case 0x302A: case 0x302C: return 4096;       // MAX_PBUFFER_HEIGHT / WIDTH
        case 0x302B: return 4096 * 4096;             // MAX_PBUFFER_PIXELS
        case 0x302D: return 1;                       // NATIVE_RENDERABLE
        case 0x302E: return c.nativeFormat;          // NATIVE_VISUAL_ID (AHardwareBuffer format)
        case 0x302F: return 0;                       // NATIVE_VISUAL_TYPE
        case 0x3031: case 0x3032: return 0;          // SAMPLES / SAMPLE_BUFFERS
        case 0x3033: return 0x4 | 0x1;               // SURFACE_TYPE: WINDOW | PBUFFER
        case 0x3034: return EGL_NONE;                // TRANSPARENT_TYPE
        case 0x3035: case 0x3036: case 0x3037: return 0;
        case 0x3039: case 0x303A: return 0;          // BIND_TO_TEXTURE_RGB(A)
        case 0x303B: return 0; case 0x303C: return 1;   // MIN / MAX_SWAP_INTERVAL
        case 0x303D: return 0; case 0x303E: return 0;   // LUMINANCE_SIZE, ALPHA_MASK_SIZE
        case 0x303F: return 0x308E;                  // COLOR_BUFFER_TYPE: RGB
        // RENDERABLE_TYPE / CONFORMANT: ES 1 and 2. Contexts are ES 1.1 or 2.0; no ES 3 entry points
        // exist, so ES 3 is not offered (engines that ask for it fall back to ES 2).
        case 0x3040: case 0x3042: return 0x1 | 0x4;
        case 0x3142: return 0;                       // RECORDABLE_ANDROID
        default: known = false; return 0;
    }
}
bool matches(const Config& c, uint32_t attr, uint32_t want) {
    bool known = false;
    const uint32_t have = attrOf(c, int32_t(attr), known);
    if (!known) return true;   // an attribute we don't model doesn't rule a config out
    switch (attr) {
        case 0x3020: case 0x3021: case 0x3022: case 0x3023: case 0x3024: case 0x3025: case 0x3026:
        case 0x3031: case 0x3032: case 0x303D: case 0x303E:
            return want == 0xFFFFFFFFu || have >= want;   // EGL_DONT_CARE or at least
        case 0x3033: case 0x3040: case 0x3042: return (have & want) == want;
        case 0x3028: return want == 0xFFFFFFFFu || have == want;
        case 0x3027: case 0x3034: case 0x303F: case 0x302E: return want == 0xFFFFFFFFu || have == want;
        default: return true;
    }
}
uint32_t e_getConfigs(Ctx& x, uint32_t, GuestPtr out, int32_t size, GuestPtr num) {
    int n = 0;
    for (const auto& c : kConfigs) {
        if (out && n < size) x.mem.write<uint32_t>(out + uint32_t(n) * 4, c.id);
        ++n;
    }
    if (num) x.mem.write<int32_t>(num, out ? std::min(n, int(size)) : n);
    return ok();
}
uint32_t e_chooseConfig(Ctx& x, uint32_t dpy, GuestPtr attribs, GuestPtr out, int32_t size, GuestPtr num) {
    if (dpy != kDisplay) return fail(EGL_BAD_DISPLAY);
    std::vector<std::pair<uint32_t, uint32_t>> want;
    if (attribs)
        for (uint32_t a = attribs;; a += 8) {
            const uint32_t k = x.mem.read<uint32_t>(a);
            if (k == EGL_NONE) break;
            want.emplace_back(k, x.mem.read<uint32_t>(a + 4));
            if (want.size() > 64) return fail(EGL_BAD_ATTRIBUTE);
        }
    // Spec order puts the configs with the most colour bits first; among them, the smallest
    // depth/stencil that satisfies the request. Ours are listed so.
    int n = 0;
    for (const auto& c : kConfigs) {
        bool ok = true;
        for (const auto& [k, v] : want) ok = ok && matches(c, k, v);
        if (!ok) continue;
        if (out && n < size) x.mem.write<uint32_t>(out + uint32_t(n) * 4, c.id);
        ++n;
    }
    if (num) x.mem.write<int32_t>(num, out ? std::min(n, int(size)) : n);
    return ok();
}
uint32_t e_getConfigAttrib(Ctx& x, uint32_t, uint32_t config, int32_t attr, GuestPtr value) {
    const Config* c = configById(config);
    if (!c) return fail(EGL_BAD_CONFIG);
    bool known = false;
    const uint32_t v = attrOf(*c, attr, known);
    if (!known) return fail(EGL_BAD_ATTRIBUTE);
    if (value) x.mem.write<uint32_t>(value, v);
    return ok();
}

uint32_t e_createWindowSurface(Ctx& x, uint32_t, uint32_t config, uint32_t window, GuestPtr) {
    auto& e = E(x);
    std::lock_guard<std::recursive_mutex> lk(e.mu);
    if (!configById(config)) return fail(EGL_BAD_CONFIG);
    if (!windowOf(e, window)) return fail(EGL_BAD_NATIVE_WINDOW);
    const uint32_t h = x.proc.allocStatic(16);
    EglSurface s;
    s.window = true;
    s.nativeWindow = window;
    s.config = config;
    e.surfaces[h] = s;
    return ok(h);
}
uint32_t e_createPbufferSurface(Ctx& x, uint32_t, uint32_t config, GuestPtr attribs) {
    auto& e = E(x);
    std::lock_guard<std::recursive_mutex> lk(e.mu);
    if (!configById(config)) return fail(EGL_BAD_CONFIG);
    EglSurface s;
    s.config = config;
    s.width = s.height = 1;
    if (attribs)
        for (uint32_t a = attribs; x.mem.read<uint32_t>(a) != EGL_NONE; a += 8) {
            const uint32_t k = x.mem.read<uint32_t>(a), v = x.mem.read<uint32_t>(a + 4);
            if (k == EGL_WIDTH) s.width = int(v);
            if (k == EGL_HEIGHT) s.height = int(v);
        }
    const uint32_t h = x.proc.allocStatic(16);
    e.surfaces[h] = s;
    return ok(h);
}
uint32_t e_createPixmapSurface(Ctx&, uint32_t, uint32_t, uint32_t, GuestPtr) { return fail(EGL_BAD_NATIVE_WINDOW); }
uint32_t e_destroySurface(Ctx& x, uint32_t, uint32_t surface) {
    auto& e = E(x);
    std::lock_guard<std::recursive_mutex> lk(e.mu);
    auto it = e.surfaces.find(surface);
    if (it == e.surfaces.end()) return fail(EGL_BAD_SURFACE);
    if (currentSurface() == &it->second.g) setCurrentSurface(nullptr);
    if (it->second.hasDrawable) HostGL::layerGone(x.proc, it->second.g.layer);
    e.surfaces.erase(it);
    return ok();
}
uint32_t e_createContext(Ctx& x, uint32_t, uint32_t config, uint32_t share, GuestPtr attribs) {
    auto& e = E(x);
    std::lock_guard<std::recursive_mutex> lk(e.mu);
    if (!configById(config)) return fail(EGL_BAD_CONFIG);
    int version = 1;
    if (attribs)
        for (uint32_t a = attribs; x.mem.read<uint32_t>(a) != EGL_NONE; a += 8)
            if (x.mem.read<uint32_t>(a) == 0x3098 /*CONTEXT_CLIENT_VERSION*/) version = int(x.mem.read<uint32_t>(a + 4));
    // ES 3 needs entry points HydrogenVM does not have (glGetStringi, ...). A device without
    // ES 3 refuses the version, and engines then retry with 2 (Unity 2017: Pixel Gun 3D).
    if (version >= 3) return fail(EGL_BAD_MATCH);
    uint32_t shareGroup = 0;
    if (share) {
        auto it = e.contexts.find(share);
        if (it == e.contexts.end()) return fail(EGL_BAD_CONTEXT);
        shareGroup = it->second.g.sharegroup;
    }
    EglContext c;
    c.config = config;
    c.version = version;
    createContext(x.proc, c.g, version == 2 ? 2 : 1, shareGroup);
    const uint32_t h = c.g.context;
    e.contexts[h] = c;
    return ok(h);
}
uint32_t e_destroyContext(Ctx& x, uint32_t, uint32_t ctx) {
    auto& e = E(x);
    std::lock_guard<std::recursive_mutex> lk(e.mu);
    auto it = e.contexts.find(ctx);
    if (it == e.contexts.end()) return fail(EGL_BAD_CONTEXT);
    if (tCur.ctx == ctx) { HostGL::contextMakeCurrent(x.proc, 0); tCur = {}; setCurrentSurface(nullptr); }
    HostGL::contextDestroy(x.proc, ctx);
    e.contexts.erase(it);
    return ok();
}
uint32_t e_makeCurrent(Ctx& x, uint32_t, uint32_t draw, uint32_t read, uint32_t ctx) {
    auto& e = E(x);
    std::lock_guard<std::recursive_mutex> lk(e.mu);
    if (!ctx) {
        HostGL::contextMakeCurrent(x.proc, 0);
        setCurrentSurface(nullptr);
        tCur.ctx = tCur.draw = tCur.read = 0;
        return ok();
    }
    auto c = e.contexts.find(ctx);
    if (c == e.contexts.end()) return fail(EGL_BAD_CONTEXT);
    EglSurface* s = nullptr;
    if (draw) {
        auto it = e.surfaces.find(draw);
        if (it == e.surfaces.end()) return fail(EGL_BAD_SURFACE);
        s = &it->second;
    }
    HostGL::contextMakeCurrent(x.proc, ctx);
    tCur.ctx = ctx; tCur.draw = draw; tCur.read = read;
    if (s && s->window) {
        const Window* win = windowOf(e, s->nativeWindow);
        const auto [w, h] = windowSize(x, win ? *win : Window{});
        // The drawable belongs to a context: made with the first one current on the surface, and
        // made again if another (unshared) context draws to it or the window's size changed.
        if (!s->hasDrawable || (s->g.context != ctx && s->g.sharegroup != c->second.g.sharegroup) ||
            s->g.width != w || s->g.height != h) {
            if (s->hasDrawable) HostGL::layerGone(x.proc, s->g.layer);
            s->g = c->second.g;
            attachDrawable(x.proc, x.cpu, s->g, w, h, configById(s->config)->depth > 0);
            s->hasDrawable = true;
        }
        s->g.context = ctx;
        setCurrentSurface(&s->g);
        // Framebuffer 0 bound, as after eglMakeCurrent on a window.
        x.proc.callGuest(x.cpu, glFn(x.proc, fbName(s->g.api, "glBindFramebuffer")), {0x8D40, s->g.fbo});
    } else {
        setCurrentSurface(nullptr);   // pbuffer or no surface: offscreen work only
    }
    return ok();
}
uint32_t e_swapBuffers(Ctx& x, uint32_t, uint32_t surface) {
    x.proc.perf().gameSwaps++;
    GlSurface* g = nullptr;
    {
        auto& e = E(x);
        std::lock_guard<std::recursive_mutex> lk(e.mu);
        auto it = e.surfaces.find(surface);
        if (it == e.surfaces.end()) return fail(EGL_BAD_SURFACE);
        if (!it->second.window || !it->second.hasDrawable) return ok();
        g = &it->second.g;
    }
    present(x.proc, x.cpu, *g, surfaceFor(x.proc));
    return ok();
}
uint32_t e_querySurface(Ctx& x, uint32_t, uint32_t surface, int32_t attr, GuestPtr value) {
    auto& e = E(x);
    std::lock_guard<std::recursive_mutex> lk(e.mu);
    auto it = e.surfaces.find(surface);
    if (it == e.surfaces.end()) return fail(EGL_BAD_SURFACE);
    const EglSurface& s = it->second;
    int w = s.width, h = s.height;
    if (s.window) {
        if (Window* win = windowOf(e, s.nativeWindow)) std::tie(w, h) = windowSize(x, *win);
    }
    uint32_t v = 0;
    switch (attr) {
        case int32_t(EGL_WIDTH): v = uint32_t(w); break;
        case int32_t(EGL_HEIGHT): v = uint32_t(h); break;
        case int32_t(EGL_CONFIG_ID): v = s.config; break;
        case 0x3086: v = 0x3084; break;            // RENDER_BUFFER: BACK_BUFFER
        case 0x3093: v = 0x3095; break;            // SWAP_BEHAVIOR: BUFFER_DESTROYED
        case 0x3090: case 0x3091: v = 2834; break; // HORIZONTAL / VERTICAL_RESOLUTION
        default: return fail(EGL_BAD_ATTRIBUTE);
    }
    if (value) x.mem.write<uint32_t>(value, v);
    return ok();
}
uint32_t e_queryContext(Ctx& x, uint32_t, uint32_t ctx, int32_t attr, GuestPtr value) {
    auto& e = E(x);
    std::lock_guard<std::recursive_mutex> lk(e.mu);
    auto it = e.contexts.find(ctx);
    if (it == e.contexts.end()) return fail(EGL_BAD_CONTEXT);
    uint32_t v = 0;
    if (attr == 0x3098) v = uint32_t(it->second.version);
    else if (attr == 0x3028) v = it->second.config;
    else if (attr == 0x3097) v = 0x30A0;   // CONTEXT_CLIENT_TYPE: OPENGL_ES_API
    else if (attr == 0x3086) v = 0x3084;
    else return fail(EGL_BAD_ATTRIBUTE);
    if (value) x.mem.write<uint32_t>(value, v);
    return ok();
}
uint32_t e_getCurrentContext(Ctx&) { return tCur.ctx; }
uint32_t e_getCurrentSurface(Ctx&, int32_t which) { return which == 0x305A /*READ*/ ? tCur.read : tCur.draw; }
uint32_t e_getCurrentDisplay(Ctx&) { return tCur.ctx ? kDisplay : 0; }
uint32_t e_true1(Ctx&, uint32_t) { return ok(); }
uint32_t e_true2(Ctx&, uint32_t, int32_t) { return ok(); }
uint32_t e_true4(Ctx&, uint32_t, uint32_t, int32_t, int32_t) { return ok(); }
uint32_t e_true0(Ctx&) { return ok(); }
uint32_t e_queryApi(Ctx&) { return 0x30A0; }
uint32_t e_getProcAddress(Ctx& x, GuestPtr nameP) {
    const std::string name = x.proc.readCString(nameP, 256);
    if (auto a = x.proc.lookupSymbol("android:" + name)) return *a;
    if (auto a = x.proc.lookupSymbol("_" + name)) return *a;
    return 0;
}

} // namespace

uint32_t screenNativeWindow(Process& p) { return screenWindow(p); }

void registerEgl(Process& p) {
#define R(F, NAME, WHAT) supportDef<&F>(p, std::string("android:") + NAME, SupportStatus::Real, WHAT)
#define A(F, NAME, WHAT) supportDef<&F>(p, std::string("android:") + NAME, SupportStatus::Approx, WHAT)
    R(w_fromSurface, "ANativeWindow_fromSurface", "the screen's window");
    R(w_acquire, "ANativeWindow_acquire", "reference count");
    R(w_release, "ANativeWindow_release", "reference count");
    R(w_width, "ANativeWindow_getWidth", "buffer size or the screen's");
    R(w_height, "ANativeWindow_getHeight", "buffer size or the screen's");
    R(w_format, "ANativeWindow_getFormat", "buffer format");
    R(w_setGeometry, "ANativeWindow_setBuffersGeometry", "the size GL draws at; the screen scales it");
    A(w_lock, "ANativeWindow_lock", "no software drawing: error");
    A(w_unlockAndPost, "ANativeWindow_unlockAndPost", "no software drawing: error");
    R(e_getDisplay, "eglGetDisplay", "the one display");
    R(e_initialize, "eglInitialize", "EGL 1.4");
    R(e_terminate, "eglTerminate", "accepted");
    R(e_getError, "eglGetError", "the thread's last error");
    R(e_queryString, "eglQueryString", "vendor, version, extensions, client APIs");
    R(e_getConfigs, "eglGetConfigs", "four configs (RGBA8888/RGB888/RGB565, depth, stencil)");
    R(e_chooseConfig, "eglChooseConfig", "by the spec's at-least / mask rules");
    R(e_getConfigAttrib, "eglGetConfigAttrib", "config attributes");
    R(e_createWindowSurface, "eglCreateWindowSurface", "the screen's drawable");
    R(e_createPbufferSurface, "eglCreatePbufferSurface", "offscreen: no drawable");
    A(e_createPixmapSurface, "eglCreatePixmapSurface", "no pixmaps: error");
    R(e_destroySurface, "eglDestroySurface", "releases the drawable");
    R(e_createContext, "eglCreateContext", "host GL context of the client version, shared as asked");
    R(e_destroyContext, "eglDestroyContext", "host context destroyed");
    R(e_makeCurrent, "eglMakeCurrent", "host context current; the window's framebuffer is framebuffer 0");
    R(e_swapBuffers, "eglSwapBuffers", "presents the drawable");
    R(e_querySurface, "eglQuerySurface", "size and config");
    R(e_queryContext, "eglQueryContext", "client version and config");
    R(e_getCurrentContext, "eglGetCurrentContext", "this thread's");
    R(e_getCurrentSurface, "eglGetCurrentSurface", "this thread's");
    R(e_getCurrentDisplay, "eglGetCurrentDisplay", "this thread's");
    A(e_true2, "eglSwapInterval", "presents are paced to the display anyway");
    A(e_true1, "eglBindAPI", "OpenGL ES only");
    R(e_queryApi, "eglQueryAPI", "OpenGL ES");
    A(e_true0, "eglReleaseThread", "accepted");
    A(e_true0, "eglWaitClient", "accepted");
    A(e_true0, "eglWaitGL", "accepted");
    A(e_true1, "eglWaitNative", "accepted");
    A(e_true4, "eglSurfaceAttrib", "accepted");
    R(e_getProcAddress, "eglGetProcAddress", "HydrogenVM's GL and EGL functions");
#undef R
#undef A
}

} // namespace Hydrogen::Runtime::Android
