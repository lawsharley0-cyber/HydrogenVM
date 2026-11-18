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
#define EIO 5
#define EDEADLK 35
#define ETIMEDOUT 110
#define SYS_futex 240
#define SYS_sched_setaffinity 241
#define FUTEX_WAIT 0
#define FUTEX_WAKE 1
#define RTLD_DEFAULT ((void*)0xffffffffu)

struct dirent_t {
    unsigned long long d_ino;
    long long d_off;
    unsigned short d_reclen;
    unsigned char d_type;
    char d_name[256];
};

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
void free(void*);
void* dlsym(void*, const char*);
void sincosf(float, float*, float*);
void sincos(double, double*, double*);
size_t __strlen_chk(const char*, size_t);
char* __strchr_chk(const char*, int, size_t);
char* __strrchr_chk(const char*, int, size_t);
void __FD_SET_chk(int, void*, size_t);
int __FD_ISSET_chk(int, const void*, size_t);
int __open_2(const char*, int);
ssize_t __read_chk(int, void*, size_t, size_t);
long long strtoimax(const char*, char**, int);
unsigned long long strtoumax(const char*, char**, int);
int scandir(const char*, struct dirent_t***, int (*)(const struct dirent_t*),
            int (*)(const struct dirent_t**, const struct dirent_t**));
int alphasort(const struct dirent_t**, const struct dirent_t**);
int getentropy(void*, size_t);
int feholdexcept(unsigned*);
int fesetenv(const unsigned*);
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

/* Linux ARM EABI UAPI and bionic netdb.h. */
struct b3_sockaddr { unsigned short family; unsigned char bytes[14]; };
struct b3_sockaddr6 { unsigned short family; unsigned char port_flow_addr_scope[26]; };
struct b3_addrinfo {
    int flags, family, type, protocol;
    unsigned addrlen;
    char* canonname;
    struct b3_sockaddr* addr;
    struct b3_addrinfo* next;
};
struct b3_pollfd { int fd; short events, revents; };
struct b3_epoll_event { unsigned events; unsigned long long data; };
struct b3_itimerspec { timespec_t interval, value; };
int socket(int, int, int);
int bind(int, const void*, unsigned);
int listen(int, int);
int connect(int, const void*, unsigned);
int accept4(int, void*, unsigned*, int);
int getsockname(int, void*, unsigned*);
int getsockopt(int, int, int, void*, unsigned*);
int setsockopt(int, int, int, const void*, unsigned);
ssize_t send(int, const void*, size_t, int);
ssize_t sendto(int, const void*, size_t, int, const void*, unsigned);
ssize_t recv(int, void*, size_t, int);
int getaddrinfo(const char*, const char*, const struct b3_addrinfo*, struct b3_addrinfo**);
void freeaddrinfo(struct b3_addrinfo*);
int fcntl(int, int, ...);
int eventfd(unsigned, int);
int timerfd_create(int, int);
int timerfd_settime(int, int, const struct b3_itimerspec*, struct b3_itimerspec*);
int timerfd_gettime(int, struct b3_itimerspec*);
int epoll_create1(int);
int epoll_ctl(int, int, int, struct b3_epoll_event*);
int epoll_wait(int, struct b3_epoll_event*, int, int);
int poll(struct b3_pollfd*, unsigned, int);
int pipe2(int[2], int);
off_t lseek(int, off_t, int);
ssize_t pread(int, void*, size_t, off_t);
ssize_t pwrite(int, const void*, size_t, off_t);
ssize_t pread64(int, void*, size_t, long long);
ssize_t pwrite64(int, const void*, size_t, long long);
ssize_t sendfile(int, int, off_t*, size_t);
int unlink(const char*);
