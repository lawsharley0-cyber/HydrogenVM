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

/* --------------------------------------------------------------- audio --- */

#ifndef O_WRONLY
#define O_WRONLY 1
#define O_CREAT 0x40
#define O_TRUNC 0x200
#endif

static void write_u16(unsigned char* p, unsigned short v) { p[0] = v & 0xff; p[1] = (v >> 8) & 0xff; }
static void write_u32(unsigned char* p, unsigned int v) {
    p[0] = v & 0xff; p[1] = (v >> 8) & 0xff; p[2] = (v >> 16) & 0xff; p[3] = (v >> 24) & 0xff;
}

static int write_test_wav(const char* path) {
    unsigned char hdr[44] = {0};
    const unsigned int frames = 4410;
    const unsigned int dataBytes = frames * 2;
    hdr[0]='R'; hdr[1]='I'; hdr[2]='F'; hdr[3]='F';
    write_u32(hdr + 4, 36 + dataBytes);
    hdr[8]='W'; hdr[9]='A'; hdr[10]='V'; hdr[11]='E';
    hdr[12]='f'; hdr[13]='m'; hdr[14]='t'; hdr[15]=' ';
    write_u32(hdr + 16, 16);
    write_u16(hdr + 20, 1); /* PCM */
    write_u16(hdr + 22, 1); /* mono */
    write_u32(hdr + 24, 44100);
    write_u32(hdr + 28, 44100 * 2);
    write_u16(hdr + 32, 2);
    write_u16(hdr + 34, 16);
    hdr[36]='d'; hdr[37]='a'; hdr[38]='t'; hdr[39]='a';
    write_u32(hdr + 40, dataBytes);
    const int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) return -1;
    write(fd, hdr, 44);
    short pcm[256];
    for (int i = 0; i < 256; i++) pcm[i] = (i & 16) ? 12000 : -12000;
    unsigned int rem = frames;
    while (rem > 0) {
        const unsigned int chunk = rem > 256 ? 256 : rem;
        write(fd, pcm, chunk * 2);
        rem -= chunk;
    }
    close(fd);
    return 0;
}

