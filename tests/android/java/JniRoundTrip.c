#include "../hvm_android.h"

#ifdef __GNUC__
#define EXPORT __attribute__((visibility("default")))
#else
#define EXPORT
#endif

typedef void* Object;
typedef struct { const char* name; const char* signature; void* function; } NativeMethod;
typedef union { long long j; int i; void* l; } Argument;

static int exportedCalls, registeredCalls, failures;

EXPORT int Java_test_Bridge_add(JNIEnv* env, Object cls, int x) {
    (void)env;
    (void)cls;
    ++exportedCalls;
    return 3*x+1;
}

static int addFour(JNIEnv* env, Object cls, int x) {
    (void)env;
    (void)cls;
    ++registeredCalls;
    return x+4;
}

static void say(const char* s) {
    size_t n = 0;
    while (s[n]) ++n;
    write(1, s, n);
}

static void test(const char* label, int ok) {
    say(ok ? "ok " : "FAIL ");
    say(label);
    say("\n");
    failures += !ok;
}

EXPORT int hvm_test_main(JNIEnv* env, void* vm) {
    (void)vm;
    Object (*findClass)(JNIEnv*, const char*) = (void*) (*env)[6];
    void* (*getMethod)(JNIEnv*, Object, const char*, const char*) = (void*) (*env)[113];
    int (*call)(JNIEnv*, Object, void*, const Argument*) = (void*) (*env)[131];
    int (*registerNatives)(JNIEnv*, Object, const NativeMethod*, int) = (void*) (*env)[215];
    unsigned char (*exceptionCheck)(JNIEnv*) = (void*) (*env)[228];
    Object (*exceptionOccurred)(JNIEnv*) = (void*) (*env)[15];
    void (*exceptionClear)(JNIEnv*) = (void*) (*env)[17];

    Object cls = findClass(env, "test/Bridge");
    const NativeMethod callback = {"registered", "(I)I", (void*)addFour};
    test("FindClass and RegisterNatives accept a DEX-defined class",
         cls && registerNatives(env, cls, &callback, 1) == 0);
    const Argument input = {.i = 5};
    void* step = cls ? getMethod(env, cls, "step", "(I)I") : 0;
    const int outcome = step ? call(env, cls, step, &input) : -1;
    /* Export returns 3*5+1=16. Registered native adds 4.
       The host InputDevice returns an empty int[] and the host SDK_INT is 19. */
    test("native to DEX to exported and registered natives plus host class fallback",
         outcome == 39 && exportedCalls == 1 && registeredCalls == 1 && !exceptionCheck(env));

    void* fault = cls ? getMethod(env, cls, "fault", "()I") : 0;
    if (fault) call(env, cls, fault, 0);
    test("interpreter exception is visible to JNI", fault && exceptionCheck(env) && exceptionOccurred(env));
    exceptionClear(env);
    test("ExceptionClear clears the pending JNI exception", !exceptionCheck(env));
    Object worker = findClass(env, "test/JniWorker");
    void* parallel = worker ? getMethod(env, worker, "parallel", "()I") : 0;
    test("Java Thread.start runs through Process guest workers", parallel &&
         call(env, worker, parallel, 0) == 1000 && !exceptionCheck(env));
    Object collections = findClass(env, "test/Collections");
    void* check = collections ? getMethod(env, collections, "check", "()I") : 0;
    /* The sorted copy starts with -2 and the list contains one string. */
    test("DEX collections sort, copy, and dispatch ArrayList through List",
         check && call(env, collections, check, 0) == -1 && !exceptionCheck(env));
    void* map = getMethod(env, collections, "mapCheck", "()I");
    test("Map supports a null key, returns the previous value, and removes it",
         map && call(env, collections, map, 0) == 0 && !exceptionCheck(env));
    exceptionClear(env);
    void* order = getMethod(env, collections, "orderCheck", "()I");
    test("Collections reverse and swap update the ArrayList order",
         order && call(env, collections, order, 0) == 1 && !exceptionCheck(env));
    exceptionClear(env);
    void* set = getMethod(env, collections, "setCheck", "()I");
    test("Set accepts null, rejects duplicate insertion, and removes membership",
         set && call(env, collections, set, 0) == 1 && !exceptionCheck(env));
    say("done: JNI round-trip\n");
    return failures;
}
