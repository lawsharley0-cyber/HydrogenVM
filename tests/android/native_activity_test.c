/* HydrogenVM Android Test Lab — NativeActivity guest.
 *
 * Exports ANativeActivity_onCreate (no hvm_test_main) so HydrogenVM runs it through
 * NativeActivityDriver. Verifies:
 *   1. ANativeActivity struct fields & ANativeActivityCallbacks lifecycle
 *   2. ANativeWindow + EGL initialization + glClear + eglSwapBuffers
 *   3. AInputQueue + ALooper_pollAll receiving a touch event from --tap
 *
 * Built by tools/android_testlab.py with plain clang (no NDK). */
#include "hvm_android.h"

typedef void* EGLDisplay;
typedef void* EGLConfig;
typedef void* EGLSurface;
typedef void* EGLContext;
typedef unsigned int EGLBoolean;
typedef int EGLint;

extern EGLDisplay eglGetDisplay(void* display_id);
extern EGLBoolean eglInitialize(EGLDisplay dpy, EGLint* major, EGLint* minor);
extern EGLBoolean eglChooseConfig(EGLDisplay dpy, const EGLint* attrib_list, EGLConfig* configs, EGLint config_size, EGLint* num_config);
extern EGLSurface eglCreateWindowSurface(EGLDisplay dpy, EGLConfig config, void* win, const EGLint* attrib_list);
extern EGLContext eglCreateContext(EGLDisplay dpy, EGLConfig config, EGLContext share_context, const EGLint* attrib_list);
extern EGLBoolean eglMakeCurrent(EGLDisplay dpy, EGLSurface draw, EGLSurface read, EGLContext ctx);
extern EGLBoolean eglSwapBuffers(EGLDisplay dpy, EGLSurface surface);

extern void glClearColor(float r, float g, float b, float a);
extern void glClear(unsigned int mask);

extern int ANativeWindow_getWidth(void* window);
extern int ANativeWindow_getHeight(void* window);
extern void ANativeActivity_finish(void* activity);

extern void* ALooper_prepare(int opts);
extern int ALooper_pollAll(int timeoutMillis, int* outFd, int* outEvents, void** outData);
extern void AInputQueue_attachLooper(void* queue, void* looper, int ident, void* callback, void* data);
extern int AInputQueue_getEvent(void* queue, void** outEvent);
extern int AInputQueue_preDispatchEvent(void* queue, void* event);
extern void AInputQueue_finishEvent(void* queue, void* event, int handled);
extern int AInputEvent_getType(const void* event);
extern float AMotionEvent_getX(const void* event, size_t pointer_index);
extern float AMotionEvent_getY(const void* event, size_t pointer_index);

typedef struct ANativeActivityCallbacks {
    void (*onStart)(void* activity);
    void (*onResume)(void* activity);
    void* (*onSaveInstanceState)(void* activity, size_t* outSize);
    void (*onPause)(void* activity);
    void (*onStop)(void* activity);
    void (*onDestroy)(void* activity);
    void (*onWindowFocusChanged)(void* activity, int hasFocus);
    void (*onNativeWindowCreated)(void* activity, void* window);
    void (*onNativeWindowResized)(void* activity, void* window);
    void (*onNativeWindowRedrawNeeded)(void* activity, void* window);
    void (*onNativeWindowDestroyed)(void* activity, void* window);
    void (*onInputQueueCreated)(void* activity, void* queue);
    void (*onInputQueueDestroyed)(void* activity, void* queue);
    void (*onContentRectChanged)(void* activity, const void* rect);
    void (*onConfigurationChanged)(void* activity);
    void (*onLowMemory)(void* activity);
} ANativeActivityCallbacks;

typedef struct ANativeActivity {
    ANativeActivityCallbacks* callbacks;
    void* vm;
    void* env;
    void* clazz;
    const char* internalDataPath;
    const char* externalDataPath;
    int sdkVersion;
    void* instance;
    void* assetManager;
    const char* obbPath;
} ANativeActivity;

static size_t s_len(const char* s) { size_t n = 0; while (s[n]) n++; return n; }
static void say(const char* s) { write(1, s, s_len(s)); }
static void check(const char* name, int pass, const char* why) {
    if (pass) { say("ok "); say(name); say("\n"); return; }
    say("FAIL "); say(name); say(": "); say(why); say("\n");
}

static volatile int g_started, g_resumed, g_focused;
static void* volatile g_window;
static void* volatile g_queue;
static ANativeActivity* g_activity;

static void on_start(void* a) { (void)a; g_started = 1; }
static void on_resume(void* a) { (void)a; g_resumed = 1; }
static void on_focus(void* a, int f) { (void)a; g_focused = f; }
static void on_win_created(void* a, void* w) { (void)a; g_window = w; }
static void on_queue_created(void* a, void* q) { (void)a; g_queue = q; }