static void test_audio_apis(JNIEnv* env) {
    const char* wavPath = "/tmp/testlab_tone.wav";
    const int w_ok = write_test_wav(wavPath);
    check("test WAV file created", w_ok == 0, "open/write failed", w_ok);

    jobject (*find_cls)(JNIEnv*, const char*) = JNI_FN(env, JNI_FindClass, jobject (*)(JNIEnv*, const char*));
    jmethodID (*get_mid)(JNIEnv*, jobject, const char*, const char*) = JNI_FN(env, JNI_GetMethodID, jmethodID (*)(JNIEnv*, jobject, const char*, const char*));
    jobject (*new_obj)(JNIEnv*, jobject, jmethodID, const jvalue*) = JNI_FN(env, JNI_NewObjectA, jobject (*)(JNIEnv*, jobject, jmethodID, const jvalue*));
    void (*call_void)(JNIEnv*, jobject, jmethodID, const jvalue*) = JNI_FN(env, JNI_CallVoidMethodA, void (*)(JNIEnv*, jobject, jmethodID, const jvalue*));
    int (*call_int)(JNIEnv*, jobject, jmethodID, const jvalue*) = JNI_FN(env, JNI_CallIntMethodA, int (*)(JNIEnv*, jobject, jmethodID, const jvalue*));
    int (*call_bool)(JNIEnv*, jobject, jmethodID, const jvalue*) = JNI_FN(env, JNI_CallBooleanMethodA, int (*)(JNIEnv*, jobject, jmethodID, const jvalue*));
    jobject (*new_str)(JNIEnv*, const char*) = JNI_FN(env, JNI_NewStringUTF, jobject (*)(JNIEnv*, const char*));

    /* MediaPlayer */
    jobject mpCls = find_cls(env, "android/media/MediaPlayer");
    jobject mp = new_obj(env, mpCls, get_mid(env, mpCls, "<init>", "()V"), 0);
    jvalue pathArg = {.l = new_str(env, wavPath)};
    call_void(env, mp, get_mid(env, mpCls, "setDataSource", "(Ljava/lang/String;)V"), &pathArg);
    call_void(env, mp, get_mid(env, mpCls, "prepare", "()V"), 0);
    const int dur = call_int(env, mp, get_mid(env, mpCls, "getDuration", "()I"), 0);
    call_void(env, mp, get_mid(env, mpCls, "start", "()V"), 0);
    const int playing = call_bool(env, mp, get_mid(env, mpCls, "isPlaying", "()Z"), 0);
    jvalue seekArg = {.i = 50};
    call_void(env, mp, get_mid(env, mpCls, "seekTo", "(I)V"), &seekArg);
    const int pos = call_int(env, mp, get_mid(env, mpCls, "getCurrentPosition", "()I"), 0);
    call_void(env, mp, get_mid(env, mpCls, "stop", "()V"), 0);
    call_void(env, mp, get_mid(env, mpCls, "release", "()V"), 0);
    check("MediaPlayer loads 100ms WAV, plays, and seeks",
          mp != 0 && dur >= 90 && dur <= 110 && playing == 1 && pos >= 40, "dur", dur);

    /* SoundPool */
    jobject spCls = find_cls(env, "android/media/SoundPool");
    jvalue spInit[3] = {{.i = 4}, {.i = 3}, {.i = 0}};
    jobject sp = new_obj(env, spCls, get_mid(env, spCls, "<init>", "(III)V"), spInit);
    jvalue loadArgs[2] = {{.l = new_str(env, wavPath)}, {.i = 1}};
    const int sndId = call_int(env, sp, get_mid(env, spCls, "load", "(Ljava/lang/String;I)I"), loadArgs);
    jvalue playArgs[6] = {{.i = sndId}, {.f = 1.0f}, {.f = 1.0f}, {.i = 1}, {.i = 0}, {.f = 1.0f}};
    const int streamId = call_int(env, sp, get_mid(env, spCls, "play", "(IFFIIF)I"), playArgs);
    jvalue stopArg = {.i = streamId};
    call_void(env, sp, get_mid(env, spCls, "stop", "(I)V"), &stopArg);
    jvalue unloadArg = {.i = sndId};
    const int unloaded = call_bool(env, sp, get_mid(env, spCls, "unload", "(I)Z"), &unloadArg);
    call_void(env, sp, get_mid(env, spCls, "release", "()V"), 0);
    check("SoundPool loads, plays, stops, and unloads WAV",
          sndId > 0 && streamId > 0 && unloaded == 1, "sndId", sndId);

    /* AAudio */
    int (*aa_create)(void**) = (int (*)(void**))dlsym(RTLD_DEFAULT, "AAudio_createStreamBuilder");
    void (*aa_rate)(void*, int) = (void (*)(void*, int))dlsym(RTLD_DEFAULT, "AAudioStreamBuilder_setSampleRate");
    void (*aa_ch)(void*, int) = (void (*)(void*, int))dlsym(RTLD_DEFAULT, "AAudioStreamBuilder_setChannelCount");
    void (*aa_fmt)(void*, int) = (void (*)(void*, int))dlsym(RTLD_DEFAULT, "AAudioStreamBuilder_setFormat");
    int (*aa_open)(void*, void**) = (int (*)(void*, void**))dlsym(RTLD_DEFAULT, "AAudioStreamBuilder_openStream");
    int (*aa_del)(void*) = (int (*)(void*))dlsym(RTLD_DEFAULT, "AAudioStreamBuilder_delete");
    int (*aa_start)(void*) = (int (*)(void*))dlsym(RTLD_DEFAULT, "AAudioStream_requestStart");
    int (*aa_stop)(void*) = (int (*)(void*))dlsym(RTLD_DEFAULT, "AAudioStream_requestStop");
    int (*aa_close)(void*) = (int (*)(void*))dlsym(RTLD_DEFAULT, "AAudioStream_close");
    int (*aa_state)(void*) = (int (*)(void*))dlsym(RTLD_DEFAULT, "AAudioStream_getState");
    int (*aa_get_rate)(void*) = (int (*)(void*))dlsym(RTLD_DEFAULT, "AAudioStream_getSampleRate");
    int (*aa_write)(void*, const void*, int, long long) = (int (*)(void*, const void*, int, long long))dlsym(RTLD_DEFAULT, "AAudioStream_write");
    long long (*aa_written)(void*) = (long long (*)(void*))dlsym(RTLD_DEFAULT, "AAudioStream_getFramesWritten");
    void* builder = 0;
    void* stream = 0;
    int aa_ok = 0;
    if (aa_create && aa_open && aa_create(&builder) == 0 && builder) {
        if (aa_rate) aa_rate(builder, 44100);
        if (aa_ch) aa_ch(builder, 2);
        if (aa_fmt) aa_fmt(builder, 1);
        if (aa_open(builder, &stream) == 0 && stream) {
            aa_start(stream);
            short frames[256 * 2];
            for (int i = 0; i < 256 * 2; i++) frames[i] = 1000;
            const int w = aa_write(stream, frames, 256, 0);
            aa_ok = aa_state(stream) == 3 && aa_get_rate(stream) == 44100 && w == 256 && aa_written(stream) == 256;
            aa_stop(stream);
            aa_close(stream);
        }
        if (aa_del) aa_del(builder);
    }
    check("AAudio builder/stream opens, starts, writes 256 frames, and closes", aa_ok, "ok", aa_ok);
}

