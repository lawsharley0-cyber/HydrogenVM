/* HydrogenVM Android Test Lab.
 *
 * A tiny armeabi-v7a library that HydrogenVM loads like a game's native code. Instead of a game,
 * it exports hvm_test_main(), which runs one check per bug class we've fixed in the Android layer
 * and prints "ok <name>" or "FAIL <name>: <why>" for each. The expected values come from Linux,
 * bionic and the JNI spec — never from HydrogenVM's own output — so a regression shows up here
 * before it shows up as a crashed game.
 *
 * Built by tools/android_testlab.py with plain clang (no NDK). */
#include "hvm_android.h"

static int failures;

static size_t len(const char* s) { size_t n = 0; while (s[n]) n++; return n; }
static void say(const char* s) { write(1, s, len(s)); }
static void num(long v) {
    char buf[16]; int i = 15; buf[i] = 0;
    unsigned long u = v < 0 ? -(unsigned long)v : (unsigned long)v;
    do { buf[--i] = '0' + u % 10; u /= 10; } while (u);
    if (v < 0) buf[--i] = '-';
    say(buf + i);
}
static void check(const char* name, int pass, const char* why, long got) {
    if (pass) { say("ok "); say(name); say("\n"); return; }
    failures++;
    say("FAIL "); say(name); say(": "); say(why); say(" (got "); num(got); say(")\n");
}

/* ---------------------------------------------------------------- signals --- */

static volatile int usr1_hits;
static void on_usr1(int sig) { if (sig == SIGUSR1) usr1_hits++; }

static void test_signal_numbers(void) {
    struct sigaction_t sa = {0}, old = {0};
    sa.handler = on_usr1;
    const int r = sigaction(SIGUSR1, &sa, &old);
    check("sigaction(SIGUSR1=10) installs", r == 0, "sigaction failed", r);
    usr1_hits = 0;
    raise(SIGUSR1);
    check("raise(10) runs the SIGUSR1 handler", usr1_hits == 1, "handler hits", usr1_hits);
}

static void test_signal_mask(void) {
    sigset_t block = 1ul << (SIGUSR1 - 1), pending = 0, old = 0;
    usr1_hits = 0;
    sigprocmask(SIG_BLOCK, &block, &old);
    raise(SIGUSR1);
    check("blocked signal is held", usr1_hits == 0, "handler ran while blocked", usr1_hits);
    sigpending(&pending);
    check("sigpending reports it", (pending & block) != 0, "pending mask", (long)pending);
    sigprocmask(SIG_SETMASK, &old, 0);
    check("unblocking delivers it", usr1_hits == 1, "handler hits after unblock", usr1_hits);
}

static volatile int worker_ready, worker_done;
static void* worker(void* arg) {
    (void)arg;
    worker_ready = 1;
    for (int i = 0; i < 400 && !usr1_hits; i++) usleep(5000);
    worker_done = 1;
    return 0;
}

static void test_pthread_kill(void) {
    pthread_t t;
    usr1_hits = 0; worker_ready = 0; worker_done = 0;
    if (pthread_create(&t, 0, worker, 0) != 0) { check("pthread_kill", 0, "pthread_create failed", -1); return; }
    while (!worker_ready) usleep(1000);
    const int r = pthread_kill(t, SIGUSR1);
    pthread_join(t, 0);
    check("pthread_kill reaches another thread", r == 0 && usr1_hits == 1, "handler hits", usr1_hits);
}

/* ---------------------------------------------------------------- threads --- */

static void test_recursive_mutex(void) {
    pthread_mutexattr_t attr;
    pthread_mutex_t m;
    pthread_mutexattr_init(&attr);
    pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE);   /* 1 on Linux, 2 on Darwin */
    pthread_mutex_init(&m, &attr);
    const int a = pthread_mutex_lock(&m), b = pthread_mutex_lock(&m);
    check("recursive mutex (type 1) relocks", a == 0 && b == 0, "second lock", b);
    pthread_mutex_unlock(&m);
    pthread_mutex_unlock(&m);
    pthread_mutex_destroy(&m);

    pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_ERRORCHECK);
    pthread_mutex_init(&m, &attr);
    pthread_mutex_lock(&m);
    const int c = pthread_mutex_lock(&m);
    check("errorcheck mutex (type 2) reports EDEADLK", c == EDEADLK, "second lock", c);
    pthread_mutex_unlock(&m);
    pthread_mutex_destroy(&m);
}