static void* android_main(void* arg) {
    (void)arg;
    void* looper = ALooper_prepare(1); /* ALOOPER_PREPARE_ALLOW_NON_CALLBACKS */
    for (int i = 0; i < 400 && (!g_window || !g_queue || !g_focused); i++) usleep(5000);

    check("NativeActivity callbacks",
          g_started && g_resumed && g_focused && g_window && g_queue &&
          g_activity->assetManager != 0 && g_activity->internalDataPath != 0 &&
          g_activity->sdkVersion == 19,
          "missing lifecycle callback or field");

    AInputQueue_attachLooper(g_queue, looper, 1, 0, 0);

    EGLDisplay dpy = eglGetDisplay(0);
    EGLint maj = 0, min = 0, ncfg = 0;
    EGLConfig cfg = 0;
    const EGLint attribs[] = {
        0x3024, 8, /* EGL_RED_SIZE */
        0x3023, 8, /* EGL_GREEN_SIZE */
        0x3022, 8, /* EGL_BLUE_SIZE */
        0x3021, 8, /* EGL_ALPHA_SIZE */
        0x3040, 4, /* EGL_RENDERABLE_TYPE = EGL_OPENGL_ES2_BIT */
        0x3038     /* EGL_NONE */
    };
    const EGLint ctx_attribs[] = {0x3098, 2, 0x3038}; /* EGL_CONTEXT_CLIENT_VERSION = 2 */
    int egl_ok = eglInitialize(dpy, &maj, &min) &&
                 eglChooseConfig(dpy, attribs, &cfg, 1, &ncfg) && ncfg > 0;
    EGLSurface surf = egl_ok ? eglCreateWindowSurface(dpy, cfg, g_window, 0) : 0;
    EGLContext ctx = (egl_ok && surf) ? eglCreateContext(dpy, cfg, 0, ctx_attribs) : 0;
    int cur_ok = ctx && eglMakeCurrent(dpy, surf, surf, ctx);
    int w = ANativeWindow_getWidth(g_window);
    int h = ANativeWindow_getHeight(g_window);
    if (cur_ok) {
        glClearColor(0.2f, 0.7f, 0.3f, 1.0f);
        glClear(0x4000); /* GL_COLOR_BUFFER_BIT */
        eglSwapBuffers(dpy, surf);
    }
    check("NativeActivity EGL draw", cur_ok && w > 0 && h > 0, "EGL setup or window size failed");

    int got_touch = 0;
    for (int iter = 0; iter < 50 && !got_touch; iter++) {
        int fd = 0, events = 0;
        void* data = 0;
        int ident = ALooper_pollAll(100, &fd, &events, &data);
        if (ident == 1) {
            void* ev = 0;
            while (AInputQueue_getEvent(g_queue, &ev) >= 0) {
                if (AInputQueue_preDispatchEvent(g_queue, ev)) continue;
                if (AInputEvent_getType(ev) == 2) { /* AINPUT_EVENT_TYPE_MOTION */
                    float tx = AMotionEvent_getX(ev, 0);
                    float ty = AMotionEvent_getY(ev, 0);
                    if (tx >= 0.0f && ty >= 0.0f) got_touch = 1;
                }
                AInputQueue_finishEvent(g_queue, ev, 1);
            }
        }
    }
    check("NativeActivity touch input", got_touch, "no motion event received");

    say("done: native_activity\n");
    ANativeActivity_finish(g_activity);
    return 0;
}

typedef const void** TestJNI;

__attribute__((visibility("default")))
int Java_test_Driver_nativeAnswer(TestJNI* env, void* cls) {
    (void)env;
    (void)cls;
    return 17;
}

__attribute__((visibility("default")))
void ANativeActivity_onCreate(ANativeActivity* activity, void* savedState, size_t savedStateSize) {
    (void)savedState;
    (void)savedStateSize;
    TestJNI* env = activity->env;
    void* (*findClass)(TestJNI*, const char*) = (void*)(*env)[6];
    void* (*methodId)(TestJNI*, void*, const char*, const char*) = (void*)(*env)[113];
    int (*callInt)(TestJNI*, void*, void*, const void*) = (void*)(*env)[131];
    void* cls = findClass(env, "test/Driver");
    void* method = methodId(env, cls, "nativeAnswer", "()I");
    check("NativeActivity retains native JNI dispatch with bundled DEX", callInt(env, cls, method, 0) == 17,
          "bundled bytecode overrode the native driver");
    g_activity = activity;
    activity->callbacks->onStart = on_start;
    activity->callbacks->onResume = on_resume;
    activity->callbacks->onWindowFocusChanged = on_focus;
    activity->callbacks->onNativeWindowCreated = on_win_created;
    activity->callbacks->onInputQueueCreated = on_queue_created;
    pthread_t t;
    pthread_create(&t, 0, android_main, 0);
}