/* ------------------------------------------------------------------- main --- */

static void test_phase_b3(void);

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
    test_phase_b3();
    test_audio_apis(env);
    say("done: "); num(failures); say(" failure(s)\n");
    return failures;
}

/* Linux socket(2), ip(7), ipv6(7) and bionic netdb.h. The byte arrays are in network order. */
static void test_b3_sockets(void) {
    enum { inet = 2, inet6 = 10, stream = 1, dgram = 2, nonblock = 0x800, cloexec = 0x80000 };
    struct b3_sockaddr loop = {inet, {0, 0, 127, 0, 0, 1}};
    unsigned length = sizeof loop;
    const int listener = socket(inet, stream | cloexec, 0);
    const int client = socket(inet, stream, 0);
    const int reuse = 1;
    int value = 0;
    unsigned optlen = sizeof value;
    check("socket Linux SOL_SOCKET, SO_REUSEADDR, SOCK_CLOEXEC",
          listener >= 0 && fcntl(listener, 1, 0) == 1 &&
          setsockopt(listener, 1, 2, &reuse, sizeof reuse) == 0 &&
          getsockopt(listener, 1, 2, &value, &optlen) == 0 && value == 1 &&
          optlen == sizeof value, "errno", errno);
    const int connected = listener >= 0 && client >= 0 &&
        bind(listener, &loop, sizeof loop) == 0 && listen(listener, 1) == 0 &&
        getsockname(listener, &loop, &length) == 0 && loop.family == inet &&
        length == sizeof loop && (loop.bytes[0] || loop.bytes[1]) &&
        connect(client, &loop, length) == 0;
    const int server = connected ? accept4(listener, 0, 0, nonblock | cloexec) : -1;
    check("IPv4 loopback TCP and accept4 descriptor flags",
          server >= 0 && fcntl(server, 1, 0) == 1 && (fcntl(server, 3, 0) & nonblock),
          "errno", errno);
    char data = 0;
    check("MSG_PEEK preserves a byte and MSG_DONTWAIT returns Linux EAGAIN",
          server >= 0 && send(client, "Q", 1, 0x4000) == 1 &&
          recv(server, &data, 1, 2) == 1 && data == 'Q' &&
          recv(server, &data, 1, 0) == 1 && data == 'Q' &&
          recv(server, &data, 1, 0x40) == -1 && errno == 11, "errno", errno);
    if (server >= 0) close(server);
    if (client >= 0) close(client);
    if (listener >= 0) close(listener);

    struct b3_sockaddr external = {inet, {0, 9, 192, 0, 2, 1}};
    const int udp = socket(inet, dgram | nonblock, 0);
    check("offline IPv4 connect and sendto return ENETUNREACH",
          udp >= 0 && connect(udp, &external, sizeof external) == -1 && errno == 101 &&
          sendto(udp, "Q", 1, 0, &external, sizeof external) == -1 && errno == 101,
          "errno", errno);
    if (udp >= 0) close(udp);

    struct b3_sockaddr6 ipv6 = {inet6, {0}};
    ipv6.port_flow_addr_scope[21] = 1; /* ::1 */
    const int udp6 = socket(inet6, dgram, 0);
    length = sizeof ipv6;
    check("IPv6 loopback uses the 28-byte Linux sockaddr_in6",
          udp6 >= 0 && bind(udp6, &ipv6, sizeof ipv6) == 0 &&
          getsockname(udp6, &ipv6, &length) == 0 && length == sizeof ipv6 &&
          ipv6.family == inet6 && ipv6.port_flow_addr_scope[21] == 1, "errno", errno);
    if (udp6 >= 0) close(udp6);

    struct b3_addrinfo hints = {.flags = 4 | 8, .family = inet6, .type = stream};
    struct b3_addrinfo* result = 0;
    int rc = getaddrinfo("::1", "80", &hints, &result);
    check("bionic numeric getaddrinfo places ai_addr after ai_canonname",
          rc == 0 && result && result->addr && !result->canonname &&
          result->family == inet6 && result->addrlen == sizeof ipv6 &&
          result->addr->family == inet6 && result->addr->bytes[1] == 80,
          "EAI", rc);
    if (result) freeaddrinfo(result);
    result = 0;
    rc = getaddrinfo("invalid.test", "80", &hints, &result);
    check("AI_NUMERICHOST failure returns bionic EAI_NONAME (8)", rc == 8 && !result, "EAI", rc);
    rc = getaddrinfo("::1", "http", &hints, &result);
    check("bionic AI_NUMERICSERV value 8 rejects a named service", rc == 8 && !result, "EAI", rc);
}