static int futex_word;
static void test_futex_timeout(void) {
    timespec_t ts = {0, 20 * 1000 * 1000};
    futex_word = 0;
    errno = 0;
    const long r = syscall(SYS_futex, &futex_word, FUTEX_WAIT, 0, &ts, 0, 0);
    check("futex wait times out with ETIMEDOUT", r == -1 && errno == ETIMEDOUT, "errno", errno);
    const long w = syscall(SYS_futex, &futex_word, FUTEX_WAKE, 1, 0, 0, 0);
    check("futex wake with no waiters returns 0", w == 0, "woken", w);
}

static void test_affinity(void) {
    unsigned long mask = 1;
    const long r = syscall(SYS_sched_setaffinity, 0, sizeof mask, &mask);
    check("sched_setaffinity succeeds", r == 0, "result", r);
}

/* ------------------------------------------------------------ /proc, cpu --- */

static int contains(const char* hay, int n, const char* needle) {
    const int m = (int)len(needle);
    for (int i = 0; i + m <= n; i++) {
        int j = 0;
        while (j < m && hay[i + j] == needle[j]) j++;
        if (j == m) return 1;
    }
    return 0;
}

static int slurp(const char* path, char* buf, int cap) {
    const int fd = open(path, O_RDONLY);
    if (fd < 0) return -1;
    int total = 0;
    for (;;) {
        const int n = read(fd, buf + total, cap - 1 - total);
        if (n <= 0) break;
        total += n;
        if (total >= cap - 1) break;
    }
    close(fd);
    buf[total] = 0;
    return total;
}

static char text[16384];

static void test_proc(void) {
    check("/proc/self/maps exists", access("/proc/self/maps", F_OK) == 0, "access", errno);
    int n = slurp("/proc/self/maps", text, sizeof text);
    check("/proc/self/maps lists mappings", n > 0 && contains(text, n, "r-xp"), "bytes", n);
    n = slurp("/proc/cpuinfo", text, sizeof text);
    check("/proc/cpuinfo reports neon", n > 0 && contains(text, n, "neon"), "bytes", n);

    n = slurp("/proc/self/auxv", text, sizeof text);
    unsigned long hwcap = 0;
    for (int i = 0; i + 8 <= n; i += 8) {
        const unsigned long* pair = (const unsigned long*)(text + i);
        if (pair[0] == 16) hwcap = pair[1];       /* AT_HWCAP */
    }
    check("auxv AT_HWCAP has NEON (bit 12)", (hwcap & (1ul << 12)) != 0, "hwcap", (long)hwcap);
}

/* ----------------------------------------------------- self-modifying code --- */

static long cacheflush_svc(void* start, void* end) {
    register long r0 __asm__("r0") = (long)start;
    register long r1 __asm__("r1") = (long)end;
    register long r2 __asm__("r2") = 0;
    register long r7 __asm__("r7") = 0xf0002;    /* __ARM_NR_cacheflush */
    __asm__ volatile("svc #0" : "+r"(r0) : "r"(r1), "r"(r2), "r"(r7) : "memory");
    return r0;
}

