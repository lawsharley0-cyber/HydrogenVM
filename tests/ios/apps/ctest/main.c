// C runtime test app. Built for the guest (32-bit iOS) and natively for the
// host; the two outputs must be identical.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <setjmp.h>
#include <pthread.h>
#include <ctype.h>
#include <errno.h>

static int ctor_ran;
__attribute__((constructor)) static void early(void) { printf("constructor ran\n"); ctor_ran = 42; }

static int cmp_int(const void *a, const void *b) {
    int x = *(const int *)a, y = *(const int *)b;
    return (x > y) - (x < y);
}

static jmp_buf jb;
static int depth;
static void dive(int n) {
    depth = n;
    if (n == 7) longjmp(jb, n * 3);
    dive(n + 1);
}

static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
static long shared_counter;
static void *worker(void *arg) {
    long id = (long)arg;
    for (int i = 0; i < 2000; ++i) {
        pthread_mutex_lock(&mu);
        shared_counter += id;
        pthread_mutex_unlock(&mu);
    }
    return (void *)(id * 10);
}

static pthread_once_t once = PTHREAD_ONCE_INIT;
static int once_count;
static void once_fn(void) { once_count++; }

static void bye(void) { printf("atexit handler ran\n"); }

int main(int argc, char **argv) {
    printf("hello from %s, argc=%d ctor=%d\n", "HydrogenVM", argc, ctor_ran);

    // printf formatting
    printf("[%d] [%u] [%x] [%X] [%o] [%c] [%s]\n", -123, 4000000000u, 0xbeef, 0xbeef, 8, 'Z', "str");
    printf("[%5d] [%-5d] [%05d] [%+d] [% d] [%*d] [%.*s]\n", 42, 42, 42, 42, 42, 6, 7, 3, "abcdef");
    printf("[%ld] [%lld] [%llu] [%llx] [%hhd] [%hd] [%zu]\n", 123456789L, -9876543210123LL, 18446744073709551615ULL, 0x1234567890abcdefULL, 300, 70000, (size_t)77);
    printf("[%f] [%.3f] [%e] [%g] [%g] [%10.4f] [%-8.2f|]\n", 3.14159265, 2.0 / 3.0, 12345.678, 0.0001, 1e20, -1.5, 2.25);
    printf("[%%] [%s] [%5.2s] [%c%c]\n", "", "hello", 'o', 'k');
    float fv = 1.25f;
    printf("float via varargs: %f %.1f\n", fv, (double)fv * 2);

    // strings
    char buf[128];
    strcpy(buf, "Hydrogen");
    strcat(buf, "VM");
    printf("strlen=%zu strcmp=%d strncmp=%d strchr=%s strrchr=%s strstr=%s\n", strlen(buf),
           strcmp(buf, "HydrogenVM") == 0, strncmp(buf, "Hydra", 4) == 0, strchr(buf, 'g'), strrchr(buf, 'n'), strstr(buf, "genV"));
    char *dup = strdup(buf);
    for (char *p = dup; *p; ++p) *p = (char)toupper((unsigned char)*p);
    printf("upper=%s casecmp=%d\n", dup, strcasecmp(dup, buf) == 0);
    free(dup);
    snprintf(buf, sizeof buf, "%s-%d-%.2f", "snp", 9, 0.5);
    printf("snprintf=%s len=%d\n", buf, snprintf(NULL, 0, "%d", 123456));
    printf("atoi=%d strtol=%ld strtoul=%lu strtod=%.3f atof=%.2f\n", atoi("  -77x"), strtol("0x1F", NULL, 16),
           strtoul("777", NULL, 8), strtod("3.25e2", NULL), atof("-0.5"));
    char *end;
    long v = strtol("123abc", &end, 10);
    printf("strtol end: %ld '%s'\n", v, end);
    memset(buf, 'x', 10); buf[10] = 0;
    memmove(buf + 2, buf, 5);
    printf("memset/move=%s memcmp=%d\n", buf, memcmp("abc", "abd", 3) < 0);

    // heap
    unsigned sum = 0;
    void *ptrs[200];
    for (int i = 0; i < 200; ++i) {
        ptrs[i] = malloc((size_t)(i * 37 % 500 + 1));
        memset(ptrs[i], i & 0xFF, (size_t)(i * 37 % 500 + 1));
    }
    for (int i = 0; i < 200; i += 2) { free(ptrs[i]); ptrs[i] = 0; }
    for (int i = 1; i < 200; i += 2) {
        ptrs[i] = realloc(ptrs[i], (size_t)(i * 37 % 500 + 1) * 3);
        sum += ((unsigned char *)ptrs[i])[0];
    }
    int *zero = calloc(100, sizeof(int));
    int zsum = 0;
    for (int i = 0; i < 100; ++i) zsum += zero[i];
    for (int i = 1; i < 200; i += 2) free(ptrs[i]);
    free(zero);
    printf("heap checksum=%u calloc zero=%d\n", sum, zsum);

    // qsort / bsearch with guest callbacks
    int arr[20];
    for (int i = 0; i < 20; ++i) arr[i] = (i * 7919) % 101 - 50;
    qsort(arr, 20, sizeof(int), cmp_int);
    printf("sorted:");
    for (int i = 0; i < 20; ++i) printf(" %d", arr[i]);
    int key = arr[13];
    int *found = bsearch(&key, arr, 20, sizeof(int), cmp_int);
    printf("\nbsearch index=%d\n", found ? (int)(found - arr) : -1);

    // setjmp / longjmp
    int r = setjmp(jb);
    if (r == 0) { dive(0); printf("unreachable\n"); }
    else printf("longjmp returned %d at depth %d\n", r, depth);

    // math (host libm on both sides)
    printf("math: %.6f %.6f %.6f %.6f %.6f %.6f\n", sin(1.0), cos(2.0), sqrt(2.0), pow(2.5, 3.3), atan2(1.0, -2.0), exp(1.5));
    printf("mathf: %.5f %.5f %.5f %.5f\n", (double)sinf(0.5f), (double)sqrtf(10.0f), (double)floorf(-2.5f), (double)fmodf(7.5f, 2.0f));

    // threads
    pthread_t th[4];
    for (long i = 0; i < 4; ++i) pthread_create(&th[i], NULL, worker, (void *)(i + 1));
    long rsum = 0;
    for (int i = 0; i < 4; ++i) { void *ret; pthread_join(th[i], &ret); rsum += (long)ret; }
    pthread_once(&once, once_fn);
    pthread_once(&once, once_fn);
    printf("threads: counter=%ld joined=%ld once=%d\n", shared_counter, rsum, once_count);

    // files
    const char *tmp = getenv("TMPDIR");
    if (tmp) {
        char path[512];
        snprintf(path, sizeof path, "%s/hvm_test.txt", tmp);
        FILE *f = fopen(path, "w");
        fprintf(f, "line one\nline %d\n", 2);
        fclose(f);
        f = fopen(path, "r");
        char line[64];
        int n = 0;
        while (fgets(line, sizeof line, f)) { line[strcspn(line, "\n")] = 0; printf("file[%d]=%s\n", n++, line); }
        fseek(f, 0, SEEK_END);
        printf("file size=%ld eof=%d\n", ftell(f), feof(f) != 0);
        fclose(f);
        remove(path);
        errno = 0;
        printf("missing file: %s errno=%d\n", fopen(path, "r") ? "opened?!" : "null", errno);
    }

    atexit(bye);
    fprintf(stderr, "stderr works\n");
    printf("exiting with 3\n");
    return 3;
}