/* Linux eventfd(2), timerfd_create(2), pipe2(2), poll(2), epoll_ctl(2). */
static void test_b3_descriptors(void) {
    enum { nonblock = 0x800, cloexec = 0x80000, readable = 1, edge_oneshot = 0xc0000000u };
    const int counter = eventfd(2, nonblock | cloexec | 1);
    unsigned long long value = 0;
    check("eventfd semaphore returns one per read and EAGAIN at zero",
          counter >= 0 && fcntl(counter, 1, 0) == 1 &&
          read(counter, &value, 8) == 8 && value == 1 &&
          read(counter, &value, 8) == 8 && value == 1 &&
          read(counter, &value, 8) == -1 && errno == 11, "errno", errno);
    value = 7;
    check("eventfd write updates poll readability", counter >= 0 && write(counter, &value, 8) == 8 &&
          poll(&(struct b3_pollfd){counter, readable, 0}, 1, 0) == 1, "errno", errno);

    int ends[2] = {-1, -1};
    const int made_pipe = pipe2(ends, nonblock | cloexec);
    const int queue = epoll_create1(cloexec);
    struct b3_epoll_event event = {readable | edge_oneshot, 0x0123456789abcdefULL};
    struct { struct b3_epoll_event event; unsigned tail; } seen = {{0, 0}, 0x11223344};
    check("pipe2 descriptors inherit both requested flags",
          made_pipe == 0 && fcntl(ends[0], 1, 0) == 1 && (fcntl(ends[1], 3, 0) & nonblock),
          "errno", errno);
    check("epoll ARM EABI payload and EPOLLONESHOT disable/rearm",
          queue >= 0 && made_pipe == 0 && epoll_ctl(queue, 1, ends[0], &event) == 0 &&
          write(ends[1], "x", 1) == 1 && epoll_wait(queue, &seen.event, 1, 100) == 1 &&
          seen.event.events == readable && seen.event.data == event.data && seen.tail == 0x11223344 &&
          epoll_wait(queue, &seen.event, 1, 0) == 0 &&
          epoll_ctl(queue, 3, ends[0], &event) == 0 && epoll_wait(queue, &seen.event, 1, 100) == 1,
          "errno", errno);
    if (counter >= 0) close(counter);
    if (queue >= 0) close(queue);
    if (made_pipe == 0) { close(ends[0]); close(ends[1]); }

    const int timer = timerfd_create(1, nonblock);
    struct b3_itimerspec spec = {{0, 0}, {0, 10000000}}, remaining = {{-1,-1},{-1,-1}};
    struct b3_pollfd poll_timer = {timer, readable, 0};
    check("timerfd one-shot awakens poll and reports one expiration",
          timer >= 0 && timerfd_settime(timer, 0, &spec, 0) == 0 &&
          timerfd_gettime(timer, &remaining) == 0 && remaining.interval.tv_sec == 0 &&
          remaining.interval.tv_nsec == 0 &&
          poll(&poll_timer, 1, 300) == 1 && poll_timer.revents == readable &&
          read(timer, &value, 8) == 8 && value == 1 &&
          timerfd_gettime(timer, &remaining) == 0 && remaining.value.tv_sec == 0 &&
          remaining.value.tv_nsec == 0, "errno", errno);
    if (timer >= 0) close(timer);
}

