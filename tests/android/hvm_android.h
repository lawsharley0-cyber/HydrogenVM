/* HydrogenVM Android Test Lab — the few bionic and JNI declarations the tests use, written out
 * for 32-bit ARM (armeabi-v7a) so the tests build with plain clang and no NDK. Layouts follow
 * bionic's ARM headers; storage for opaque types is generous on purpose. */
#pragma once

typedef unsigned int size_t;
typedef int ssize_t;
typedef long off_t;
typedef unsigned long sigset_t;          /* bionic ARM: one word */
typedef long pthread_t;
typedef long pthread_mutexattr_t;
typedef struct { int storage[16]; } pthread_mutex_t;
typedef struct { int storage[16]; } sem_t;
typedef struct { long tv_sec; long tv_nsec; } timespec_t;

struct sigaction_t {                     /* bionic ARM struct sigaction */
    void (*handler)(int);
    sigset_t mask;
    int flags;
    void (*restorer)(void);
};

#define SIGUSR1 10                       /* Linux numbers (Darwin's differ) */
#define SIGPWR 30
#define SA_SIGINFO 4
#define SA_RESTART 0x10000000
#define SIG_BLOCK 0
#define SIG_UNBLOCK 1
#define SIG_SETMASK 2
#define PTHREAD_MUTEX_RECURSIVE 1        /* Linux: NORMAL 0, RECURSIVE 1, ERRORCHECK 2 */
#define PTHREAD_MUTEX_ERRORCHECK 2
#define PROT_READ 1
#define PROT_WRITE 2
#define PROT_EXEC 4
#define MAP_PRIVATE 2
#define MAP_ANONYMOUS 0x20
#define MAP_FAILED ((void*)-1)
#define O_RDONLY 0
#define F_OK 0
#define EDEADLK 35
#define ETIMEDOUT 110
#define SYS_futex 240
#define SYS_sched_setaffinity 241
#define FUTEX_WAIT 0
#define FUTEX_WAKE 1

int sigaction(int, const struct sigaction_t*, struct sigaction_t*);
int raise(int);
int pthread_kill(pthread_t, int);
int sigprocmask(int, const sigset_t*, sigset_t*);
int sigpending(sigset_t*);
int pthread_create(pthread_t*, const void*, void* (*)(void*), void*);
int pthread_join(pthread_t, void**);
int pthread_mutexattr_init(pthread_mutexattr_t*);
int pthread_mutexattr_settype(pthread_mutexattr_t*, int);
int pthread_mutex_init(pthread_mutex_t*, const pthread_mutexattr_t*);
int pthread_mutex_lock(pthread_mutex_t*);
int pthread_mutex_unlock(pthread_mutex_t*);
int pthread_mutex_destroy(pthread_mutex_t*);
int sem_init(sem_t*, int, unsigned);
int sem_wait(sem_t*);
int sem_post(sem_t*);
void* mmap(void*, size_t, int, int, int, off_t);
int munmap(void*, size_t);
long syscall(long, ...);
int access(const char*, int);
int open(const char*, int, ...);
ssize_t read(int, void*, size_t);
ssize_t write(int, const void*, size_t);
int close(int);
int usleep(unsigned);
int* __errno(void);
#define errno (*__errno())

/* JNI: the function table by index (jni.h order), called through the "A" (jvalue array)
 * variants so no varargs cross the boundary. */
typedef void* jobject;
typedef void* jmethodID;
typedef union { int z; int i; long long j; float f; double d; jobject l; } jvalue;
typedef const void* const* JNIEnv;
#define JNI_FN(env, index, type) ((type)((*(env))[index]))
#define JNI_FindClass 6
#define JNI_FromReflectedMethod 7
#define JNI_ToReflectedMethod 9
#define JNI_ExceptionClear 17
#define JNI_GetStaticMethodID 113
#define JNI_CallStaticObjectMethodA 116
#define JNI_NewStringUTF 167
#define JNI_ExceptionCheck 228
#define JNI_NewDirectByteBuffer 229
#define JNI_GetDirectBufferAddress 230
#define JNI_GetDirectBufferCapacity 231
