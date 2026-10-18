// Differential tests: compiled once for the host (to produce expected values)
// and several times for the guest (ARM, Thumb-2, Thumb-2 + hardware divide),
// then run in ArgonVM and compared. Everything uses fixed-width types and
// avoids undefined behaviour so host and guest results must match exactly.
#include <stdint.h>

#define NOINLINE __attribute__((noinline))
#define TEST(name) uint64_t name(void)

static volatile uint32_t g_sink;

// ------------------------------------------------------------ arithmetic ---

static NOINLINE uint32_t fib(uint32_t n) { return n < 2 ? n : fib(n - 1) + fib(n - 2); }
TEST(t_fib_recursive) { return fib(22); }

TEST(t_fib_iter64) {
    uint64_t a = 0, b = 1;
    for (int i = 0; i < 90; ++i) { uint64_t t = a + b; a = b; b = t; }
    return a;
}

static NOINLINE uint32_t gcd(uint32_t a, uint32_t b) { while (b) { uint32_t t = a % b; a = b; b = t; } return a; }
TEST(t_gcd) {
    uint32_t acc = 0;
    for (uint32_t i = 1; i < 200; ++i) acc += gcd(i * 7919u, 1000u + i * 31u);
    return acc;
}

TEST(t_divmod_mix) {
    uint64_t acc = 0;
    volatile int32_t ns[] = { 100, -100, 7, -7, 2147483647, -2147483647 - 1, 0, 13 };
    volatile int32_t ds[] = { 3, -3, 7, -1, 2, 1000, -13 };
    for (int i = 0; i < 8; ++i)
        for (int j = 0; j < 7; ++j) {
            int32_t n = ns[i], d = ds[j];
            if (n == -2147483647 - 1 && d == -1) continue;
            acc = acc * 31 + (uint32_t)(n / d);
            acc = acc * 31 + (uint32_t)(n % d);
            acc = acc * 31 + ((uint32_t)n / (uint32_t)d);
            acc = acc * 31 + ((uint32_t)n % (uint32_t)d);
        }
    return acc;
}

TEST(t_div64) {
    volatile uint64_t n = 0xFEDCBA9876543210ull, d = 0x12345ull;
    volatile int64_t sn = -1234567890123ll, sd = 977;
    return (n / d) ^ (n % d) ^ (uint64_t)(sn / sd) ^ ((uint64_t)(sn % sd) << 7);
}

TEST(t_mul64) {
    uint64_t acc = 1;
    volatile uint32_t xs[] = { 0xFFFFFFFFu, 0x12345678u, 3u, 0x80000001u };
    for (int i = 0; i < 4; ++i)
        for (int j = 0; j < 4; ++j) {
            acc ^= (uint64_t)xs[i] * xs[j];
            acc += (uint64_t)((int64_t)(int32_t)xs[i] * (int32_t)xs[j]);
            acc = (acc << 3) | (acc >> 61);
        }
    return acc;
}

TEST(t_add64_carry) {
    uint64_t a = 0xFFFFFFFF00000001ull, acc = 0;
    for (uint32_t i = 0; i < 1000; ++i) { a += 0x00000000FFFFFFFFull + i; acc ^= a; a -= i * 3; }
    return acc;
}

TEST(t_shift64) {
    uint64_t acc = 0;
    volatile uint64_t v = 0x8123456789ABCDEFull;
    for (uint32_t s = 0; s < 64; ++s) {
        acc += v << s;
        acc ^= v >> s;
        acc += (uint64_t)((int64_t)v >> s);
    }
    return acc;
}

TEST(t_signed_ops) {
    int32_t acc = 0;
    volatile int32_t vals[] = { -5, 17, -2147483647, 32767, -32768, 255, -1 };
    for (int i = 0; i < 7; ++i) {
        int32_t v = vals[i];
        acc ^= v >> 3;
        acc += (v < 0) ? -v / 2 : v / 3;
        acc ^= (int32_t)((uint32_t)v << 2);
    }
    return (uint32_t)acc;
}

// ------------------------------------------------------------- bit tricks ---

TEST(t_popcount_clz) {
    uint32_t acc = 0;
    for (uint32_t i = 1; i < 5000; i += 7) {
        uint32_t v = i * 2654435761u;
        acc += (uint32_t)__builtin_popcount(v) * 3 + (uint32_t)__builtin_clz(v) + (uint32_t)__builtin_ctz(v);
    }
    return acc;
}

TEST(t_bswap_rotate) {
    uint32_t v = 0x01234567u, acc = 0;
    for (int i = 0; i < 100; ++i) {
        v = __builtin_bswap32(v) ^ ((v << 5) | (v >> 27));
        acc += (uint16_t)__builtin_bswap16((uint16_t)v);
        acc ^= (v >> 13) | (v << 19);
    }
    return ((uint64_t)acc << 32) | v;
}

