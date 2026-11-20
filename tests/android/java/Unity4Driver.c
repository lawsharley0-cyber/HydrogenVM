/* A Unity 4 player as the Unity driver meets it: libunity.so registers UnityPlayer's natives in
   JNI_OnLoad, with Unity 4's one-argument nativeRecreateGfxState(Surface). Unity 5 added a
   leading int; a driver that always passes (0, surface) hands Unity 4 a null Surface, and its
   render loop then waits forever for a window (Crossy Road 0.1.9 on the phone, #530). */
#include "../hvm_android.h"

#define EXPORT __attribute__((visibility("default")))

typedef void* Object;
typedef const void* const* JavaVM;
typedef struct { const char* name; const char* signature; void* function; } NativeMethod;

void* ANativeWindow_fromSurface(JNIEnv* env, Object surface);

static int recreated, gotWindow, reported;

static void say(const char* s) {
    size_t n = 0;
    while (s[n]) ++n;
    write(1, s, n);
}

static void recreateGfxState(JNIEnv* env, Object self, Object surface) {
    (void)self;
    ++recreated;
    gotWindow = surface && ANativeWindow_fromSurface(env, surface) != 0;
}

static unsigned char render(JNIEnv* env, Object self) {
    (void)env;
    (void)self;
    if (!reported) {
        reported = 1;
        say(recreated && gotWindow ? "ok " : "FAIL ");
        say("Unity 4 nativeRecreateGfxState(Surface) receives a window before the first nativeRender\n");
        say("done: Unity 4 driver\n");
    }
    return 1;
}

EXPORT int JNI_OnLoad(JavaVM* vm, void* reserved) {
    (void)reserved;
    JNIEnv* env = 0;
    int (*getEnv)(JavaVM*, JNIEnv**, int) = (void*)(*vm)[6];
    if (getEnv(vm, &env, 0x10006) != 0 || !env) return -1;
    Object (*findClass)(JNIEnv*, const char*) = (void*)(*env)[6];
    int (*registerNatives)(JNIEnv*, Object, const NativeMethod*, int) = (void*)(*env)[215];
    const NativeMethod natives[] = {
        {"nativeRecreateGfxState", "(Landroid/view/Surface;)V", (void*)recreateGfxState},
        {"nativeRender", "()Z", (void*)render},
    };
    Object player = findClass(env, "com/unity3d/player/UnityPlayer");
    if (!player || registerNatives(env, player, natives, 2) != 0) {
        say("FAIL Unity 4 player registers its natives\ndone: Unity 4 driver\n");
        return -1;
    }
    return 0x10006;
}