/* POSIX positional I/O and Linux mmap(2), mmap2(2), sendfile(2). */
static void test_b3_files(void) {
    const char* path = "/tmp/b3-positional.tmp";
    const int fd = open(path, 2 | 0x40 | 0x200, 0600);
    check("open Android scratch file for positional I/O", fd >= 0, "errno", errno);
    if (fd < 0) return;
    const long long sparse = 0x100000008LL;
    char low = 0, high = 0;
    check("pread/pwrite and aligned 64-bit offsets preserve the descriptor position",
          lseek(fd, 3, 0) == 3 && pwrite(fd, "A", 1, 4096) == 1 &&
          pwrite64(fd, "B", 1, sparse) == 1 && pread(fd, &low, 1, 4096) == 1 &&
          pread64(fd, &high, 1, sparse) == 1 && low == 'A' && high == 'B' &&
          lseek(fd, 0, 1) == 3, "errno", errno);
    char* private_page = mmap(0, 4096, PROT_READ | PROT_WRITE, MAP_PRIVATE, fd, 4096);
    if (private_page != MAP_FAILED) private_page[0] = 'Z';
    check("MAP_PRIVATE writes are isolated and mmap offset selects a file page",
          private_page != MAP_FAILED && private_page[0] == 'Z' &&
          pread(fd, &low, 1, 4096) == 1 && low == 'A', "errno", errno);
    if (private_page != MAP_FAILED) munmap(private_page, 4096);
    char* readonly = (char*)syscall(192, 0, 4096, PROT_READ, MAP_PRIVATE, fd, 1);
    check("ARM mmap2 takes a 4096-byte page index",
          readonly != MAP_FAILED && readonly[0] == 'A' && munmap(readonly, 4096) == 0, "errno", errno);

    int ends[2];
    if (pipe2(ends, 0) == 0) {
        off_t offset = 4096;
        check("sendfile updates its offset pointer while leaving input position unchanged",
              sendfile(ends[1], fd, &offset, 1) == 1 && offset == 4097 &&
              read(ends[0], &low, 1) == 1 && low == 'A' && lseek(fd, 0, 1) == 3, "errno", errno);
        close(ends[0]);
        close(ends[1]);
    } else {
        check("sendfile pipe setup", 0, "errno", errno);
    }
    close(fd);
    unlink(path);
}

static void test_phase_b3(void) {
    test_b3_sockets();
    test_b3_descriptors();
    test_b3_files();
}
