// HydrogenVM — Android GLSurfaceView, EGL10/GL10, and GLES20 Java bindings forwarding to HostGL.
#include "runtime/android/Jni.hpp"
#include "runtime/android/Surface.hpp"
#include "runtime/Heap.hpp"
#include <algorithm>
#include <cstring>
#include <deque>
#include <mutex>
#include <string>
#include <vector>

namespace Hydrogen::Runtime::Android {
namespace {

struct GlViewState {
    explicit GlViewState(Process&) {}
    std::mutex mu;
    jobject view = 0;
    jobject renderer = 0;
    std::deque<jobject> events;
};

// Temporary guest-heap allocation for passing Java arrays / non-direct buffers / strings into glFn.
struct GuestScratch {
    Process& proc;
    uint32_t addr = 0;
    GuestScratch(Process& p, uint32_t bytes) : proc(p), addr(p.heap().allocZeroed(std::max<uint32_t>(bytes, 16))) {}
    ~GuestScratch() { if (addr) proc.heap().free(addr); }
    GuestScratch(const GuestScratch&) = delete;
    GuestScratch& operator=(const GuestScratch&) = delete;
};

// Resolve a java.nio.Buffer (direct or array-backed) to a guest pointer. If the buffer is
// non-direct, stages its remaining bytes into `scratch` (caller must copy back if it's an out-param).
struct BufferSpan {
    uint32_t addr = 0;
    uint32_t bytes = 0;
    uint32_t elemSize = 1;
    uint32_t byteOffset = 0;
    jobject backingArray = 0;
    bool direct = false;
    std::unique_ptr<GuestScratch> scratch;
};

BufferSpan resolveBuffer(Jni& j, Ctx& x, jobject bufHandle, bool copyIn) {
    BufferSpan out;
    if (!bufHandle) return out;
    std::lock_guard<std::recursive_mutex> lk(j.mu);
    JObject* o = j.get(bufHandle);
    if (!o) return out;
    if (o->fields.count("hvm.address")) {
        const uint32_t base = uint32_t(o->fields["hvm.address"]);
        const uint32_t cap = uint32_t(o->fields["hvm.capacity"]);
        const uint32_t pos = uint32_t(o->fields["hvm.position"]);
        const uint32_t lim = o->fields.count("hvm.limit") ? uint32_t(o->fields["hvm.limit"]) : cap;
        const uint32_t esz = o->fields.count("hvm.elemSize") ? uint32_t(o->fields["hvm.elemSize"]) : 1u;
        out.direct = true;
        out.elemSize = esz;
        out.addr = base + pos * esz;
        out.bytes = (lim > pos ? lim - pos : 0) * esz;
        return out;
    }
    // Fallback for non-direct buffers (ByteBuffer.wrap / allocate): copy bytes via host scratch.
    // JavaUi stores NioBuf in o->host; if it's not a direct buffer, check if it's a raw byte[].
    if (o->elem) {
        out.backingArray = bufHandle;
        out.bytes = uint32_t(o->data.size());
        out.scratch = std::make_unique<GuestScratch>(x.proc, out.bytes);
        out.addr = out.scratch->addr;
        if (copyIn && out.bytes) std::memcpy(x.mem.host(out.addr), o->data.data(), out.bytes);
    }
    return out;
}

jobject singleton(Jni& j, const std::string& cls) {
    const std::string key = "hvm.singleton." + cls;
    std::lock_guard<std::recursive_mutex> lk(j.mu);
    if (auto it = j.staticFields.find(key); it != j.staticFields.end()) return jobject(it->second);
    const jobject h = j.newObject(cls);
    if (JObject* o = j.get(h)) o->permanent = true;
    j.staticFields[key] = h;
    return h;
}

uint32_t glCall(Ctx& x, const char* name, std::initializer_list<uint32_t> args) {
    return uint32_t(x.proc.callGuest(x.cpu, glFn(x.proc, name), args));
}

// Helper for glGenBuffers / glGenTextures / glDeleteBuffers / glDeleteTextures / glGenFramebuffers...
void glIntArrayOut(Jni& j, Ctx& x, const char* fn, int32_t n, jobject arrHandle, int32_t offset) {
    if (n <= 0 || offset < 0) return;
    GuestScratch s(x.proc, uint32_t(n) * 4u);
    glCall(x, fn, {uint32_t(n), s.addr});
    std::lock_guard<std::recursive_mutex> lk(j.mu);
    if (JObject* arr = j.get(arrHandle)) {
        const size_t byteOff = size_t(offset) * 4;
        const size_t bytes = size_t(n) * 4;
        if (byteOff + bytes <= arr->data.size())
            std::memcpy(arr->data.data() + byteOff, x.mem.host(s.addr), bytes);
    }
}

void glIntArrayIn(Jni& j, Ctx& x, const char* fn, int32_t n, jobject arrHandle, int32_t offset) {
    if (n <= 0 || offset < 0) return;
    GuestScratch s(x.proc, uint32_t(n) * 4u);
    {
        std::lock_guard<std::recursive_mutex> lk(j.mu);
        if (JObject* arr = j.get(arrHandle)) {
            const size_t byteOff = size_t(offset) * 4;
            const size_t bytes = size_t(n) * 4;
            if (byteOff + bytes <= arr->data.size())
                std::memcpy(x.mem.host(s.addr), arr->data.data() + byteOff, bytes);
        }
    }
    glCall(x, fn, {uint32_t(n), s.addr});
}

} // namespace

jobject glSurfaceViewRenderer(Process& p) {
    auto& st = p.service<GlViewState>();
    std::lock_guard<std::mutex> lk(st.mu);
    return st.renderer;
}

void drainGlSurfaceViewEvents(Jni& j, Ctx& x) {
    auto& st = x.proc.service<GlViewState>();
    std::deque<jobject> batch;
    {
        std::lock_guard<std::mutex> lk(st.mu);
        batch.swap(st.events);
    }
    const uint32_t runId = j.methodId("java/lang/Runnable", "run", "()V", false);
    for (jobject r : batch) {
        if (r) j.invoke(x, runId, r, {});
    }
}

void registerJavaGles(Jni& j) {
    // android.opengl.GLSurfaceView
    const std::string kGsv = "android/opengl/GLSurfaceView";
    j.method(kGsv, "<init>", "(Landroid/content/Context;)V", [](Jni& j, Ctx& x, jobject self, const auto&) {
        auto& st = x.proc.service<GlViewState>();
        std::lock_guard<std::mutex> lk(st.mu);
        st.view = self;
        if (JObject* o = j.get(self)) o->permanent = true;
        return JValue(0);
    });
    j.method(kGsv, "setRenderer", "(Landroid/opengl/GLSurfaceView$Renderer;)V", [](Jni& j, Ctx& x, jobject self, const auto& a) {
        auto& st = x.proc.service<GlViewState>();
        std::lock_guard<std::mutex> lk(st.mu);
        st.view = self;
        st.renderer = jobject(a[0]);
        if (JObject* o = j.get(st.renderer)) o->permanent = true;
        return JValue(0);
    });
    j.method(kGsv, "queueEvent", "(Ljava/lang/Runnable;)V", [](Jni& j, Ctx& x, jobject, const auto& a) {
        const jobject r = jobject(a[0]);
        if (!r) return JValue(0);
        if (JObject* o = j.get(r)) o->permanent = true;
        auto& st = x.proc.service<GlViewState>();
        std::lock_guard<std::mutex> lk(st.mu);
        st.events.push_back(r);
        return JValue(0);
    });
    j.method(kGsv, "setEGLConfigChooser", "(Landroid/opengl/GLSurfaceView$EGLConfigChooser;)V", [](Jni&, Ctx&, jobject, const auto&) { return JValue(0); });
    j.method(kGsv, "setEGLConfigChooser", "(IIIIII)V", [](Jni&, Ctx&, jobject, const auto&) { return JValue(0); });
    j.method(kGsv, "setEGLContextFactory", "(Landroid/opengl/GLSurfaceView$EGLContextFactory;)V", [](Jni&, Ctx&, jobject, const auto&) { return JValue(0); });
    j.method(kGsv, "setEGLContextClientVersion", "(I)V", [](Jni&, Ctx&, jobject, const auto&) { return JValue(0); });
    j.method(kGsv, "setPreserveEGLContextOnPause", "(Z)V", [](Jni&, Ctx&, jobject, const auto&) { return JValue(0); });
    j.method(kGsv, "setRenderMode", "(I)V", [](Jni&, Ctx&, jobject, const auto&) { return JValue(0); });
    j.method(kGsv, "requestRender", "()V", [](Jni&, Ctx&, jobject, const auto&) { return JValue(0); });
    j.method(kGsv, "onPause", "()V", [](Jni&, Ctx&, jobject, const auto&) { return JValue(0); });
    j.method(kGsv, "onResume", "()V", [](Jni&, Ctx&, jobject, const auto&) { return JValue(0); });
    j.method(kGsv, "onDetachedFromWindow", "()V", [](Jni&, Ctx&, jobject, const auto&) { return JValue(0); });

    // javax.microedition.khronos.egl.EGLContext / EGL10 / GL10
    const std::string kEgl = "javax/microedition/khronos/egl/EGL10";
    j.method("javax/microedition/khronos/egl/EGLContext", "getEGL", "()Ljavax/microedition/khronos/egl/EGL;",
             [kEgl](Jni& j, Ctx&, jobject, const auto&) { return JValue(singleton(j, kEgl)); });
    j.staticFields[kEgl + ".EGL_DEFAULT_DISPLAY"] = singleton(j, "java/lang/Object");
    j.staticFields[kEgl + ".EGL_NO_CONTEXT"] = singleton(j, "javax/microedition/khronos/egl/EGLContext");
    j.staticFields[kEgl + ".EGL_NO_DISPLAY"] = singleton(j, "javax/microedition/khronos/egl/EGLDisplay");
    j.staticFields[kEgl + ".EGL_NO_SURFACE"] = singleton(j, "javax/microedition/khronos/egl/EGLSurface");
    j.method(kEgl, "eglGetDisplay", "(Ljava/lang/Object;)Ljavax/microedition/khronos/egl/EGLDisplay;",
             [](Jni& j, Ctx&, jobject, const auto&) { return JValue(singleton(j, "javax/microedition/khronos/egl/EGLDisplay")); });
    j.method(kEgl, "eglInitialize", "(Ljavax/microedition/khronos/egl/EGLDisplay;[I)Z",
             [](Jni& j, Ctx&, jobject, const auto& a) {
                 std::lock_guard<std::recursive_mutex> lk(j.mu);
                 if (JObject* ver = j.get(jobject(a[1])); ver && ver->data.size() >= 8) {
                     const int32_t maj = 1, min = 4;
                     std::memcpy(ver->data.data(), &maj, 4);
                     std::memcpy(ver->data.data() + 4, &min, 4);
                 }
                 return JValue(1);
             });
    j.method(kEgl, "eglTerminate", "(Ljavax/microedition/khronos/egl/EGLDisplay;)Z", [](Jni&, Ctx&, jobject, const auto&) { return JValue(1); });
    j.method(kEgl, "eglGetError", "()I", [](Jni&, Ctx&, jobject, const auto&) { return JValue(0x3000); });
    j.method(kEgl, "eglGetCurrentContext", "()Ljavax/microedition/khronos/egl/EGLContext;",
             [](Jni& j, Ctx&, jobject, const auto&) { return JValue(singleton(j, "javax/microedition/khronos/egl/EGLContext")); });
    j.method(kEgl, "eglCreateContext",
             "(Ljavax/microedition/khronos/egl/EGLDisplay;Ljavax/microedition/khronos/egl/EGLConfig;Ljavax/microedition/khronos/egl/EGLContext;[I)Ljavax/microedition/khronos/egl/EGLContext;",
             [](Jni& j, Ctx&, jobject, const auto&) { return JValue(singleton(j, "javax/microedition/khronos/egl/EGLContext")); });
    j.method(kEgl, "eglDestroyContext", "(Ljavax/microedition/khronos/egl/EGLDisplay;Ljavax/microedition/khronos/egl/EGLContext;)Z",
             [](Jni&, Ctx&, jobject, const auto&) { return JValue(1); });
    j.method(kEgl, "eglChooseConfig",
             "(Ljavax/microedition/khronos/egl/EGLDisplay;[I[Ljavax/microedition/khronos/egl/EGLConfig;I[I)Z",
             [](Jni& j, Ctx&, jobject, const auto& a) {
                 const jobject cfgs = jobject(a[2]);
                 const int32_t cap = int32_t(a[3]);
                 const jobject num = jobject(a[4]);
                 std::lock_guard<std::recursive_mutex> lk(j.mu);
                 if (cfgs && cap > 0) {
                     if (JObject* arr = j.get(cfgs); arr && !arr->refs.empty())
                         arr->refs[0] = singleton(j, "javax/microedition/khronos/egl/EGLConfig");
                 }
                 if (JObject* n = j.get(num); n && n->data.size() >= 4) {
                     const int32_t one = 1;
                     std::memcpy(n->data.data(), &one, 4);
                 }
                 return JValue(1);
             });
    j.method(kEgl, "eglGetConfigAttrib",
             "(Ljavax/microedition/khronos/egl/EGLDisplay;Ljavax/microedition/khronos/egl/EGLConfig;I[I)Z",
             [](Jni& j, Ctx&, jobject, const auto& a) {
                 int32_t val = 0;
                 switch (uint32_t(a[2])) {
                     case 0x3024: case 0x3023: case 0x3022: case 0x3021: val = 8; break; // RGBA8888
                     case 0x3025: val = 24; break;                                        // DEPTH_SIZE
                     case 0x3026: val = 8; break;                                         // STENCIL_SIZE
                     case 0x3040: val = 4; break;                                         // RENDERABLE_TYPE: ES2
                     case 0x3033: val = 4; break;                                         // SURFACE_TYPE: WINDOW
                     default: val = 0; break;
                 }
                 std::lock_guard<std::recursive_mutex> lk(j.mu);
                 if (JObject* out = j.get(jobject(a[3])); out && out->data.size() >= 4)
                     std::memcpy(out->data.data(), &val, 4);
                 return JValue(1);
             });

    // javax.microedition.khronos.opengles.GL10
    const std::string kGl10 = "javax/microedition/khronos/opengles/GL10";
    j.method(kGl10, "glGetString", "(I)Ljava/lang/String;", [](Jni& j, Ctx& x, jobject, const auto& a) {
        const uint32_t ptr = glCall(x, "glGetString", {uint32_t(a[0])});
        return JValue(j.newString(ptr ? x.proc.readCString(ptr, 4096) : ""));
    });
    j.method(kGl10, "glViewport", "(IIII)V", [](Jni&, Ctx& x, jobject, const auto& a) {
        glCall(x, "glViewport", {uint32_t(a[0]), uint32_t(a[1]), uint32_t(a[2]), uint32_t(a[3])});
        return JValue(0);
    });

    // android.opengl.GLES20 — forwards to HydrogenVM's GLES2 / HostGL functions.
    const std::string kGl = "android/opengl/GLES20";

    j.method(kGl, "glActiveTexture", "(I)V", [](Jni&, Ctx& x, jobject, const auto& a) {
        glCall(x, "glActiveTexture", {uint32_t(a[0])}); return JValue(0);
    });
    j.method(kGl, "glAttachShader", "(II)V", [](Jni&, Ctx& x, jobject, const auto& a) {
        glCall(x, "glAttachShader", {uint32_t(a[0]), uint32_t(a[1])}); return JValue(0);
    });
    j.method(kGl, "glBindAttribLocation", "(IILjava/lang/String;)V", [](Jni& j, Ctx& x, jobject, const auto& a) {
        const std::string name = j.string(jobject(a[2]));
        GuestScratch s(x.proc, uint32_t(name.size() + 1));
        std::memcpy(x.mem.host(s.addr), name.c_str(), name.size() + 1);
        glCall(x, "glBindAttribLocation", {uint32_t(a[0]), uint32_t(a[1]), s.addr});
        return JValue(0);
    });
    j.method(kGl, "glBindBuffer", "(II)V", [](Jni&, Ctx& x, jobject, const auto& a) {
        glCall(x, "glBindBuffer", {uint32_t(a[0]), uint32_t(a[1])}); return JValue(0);
    });
    j.method(kGl, "glBindFramebuffer", "(II)V", [](Jni&, Ctx& x, jobject, const auto& a) {
        uint32_t fb = uint32_t(a[1]);
        if (fb == 0 && currentSurface() && currentSurface()->fbo) fb = currentSurface()->fbo;
        glCall(x, "glBindFramebuffer", {uint32_t(a[0]), fb});
        return JValue(0);
    });
    j.method(kGl, "glBindRenderbuffer", "(II)V", [](Jni&, Ctx& x, jobject, const auto& a) {
        glCall(x, "glBindRenderbuffer", {uint32_t(a[0]), uint32_t(a[1])}); return JValue(0);
    });
    j.method(kGl, "glBindTexture", "(II)V", [](Jni&, Ctx& x, jobject, const auto& a) {
        glCall(x, "glBindTexture", {uint32_t(a[0]), uint32_t(a[1])}); return JValue(0);
    });
    j.method(kGl, "glBlendColor", "(FFFF)V", [](Jni&, Ctx& x, jobject, const auto& a) {
        glCall(x, "glBlendColor", {uint32_t(a[0]), uint32_t(a[1]), uint32_t(a[2]), uint32_t(a[3])}); return JValue(0);
    });
    j.method(kGl, "glBlendEquation", "(I)V", [](Jni&, Ctx& x, jobject, const auto& a) {
        glCall(x, "glBlendEquation", {uint32_t(a[0])}); return JValue(0);
    });
    j.method(kGl, "glBlendEquationSeparate", "(II)V", [](Jni&, Ctx& x, jobject, const auto& a) {
        glCall(x, "glBlendEquationSeparate", {uint32_t(a[0]), uint32_t(a[1])}); return JValue(0);
    });
    j.method(kGl, "glBlendFunc", "(II)V", [](Jni&, Ctx& x, jobject, const auto& a) {
        glCall(x, "glBlendFunc", {uint32_t(a[0]), uint32_t(a[1])}); return JValue(0);
    });
    j.method(kGl, "glBlendFuncSeparate", "(IIII)V", [](Jni&, Ctx& x, jobject, const auto& a) {
        glCall(x, "glBlendFuncSeparate", {uint32_t(a[0]), uint32_t(a[1]), uint32_t(a[2]), uint32_t(a[3])}); return JValue(0);
    });
    j.method(kGl, "glBufferData", "(IILjava/nio/Buffer;I)V", [](Jni& j, Ctx& x, jobject, const auto& a) {
        const auto buf = resolveBuffer(j, x, jobject(a[2]), true);
        glCall(x, "glBufferData", {uint32_t(a[0]), uint32_t(a[1]), buf.addr, uint32_t(a[3])});
        return JValue(0);
    });
    j.method(kGl, "glBufferSubData", "(IIILjava/nio/Buffer;)V", [](Jni& j, Ctx& x, jobject, const auto& a) {
        const auto buf = resolveBuffer(j, x, jobject(a[3]), true);
        glCall(x, "glBufferSubData", {uint32_t(a[0]), uint32_t(a[1]), uint32_t(a[2]), buf.addr});
        return JValue(0);
    });
    j.method(kGl, "glCheckFramebufferStatus", "(I)I", [](Jni&, Ctx& x, jobject, const auto& a) {
        return JValue(glCall(x, "glCheckFramebufferStatus", {uint32_t(a[0])}));
    });
    j.method(kGl, "glClear", "(I)V", [](Jni&, Ctx& x, jobject, const auto& a) {
        glCall(x, "glClear", {uint32_t(a[0])}); return JValue(0);
    });
    j.method(kGl, "glClearColor", "(FFFF)V", [](Jni&, Ctx& x, jobject, const auto& a) {
        glCall(x, "glClearColor", {uint32_t(a[0]), uint32_t(a[1]), uint32_t(a[2]), uint32_t(a[3])}); return JValue(0);
    });
    j.method(kGl, "glClearDepthf", "(F)V", [](Jni&, Ctx& x, jobject, const auto& a) {
        glCall(x, "glClearDepthf", {uint32_t(a[0])}); return JValue(0);
    });
    j.method(kGl, "glClearStencil", "(I)V", [](Jni&, Ctx& x, jobject, const auto& a) {
        glCall(x, "glClearStencil", {uint32_t(a[0])}); return JValue(0);
    });
    j.method(kGl, "glColorMask", "(ZZZZ)V", [](Jni&, Ctx& x, jobject, const auto& a) {
        glCall(x, "glColorMask", {uint32_t(a[0]) & 1, uint32_t(a[1]) & 1, uint32_t(a[2]) & 1, uint32_t(a[3]) & 1}); return JValue(0);
    });
    j.method(kGl, "glCompileShader", "(I)V", [](Jni&, Ctx& x, jobject, const auto& a) {
        glCall(x, "glCompileShader", {uint32_t(a[0])}); return JValue(0);
    });
    j.method(kGl, "glCompressedTexImage2D", "(IIIIIIILjava/nio/Buffer;)V", [](Jni& j, Ctx& x, jobject, const auto& a) {
        const auto buf = resolveBuffer(j, x, jobject(a[7]), true);
        glCall(x, "glCompressedTexImage2D", {uint32_t(a[0]), uint32_t(a[1]), uint32_t(a[2]), uint32_t(a[3]),
                                             uint32_t(a[4]), uint32_t(a[5]), uint32_t(a[6]), buf.addr});
        return JValue(0);
    });
    j.method(kGl, "glCreateProgram", "()I", [](Jni&, Ctx& x, jobject, const auto&) {
        return JValue(glCall(x, "glCreateProgram", {}));
    });
    j.method(kGl, "glCreateShader", "(I)I", [](Jni&, Ctx& x, jobject, const auto& a) {
        return JValue(glCall(x, "glCreateShader", {uint32_t(a[0])}));
    });
    j.method(kGl, "glCullFace", "(I)V", [](Jni&, Ctx& x, jobject, const auto& a) {
        glCall(x, "glCullFace", {uint32_t(a[0])}); return JValue(0);
    });
    j.method(kGl, "glDeleteBuffers", "(I[II)V", [](Jni& j, Ctx& x, jobject, const auto& a) {
        glIntArrayIn(j, x, "glDeleteBuffers", int32_t(a[0]), jobject(a[1]), int32_t(a[2])); return JValue(0);
    });
    j.method(kGl, "glDeleteFramebuffers", "(I[II)V", [](Jni& j, Ctx& x, jobject, const auto& a) {
        glIntArrayIn(j, x, "glDeleteFramebuffers", int32_t(a[0]), jobject(a[1]), int32_t(a[2])); return JValue(0);
    });
    j.method(kGl, "glDeleteProgram", "(I)V", [](Jni&, Ctx& x, jobject, const auto& a) {
        glCall(x, "glDeleteProgram", {uint32_t(a[0])}); return JValue(0);
    });
    j.method(kGl, "glDeleteRenderbuffers", "(I[II)V", [](Jni& j, Ctx& x, jobject, const auto& a) {
        glIntArrayIn(j, x, "glDeleteRenderbuffers", int32_t(a[0]), jobject(a[1]), int32_t(a[2])); return JValue(0);
    });
    j.method(kGl, "glDeleteShader", "(I)V", [](Jni&, Ctx& x, jobject, const auto& a) {
        glCall(x, "glDeleteShader", {uint32_t(a[0])}); return JValue(0);
    });
    j.method(kGl, "glDeleteTextures", "(I[II)V", [](Jni& j, Ctx& x, jobject, const auto& a) {
        glIntArrayIn(j, x, "glDeleteTextures", int32_t(a[0]), jobject(a[1]), int32_t(a[2])); return JValue(0);
    });
    j.method(kGl, "glDepthFunc", "(I)V", [](Jni&, Ctx& x, jobject, const auto& a) {
        glCall(x, "glDepthFunc", {uint32_t(a[0])}); return JValue(0);
    });
    j.method(kGl, "glDepthMask", "(Z)V", [](Jni&, Ctx& x, jobject, const auto& a) {
        glCall(x, "glDepthMask", {uint32_t(a[0]) & 1}); return JValue(0);
    });
    j.method(kGl, "glDisable", "(I)V", [](Jni&, Ctx& x, jobject, const auto& a) {
        glCall(x, "glDisable", {uint32_t(a[0])}); return JValue(0);
    });
    j.method(kGl, "glDisableVertexAttribArray", "(I)V", [](Jni&, Ctx& x, jobject, const auto& a) {
        glCall(x, "glDisableVertexAttribArray", {uint32_t(a[0])}); return JValue(0);
    });
    j.method(kGl, "glDrawArrays", "(III)V", [](Jni&, Ctx& x, jobject, const auto& a) {
        glCall(x, "glDrawArrays", {uint32_t(a[0]), uint32_t(a[1]), uint32_t(a[2])}); return JValue(0);
    });
    j.method(kGl, "glDrawElements", "(IIII)V", [](Jni&, Ctx& x, jobject, const auto& a) {
        glCall(x, "glDrawElements", {uint32_t(a[0]), uint32_t(a[1]), uint32_t(a[2]), uint32_t(a[3])}); return JValue(0);
    });
    j.method(kGl, "glDrawElements", "(IIILjava/nio/Buffer;)V", [](Jni& j, Ctx& x, jobject, const auto& a) {
        const auto buf = resolveBuffer(j, x, jobject(a[3]), true);
        glCall(x, "glDrawElements", {uint32_t(a[0]), uint32_t(a[1]), uint32_t(a[2]), buf.addr});
        return JValue(0);
    });
    j.method(kGl, "glEnable", "(I)V", [](Jni&, Ctx& x, jobject, const auto& a) {
        glCall(x, "glEnable", {uint32_t(a[0])}); return JValue(0);
    });
    j.method(kGl, "glEnableVertexAttribArray", "(I)V", [](Jni&, Ctx& x, jobject, const auto& a) {
        glCall(x, "glEnableVertexAttribArray", {uint32_t(a[0])}); return JValue(0);
    });
    j.method(kGl, "glFinish", "()V", [](Jni&, Ctx& x, jobject, const auto&) {
        glCall(x, "glFinish", {}); return JValue(0);
    });
    j.method(kGl, "glFlush", "()V", [](Jni&, Ctx& x, jobject, const auto&) {
        glCall(x, "glFlush", {}); return JValue(0);
    });
    j.method(kGl, "glFramebufferRenderbuffer", "(IIII)V", [](Jni&, Ctx& x, jobject, const auto& a) {
        glCall(x, "glFramebufferRenderbuffer", {uint32_t(a[0]), uint32_t(a[1]), uint32_t(a[2]), uint32_t(a[3])}); return JValue(0);
    });
    j.method(kGl, "glFramebufferTexture2D", "(IIIII)V", [](Jni&, Ctx& x, jobject, const auto& a) {
        glCall(x, "glFramebufferTexture2D", {uint32_t(a[0]), uint32_t(a[1]), uint32_t(a[2]), uint32_t(a[3]), uint32_t(a[4])}); return JValue(0);
    });
    j.method(kGl, "glFrontFace", "(I)V", [](Jni&, Ctx& x, jobject, const auto& a) {
        glCall(x, "glFrontFace", {uint32_t(a[0])}); return JValue(0);
    });
    j.method(kGl, "glGenBuffers", "(I[II)V", [](Jni& j, Ctx& x, jobject, const auto& a) {
        glIntArrayOut(j, x, "glGenBuffers", int32_t(a[0]), jobject(a[1]), int32_t(a[2])); return JValue(0);
    });
    j.method(kGl, "glGenFramebuffers", "(I[II)V", [](Jni& j, Ctx& x, jobject, const auto& a) {
        glIntArrayOut(j, x, "glGenFramebuffers", int32_t(a[0]), jobject(a[1]), int32_t(a[2])); return JValue(0);
    });
    j.method(kGl, "glGenRenderbuffers", "(I[II)V", [](Jni& j, Ctx& x, jobject, const auto& a) {
        glIntArrayOut(j, x, "glGenRenderbuffers", int32_t(a[0]), jobject(a[1]), int32_t(a[2])); return JValue(0);
    });
    j.method(kGl, "glGenTextures", "(I[II)V", [](Jni& j, Ctx& x, jobject, const auto& a) {
        glIntArrayOut(j, x, "glGenTextures", int32_t(a[0]), jobject(a[1]), int32_t(a[2])); return JValue(0);
    });
    j.method(kGl, "glGenerateMipmap", "(I)V", [](Jni&, Ctx& x, jobject, const auto& a) {
        glCall(x, "glGenerateMipmap", {uint32_t(a[0])}); return JValue(0);
    });
    auto getActive = [](Jni& j, Ctx& x, const char* fn, const std::vector<JValue>& a) {
        const uint32_t prog = uint32_t(a[0]), idx = uint32_t(a[1]);
        const int32_t bufsize = std::max<int32_t>(1, int32_t(a[2]));
        GuestScratch s(x.proc, uint32_t(16 + bufsize));
        glCall(x, fn, {prog, idx, uint32_t(bufsize), s.addr, s.addr + 4, s.addr + 8, s.addr + 16});
        const int32_t len = x.mem.read<int32_t>(s.addr);
        const int32_t size = x.mem.read<int32_t>(s.addr + 4);
        const int32_t type = x.mem.read<int32_t>(s.addr + 8);
        std::lock_guard<std::recursive_mutex> lk(j.mu);
        auto writeInt = [&](jobject arrH, int32_t off, int32_t val) {
            if (JObject* arr = j.get(arrH); arr && off >= 0 && size_t(off) * 4 + 4 <= arr->data.size())
                std::memcpy(arr->data.data() + size_t(off) * 4, &val, 4);
        };
        writeInt(jobject(a[3]), int32_t(a[4]), len);
        writeInt(jobject(a[5]), int32_t(a[6]), size);
        writeInt(jobject(a[7]), int32_t(a[8]), type);
        if (JObject* nameArr = j.get(jobject(a[9]))) {
            const int32_t off = int32_t(a[10]);
            const int32_t copyLen = std::min<int32_t>(bufsize, std::max<int32_t>(0, int32_t(nameArr->data.size()) - off));
            if (off >= 0 && copyLen > 0)
                std::memcpy(nameArr->data.data() + off, x.mem.host(s.addr + 16), size_t(copyLen));
        }
        return JValue(0);
    };
    j.method(kGl, "glGetActiveAttrib", "(III[II[II[II[BI)V", [getActive](Jni& j, Ctx& x, jobject, const auto& a) {
        return getActive(j, x, "glGetActiveAttrib", a);
    });
    j.method(kGl, "glGetActiveUniform", "(III[II[II[II[BI)V", [getActive](Jni& j, Ctx& x, jobject, const auto& a) {
        return getActive(j, x, "glGetActiveUniform", a);
    });
    j.method(kGl, "glGetAttribLocation", "(ILjava/lang/String;)I", [](Jni& j, Ctx& x, jobject, const auto& a) {
        const std::string name = j.string(jobject(a[1]));
        GuestScratch s(x.proc, uint32_t(name.size() + 1));
        std::memcpy(x.mem.host(s.addr), name.c_str(), name.size() + 1);
        return JValue(glCall(x, "glGetAttribLocation", {uint32_t(a[0]), s.addr}));
    });
    j.method(kGl, "glGetError", "()I", [](Jni&, Ctx& x, jobject, const auto&) {
        return JValue(glCall(x, "glGetError", {}));
    });
    j.method(kGl, "glGetFloatv", "(ILjava/nio/FloatBuffer;)V", [](Jni& j, Ctx& x, jobject, const auto& a) {
        const auto buf = resolveBuffer(j, x, jobject(a[1]), false);
        if (buf.addr) glCall(x, "glGetFloatv", {uint32_t(a[0]), buf.addr});
        return JValue(0);
    });
    j.method(kGl, "glGetIntegerv", "(ILjava/nio/IntBuffer;)V", [](Jni& j, Ctx& x, jobject, const auto& a) {
        const auto buf = resolveBuffer(j, x, jobject(a[1]), false);
        if (buf.addr) {
            glCall(x, "glGetIntegerv", {uint32_t(a[0]), buf.addr});
            if (uint32_t(a[0]) == 0x8CA6 && currentSurface() && currentSurface()->fbo &&
                x.mem.read<uint32_t>(buf.addr) == currentSurface()->fbo)
                x.mem.write<uint32_t>(buf.addr, 0);
        }
        return JValue(0);
    });
    j.method(kGl, "glGetProgramInfoLog", "(I)Ljava/lang/String;", [](Jni& j, Ctx& x, jobject, const auto& a) {
        GuestScratch s(x.proc, 4100);
        glCall(x, "glGetProgramInfoLog", {uint32_t(a[0]), 4096, s.addr, s.addr + 4});
        return JValue(j.newString(x.proc.readCString(s.addr + 4, 4096)));
    });
    j.method(kGl, "glGetProgramiv", "(IILjava/nio/IntBuffer;)V", [](Jni& j, Ctx& x, jobject, const auto& a) {
        const auto buf = resolveBuffer(j, x, jobject(a[2]), false);
        if (buf.addr) glCall(x, "glGetProgramiv", {uint32_t(a[0]), uint32_t(a[1]), buf.addr});
        return JValue(0);
    });
    j.method(kGl, "glGetShaderInfoLog", "(I)Ljava/lang/String;", [](Jni& j, Ctx& x, jobject, const auto& a) {
        GuestScratch s(x.proc, 4100);
        glCall(x, "glGetShaderInfoLog", {uint32_t(a[0]), 4096, s.addr, s.addr + 4});
        return JValue(j.newString(x.proc.readCString(s.addr + 4, 4096)));
    });
    j.method(kGl, "glGetShaderiv", "(IILjava/nio/IntBuffer;)V", [](Jni& j, Ctx& x, jobject, const auto& a) {
        const auto buf = resolveBuffer(j, x, jobject(a[2]), false);
        if (buf.addr) glCall(x, "glGetShaderiv", {uint32_t(a[0]), uint32_t(a[1]), buf.addr});
        return JValue(0);
    });
    j.method(kGl, "glGetString", "(I)Ljava/lang/String;", [](Jni& j, Ctx& x, jobject, const auto& a) {
        const uint32_t ptr = glCall(x, "glGetString", {uint32_t(a[0])});
        return JValue(j.newString(ptr ? x.proc.readCString(ptr, 4096) : ""));
    });
    j.method(kGl, "glGetUniformLocation", "(ILjava/lang/String;)I", [](Jni& j, Ctx& x, jobject, const auto& a) {
        const std::string name = j.string(jobject(a[1]));
        GuestScratch s(x.proc, uint32_t(name.size() + 1));
        std::memcpy(x.mem.host(s.addr), name.c_str(), name.size() + 1);
        return JValue(glCall(x, "glGetUniformLocation", {uint32_t(a[0]), s.addr}));
    });
    j.method(kGl, "glLinkProgram", "(I)V", [](Jni&, Ctx& x, jobject, const auto& a) {
        glCall(x, "glLinkProgram", {uint32_t(a[0])}); return JValue(0);
    });
    j.method(kGl, "glPixelStorei", "(II)V", [](Jni&, Ctx& x, jobject, const auto& a) {
        glCall(x, "glPixelStorei", {uint32_t(a[0]), uint32_t(a[1])}); return JValue(0);
    });
    j.method(kGl, "glRenderbufferStorage", "(IIII)V", [](Jni&, Ctx& x, jobject, const auto& a) {
        glCall(x, "glRenderbufferStorage", {uint32_t(a[0]), uint32_t(a[1]), uint32_t(a[2]), uint32_t(a[3])}); return JValue(0);
    });
    j.method(kGl, "glScissor", "(IIII)V", [](Jni&, Ctx& x, jobject, const auto& a) {
        glCall(x, "glScissor", {uint32_t(a[0]), uint32_t(a[1]), uint32_t(a[2]), uint32_t(a[3])}); return JValue(0);
    });
    j.method(kGl, "glShaderSource", "(ILjava/lang/String;)V", [](Jni& j, Ctx& x, jobject, const auto& a) {
        const std::string src = j.string(jobject(a[1]));
        GuestScratch s(x.proc, uint32_t(8 + src.size() + 1));
        const uint32_t strAddr = s.addr + 8;
        std::memcpy(x.mem.host(strAddr), src.c_str(), src.size() + 1);
        x.mem.write<uint32_t>(s.addr, strAddr);
        x.mem.write<int32_t>(s.addr + 4, int32_t(src.size()));
        glCall(x, "glShaderSource", {uint32_t(a[0]), 1, s.addr, s.addr + 4});
        return JValue(0);
    });
    j.method(kGl, "glTexImage2D", "(IIIIIIIILjava/nio/Buffer;)V", [](Jni& j, Ctx& x, jobject, const auto& a) {
        const auto buf = resolveBuffer(j, x, jobject(a[8]), true);
        glCall(x, "glTexImage2D", {uint32_t(a[0]), uint32_t(a[1]), uint32_t(a[2]), uint32_t(a[3]),
                                   uint32_t(a[4]), uint32_t(a[5]), uint32_t(a[6]), uint32_t(a[7]), buf.addr});
        return JValue(0);
    });
    j.method(kGl, "glTexParameterf", "(IIF)V", [](Jni&, Ctx& x, jobject, const auto& a) {
        glCall(x, "glTexParameterf", {uint32_t(a[0]), uint32_t(a[1]), uint32_t(a[2])}); return JValue(0);
    });
    j.method(kGl, "glTexParameteri", "(III)V", [](Jni&, Ctx& x, jobject, const auto& a) {
        glCall(x, "glTexParameteri", {uint32_t(a[0]), uint32_t(a[1]), uint32_t(a[2])}); return JValue(0);
    });
    j.method(kGl, "glTexSubImage2D", "(IIIIIIIILjava/nio/Buffer;)V", [](Jni& j, Ctx& x, jobject, const auto& a) {
        const auto buf = resolveBuffer(j, x, jobject(a[8]), true);
        glCall(x, "glTexSubImage2D", {uint32_t(a[0]), uint32_t(a[1]), uint32_t(a[2]), uint32_t(a[3]),
                                      uint32_t(a[4]), uint32_t(a[5]), uint32_t(a[6]), uint32_t(a[7]), buf.addr});
        return JValue(0);
    });
    j.method(kGl, "glUniform1f", "(IF)V", [](Jni&, Ctx& x, jobject, const auto& a) {
        glCall(x, "glUniform1f", {uint32_t(a[0]), uint32_t(a[1])}); return JValue(0);
    });
    j.method(kGl, "glUniform1i", "(II)V", [](Jni&, Ctx& x, jobject, const auto& a) {
        glCall(x, "glUniform1i", {uint32_t(a[0]), uint32_t(a[1])}); return JValue(0);
    });
    j.method(kGl, "glUniform2f", "(IFF)V", [](Jni&, Ctx& x, jobject, const auto& a) {
        glCall(x, "glUniform2f", {uint32_t(a[0]), uint32_t(a[1]), uint32_t(a[2])}); return JValue(0);
    });
    j.method(kGl, "glUniform3f", "(IFFF)V", [](Jni&, Ctx& x, jobject, const auto& a) {
        glCall(x, "glUniform3f", {uint32_t(a[0]), uint32_t(a[1]), uint32_t(a[2]), uint32_t(a[3])}); return JValue(0);
    });
    j.method(kGl, "glUniform4f", "(IFFFF)V", [](Jni&, Ctx& x, jobject, const auto& a) {
        glCall(x, "glUniform4f", {uint32_t(a[0]), uint32_t(a[1]), uint32_t(a[2]), uint32_t(a[3]), uint32_t(a[4])}); return JValue(0);
    });
    j.method(kGl, "glUniformMatrix4fv", "(IIZ[FI)V", [](Jni& j, Ctx& x, jobject, const auto& a) {
        const int32_t count = int32_t(a[1]);
        const int32_t off = int32_t(a[4]);
        if (count <= 0 || off < 0) return JValue(0);
        const size_t bytes = size_t(count) * 16 * 4;
        GuestScratch s(x.proc, uint32_t(bytes));
        {
            std::lock_guard<std::recursive_mutex> lk(j.mu);
            if (JObject* arr = j.get(jobject(a[3])); arr && size_t(off) * 4 + bytes <= arr->data.size())
                std::memcpy(x.mem.host(s.addr), arr->data.data() + size_t(off) * 4, bytes);
        }
        glCall(x, "glUniformMatrix4fv", {uint32_t(a[0]), uint32_t(count), uint32_t(a[2]) & 1, s.addr});
        return JValue(0);
    });
    j.method(kGl, "glUseProgram", "(I)V", [](Jni&, Ctx& x, jobject, const auto& a) {
        glCall(x, "glUseProgram", {uint32_t(a[0])}); return JValue(0);
    });
    j.method(kGl, "glVertexAttribPointer", "(IIIZII)V", [](Jni&, Ctx& x, jobject, const auto& a) {
        glCall(x, "glVertexAttribPointer", {uint32_t(a[0]), uint32_t(a[1]), uint32_t(a[2]),
                                            uint32_t(a[3]) & 1, uint32_t(a[4]), uint32_t(a[5])});
        return JValue(0);
    });
    j.method(kGl, "glVertexAttribPointer", "(IIIZILjava/nio/Buffer;)V", [](Jni& j, Ctx& x, jobject, const auto& a) {
        const auto buf = resolveBuffer(j, x, jobject(a[5]), true);
        glCall(x, "glVertexAttribPointer", {uint32_t(a[0]), uint32_t(a[1]), uint32_t(a[2]),
                                            uint32_t(a[3]) & 1, uint32_t(a[4]), buf.addr});
        return JValue(0);
    });
    j.method(kGl, "glViewport", "(IIII)V", [](Jni&, Ctx& x, jobject, const auto& a) {
        glCall(x, "glViewport", {uint32_t(a[0]), uint32_t(a[1]), uint32_t(a[2]), uint32_t(a[3])}); return JValue(0);
    });
}

} // namespace Hydrogen::Runtime::Android
