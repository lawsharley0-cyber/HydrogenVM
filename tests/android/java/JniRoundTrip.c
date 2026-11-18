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
    Object (*callObject)(JNIEnv*, Object, void*, const Argument*) = (void*)(*env)[116];
    Object (*newString)(JNIEnv*, const char*) = (void*)(*env)[167];
    Object (*newIntArray)(JNIEnv*, int) = (void*)(*env)[179];
    void (*getInts)(JNIEnv*, Object, int, int, int*) = (void*)(*env)[203];
    void (*setInts)(JNIEnv*, Object, int, int, const int*) = (void*)(*env)[211];
    Object (*newObjectArray)(JNIEnv*, int, Object, Object) = (void*)(*env)[172];
    Object (*getElement)(JNIEnv*, Object, int) = (void*)(*env)[173];
    void (*setElement)(JNIEnv*, Object, int, Object) = (void*)(*env)[174];
    void* (*constructor)(JNIEnv*, Object, const char*, const char*) = (void*)(*env)[33];
    Object (*newObject)(JNIEnv*, Object, void*, const Argument*) = (void*)(*env)[30];

    void* writeInt = getMethod(env, cls, "write", "([II)I");
    void* newInts = getMethod(env, cls, "newInts", "()[I");
    for (int native = 0; native < 2; ++native) {
        Object ints = native ? newIntArray(env, 1) : callObject(env, cls, newInts, 0);
        Argument a[] = {{.l = ints}, {.i = 19}};
        int before = call(env, cls, writeInt, a);
        int observed = 0, changed = 37;
        getInts(env, ints, 0, 1, &observed);
        setInts(env, ints, 0, 1, &changed);
        int reread = call(env, cls, writeInt, a);
        test(native ? "JNI-created int array shares writes with DEX" : "DEX-created int array shares writes with JNI",
             before == (native ? 0 : 5) && observed == 19 && reread == 37 && !exceptionCheck(env));
    }
    Object first = newString(env, "same"), second = newString(env, "same");
    Argument pair[] = {{.l = first}, {.l = second}};
    void* same = getMethod(env, cls, "same", "(Ljava/lang/String;Ljava/lang/String;)I");
    int distinct = call(env, cls, same, pair) == 0;
    pair[1].l = first;
    test("separate NewStringUTF allocations keep distinct DEX identities",
         distinct && call(env, cls, same, pair) == 1 && !exceptionCheck(env));

    Object strings = newObjectArray(env, 1, findClass(env, "java/lang/String"), first);
    Argument storeArgs[] = {{.l = strings}, {.l = second}};
    void* store = getMethod(env, cls, "store", "([Ljava/lang/String;Ljava/lang/String;)Ljava/lang/String;");
    Object was = callObject(env, cls, store, storeArgs);
    Object now = getElement(env, strings, 0);
    setElement(env, strings, 0, first);
    test("object-array element identity survives mutations from both runtimes",
         was == first && now == second && callObject(env, cls, store, storeArgs) == first && !exceptionCheck(env));

    Object throwing = findClass(env, "test/Throwing");
    void* init = constructor(env, throwing, "<init>", "()V");
    Object failed = newObject(env, throwing, init, 0);
    test("throwing DEX constructor returns NULL with a pending JNI exception",
         !failed && exceptionCheck(env) && exceptionOccurred(env));
    exceptionClear(env);
    Object worker = findClass(env, "test/JniWorker");
    void* parallel = worker ? getMethod(env, worker, "parallel", "()I") : 0;
    test("Java Thread.start runs through Process guest workers", parallel &&
         call(env, worker, parallel, 0) == 1000 && !exceptionCheck(env));
    say("done: JNI round-trip\n");
    return failures;
}