TEST(t_bitfields) {
    struct { uint32_t a : 3, b : 7, c : 13, d : 9; } s = {0};
    struct { int32_t x : 5; int32_t y : 11; } t = {0};
    uint32_t acc = 0;
    for (uint32_t i = 0; i < 300; ++i) {
        s.a = i; s.b = i * 3; s.c = i * 77; s.d = i ^ 0x155;
        t.x = (int32_t)(i & 31) - 16; t.y = (int32_t)(i * 5 & 2047) - 1024;
        acc = acc * 33 + s.a + s.b + s.c + s.d + (uint32_t)t.x + (uint32_t)t.y;
    }
    return acc;
}

TEST(t_crc32) {
    uint32_t table[256];
    for (uint32_t i = 0; i < 256; ++i) {
        uint32_t c = i;
        for (int k = 0; k < 8; ++k) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
        table[i] = c;
    }
    const char* s = "The quick brown fox jumps over the lazy dog";
    uint32_t crc = 0xFFFFFFFFu;
    for (const char* p = s; *p; ++p) crc = table[(crc ^ (uint8_t)*p) & 0xFF] ^ (crc >> 8);
    return crc ^ 0xFFFFFFFFu;  // 0x414FA339
}

TEST(t_xorshift) {
    uint32_t x = 2463534242u, acc = 0;
    for (int i = 0; i < 10000; ++i) { x ^= x << 13; x ^= x >> 17; x ^= x << 5; acc += x; }
    return ((uint64_t)x << 32) | acc;
}

// -------------------------------------------------------- memory / arrays ---

static NOINLINE void qsort_u32(uint32_t* a, int lo, int hi) {
    while (lo < hi) {
        uint32_t p = a[(lo + hi) / 2];
        int i = lo, j = hi;
        while (i <= j) {
            while (a[i] < p) ++i;
            while (a[j] > p) --j;
            if (i <= j) { uint32_t t = a[i]; a[i] = a[j]; a[j] = t; ++i; --j; }
        }
        if (j - lo < hi - i) { qsort_u32(a, lo, j); lo = i; }
        else { qsort_u32(a, i, hi); hi = j; }
    }
}
TEST(t_quicksort) {
    uint32_t a[500];
    uint32_t x = 12345;
    for (int i = 0; i < 500; ++i) { x = x * 1103515245u + 12345u; a[i] = x >> 8; }
    qsort_u32(a, 0, 499);
    uint64_t h = 0;
    for (int i = 0; i < 500; ++i) { h = h * 1000003u + a[i]; if (i && a[i - 1] > a[i]) return 0xBAD; }
    return h;
}

static NOINLINE void bubble_i16(int16_t* a, int n) {
    for (int i = 0; i < n; ++i)
        for (int j = 0; j + 1 < n - i; ++j)
            if (a[j] > a[j + 1]) { int16_t t = a[j]; a[j] = a[j + 1]; a[j + 1] = t; }
}
TEST(t_bubble_i16) {
    int16_t a[64];
    for (int i = 0; i < 64; ++i) a[i] = (int16_t)((i * 7919) ^ 0x5A5A);
    bubble_i16(a, 64);
    uint32_t h = 0;
    for (int i = 0; i < 64; ++i) h = h * 31 + (uint32_t)(int32_t)a[i];
    return h;
}

TEST(t_strings) {
    char buf[64];
    const char* src = "ArgonVM: legacy iOS on modern silicon";
    int n = 0;
    while (src[n]) { buf[n] = src[n]; ++n; }
    buf[n] = 0;
    for (int i = 0, j = n - 1; i < j; ++i, --j) { char t = buf[i]; buf[i] = buf[j]; buf[j] = t; }
    uint32_t h = 5381;
    for (int i = 0; buf[i]; ++i) {
        uint8_t ch = (uint8_t)buf[i];
        if (ch >= 'a' && ch <= 'z') ch = (uint8_t)(ch - 32);
        h = h * 33 ^ ch;
    }
    return ((uint64_t)n << 32) | h;
}

struct Vec { int32_t x, y, z, w; uint8_t tag; int16_t s; };
static NOINLINE struct Vec vadd(struct Vec a, struct Vec b) {
    struct Vec r = { a.x + b.x, a.y + b.y, a.z + b.z, a.w + b.w, (uint8_t)(a.tag ^ b.tag), (int16_t)(a.s - b.s) };
    return r;
}
TEST(t_struct_copy) {
    struct Vec acc = { 1, 2, 3, 4, 5, 6 };
    struct Vec arr[8];
    for (int i = 0; i < 8; ++i) { struct Vec v = { i, -i, i * i, 7, (uint8_t)i, (int16_t)(-3 * i) }; arr[i] = v; }
    for (int k = 0; k < 50; ++k) acc = vadd(acc, arr[k & 7]);
    return (uint32_t)acc.x ^ ((uint32_t)acc.y << 3) ^ ((uint32_t)acc.z << 7) ^ (uint32_t)acc.w ^ acc.tag ^ (uint32_t)(int32_t)acc.s;
}