static void test_code_rewrite(void) {
    unsigned* code = mmap(0, 4096, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (code == MAP_FAILED) { check("RWX mmap", 0, "mmap failed", errno); return; }
    typedef int (*fn)(void);
    code[0] = 0xe3a00007;   /* mov r0, #7 */
    code[1] = 0xe12fff1e;   /* bx lr */
    cacheflush_svc(code, code + 2);
    const int first = ((fn)code)();
    code[0] = 0xe3a00009;   /* mov r0, #9 */
    const long r = cacheflush_svc(code, code + 2);
    const int second = ((fn)code)();
    check("generated code runs", first == 7, "first call", first);
    check("cacheflush returns 0", r == 0, "result", r);
    check("rewritten code runs the new version", second == 9, "second call", second);
    munmap(code, 4096);
}

/* -------------------------------------------------------------------- JNI --- */

static void test_jni(JNIEnv* env) {
    typedef void* (*FindClass)(JNIEnv*, const char*);
    typedef jmethodID (*GetStaticMethodID)(JNIEnv*, void*, const char*, const char*);
    typedef jobject (*CallStaticObjectA)(JNIEnv*, void*, jmethodID, const jvalue*);
    typedef int (*ExceptionCheck)(JNIEnv*);
    typedef void (*ExceptionClear)(JNIEnv*);
    typedef jobject (*ToReflected)(JNIEnv*, void*, jmethodID, int);
    typedef jmethodID (*FromReflected)(JNIEnv*, jobject);
    typedef jobject (*NewDirect)(JNIEnv*, void*, long long);
    typedef void* (*DirectAddress)(JNIEnv*, jobject);
    typedef long long (*DirectCapacity)(JNIEnv*, jobject);

    /* A game's own Java plugin class (ads, analytics, store) that HydrogenVM has never heard of. */
    void* plugin = JNI_FN(env, JNI_FindClass, FindClass)(env, "com/hvmtest/SomePlugin");
    check("FindClass of an unknown plugin class", plugin != 0, "class", (long)plugin);
    jmethodID make = JNI_FN(env, JNI_GetStaticMethodID, GetStaticMethodID)(
        env, plugin, "instance", "()Lcom/hvmtest/SomePlugin;");
    check("GetStaticMethodID on the plugin", make != 0, "method id", (long)make);
    jobject obj = make ? JNI_FN(env, JNI_CallStaticObjectMethodA, CallStaticObjectA)(env, plugin, make, 0) : 0;
    const int threw = JNI_FN(env, JNI_ExceptionCheck, ExceptionCheck)(env);
    if (threw) JNI_FN(env, JNI_ExceptionClear, ExceptionClear)(env);
    check("plugin call returns an object, no exception", obj != 0 && !threw, "threw", threw);

    if (make) {
        jobject reflected = JNI_FN(env, JNI_ToReflectedMethod, ToReflected)(env, plugin, make, 1);
        jmethodID back = reflected ? JNI_FN(env, JNI_FromReflectedMethod, FromReflected)(env, reflected) : 0;
        check("ToReflectedMethod/FromReflectedMethod round-trip", back == make, "id", (long)back);
    }

    static char buffer[256];
    jobject direct = JNI_FN(env, JNI_NewDirectByteBuffer, NewDirect)(env, buffer, sizeof buffer);
    void* addr = direct ? JNI_FN(env, JNI_GetDirectBufferAddress, DirectAddress)(env, direct) : 0;
    long long cap = direct ? JNI_FN(env, JNI_GetDirectBufferCapacity, DirectCapacity)(env, direct) : -1;
    check("direct buffer keeps its address", addr == buffer, "address", (long)addr);
    check("direct buffer keeps its capacity", cap == (long long)sizeof buffer, "capacity", (long)cap);
}

/* ----------------------------------------------------------- bionic & NDK --- */

static int streq(const char* a, const char* b) {
    while (*a && *a == *b) { a++; b++; }
    return *a == *b;
}

static void test_bionic_gaps(void) {
    float sf = -1.0f, cf = -1.0f;
    sincosf(0.0f, &sf, &cf);
    check("sincosf(0) writes sin=0, cos=1", sf == 0.0f && cf == 1.0f, "cos*100", (long)(cf * 100.0f));

    double sd = -1.0, cd = -1.0;
    sincos(0.0, &sd, &cd);
    check("sincos(0) writes sin=0, cos=1", sd == 0.0 && cd == 1.0, "cos*100", (long)(cd * 100.0));

    const char* hello = "hello";
    check("__strlen_chk returns string length", __strlen_chk(hello, 16) == 5, "len", (long)__strlen_chk(hello, 16));
    check("__strchr_chk finds first match", __strchr_chk(hello, 'l', 16) == hello + 2, "offset",
          (long)(__strchr_chk(hello, 'l', 16) - hello));
    check("__strrchr_chk finds last match", __strrchr_chk(hello, 'l', 16) == hello + 3, "offset",
          (long)(__strrchr_chk(hello, 'l', 16) - hello));

    unsigned long fdset[32];
    for (int i = 0; i < 32; i++) fdset[i] = 0;
    __FD_SET_chk(5, fdset, sizeof fdset);
    check("__FD_SET_chk / __FD_ISSET_chk tracks fd bit",
          __FD_ISSET_chk(5, fdset, sizeof fdset) != 0 && __FD_ISSET_chk(4, fdset, sizeof fdset) == 0,
          "isset(5)", __FD_ISSET_chk(5, fdset, sizeof fdset));

    const int fd = __open_2("/proc/cpuinfo", O_RDONLY);
    char buf[64];
    const ssize_t nr = fd >= 0 ? __read_chk(fd, buf, 32, sizeof buf) : -1;
    if (fd >= 0) close(fd);
    check("__open_2 + __read_chk reads file bytes", nr > 0, "bytes", nr);

    char* end = 0;
    const long long imax = strtoimax("-1234567890123", &end, 10);
    check("strtoimax parses signed 64-bit decimal", imax == -1234567890123LL && end && *end == 0, "low", (long)imax);
    const unsigned long long umax = strtoumax("0xffffffff00000001", &end, 16);
    check("strtoumax parses unsigned 64-bit hex", umax == 0xffffffff00000001ULL && end && *end == 0, "low", (long)umax);

    struct dirent_t** namelist = 0;
    const int count = scandir(".", &namelist, 0, alphasort);
    int dot_idx = -1, dotdot_idx = -1, sorted = 1;
    for (int i = 0; i < count && namelist; i++) {
        if (namelist[i]) {
            if (streq(namelist[i]->d_name, ".")) dot_idx = i;
            if (streq(namelist[i]->d_name, "..")) dotdot_idx = i;
            if (i > 0 && namelist[i - 1] && alphasort((const struct dirent_t**)&namelist[i - 1],
                                                      (const struct dirent_t**)&namelist[i]) > 0)
                sorted = 0;
        }
    }
    for (int i = 0; i < count && namelist; i++) if (namelist[i]) free(namelist[i]);
    if (namelist) free(namelist);
    check("scandir + alphasort lists directory entries in order",
          count >= 2 && dot_idx >= 0 && dotdot_idx > dot_idx && sorted, "count", count);

    unsigned char rnd[32];
    for (int i = 0; i < 32; i++) rnd[i] = 0;
    const int ge_ok = getentropy(rnd, sizeof rnd);
    int nonzero = 0;
    for (int i = 0; i < 32; i++) if (rnd[i] != 0) nonzero++;
    errno = 0;
    const int ge_big = getentropy(rnd, 257);
    const int ge_err = errno;
    check("getentropy(32) fills buffer and >256 sets EIO",
          ge_ok == 0 && nonzero > 0 && ge_big == -1 && ge_err == EIO, "errno", ge_err);

    void* sl_rec = dlsym(RTLD_DEFAULT, "SL_IID_RECORD");
    check("SL_IID_RECORD OpenSL ES symbol resolves", sl_rec != 0, "ptr", (long)sl_rec);

    struct { unsigned env; unsigned canary[3]; } fenv_frame = {0, {0xDEADBEEFu, 0xCAFEBABEu, 0x12345678u}};
    const int fh = feholdexcept(&fenv_frame.env);
    const int fs = fesetenv((const unsigned*)-1);
    check("feholdexcept/fesetenv use 4-byte Bionic fenv_t and accept FE_DFL_ENV (-1)",
          fh == 0 && fs == 0 && fenv_frame.canary[0] == 0xDEADBEEFu &&
              fenv_frame.canary[1] == 0xCAFEBABEu && fenv_frame.canary[2] == 0x12345678u,
          "canary0", (long)fenv_frame.canary[0]);
}

/* ------------------------------------------------------------------- main --- */

int hvm_test_main(JNIEnv* env, void* vm) {
    (void)vm;
    say("HydrogenVM Android Test Lab\n");
    test_signal_numbers();
    test_signal_mask();
    test_pthread_kill();
    test_recursive_mutex();
    test_futex_timeout();
    test_affinity();
    test_proc();
    test_code_rewrite();
    test_jni(env);
    test_bionic_gaps();
    say("done: "); num(failures); say(" failure(s)\n");
    return failures;
}
