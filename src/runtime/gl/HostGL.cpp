// HydrogenVM — OpenGL ES forwarded to the host GPU. See HostGL.hpp.
#include "runtime/gl/HostGL.hpp"
#include "runtime/gl/HostGLInternal.hpp"
#include "runtime/Process.hpp"
#include "runtime/graphics/Pvrtc.hpp"
#include <algorithm>
#include <cmath>
#include <chrono>
#include <thread>
#include <cstring>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <set>
#include <type_traits>
#include <unordered_map>

namespace Hydrogen::Runtime::HostGL {
namespace {

// GL enums used here (the guest's GLES headers use the same values).
constexpr uint32_t kArrayBufferBinding = 0x8894, kElementArrayBufferBinding = 0x8895;
constexpr uint32_t kRenderbuffer = 0x8D41, kRenderbufferBinding = 0x8CA7;
constexpr uint32_t kFramebuffer = 0x8D40, kFramebufferBinding = 0x8CA6;
constexpr uint32_t kReadFramebuffer = 0x8CA8, kDrawFramebuffer = 0x8CA9;
constexpr uint32_t kReadFramebufferBinding = 0x8CAA, kDrawFramebufferBinding = 0x8CA6;
constexpr uint32_t kColorAttachment0 = 0x8CE0, kRgba8 = 0x8058, kRgba = 0x1908, kBgra = 0x80E1;
constexpr uint32_t kUnsignedByte = 0x1401, kPackAlignment = 0x0D05;
constexpr uint32_t kRenderbufferWidth = 0x8D42, kRenderbufferHeight = 0x8D43;
constexpr uint32_t kFbAttachmentObjectType = 0x8CD0, kFbAttachmentObjectName = 0x8CD1;
constexpr uint32_t kColorBufferBit = 0x4000, kNearest = 0x2600;
constexpr uint32_t kNumCompressedTextureFormats = 0x86A2, kCompressedTextureFormats = 0x86A3;
constexpr uint32_t kMaxTextureSize = 0x0D33;
constexpr uint32_t kVertexArrayPointer = 0x808E, kNormalArrayPointer = 0x808F, kColorArrayPointer = 0x8090,
                   kTexCoordArrayPointer = 0x8092, kPointSizeArrayPointer = 0x898C;
constexpr uint32_t kBufferMapPointer = 0x88BD;
constexpr uint32_t kMapWriteBit = 0x0002, kMapReadBit = 0x0001, kMapFlushExplicitBit = 0x0010;
constexpr uint32_t kBufferSize = 0x8764;
// PVRTC (GL_IMG_texture_compression_pvrtc).
constexpr uint32_t kPvrtcRgb4 = 0x8C00, kPvrtcRgb2 = 0x8C01, kPvrtcRgba4 = 0x8C02, kPvrtcRgba2 = 0x8C03;

} // namespace

namespace {

struct Drawable {
    uint32_t layer = 0;
    int width = 0, height = 0;
};
struct Placement { float unit[6] = {}; };

struct State {
    explicit State(Process&) {}
    std::mutex mu;
    std::shared_ptr<Platform> platform;
    std::unordered_map<uint32_t, std::unique_ptr<HostContext>> contexts;          // guest EAGLContext -> host
    std::unordered_map<uint64_t, std::unique_ptr<HostContext>> workers;           // (thread, sharegroup) -> implicit
    std::unordered_map<uint32_t, HostContext*> shareRoot;                          // guest sharegroup -> first host ctx
    std::unordered_map<uint64_t, Drawable> drawables;                              // (sharegroup << 32 | rb) -> drawable
    std::unordered_map<uint32_t, void*> syncs;                                     // guest handle -> GLsync
    uint32_t nextSync = 1;
    std::set<std::string> missingLogged;
    uint32_t lastGuestContext = 0;
    std::unordered_map<std::string, uint32_t> strings;
    std::unordered_map<uint32_t, Placement> placed;                                // guest layer -> last geometry sent
    std::atomic<int> presents{0};                                                  // direct presents so far
    std::atomic<bool> toldReadback{false};
    std::chrono::steady_clock::time_point lastPresent{};                           // for 60 Hz pacing
};

State& S(Process& p) { return p.service<State>(); }

struct Tls {
    Process* proc = nullptr;
    HostContext* ctx = nullptr;
    bool software = false;   // the guest made a software-rendered context current on this thread
};
thread_local Tls tls;

uint64_t key2(uint32_t a, uint32_t b) { return (uint64_t(a) << 32) | b; }

const char* stripSuffix(const char* name, std::string& buf) {
    static const char* kSuffixes[] = {"OES", "EXT", "APPLE", "ANGLE", "NV"};
    const size_t n = std::strlen(name);
    for (const char* s : kSuffixes) {
        const size_t k = std::strlen(s);
        if (n > k && std::strcmp(name + n - k, s) == 0) {
            buf.assign(name, n - k);
            return buf.c_str();
        }
    }
    return nullptr;
}

void* resolve(Platform& pf, void* handle, const char* name, bool desktop) {
    std::string buf;
    const char* stripped = stripSuffix(name, buf);
    // GLVND hands out dispatch stubs for any name, so the order matters: a
    // desktop context only implements the core spellings.
    if (desktop && stripped) {
        if (void* f = pf.proc(handle, stripped)) return f;
        return pf.proc(handle, name);
    }
    if (void* f = pf.proc(handle, name)) return f;
    return stripped ? pf.proc(handle, stripped) : nullptr;
}

void loadFns(Platform& pf, HostContext& hc) {
#define HOSTGL_FN(name, ret, params) hc.fn.name = reinterpret_cast<ret(*) params>(resolve(pf, hc.handle, #name, hc.desktop));
#define HOSTGL_TABLE
#include "runtime/gl/HostGLTable.inc"
#undef HOSTGL_TABLE
#undef HOSTGL_FN
    // Apple extension -> ES 3.0 / desktop core spellings when the driver lacks them.
    if (!hc.fn.glDiscardFramebufferEXT) hc.fn.glDiscardFramebufferEXT = hc.fn.glInvalidateFramebuffer;
    auto alias = [&](auto& dst, const char* name) {
        if (!dst) dst = reinterpret_cast<std::remove_reference_t<decltype(dst)>>(pf.proc(hc.handle, name));
    };
    alias(hc.fn.glRenderbufferStorageMultisampleAPPLE, "glRenderbufferStorageMultisample");
    alias(hc.fn.glTexStorage2DEXT, "glTexStorage2D");
    alias(hc.fn.glDrawArraysInstancedEXT, "glDrawArraysInstanced");
    alias(hc.fn.glDrawElementsInstancedEXT, "glDrawElementsInstanced");
    alias(hc.fn.glVertexAttribDivisorEXT, "glVertexAttribDivisor");
    alias(hc.fn.glMapBufferRangeEXT, "glMapBufferRange");
    alias(hc.fn.glFlushMappedBufferRangeEXT, "glFlushMappedBufferRange");
    if (hc.desktop) {
        hc.fn.glOrtho = reinterpret_cast<decltype(hc.fn.glOrtho)>(pf.proc(hc.handle, "glOrtho"));
        hc.fn.glFrustum = reinterpret_cast<decltype(hc.fn.glFrustum)>(pf.proc(hc.handle, "glFrustum"));
        hc.fn.glClipPlane = reinterpret_cast<decltype(hc.fn.glClipPlane)>(pf.proc(hc.handle, "glClipPlane"));
        hc.fn.glGetClipPlane = reinterpret_cast<decltype(hc.fn.glGetClipPlane)>(pf.proc(hc.handle, "glGetClipPlane"));
        // ES-only entry points have no implementation on a desktop context.
        hc.fn.glPointSizePointerOES = nullptr;
        hc.fn.glDrawTexfOES = nullptr; hc.fn.glDrawTexiOES = nullptr; hc.fn.glDrawTexsOES = nullptr; hc.fn.glDrawTexxOES = nullptr;
        hc.fn.glOrthof = nullptr; hc.fn.glFrustumf = nullptr; hc.fn.glClipPlanef = nullptr; hc.fn.glGetClipPlanef = nullptr;
    }
    alias(hc.fn.glFenceSync, "glFenceSync");
    alias(hc.fn.glIsSync, "glIsSync");
    alias(hc.fn.glDeleteSync, "glDeleteSync");
    alias(hc.fn.glClientWaitSync, "glClientWaitSync");
    alias(hc.fn.glWaitSync, "glWaitSync");
    alias(hc.fn.glGetSynciv, "glGetSynciv");
    if (hc.fn.glFenceSyncAPPLE) {
        hc.fn.glFenceSync = hc.fn.glFenceSyncAPPLE;
        hc.fn.glIsSync = hc.fn.glIsSyncAPPLE;
        hc.fn.glDeleteSync = hc.fn.glDeleteSyncAPPLE;
        hc.fn.glClientWaitSync = hc.fn.glClientWaitSyncAPPLE;
        hc.fn.glWaitSync = hc.fn.glWaitSyncAPPLE;
        hc.fn.glGetSynciv = reinterpret_cast<decltype(hc.fn.glGetSynciv)>(hc.fn.glGetSyncivAPPLE);
    }
}

std::unique_ptr<HostContext> makeContext(Process& p, int api, uint32_t guestContext, uint32_t sharegroup, bool worker);

// iOS lets one EAGLContext be current on several threads (the app serializes
// its use; loader threads do this). EGL refuses a context that is current on
// another thread, and the calls on this thread would then go nowhere. In that
// case this thread gets its own context in the same share group (textures,
// buffers and programs are shared; framebuffers and vertex arrays are not).
HostContext* threadClone(Process& p, HostContext* hc) {
    State& st = S(p);
    const uint64_t k = key2(p.currentThread().id, hc->guestSharegroup);
    {
        std::lock_guard<std::mutex> lk(st.mu);
        auto it = st.workers.find(k);
        if (it != st.workers.end()) return it->second.get();
    }
    auto made = makeContext(p, hc->api, hc->guestContext, hc->guestSharegroup, true);
    if (!made) return nullptr;
    p.log(LogLevel::Info, "GPU: thread " + std::to_string(p.currentThread().id) +
                              " made a context current that is in use on another thread; gave it a shared context");
    HostContext* raw = made.get();
    std::lock_guard<std::mutex> lk(st.mu);
    st.workers[k] = std::move(made);
    return raw;
}

void bind(Process& p, HostContext* hc) {
    State& st = S(p);
    if (hc && !st.platform->makeCurrent(hc->handle) && !hc->implicitWorker) {
        if (HostContext* clone = threadClone(p, hc); clone && st.platform->makeCurrent(clone->handle)) hc = clone;
    } else if (!hc && st.platform) {
        st.platform->makeCurrent(nullptr);
    }
    tls.proc = &p;
    tls.ctx = hc;
    tls.software = false;
}

std::unique_ptr<HostContext> makeContext(Process& p, int api, uint32_t guestContext, uint32_t sharegroup, bool worker) {
    State& st = S(p);
    HostContext* root = nullptr;
    {
        std::lock_guard<std::mutex> lk(st.mu);
        auto it = st.shareRoot.find(sharegroup);
        if (it != st.shareRoot.end()) root = it->second;
    }
    auto hc = std::make_unique<HostContext>();
    hc->api = api;
    hc->guestContext = guestContext;
    hc->guestSharegroup = sharegroup;
    hc->implicitWorker = worker;
    bool desktop = false;
    // ES 1.1 guests: a native ES 1.1 context when the driver has one; otherwise
    // (or with HVM_GL1_EMULATE=1) an ES 2/3 context with the fixed-function
    // pipeline emulated in shaders (Gles1Emu.cpp).
    const char* forceEmu = std::getenv("HVM_GL1_EMULATE");
    bool emulate = api == 1 && forceEmu && *forceEmu && *forceEmu != '0';
    if (!emulate) {
        hc->handle = st.platform->createContext(api, root ? root->handle : nullptr, desktop);
        if (!hc->handle && root) hc->handle = st.platform->createContext(api, nullptr, desktop);   // e.g. ES1 and ES2 can't share
    }
    if (!hc->handle && api == 1) {
        emulate = true;
        desktop = false;
        hc->handle = st.platform->createContext(2, root ? root->handle : nullptr, desktop);
        if (!hc->handle && root) hc->handle = st.platform->createContext(2, nullptr, desktop);
        if (hc->handle) p.log(LogLevel::Info, "GPU: OpenGL ES 1.1 runs on an ES 2 context (fixed function in shaders)");
    }
    if (!hc->handle) {
        p.log(LogLevel::Error, std::string("GPU: couldn't create a host OpenGL ES ") + std::to_string(api) + " context");
        return nullptr;
    }
    hc->desktop = desktop;
    if (emulate) hc->api = 2;          // resolve the table as an ES 2 context
    loadFns(*st.platform, *hc);
    if (emulate) {
        hc->api = 1;
        installGles1Emulation(*hc);
    }
    {
        std::lock_guard<std::mutex> lk(st.mu);
        if (!st.shareRoot.count(sharegroup)) st.shareRoot[sharegroup] = hc.get();
    }
    return hc;
}

} // namespace

HostContext* currentHostContext() { return tls.ctx; }

// ------------------------------------------------------------ current context ---

HostContext* hgCurrent(Ctx& x) {
    if (tls.proc == &x.proc && tls.ctx) return tls.ctx;
    if (tls.proc == &x.proc && tls.software) return nullptr;
    // No context on this thread. iOS apps (Cocos2d texture loaders, for one) issue
    // GL from worker threads after setting the shared context there; when they
    // don't, the software path borrowed the last active context. A host context
    // can't be current on two threads, so give this thread its own context in the
    // same share group instead.
    State& st = S(x.proc);
    uint32_t guest = 0;
    int api = 2;
    uint32_t sg = 0;
    {
        std::lock_guard<std::mutex> lk(st.mu);
        guest = st.lastGuestContext;
        auto it = st.contexts.find(guest);
        if (it == st.contexts.end() || !it->second) return nullptr;
        api = it->second->api;
        sg = it->second->guestSharegroup;
    }
    const uint64_t k = key2(x.proc.currentThread().id, sg);
    HostContext* hc = nullptr;
    {
        std::lock_guard<std::mutex> lk(st.mu);
        auto it = st.workers.find(k);
        if (it != st.workers.end()) hc = it->second.get();
    }
    if (!hc) {
        auto made = makeContext(x.proc, api, 0, sg, true);
        if (!made) return nullptr;
        x.proc.log(LogLevel::Info, "GPU: thread " + std::to_string(x.proc.currentThread().id) +
                                       " issued GL without a current context; gave it a shared worker context");
        hc = made.get();
        std::lock_guard<std::mutex> lk(st.mu);
        st.workers[k] = std::move(made);
    }
    bind(x.proc, hc);
    return hc;
}

void hgMissing(Ctx& x, HostContext* hc, const char* name) {
    State& st = S(x.proc);
    std::lock_guard<std::mutex> lk(st.mu);
    const std::string key = std::string(hc ? name : "(no current context)");
    if (!st.missingLogged.insert(key).second) return;
    if (!hc) x.proc.log(LogLevel::Warning, std::string("GPU: ") + name + " called with no current EAGLContext; ignored");
    else x.proc.log(LogLevel::Warning, std::string("GPU: the host driver has no ") + name + "; the call is ignored");
}

#define HG_PTR(x, g) ((g) ? static_cast<void*>((x).mem.host(g)) : nullptr)

// HVM_GL_DEBUG=1: check glGetError after every forwarded call and log the first
// errors with the call that raised them. HVM_GL_DEBUG=2 also logs every call with
// its arguments. HVM_GL_DUMP_SHADERS=<dir> writes each shader's source there.
static const bool hgDebug = [] { const char* e = std::getenv("HVM_GL_DEBUG"); return e && *e && *e != '0'; }();
static const int hgDebugLevel = [] { const char* e = std::getenv("HVM_GL_DEBUG"); return e ? std::atoi(e) : 0; }();
inline void hgArg(std::string& s, float v) { char b[32]; std::snprintf(b, sizeof b, "%g", double(v)); s += b; }
inline void hgArg(std::string& s, uint64_t v) { s += std::to_string(v); }
template <typename T> inline void hgArg(std::string& s, T v) {
    char b[32];
    if constexpr (std::is_signed_v<T>) std::snprintf(b, sizeof b, "%d", int(v));
    else std::snprintf(b, sizeof b, v > 0xFFFF ? "0x%x" : "%u", unsigned(v));
    s += b;
}
template <typename... A> std::string hgArgs(A... a) {
    std::string s;
    ((s += s.empty() ? "" : ", ", hgArg(s, a)), ...);
    return s;
}
static int32_t hgGetIntFwd(HostContext* hc, uint32_t pname);
template <typename F> static void hgAfter(Ctx& x, HostContext* hc, const char* name, F args) {
    if (hgDebugLevel >= 2) x.proc.log(LogLevel::Debug, std::string("GL ") + name + "(" + args() + ")");
    if (!hc->fn.glGetError) return;
    const uint32_t e = hc->fn.glGetError();
    if (!e) return;
    static std::atomic<int> logged{0};
    if (logged.fetch_add(1) < 200)
        x.proc.log(LogLevel::Warning, std::string("GPU: ") + name + " raised GL error 0x" + [&] { char b[8]; std::snprintf(b, 8, "%04x", e); return std::string(b); }() +
                                          (e == 0x0506 && hc->fn.glCheckFramebufferStatus ? [&] {
                                              // Say why the framebuffer is incomplete.
                                              char b[64];
                                              std::snprintf(b, sizeof b, " (framebuffer %d status 0x%04x)", hgGetIntFwd(hc, 0x8CA6),
                                                            hc->fn.glCheckFramebufferStatus(0x8D40));
                                              return std::string(b);
                                          }() : std::string()));
}

static int32_t hgGetIntFwd(HostContext* hc, uint32_t pname) {
    int32_t v[4] = {};
    if (hc && hc->fn.glGetIntegerv) hc->fn.glGetIntegerv(pname, v);
    return v[0];
}
static int32_t hgGetInt(HostContext* hc, uint32_t pname) {
    int32_t v[4] = {};
    if (hc && hc->fn.glGetIntegerv) hc->fn.glGetIntegerv(pname, v);
    return v[0];
}

// A gl*Pointer argument is an offset into the bound GL_ARRAY_BUFFER, or a guest address.
const void* hgArrayPtr(Ctx& x, GuestPtr g) {
    HostContext* hc = hgCurrent(x);
    if (!g) return nullptr;
    if (hgGetInt(hc, kArrayBufferBinding)) return reinterpret_cast<const void*>(uintptr_t(g));
    return x.mem.host(g);
}
const void* hgIndexPtr(Ctx& x, GuestPtr g) {
    HostContext* hc = hgCurrent(x);
    if (hgGetInt(hc, kElementArrayBufferBinding)) return reinterpret_cast<const void*>(uintptr_t(g));
    return g ? x.mem.host(g) : nullptr;
}

// Guest pointer for a host pointer the driver hands back (client array pointers).
static uint32_t toGuest(Ctx& x, const void* p) {
    if (!p) return 0;
    const uint8_t* base = x.mem.host(0);
    const uintptr_t a = uintptr_t(p), b = uintptr_t(base);
    if (a >= b && a - b < (uint64_t(1) << 32)) return uint32_t(a - b);
    return uint32_t(a);   // an offset into a bound buffer
}

#define HOSTGL_WRAPPERS
#include "runtime/gl/HostGLTable.inc"
#undef HOSTGL_WRAPPERS

// ------------------------------------------------------------- hand wrappers ---

#define HG_BEGIN(name, ...)                                   \
    HostContext* hc = hgCurrent(x);                           \
    if (!hc || !hc->fn.name) { hgMissing(x, hc, #name); return __VA_ARGS__; }


void hg_glGetIntegerv(Ctx& x, uint32_t pname, GuestPtr data) {
    HG_BEGIN(glGetIntegerv)
    auto* out = static_cast<int32_t*>(HG_PTR(x, data));
    if (!out) return;
    // PVRTC is decoded on upload when the driver lacks it, so it is always offered.
    if (pname == kNumCompressedTextureFormats || pname == kCompressedTextureFormats) {
        int32_t n = 0;
        hc->fn.glGetIntegerv(kNumCompressedTextureFormats, &n);
        std::vector<int32_t> fmts(size_t(std::max(n, 0)));
        if (n > 0) hc->fn.glGetIntegerv(kCompressedTextureFormats, fmts.data());
        for (uint32_t f : {kPvrtcRgb4, kPvrtcRgb2, kPvrtcRgba4, kPvrtcRgba2})
            if (std::find(fmts.begin(), fmts.end(), int32_t(f)) == fmts.end()) fmts.push_back(int32_t(f));
        if (pname == kNumCompressedTextureFormats) *out = int32_t(fmts.size());
        else std::memcpy(out, fmts.data(), fmts.size() * 4);
        return;
    }
    hc->fn.glGetIntegerv(pname, out);
    if (pname == kMaxTextureSize) *out = std::min(*out, 4096);   // what iOS devices of this era report
}

static bool hostHasPvrtc(HostContext* hc) {
    int32_t n = 0;
    hc->fn.glGetIntegerv(kNumCompressedTextureFormats, &n);
    if (n <= 0) return false;
    std::vector<int32_t> fmts(static_cast<size_t>(n));
    hc->fn.glGetIntegerv(kCompressedTextureFormats, fmts.data());
    return std::find(fmts.begin(), fmts.end(), int32_t(kPvrtcRgba4)) != fmts.end();
}

static bool isPvrtc(uint32_t f) { return f >= kPvrtcRgb4 && f <= kPvrtcRgba2; }

void hg_glCompressedTexImage2D(Ctx& x, uint32_t target, int32_t level, uint32_t internalformat, int32_t width, int32_t height,
                               int32_t border, int32_t imageSize, GuestPtr data) {
    HG_BEGIN(glCompressedTexImage2D)
    if (isPvrtc(internalformat) && !hostHasPvrtc(hc) && hc->fn.glTexImage2D) {
        const bool twoBpp = internalformat == kPvrtcRgb2 || internalformat == kPvrtcRgba2;
        const bool alpha = internalformat == kPvrtcRgba4 || internalformat == kPvrtcRgba2;
        std::vector<uint8_t> rgba;
        if (!data || !Graphics::decodePvrtc(x.mem.host(data), size_t(imageSize), width, height, twoBpp, alpha, rgba))
            rgba.assign(size_t(std::max(width, 1)) * size_t(std::max(height, 1)) * 4, 0);
        hc->fn.glTexImage2D(target, level, int32_t(kRgba), width, height, border, kRgba, kUnsignedByte, rgba.data());
        return;
    }
    hc->fn.glCompressedTexImage2D(target, level, internalformat, width, height, border, imageSize, HG_PTR(x, data));
}
void hg_glCompressedTexSubImage2D(Ctx& x, uint32_t target, int32_t level, int32_t xo, int32_t yo, int32_t width, int32_t height,
                                  uint32_t format, int32_t imageSize, GuestPtr data) {
    HG_BEGIN(glCompressedTexSubImage2D)
    if (isPvrtc(format) && !hostHasPvrtc(hc) && hc->fn.glTexSubImage2D) {
        const bool twoBpp = format == kPvrtcRgb2 || format == kPvrtcRgba2;
        const bool alpha = format == kPvrtcRgba4 || format == kPvrtcRgba2;
        std::vector<uint8_t> rgba;
        if (!data || !Graphics::decodePvrtc(x.mem.host(data), size_t(imageSize), width, height, twoBpp, alpha, rgba))
            rgba.assign(size_t(std::max(width, 1)) * size_t(std::max(height, 1)) * 4, 0);
        hc->fn.glTexSubImage2D(target, level, xo, yo, width, height, kRgba, kUnsignedByte, rgba.data());
        return;
    }
    hc->fn.glCompressedTexSubImage2D(target, level, xo, yo, width, height, format, imageSize, HG_PTR(x, data));
}

// APPLE_texture_format_BGRA8888 allows internalformat GL_RGBA with format GL_BGRA;
// EXT_texture_format_BGRA8888 (Mesa ES) wants GL_BGRA for both.
void hg_glTexImage2D(Ctx& x, uint32_t target, int32_t level, int32_t internalformat, int32_t width, int32_t height,
                     int32_t border, uint32_t format, uint32_t type, GuestPtr pixels) {
    HG_BEGIN(glTexImage2D)
    if (format == kBgra && internalformat == int32_t(kRgba) && !hc->desktop) internalformat = int32_t(kBgra);
    hc->fn.glTexImage2D(target, level, internalformat, width, height, border, format, type, HG_PTR(x, pixels));
}
void hg_glTexSubImage2D(Ctx& x, uint32_t target, int32_t level, int32_t xo, int32_t yo, int32_t width, int32_t height,
                        uint32_t format, uint32_t type, GuestPtr pixels) {
    HG_BEGIN(glTexSubImage2D)
    hc->fn.glTexSubImage2D(target, level, xo, yo, width, height, format, type, HG_PTR(x, pixels));
}

void hg_glOrthof(Ctx& x, float l, float r, float b, float t, float n, float f) {
    HostContext* hc = hgCurrent(x);
    if (hc && hc->fn.glOrthof) hc->fn.glOrthof(l, r, b, t, n, f);
    else if (hc && hc->fn.glOrtho) hc->fn.glOrtho(l, r, b, t, n, f);
    else hgMissing(x, hc, "glOrthof");
}
void hg_glFrustumf(Ctx& x, float l, float r, float b, float t, float n, float f) {
    HostContext* hc = hgCurrent(x);
    if (hc && hc->fn.glFrustumf) hc->fn.glFrustumf(l, r, b, t, n, f);
    else if (hc && hc->fn.glFrustum) hc->fn.glFrustum(l, r, b, t, n, f);
    else hgMissing(x, hc, "glFrustumf");
}
void hg_glClipPlanef(Ctx& x, uint32_t plane, GuestPtr eq) {
    HostContext* hc = hgCurrent(x);
    if (!hc || !eq) return;
    if (hc->fn.glClipPlanef) { hc->fn.glClipPlanef(plane, x.mem.host(eq)); return; }
    if (!hc->fn.glClipPlane) { hgMissing(x, hc, "glClipPlanef"); return; }
    double d[4];
    for (int i = 0; i < 4; ++i) d[i] = x.mem.read<float>(eq + uint32_t(i) * 4);
    hc->fn.glClipPlane(plane, d);
}
void hg_glGetClipPlanef(Ctx& x, uint32_t plane, GuestPtr eq) {
    HostContext* hc = hgCurrent(x);
    if (!hc || !eq) return;
    if (hc->fn.glGetClipPlanef) { hc->fn.glGetClipPlanef(plane, x.mem.host(eq)); return; }
    if (!hc->fn.glGetClipPlane) { hgMissing(x, hc, "glGetClipPlanef"); return; }
    double d[4] = {};
    hc->fn.glGetClipPlane(plane, d);
    for (int i = 0; i < 4; ++i) x.mem.write<float>(eq + uint32_t(i) * 4, float(d[i]));
}

void hg_glGetPointerv(Ctx& x, uint32_t pname, GuestPtr out) {
    HG_BEGIN(glGetPointerv)
    void* p = nullptr;
    hc->fn.glGetPointerv(pname, &p);
    if (out) x.mem.write<uint32_t>(out, toGuest(x, p));
}
void hg_glGetVertexAttribPointerv(Ctx& x, uint32_t index, uint32_t pname, GuestPtr out) {
    HG_BEGIN(glGetVertexAttribPointerv)
    void* p = nullptr;
    hc->fn.glGetVertexAttribPointerv(index, pname, &p);
    if (out) x.mem.write<uint32_t>(out, toGuest(x, p));
}

// glShaderSource(shader, count, const char** strings, const GLint* lengths):
// the string array holds guest pointers.
void hg_glShaderSource(Ctx& x, uint32_t shader, int32_t count, GuestPtr strings, GuestPtr lengths) {
    HG_BEGIN(glShaderSource)
    if (count < 0 || count > 4096) return;
    std::vector<const char*> ptrs(static_cast<size_t>(count));
    for (int32_t i = 0; i < count; ++i) {
        const uint32_t s = x.mem.read<uint32_t>(strings + uint32_t(i) * 4);
        ptrs[size_t(i)] = s ? reinterpret_cast<const char*>(x.mem.host(s)) : "";
    }
    hc->fn.glShaderSource(shader, count, ptrs.data(), HG_PTR(x, lengths));
    if (const char* dir = std::getenv("HVM_GL_DUMP_SHADERS")) {
        std::string path = std::string(dir) + "/shader-" + std::to_string(shader) + ".glsl";
        if (std::FILE* f = std::fopen(path.c_str(), "wb")) {
            for (int32_t i = 0; i < count; ++i) {
                const int32_t len = lengths ? x.mem.read<int32_t>(lengths + uint32_t(i) * 4) : -1;
                std::fwrite(ptrs[size_t(i)], 1, len >= 0 ? size_t(len) : std::strlen(ptrs[size_t(i)]), f);
            }
            std::fclose(f);
        }
    }
}

// Shader compile / link: the driver's log goes to ours when it fails, since a
// failing shader is the usual cause of missing geometry.
void hg_glCompileShader(Ctx& x, uint32_t shader) {
    HG_BEGIN(glCompileShader)
    hc->fn.glCompileShader(shader);
    int32_t ok = 1;
    if (hc->fn.glGetShaderiv) hc->fn.glGetShaderiv(shader, 0x8B81 /*COMPILE_STATUS*/, &ok);
    if (ok || !hc->fn.glGetShaderInfoLog) return;
    char log[2048] = {};
    int32_t n = 0;
    hc->fn.glGetShaderInfoLog(shader, int32_t(sizeof log) - 1, &n, log);
    x.proc.log(LogLevel::Warning, std::string("GPU: shader ") + std::to_string(shader) + " failed to compile: " + log);
}
void hg_glLinkProgram(Ctx& x, uint32_t program) {
    HG_BEGIN(glLinkProgram)
    hc->fn.glLinkProgram(program);
    int32_t ok = 1;
    if (hc->fn.glGetProgramiv) hc->fn.glGetProgramiv(program, 0x8B82 /*LINK_STATUS*/, &ok);
    if (ok || !hc->fn.glGetProgramInfoLog) return;
    char log[2048] = {};
    int32_t n = 0;
    hc->fn.glGetProgramInfoLog(program, int32_t(sizeof log) - 1, &n, log);
    x.proc.log(LogLevel::Warning, std::string("GPU: program ") + std::to_string(program) + " failed to link: " + log);
}

// ----------------------------------------------------------- mapped buffers ---
// The driver's mapping is host memory the guest can't address. Hand the guest a
// shadow copy and write it back on flush / unmap.

static uint32_t mapRange(Ctx& x, HostContext* hc, uint32_t target, intptr_t offset, intptr_t length, uint32_t access) {
    if (length <= 0) {
        int32_t size = 0;
        hc->fn.glGetBufferParameteriv(target, kBufferSize, &size);
        length = size - offset;
        if (length <= 0) {
            if (hgDebugLevel >= 2) x.proc.log(LogLevel::Debug, "GL map target " + std::to_string(target) + ": buffer size " + std::to_string(size));
            return 0;
        }
    }
    void* host = nullptr;
    if (hc->fn.glMapBufferRange) host = hc->fn.glMapBufferRange(target, offset, length, access & ~uint32_t(0x8 /*INVALIDATE_BUFFER*/));
    MappedBuffer m;
    m.offset = offset;
    m.length = length;
    m.access = access;
    m.host = host;
    m.guest = x.proc.heap().allocZeroed(uint32_t(length));
    if (host && (access & kMapReadBit)) std::memcpy(x.mem.host(m.guest), host, size_t(length));
    else if (host && !(access & (0x4 | 0x8))) std::memcpy(x.mem.host(m.guest), host, size_t(length));   // keep contents unless invalidated
    hc->mapped[target] = m;
    if (hgDebugLevel >= 2)
        x.proc.log(LogLevel::Debug, "GL map target " + std::to_string(target) + " +" + std::to_string(offset) + " len " +
                                        std::to_string(length) + (host ? "" : " (driver gave no mapping)"));
    return m.guest;
}

uint32_t hg_glMapBufferOES(Ctx& x, uint32_t target, uint32_t /*access: WRITE_ONLY_OES*/) {
    HostContext* hc = hgCurrent(x);
    if (!hc) return 0;
    return mapRange(x, hc, target, 0, 0, kMapWriteBit);
}
uint32_t hg_glMapBufferRange(Ctx& x, uint32_t target, int32_t offset, int32_t length, uint32_t access) {
    HostContext* hc = hgCurrent(x);
    if (!hc) return 0;
    return mapRange(x, hc, target, intptr_t(uint32_t(offset)), intptr_t(uint32_t(length)), access);
}
void hg_glFlushMappedBufferRange(Ctx& x, uint32_t target, int32_t offset, int32_t length) {
    HostContext* hc = hgCurrent(x);
    if (!hc) return;
    auto it = hc->mapped.find(target);
    if (it == hc->mapped.end() || !it->second.host) return;
    MappedBuffer& m = it->second;
    if (offset < 0 || length <= 0 || offset + length > m.length) return;
    std::memcpy(static_cast<uint8_t*>(m.host) + offset, x.mem.host(m.guest + uint32_t(offset)), size_t(length));
    if (hc->fn.glFlushMappedBufferRange) hc->fn.glFlushMappedBufferRange(target, offset, length);
}
bool hg_glUnmapBufferOES(Ctx& x, uint32_t target) {
    HostContext* hc = hgCurrent(x);
    if (!hc) return false;
    auto it = hc->mapped.find(target);
    if (it == hc->mapped.end()) return false;
    MappedBuffer m = it->second;
    hc->mapped.erase(it);
    bool ok = true;
    if (m.host) {
        if ((m.access & kMapWriteBit) && !(m.access & kMapFlushExplicitBit))
            std::memcpy(m.host, x.mem.host(m.guest), size_t(m.length));
        if (hc->fn.glUnmapBuffer) ok = hc->fn.glUnmapBuffer(target) != 0;
        else if (hc->fn.glUnmapBufferOES) ok = hc->fn.glUnmapBufferOES(target) != 0;
    } else if (m.access & kMapWriteBit) {
        // No driver mapping (ES 1.1 without MapBufferRange): write the shadow back.
        if (hc->fn.glBufferSubData) hc->fn.glBufferSubData(target, m.offset, m.length, x.mem.host(m.guest));
    }
    x.proc.heap().free(m.guest);
    return ok;
}
uint32_t hg_glMapBuffer(Ctx& x, uint32_t target, uint32_t access) { return hg_glMapBufferOES(x, target, access); }
uint32_t hg_glMapBufferRangeEXT(Ctx& x, uint32_t target, int32_t offset, int32_t length, uint32_t access) {
    return hg_glMapBufferRange(x, target, offset, length, access);
}
void hg_glFlushMappedBufferRangeEXT(Ctx& x, uint32_t target, int32_t offset, int32_t length) {
    hg_glFlushMappedBufferRange(x, target, offset, length);
}
bool hg_glUnmapBuffer(Ctx& x, uint32_t target) { return hg_glUnmapBufferOES(x, target); }
void hg_glGetBufferPointervOES(Ctx& x, uint32_t target, uint32_t pname, GuestPtr out) {
    HostContext* hc = hgCurrent(x);
    if (!hc || !out) return;
    auto it = hc->mapped.find(target);
    x.mem.write<uint32_t>(out, pname == kBufferMapPointer && it != hc->mapped.end() ? it->second.guest : 0);
}

// ------------------------------------------------------------- multisample ---

// ------------------------------------------------------------- attachments ---
// Apple's drivers accept a detach written as (target, attachment, 0, 0); the ES
// spec wants GL_RENDERBUFFER as the third argument even then, and other drivers
// reject it, leaving the old attachment in place.
void hg_glFramebufferRenderbuffer(Ctx& x, uint32_t target, uint32_t attachment, uint32_t rbtarget, uint32_t renderbuffer) {
    HG_BEGIN(glFramebufferRenderbuffer)
    if (rbtarget == 0 && renderbuffer == 0) rbtarget = kRenderbuffer;
    hc->fn.glFramebufferRenderbuffer(target, attachment, rbtarget, renderbuffer);
    if (hgDebug) hgAfter(x, hc, "glFramebufferRenderbuffer", [&] { return hgArgs(target, attachment, rbtarget, renderbuffer); });
}
void hg_glFramebufferRenderbufferOES(Ctx& x, uint32_t target, uint32_t attachment, uint32_t rbtarget, uint32_t renderbuffer) {
    HG_BEGIN(glFramebufferRenderbufferOES)
    if (rbtarget == 0 && renderbuffer == 0) rbtarget = kRenderbuffer;
    hc->fn.glFramebufferRenderbufferOES(target, attachment, rbtarget, renderbuffer);
    if (hgDebug) hgAfter(x, hc, "glFramebufferRenderbufferOES", [&] { return hgArgs(target, attachment, rbtarget, renderbuffer); });
}

void hg_glResolveMultisampleFramebufferAPPLE(Ctx& x) {
    HostContext* hc = hgCurrent(x);
    if (!hc) return;
    if (hc->fn.glResolveMultisampleFramebufferAPPLE) { hc->fn.glResolveMultisampleFramebufferAPPLE(); return; }
    if (!hc->fn.glBlitFramebuffer) { hgMissing(x, hc, "glResolveMultisampleFramebufferAPPLE"); return; }
    // READ = multisampled source, DRAW = resolve target (APPLE uses the same enum values).
    const uint32_t readFb = uint32_t(hgGetInt(hc, kReadFramebufferBinding));
    int32_t w = 0, h = 0, type = 0, name = 0;
    if (readFb) {
        hc->fn.glGetFramebufferAttachmentParameteriv(kReadFramebuffer, kColorAttachment0, kFbAttachmentObjectType, &type);
        hc->fn.glGetFramebufferAttachmentParameteriv(kReadFramebuffer, kColorAttachment0, kFbAttachmentObjectName, &name);
        if (uint32_t(type) == kRenderbuffer && name) {
            const int32_t prev = hgGetInt(hc, kRenderbufferBinding);
            hc->fn.glBindRenderbuffer(kRenderbuffer, uint32_t(name));
            hc->fn.glGetRenderbufferParameteriv(kRenderbuffer, kRenderbufferWidth, &w);
            hc->fn.glGetRenderbufferParameteriv(kRenderbuffer, kRenderbufferHeight, &h);
            hc->fn.glBindRenderbuffer(kRenderbuffer, uint32_t(prev));
        }
    }
    if (w <= 0 || h <= 0) return;
    hc->fn.glBlitFramebuffer(0, 0, w, h, 0, 0, w, h, kColorBufferBit, kNearest);
}

// ------------------------------------------------------------------- syncs ---

uint32_t hg_glFenceSyncAPPLE(Ctx& x, uint32_t condition, uint32_t flags) {
    HostContext* hc = hgCurrent(x);
    if (!hc || !hc->fn.glFenceSync) { hgMissing(x, hc, "glFenceSyncAPPLE"); return 0; }
    void* s = hc->fn.glFenceSync(condition, flags);
    if (!s) return 0;
    State& st = S(x.proc);
    std::lock_guard<std::mutex> lk(st.mu);
    const uint32_t h = st.nextSync++;
    st.syncs[h] = s;
    return h;
}
static void* syncOf(Ctx& x, uint32_t h) {
    State& st = S(x.proc);
    std::lock_guard<std::mutex> lk(st.mu);
    auto it = st.syncs.find(h);
    return it == st.syncs.end() ? nullptr : it->second;
}
bool hg_glIsSyncAPPLE(Ctx& x, uint32_t h) { return syncOf(x, h) != nullptr; }
void hg_glDeleteSyncAPPLE(Ctx& x, uint32_t h) {
    HostContext* hc = hgCurrent(x);
    void* s = syncOf(x, h);
    if (!s) return;
    if (hc && hc->fn.glDeleteSync) hc->fn.glDeleteSync(s);
    State& st = S(x.proc);
    std::lock_guard<std::mutex> lk(st.mu);
    st.syncs.erase(h);
}
uint32_t hg_glClientWaitSyncAPPLE(Ctx& x, uint32_t h, uint32_t flags, uint64_t timeout) {
    HostContext* hc = hgCurrent(x);
    void* s = syncOf(x, h);
    if (!hc || !s || !hc->fn.glClientWaitSync) return 0x911A;   // GL_ALREADY_SIGNALED
    return hc->fn.glClientWaitSync(s, flags, timeout);
}
void hg_glWaitSyncAPPLE(Ctx& x, uint32_t h, uint32_t flags, uint64_t timeout) {
    HostContext* hc = hgCurrent(x);
    void* s = syncOf(x, h);
    if (hc && s && hc->fn.glWaitSync) hc->fn.glWaitSync(s, flags, timeout);
}
void hg_glGetSyncivAPPLE(Ctx& x, uint32_t h, uint32_t pname, int32_t bufSize, GuestPtr length, GuestPtr values) {
    HostContext* hc = hgCurrent(x);
    void* s = syncOf(x, h);
    if (!hc || !s || !hc->fn.glGetSynciv) return;
    hc->fn.glGetSynciv(s, pname, bufSize, static_cast<int32_t*>(HG_PTR(x, length)), static_cast<int32_t*>(HG_PTR(x, values)));
}

// Debug labels and markers: nothing to forward.
void hg_glLabelObjectEXT(Ctx&, uint32_t, uint32_t, int32_t, GuestPtr) {}
void hg_glPushGroupMarkerEXT(Ctx&, int32_t, GuestPtr) {}
void hg_glPopGroupMarkerEXT(Ctx&) {}
void hg_glInsertEventMarkerEXT(Ctx&, int32_t, GuestPtr) {}

// ------------------------------------------------------- ES 1.1 fixed point ---
// GLfixed is 16.16. Enum-valued parameters (texture env mode, fog mode, texture
// parameters) are passed as the enum itself, not scaled.

namespace {
inline float fx(int32_t v) { return float(v) / 65536.0f; }
int fixedCount(uint32_t pname) {
    switch (pname) {
        case 0x1200: case 0x1201: case 0x1202: case 0x1600: case 0x0B53: case 0x0B66: case 0x2201:
            return 4;   // AMBIENT, DIFFUSE, SPECULAR, EMISSION, LIGHT_MODEL_AMBIENT, FOG_COLOR, TEXTURE_ENV_COLOR
        case 0x1203: return 4;   // POSITION
        case 0x1204: return 3;   // SPOT_DIRECTION
        case 0x1602: return 4;   // AMBIENT_AND_DIFFUSE
        default: return 1;
    }
}
bool enumParam(uint32_t pname) {
    switch (pname) {
        case 0x2200:             // TEXTURE_ENV_MODE
        case 0x8571: case 0x8572: // COMBINE_RGB, COMBINE_ALPHA
        case 0x8580: case 0x8581: case 0x8582: case 0x8588: case 0x8589: case 0x858A:   // SRCn_RGB / SRCn_ALPHA
        case 0x8590: case 0x8591: case 0x8592: case 0x8598: case 0x8599: case 0x859A:   // OPERANDn
        case 0x0B65:             // FOG_MODE
        case 0x8862:             // COORD_REPLACE_OES
        case 0x2800: case 0x2801: case 0x2802: case 0x2803: case 0x8191:   // texture filters/wrap, GENERATE_MIPMAP
            return true;
        default:
            return false;
    }
}
} // namespace

#define FX_CALL(name, ...) do { HostContext* hc = hgCurrent(x); if (!hc || !hc->fn.name) { hgMissing(x, hc, #name); return; } hc->fn.name(__VA_ARGS__); } while (0)

void hgx_alphaFuncx(Ctx& x, uint32_t f, int32_t r) { FX_CALL(glAlphaFunc, f, fx(r)); }
void hgx_clearColorx(Ctx& x, int32_t r, int32_t g, int32_t b, int32_t a) { FX_CALL(glClearColor, fx(r), fx(g), fx(b), fx(a)); }
void hgx_clearDepthx(Ctx& x, int32_t d) { FX_CALL(glClearDepthf, fx(d)); }
void hgx_color4x(Ctx& x, int32_t r, int32_t g, int32_t b, int32_t a) { FX_CALL(glColor4f, fx(r), fx(g), fx(b), fx(a)); }
void hgx_depthRangex(Ctx& x, int32_t n, int32_t f) { FX_CALL(glDepthRangef, fx(n), fx(f)); }
void hgx_lineWidthx(Ctx& x, int32_t w) { FX_CALL(glLineWidth, fx(w)); }
void hgx_pointSizex(Ctx& x, int32_t s) { FX_CALL(glPointSize, fx(s)); }
void hgx_polygonOffsetx(Ctx& x, int32_t f, int32_t u) { FX_CALL(glPolygonOffset, fx(f), fx(u)); }
void hgx_sampleCoveragex(Ctx& x, int32_t v, uint32_t inv) { FX_CALL(glSampleCoverage, fx(v), uint8_t(inv & 0xFF)); }
void hgx_normal3x(Ctx& x, int32_t a, int32_t b, int32_t c) { FX_CALL(glNormal3f, fx(a), fx(b), fx(c)); }
void hgx_multiTexCoord4x(Ctx& x, uint32_t t, int32_t s, int32_t tt, int32_t r, int32_t q) { FX_CALL(glMultiTexCoord4f, t, fx(s), fx(tt), fx(r), fx(q)); }
void hgx_rotatex(Ctx& x, int32_t a, int32_t b, int32_t c, int32_t d) { FX_CALL(glRotatef, fx(a), fx(b), fx(c), fx(d)); }
void hgx_scalex(Ctx& x, int32_t a, int32_t b, int32_t c) { FX_CALL(glScalef, fx(a), fx(b), fx(c)); }
void hgx_translatex(Ctx& x, int32_t a, int32_t b, int32_t c) { FX_CALL(glTranslatef, fx(a), fx(b), fx(c)); }
void hgx_orthox(Ctx& x, int32_t l, int32_t r, int32_t b, int32_t t, int32_t n, int32_t f) { hg_glOrthof(x, fx(l), fx(r), fx(b), fx(t), fx(n), fx(f)); }
void hgx_frustumx(Ctx& x, int32_t l, int32_t r, int32_t b, int32_t t, int32_t n, int32_t f) { hg_glFrustumf(x, fx(l), fx(r), fx(b), fx(t), fx(n), fx(f)); }
void hgx_loadMatrixx(Ctx& x, GuestPtr m) {
    float f[16];
    for (int i = 0; i < 16; ++i) f[i] = fx(x.mem.read<int32_t>(m + uint32_t(i) * 4));
    FX_CALL(glLoadMatrixf, f);
}
void hgx_multMatrixx(Ctx& x, GuestPtr m) {
    float f[16];
    for (int i = 0; i < 16; ++i) f[i] = fx(x.mem.read<int32_t>(m + uint32_t(i) * 4));
    FX_CALL(glMultMatrixf, f);
}
void hgx_clipPlanex(Ctx& x, uint32_t plane, GuestPtr eq) {
    HostContext* hc = hgCurrent(x);
    if (!hc || !eq) return;
    double d[4];
    float f[4];
    for (int i = 0; i < 4; ++i) { f[i] = fx(x.mem.read<int32_t>(eq + uint32_t(i) * 4)); d[i] = f[i]; }
    if (hc->fn.glClipPlanef) hc->fn.glClipPlanef(plane, f);
    else if (hc->fn.glClipPlane) hc->fn.glClipPlane(plane, d);
}
void hgx_fogx(Ctx& x, uint32_t pname, int32_t p) {
    if (enumParam(pname)) FX_CALL(glFogf, pname, float(p));
    else FX_CALL(glFogf, pname, fx(p));
}
void hgx_fogxv(Ctx& x, uint32_t pname, GuestPtr v) {
    float f[4] = {};
    const int n = fixedCount(pname);
    for (int i = 0; i < n; ++i) { const int32_t r = x.mem.read<int32_t>(v + uint32_t(i) * 4); f[i] = enumParam(pname) ? float(r) : fx(r); }
    FX_CALL(glFogfv, pname, f);
}
void hgx_lightModelx(Ctx& x, uint32_t pname, int32_t p) { FX_CALL(glLightModelf, pname, fx(p)); }
void hgx_lightModelxv(Ctx& x, uint32_t pname, GuestPtr v) {
    float f[4] = {};
    for (int i = 0; i < fixedCount(pname); ++i) f[i] = fx(x.mem.read<int32_t>(v + uint32_t(i) * 4));
    FX_CALL(glLightModelfv, pname, f);
}
void hgx_lightx(Ctx& x, uint32_t light, uint32_t pname, int32_t p) { FX_CALL(glLightf, light, pname, fx(p)); }
void hgx_lightxv(Ctx& x, uint32_t light, uint32_t pname, GuestPtr v) {
    float f[4] = {};
    for (int i = 0; i < fixedCount(pname); ++i) f[i] = fx(x.mem.read<int32_t>(v + uint32_t(i) * 4));
    FX_CALL(glLightfv, light, pname, f);
}
void hgx_materialx(Ctx& x, uint32_t face, uint32_t pname, int32_t p) { FX_CALL(glMaterialf, face, pname, fx(p)); }
void hgx_materialxv(Ctx& x, uint32_t face, uint32_t pname, GuestPtr v) {
    float f[4] = {};
    for (int i = 0; i < fixedCount(pname); ++i) f[i] = fx(x.mem.read<int32_t>(v + uint32_t(i) * 4));
    FX_CALL(glMaterialfv, face, pname, f);
}
void hgx_pointParameterx(Ctx& x, uint32_t pname, int32_t p) { FX_CALL(glPointParameterf, pname, fx(p)); }
void hgx_pointParameterxv(Ctx& x, uint32_t pname, GuestPtr v) {
    float f[3] = {};
    const int n = pname == 0x8129 ? 3 : 1;   // POINT_DISTANCE_ATTENUATION
    for (int i = 0; i < n; ++i) f[i] = fx(x.mem.read<int32_t>(v + uint32_t(i) * 4));
    FX_CALL(glPointParameterfv, pname, f);
}
void hgx_texEnvx(Ctx& x, uint32_t target, uint32_t pname, int32_t p) {
    if (enumParam(pname) || pname == 0x8573 || pname == 0x0D1C)   // RGB_SCALE / ALPHA_SCALE are small integers in practice
        FX_CALL(glTexEnvi, target, pname, p >= 65536 ? p / 65536 : p);
    else FX_CALL(glTexEnvf, target, pname, fx(p));
}
void hgx_texEnvxv(Ctx& x, uint32_t target, uint32_t pname, GuestPtr v) {
    if (pname == 0x2201) {   // TEXTURE_ENV_COLOR
        float f[4];
        for (int i = 0; i < 4; ++i) f[i] = fx(x.mem.read<int32_t>(v + uint32_t(i) * 4));
        FX_CALL(glTexEnvfv, target, pname, f);
    } else {
        hgx_texEnvx(x, target, pname, x.mem.read<int32_t>(v));
    }
}
void hgx_texParameterx(Ctx& x, uint32_t target, uint32_t pname, int32_t p) { FX_CALL(glTexParameteri, target, pname, p); }
void hgx_texParameterxv(Ctx& x, uint32_t target, uint32_t pname, GuestPtr v) { FX_CALL(glTexParameteri, target, pname, x.mem.read<int32_t>(v)); }
void hgx_getFixedv(Ctx& x, uint32_t pname, GuestPtr out) {
    HostContext* hc = hgCurrent(x);
    if (!hc || !hc->fn.glGetFloatv || !out) return;
    float f[16] = {};
    hc->fn.glGetFloatv(pname, f);
    // Matrices return 16 values; everything the apps ask for fits.
    const int n = (pname == 0x0BA6 || pname == 0x0BA7 || pname == 0x0BA8) ? 16 : fixedCount(pname);
    for (int i = 0; i < n; ++i) x.mem.write<int32_t>(out + uint32_t(i) * 4, int32_t(std::lround(f[i] * 65536.0f)));
}

// --------------------------------------------------------------- lifecycle ---

bool enabled(Process& p) { return p.config().hostGL != nullptr; }

static State& ready(Process& p) {
    State& st = S(p);
    if (!st.platform) st.platform = p.config().hostGL;
    return st;
}

void* contextCreate(Process& p, uint32_t guestContext, int api, uint32_t guestSharegroup) {
    State& st = ready(p);
    // Testing aid: HVM_GPU_DISABLE_API=1 (or 2) acts as if the driver lacked that API.
    if (const char* off = std::getenv("HVM_GPU_DISABLE_API"); off && std::atoi(off) == api) return nullptr;
    auto hc = makeContext(p, api, guestContext, guestSharegroup, false);
    if (!hc) return nullptr;
    void* h = hc->handle;
    std::lock_guard<std::mutex> lk(st.mu);
    st.contexts[guestContext] = std::move(hc);
    return h;
}

void contextDestroy(Process& p, uint32_t guestContext) {
    State& st = ready(p);
    std::unique_ptr<HostContext> hc;
    {
        std::lock_guard<std::mutex> lk(st.mu);
        auto it = st.contexts.find(guestContext);
        if (it == st.contexts.end()) return;
        hc = std::move(it->second);
        st.contexts.erase(it);
        for (auto r = st.shareRoot.begin(); r != st.shareRoot.end();) {
            if (r->second == hc.get()) r = st.shareRoot.erase(r);
            else ++r;
        }
        if (st.lastGuestContext == guestContext) st.lastGuestContext = 0;
    }
    if (tls.proc == &p && tls.ctx == hc.get()) bind(p, nullptr);
    if (hc && hc->handle) st.platform->destroyContext(hc->handle);
}

void contextMakeCurrent(Process& p, uint32_t guestContext) {
    State& st = ready(p);
    HostContext* hc = nullptr;
    {
        std::lock_guard<std::mutex> lk(st.mu);
        auto it = st.contexts.find(guestContext);
        if (it != st.contexts.end()) hc = it->second.get();
        if (hc) st.lastGuestContext = guestContext;
    }
    if (hgDebugLevel >= 2)
        p.log(LogLevel::Debug, "GL setCurrentContext guest " + std::to_string(guestContext) + " on thread " +
                                   std::to_string(p.currentThread().id) + (hc ? "" : " (no host context)"));
    if (tls.proc == &p && hc && tls.ctx &&
        (tls.ctx == hc || (tls.ctx->implicitWorker && tls.ctx->guestContext == guestContext)))
        return;   // already current here (or this thread's shared stand-in for it)
    bind(p, hc);
    // A guest context without a host context (its API isn't available on this
    // driver) renders in software; GL calls on this thread go to that path.
    if (!hc && guestContext) tls.software = true;
}

bool isHostContext(Process& p, uint32_t guestContext) {
    if (!enabled(p)) return false;
    State& st = S(p);
    std::lock_guard<std::mutex> lk(st.mu);
    return st.contexts.count(guestContext) != 0;
}

// Which implementation serves a GL call on this thread.
static bool hgUseHost(Ctx& x) {
    if (tls.proc == &x.proc) {
        if (tls.ctx) return true;
        if (tls.software) return false;
    }
    State& st = S(x.proc);
    std::lock_guard<std::mutex> lk(st.mu);
    return st.lastGuestContext != 0 && st.contexts.count(st.lastGuestContext) != 0;
}

static HostContext* contextFor(Process& p, uint32_t guestContext) {
    State& st = S(p);
    std::lock_guard<std::mutex> lk(st.mu);
    auto it = st.contexts.find(guestContext);
    return it == st.contexts.end() ? nullptr : it->second.get();
}

bool renderbufferStorageFromDrawable(Process& p, uint32_t guestContext, uint32_t layer, int width, int height) {
    State& st = ready(p);
    HostContext* hc = contextFor(p, guestContext);
    if (!hc) return false;
    if (tls.ctx != hc) bind(p, hc);
    const uint32_t rb = uint32_t(hgGetInt(hc, kRenderbufferBinding));
    if (!rb) return false;
    bool handled = false;
    bool ok = st.platform->drawableStorage(hc->handle, layer, width, height, handled);
    if (!handled) {
        auto storage = hc->fn.glRenderbufferStorage ? hc->fn.glRenderbufferStorage : hc->fn.glRenderbufferStorageOES;
        if (!storage || !layer) return layer == 0;
        storage(kRenderbuffer, kRgba8, width, height);
        ok = true;
    }
    p.log(LogLevel::Info, "GPU: drawable " + std::to_string(width) + "x" + std::to_string(height) + " for renderbuffer " +
                              std::to_string(rb) + (handled ? " (platform layer)" : " (RGBA8 storage)") + (ok ? "" : " FAILED"));
    std::lock_guard<std::mutex> lk(st.mu);
    Drawable& d = st.drawables[key2(hc->guestSharegroup, rb)];
    d.layer = layer;
    d.width = width;
    d.height = height;
    return ok;
}

bool presentRenderbuffer(Process& p, uint32_t guestContext, uint32_t& layer, int& width, int& height,
                         std::vector<uint8_t>& rgba, bool& direct) {
    State& st = ready(p);
    direct = false;
    HostContext* hc = contextFor(p, guestContext);
    if (!hc) return false;
    if (tls.ctx != hc) bind(p, hc);
    const uint32_t rb = uint32_t(hgGetInt(hc, kRenderbufferBinding));
    Drawable d;
    uint32_t rbName = rb;
    {
        std::lock_guard<std::mutex> lk(st.mu);
        auto it = st.drawables.find(key2(hc->guestSharegroup, rb));
        if (it != st.drawables.end()) d = it->second;
        else {
            // Some apps unbind the renderbuffer before presenting; use the share group's only drawable.
            for (auto& [k, v] : st.drawables)
                if (uint32_t(k >> 32) == hc->guestSharegroup && v.layer) { d = v; rbName = uint32_t(k); break; }
        }
    }
    if (hgDebugLevel >= 2) p.log(LogLevel::Debug, "GL --- present rb " + std::to_string(rbName));
    if (!d.layer || d.width <= 0 || d.height <= 0) return false;
    layer = d.layer;
    width = d.width;
    height = d.height;
    // Per process: a later session in the same app (Test All Games) starts at frame 1 again.
    const int n = st.presents + 1;
    std::string probe;
    const bool sample = n == 1 || n == 30 || n == 60 || n == 120 || n == 300 || n == 600 || (n > 600 && n % 1800 == 0);
    if (sample && hc->fn.glReadPixels) {
        // Diagnostic: sample the frame before it goes to the display, so a blank
        // screen can be told apart from a frame the GPU really rendered black.
        uint64_t sum[3] = {}, lit = 0;
        for (int gy = 0; gy < 8; ++gy)
            for (int gx = 0; gx < 8; ++gx) {
                uint8_t px[4] = {};
                hc->fn.glReadPixels(int32_t((gx * 2 + 1) * width / 16), int32_t((gy * 2 + 1) * height / 16), 1, 1, kRgba, kUnsignedByte, px);
                sum[0] += px[0]; sum[1] += px[1]; sum[2] += px[2];
                if (px[0] > 8 || px[1] > 8 || px[2] > 8) ++lit;
            }
        probe = " (average rgb " + std::to_string(sum[0] / 64) + "," + std::to_string(sum[1] / 64) + "," +
                std::to_string(sum[2] / 64) + ", " + std::to_string(lit) + "/64 samples lit)";
    }
    // Pace direct presents to the 60 Hz display. Apple's driver blocks a game that
    // presents faster; here such a game (Angry Birds renders in a tight loop)
    // would spin at thousands of frames a second and burn the battery.
    {
        using Clock = std::chrono::steady_clock;
        const auto now = Clock::now();
        const auto minGap = std::chrono::microseconds(16400);
        if (st.lastPresent.time_since_epoch().count() && now - st.lastPresent < minGap)
            std::this_thread::sleep_for(minGap - (now - st.lastPresent));
        st.lastPresent = Clock::now();
    }
    if (st.platform->presentDirect(hc->handle, d.layer)) {
        direct = true;
        ++st.presents;
        if (!probe.empty()) p.log(LogLevel::Info, "GPU: presented frame " + std::to_string(n) + " directly" + probe);
        return true;
    }
    {
        if (!st.toldReadback.exchange(true)) p.log(LogLevel::Warning, "GPU: direct present unavailable; reading frames back");
    }

    // Read the drawable back for the software compositor.
    if (!hc->fn.glGenFramebuffers || !hc->fn.glReadPixels) return false;
    const int32_t prevFb = hgGetInt(hc, kFramebufferBinding);
    if (!hc->readFbo) hc->fn.glGenFramebuffers(1, &hc->readFbo);
    hc->fn.glBindFramebuffer(kFramebuffer, hc->readFbo);
    hc->fn.glFramebufferRenderbuffer(kFramebuffer, kColorAttachment0, kRenderbuffer, rbName);
    const int32_t prevPack = hgGetInt(hc, kPackAlignment);
    hc->fn.glPixelStorei(kPackAlignment, 4);
    rgba.resize(size_t(width) * size_t(height) * 4);
    hc->fn.glReadPixels(0, 0, width, height, kRgba, kUnsignedByte, rgba.data());
    hc->fn.glPixelStorei(kPackAlignment, prevPack ? prevPack : 4);
    hc->fn.glBindFramebuffer(kFramebuffer, uint32_t(prevFb));
    return true;
}

void layerGone(Process& p, uint32_t layer) {
    State& st = S(p);
    {
        std::lock_guard<std::mutex> lk(st.mu);
        for (auto it = st.drawables.begin(); it != st.drawables.end();) {
            if (it->second.layer == layer) it = st.drawables.erase(it);
            else ++it;
        }
        st.placed.erase(layer);
    }
    if (st.platform) st.platform->releaseLayer(layer);
}

void layerPlaced(Process& p, uint32_t layer, const float m[6], float bx, float by, float bw, float bh, int surfaceW,
                 int surfaceH) {
    State& st = S(p);
    if (!st.platform || surfaceW <= 0 || surfaceH <= 0 || bw <= 0 || bh <= 0) return;
    int w = 0, h = 0;
    Placement next;
    {
        std::lock_guard<std::mutex> lk(st.mu);
        for (const auto& [k, d] : st.drawables)
            if (d.layer == layer) { w = d.width; h = d.height; break; }
        if (w <= 0 || h <= 0) return;
        // drawable pixels -> layer bounds (points) -> surface pixels -> unit square
        const float sx = bw / float(w), sy = bh / float(h), ux = 1.0f / float(surfaceW), uy = 1.0f / float(surfaceH);
        next.unit[0] = sx * m[0] * ux;
        next.unit[1] = sx * m[1] * uy;
        next.unit[2] = sy * m[2] * ux;
        next.unit[3] = sy * m[3] * uy;
        next.unit[4] = (bx * m[0] + by * m[2] + m[4]) * ux;
        next.unit[5] = (bx * m[1] + by * m[3] + m[5]) * uy;
        auto it = st.placed.find(layer);
        if (it != st.placed.end()) {
            bool same = true;
            for (int i = 0; i < 6; ++i) same = same && std::fabs(it->second.unit[i] - next.unit[i]) < 1e-5f;
            if (same) return;
        }
        st.placed[layer] = next;
    }
    char buf[160];
    std::snprintf(buf, sizeof buf, "GPU: drawable %dx%d placed at [%.4g %.4g %.4g %.4g %.4g %.4g] of the screen", w, h,
                  next.unit[0], next.unit[1], next.unit[2], next.unit[3], next.unit[4], next.unit[5]);
    p.log(LogLevel::Info, buf);
    st.platform->layerGeometry(layer, next.unit);
}

// ------------------------------------------------------------ registration ---

static void chain(Process& p, const std::string& symbol, void (*host)(Ctx&)) {
    auto soft = p.hostFunctionFor(symbol);
    p.defineFunction(symbol, [soft, host](Ctx& x) {
        if (!soft || hgUseHost(x)) host(x);
        else soft(x.cpu);
    });
}

void registerFunctions(Process& p) {
    ready(p);
    // glGetString stays on the software path: the strings describe the iOS GPU
    // the app expects (vendor, renderer, extensions), not the host's.
// Each entry point keeps the software implementation it replaces, for contexts
// that couldn't get a host context (e.g. an API the driver lacks).
#define HOSTGL_REG(name) chain(p, "_" #name, [](Ctx& x) { callWrapped<&hg_##name>(x); });
#define HOSTGL_REGISTER
#include "runtime/gl/HostGLTable.inc"
#undef HOSTGL_REGISTER
#undef HOSTGL_REG
    // ES 1.1 fixed-point entry points.
    chain(p, "_glAlphaFuncx", [](Ctx& x) { callWrapped<&hgx_alphaFuncx>(x); });
    chain(p, "_glClearColorx", [](Ctx& x) { callWrapped<&hgx_clearColorx>(x); });
    chain(p, "_glClearDepthx", [](Ctx& x) { callWrapped<&hgx_clearDepthx>(x); });
    chain(p, "_glClipPlanex", [](Ctx& x) { callWrapped<&hgx_clipPlanex>(x); });
    chain(p, "_glColor4x", [](Ctx& x) { callWrapped<&hgx_color4x>(x); });
    chain(p, "_glDepthRangex", [](Ctx& x) { callWrapped<&hgx_depthRangex>(x); });
    chain(p, "_glFogx", [](Ctx& x) { callWrapped<&hgx_fogx>(x); });
    chain(p, "_glFogxv", [](Ctx& x) { callWrapped<&hgx_fogxv>(x); });
    chain(p, "_glFrustumx", [](Ctx& x) { callWrapped<&hgx_frustumx>(x); });
    chain(p, "_glGetFixedv", [](Ctx& x) { callWrapped<&hgx_getFixedv>(x); });
    chain(p, "_glLightModelx", [](Ctx& x) { callWrapped<&hgx_lightModelx>(x); });
    chain(p, "_glLightModelxv", [](Ctx& x) { callWrapped<&hgx_lightModelxv>(x); });
    chain(p, "_glLightx", [](Ctx& x) { callWrapped<&hgx_lightx>(x); });
    chain(p, "_glLightxv", [](Ctx& x) { callWrapped<&hgx_lightxv>(x); });
    chain(p, "_glLineWidthx", [](Ctx& x) { callWrapped<&hgx_lineWidthx>(x); });
    chain(p, "_glLoadMatrixx", [](Ctx& x) { callWrapped<&hgx_loadMatrixx>(x); });
    chain(p, "_glMaterialx", [](Ctx& x) { callWrapped<&hgx_materialx>(x); });
    chain(p, "_glMaterialxv", [](Ctx& x) { callWrapped<&hgx_materialxv>(x); });
    chain(p, "_glMultMatrixx", [](Ctx& x) { callWrapped<&hgx_multMatrixx>(x); });
    chain(p, "_glMultiTexCoord4x", [](Ctx& x) { callWrapped<&hgx_multiTexCoord4x>(x); });
    chain(p, "_glNormal3x", [](Ctx& x) { callWrapped<&hgx_normal3x>(x); });
    chain(p, "_glOrthox", [](Ctx& x) { callWrapped<&hgx_orthox>(x); });
    chain(p, "_glPointParameterx", [](Ctx& x) { callWrapped<&hgx_pointParameterx>(x); });
    chain(p, "_glPointParameterxv", [](Ctx& x) { callWrapped<&hgx_pointParameterxv>(x); });
    chain(p, "_glPointSizex", [](Ctx& x) { callWrapped<&hgx_pointSizex>(x); });
    chain(p, "_glPolygonOffsetx", [](Ctx& x) { callWrapped<&hgx_polygonOffsetx>(x); });
    chain(p, "_glRotatex", [](Ctx& x) { callWrapped<&hgx_rotatex>(x); });
    chain(p, "_glSampleCoveragex", [](Ctx& x) { callWrapped<&hgx_sampleCoveragex>(x); });
    chain(p, "_glScalex", [](Ctx& x) { callWrapped<&hgx_scalex>(x); });
    chain(p, "_glTexEnvx", [](Ctx& x) { callWrapped<&hgx_texEnvx>(x); });
    chain(p, "_glTexEnvxv", [](Ctx& x) { callWrapped<&hgx_texEnvxv>(x); });
    chain(p, "_glTexParameterx", [](Ctx& x) { callWrapped<&hgx_texParameterx>(x); });
    chain(p, "_glTexParameterxv", [](Ctx& x) { callWrapped<&hgx_texParameterxv>(x); });
    chain(p, "_glTranslatex", [](Ctx& x) { callWrapped<&hgx_translatex>(x); });
    p.log(LogLevel::Info, std::string("GPU: OpenGL ES calls go to the host driver (") + S(p).platform->name() + ")");
}

} // namespace Hydrogen::Runtime::HostGL