TEST(t_matrix) {
    int32_t a[8][8], b[8][8], c[8][8];
    for (int i = 0; i < 8; ++i) for (int j = 0; j < 8; ++j) { a[i][j] = i * 3 - j; b[i][j] = (i ^ j) - 4; }
    for (int i = 0; i < 8; ++i) for (int j = 0; j < 8; ++j) {
        int32_t s = 0;
        for (int k = 0; k < 8; ++k) s += a[i][k] * b[k][j];
        c[i][j] = s;
    }
    uint32_t h = 0;
    for (int i = 0; i < 8; ++i) for (int j = 0; j < 8; ++j) h = h * 7 + (uint32_t)c[i][j];
    return h;
}

TEST(t_sign_extend) {
    uint32_t acc = 0;
    volatile uint8_t bytes[] = { 0x00, 0x7F, 0x80, 0xFF, 0x12, 0xA5 };
    volatile uint16_t halves[] = { 0x0000, 0x7FFF, 0x8000, 0xFFFF, 0x1234, 0xA5A5 };
    for (int i = 0; i < 6; ++i) {
        acc = acc * 3 + (uint32_t)(int32_t)(int8_t)bytes[i];
        acc = acc * 5 + (uint32_t)(int32_t)(int16_t)halves[i];
        acc += bytes[i] + halves[i];
    }
    return acc;
}

// ------------------------------------------------------------ control flow ---

static NOINLINE uint32_t dispatch(uint32_t op, uint32_t a, uint32_t b) {
    switch (op) {
        case 0: return a + b;
        case 1: return a - b;
        case 2: return a * b;
        case 3: return a ^ b;
        case 4: return a | b;
        case 5: return a & ~b;
        case 6: return a << (b & 31);
        case 7: return a >> (b & 31);
        case 8: return b ? a / b : 0;
        case 9: return ~a;
        case 10: return a + 1000;
        case 11: return b - a;
        default: return 0xDEAD;
    }
}
TEST(t_switch_table) {
    uint32_t acc = 7;
    for (uint32_t i = 0; i < 400; ++i) acc = dispatch(i % 14, acc, i + 3) + (acc >> 7);
    return acc;
}

static NOINLINE uint32_t sq(uint32_t x) { return x * x; }
static NOINLINE uint32_t cube(uint32_t x) { return x * x * x; }
static NOINLINE uint32_t neg(uint32_t x) { return 0u - x; }
TEST(t_function_pointers) {
    uint32_t (*volatile fns[3])(uint32_t) = { sq, cube, neg };
    uint32_t acc = 0;
    for (uint32_t i = 0; i < 99; ++i) acc += fns[i % 3](i);
    return acc;
}

static NOINLINE uint32_t ackermann(uint32_t m, uint32_t n) {
    if (m == 0) return n + 1;
    if (n == 0) return ackermann(m - 1, 1);
    return ackermann(m - 1, ackermann(m, n - 1));
}
TEST(t_ackermann) { return ackermann(2, 300) + ackermann(3, 5); }

TEST(t_clamp_saturate) {
    uint32_t acc = 0;
    for (int32_t i = -70000; i < 70000; i += 333) {
        int32_t s16 = i > 32767 ? 32767 : (i < -32768 ? -32768 : i);
        int32_t u8 = i > 255 ? 255 : (i < 0 ? 0 : i);
        acc = acc * 17 + (uint32_t)s16 + (uint32_t)u8;
    }
    return acc;
}

TEST(t_audio_mix) {
    int16_t a[128], b[128], out[128];
    for (int i = 0; i < 128; ++i) { a[i] = (int16_t)(i * 517 - 30000); b[i] = (int16_t)(20000 - i * 311); }
    for (int i = 0; i < 128; ++i) {
        int32_t s = ((int32_t)a[i] * 23170 >> 15) + ((int32_t)b[i] * 23170 >> 15) + a[i] / 2;
        out[i] = (int16_t)(s > 32767 ? 32767 : s < -32768 ? -32768 : s);
    }
    uint32_t h = 0;
    for (int i = 0; i < 128; ++i) h = h * 31 + (uint16_t)out[i];
    return h;
}

TEST(t_global_state) {
    static uint32_t counter;
    static uint8_t bytes[37];
    for (uint32_t i = 0; i < 1000; ++i) { counter += i; bytes[i % 37] += (uint8_t)i; }
    uint32_t h = counter;
    for (int i = 0; i < 37; ++i) h = h * 3 + bytes[i];
    g_sink = h;
    return h;
}

TEST(t_atomics) {
    static uint32_t v;
    static uint64_t v64;
    uint32_t acc = 0;
    for (uint32_t i = 0; i < 100; ++i) {
        acc += __atomic_fetch_add(&v, i, __ATOMIC_SEQ_CST);
        acc ^= __atomic_exchange_n(&v, acc, __ATOMIC_SEQ_CST);
        uint32_t expected = acc;
        __atomic_compare_exchange_n(&v, &expected, i, 0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
        acc += expected;
        __atomic_fetch_add(&v64, (uint64_t)i << 20, __ATOMIC_SEQ_CST);
    }
    return ((uint64_t)acc << 32) ^ v64 ^ v;
}
