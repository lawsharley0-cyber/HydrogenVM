// Floating-point and SIMD differential tests.
// Scalar float/double code exercises VFP; the integer array loops are written
// so clang auto-vectorises them into NEON in the -mfpu=neon -O3 builds.
// No denormals, NaNs or out-of-range conversions: those differ by design
// between NEON (flush-to-zero) and the host.
#include <stdint.h>

#define NOINLINE __attribute__((noinline))
#define TEST(name) uint64_t name(void)

static inline uint32_t fb(float f) { union { float f; uint32_t u; } x = { f }; return x.u; }
static inline uint64_t db(double d) { union { double d; uint64_t u; } x = { d }; return x.u; }

// --------------------------------------------------------------- scalar FP ---

TEST(f_series) {
    float sf = 0; double sd = 0;
    for (int n = 1; n <= 2000; ++n) { sf += 1.0f / (float)(n * n); sd += 1.0 / ((double)n * n); }
    return ((uint64_t)fb(sf) << 32) ^ db(sd);
}

TEST(f_sqrt_newton) {
    uint64_t h = 0;
    for (int i = 1; i < 200; ++i) {
        float x = (float)i * 0.37f;
        float g = x;
        for (int k = 0; k < 8; ++k) g = 0.5f * (g + x / g);
        double y = (double)i * 1.618;
        h = h * 31 + fb(g) + fb(__builtin_sqrtf(x)) + (db(__builtin_sqrt(y)) >> 7);
    }
    return h;
}

TEST(f_conversions) {
    uint64_t h = 0;
    volatile float fs[] = { 0.0f, 1.5f, -1.5f, 2.5f, -2.5f, 123456.78f, -98765.4f, 0.999f, 2147483520.0f, -2147483648.0f };
    volatile double ds[] = { 3.75, -3.75, 1e9, -1e9, 4294967040.0, 0.25 };
    for (int i = 0; i < 10; ++i) {
        float f = fs[i];
        h = h * 33 + (uint32_t)(int32_t)f;
        if (f >= 0) h += (uint32_t)f;
        h ^= db((double)f);
    }
    for (int i = 0; i < 6; ++i) {
        double d = ds[i];
        if (d >= -2147483648.0 && d <= 2147483647.0) h = h * 17 + (uint32_t)(int32_t)d;
        if (d >= 0) h = h * 13 + (uint32_t)d;
        h ^= fb((float)d);
    }
    for (int32_t i = -1000; i < 1000; i += 37) {
        h = h * 7 + fb((float)i) + db((double)(uint32_t)(i + 5000));
    }
    return h;
}

static NOINLINE void sort_f(float* a, int n) {
    for (int i = 1; i < n; ++i) {
        float v = a[i]; int j = i - 1;
        while (j >= 0 && a[j] > v) { a[j + 1] = a[j]; --j; }
        a[j + 1] = v;
    }
}
TEST(f_compare_sort) {
    float a[64];
    uint32_t x = 99;
    for (int i = 0; i < 64; ++i) { x = x * 1664525u + 1013904223u; a[i] = (float)(int32_t)(x >> 8) / 1024.0f; }
    sort_f(a, 64);
    uint64_t h = 0;
    for (int i = 0; i < 64; ++i) {
        h = h * 31 + fb(a[i]);
        if (i && !(a[i - 1] <= a[i])) return 0xBAD;
    }
    float mn = a[0], mx = a[0];
    for (int i = 0; i < 64; ++i) { mn = a[i] < mn ? a[i] : mn; mx = a[i] > mx ? a[i] : mx; }
    return h ^ fb(mn) ^ ((uint64_t)fb(mx) << 20);
}

typedef struct { float x, y, z, w; } vec4;
typedef struct { float m[16]; } mat4;
static NOINLINE mat4 mat_mul(mat4 a, mat4 b) {
    mat4 r;
    for (int i = 0; i < 4; ++i)
        for (int j = 0; j < 4; ++j) {
            float s = 0;
            for (int k = 0; k < 4; ++k) s += a.m[i * 4 + k] * b.m[k * 4 + j];
            r.m[i * 4 + j] = s;
        }
    return r;
}
static NOINLINE vec4 normalize(vec4 v) {
    float l = __builtin_sqrtf(v.x * v.x + v.y * v.y + v.z * v.z + v.w * v.w);
    vec4 r = { v.x / l, v.y / l, v.z / l, v.w / l };
    return r;
}
TEST(f_matrix_vec) {
    mat4 a, b;
    for (int i = 0; i < 16; ++i) { a.m[i] = (float)(i % 5) * 0.25f - 0.5f; b.m[i] = (float)((i * 7) % 11) * 0.125f; }
    for (int k = 0; k < 10; ++k) a = mat_mul(a, b);
    vec4 v = { a.m[0], a.m[5], a.m[10], a.m[15] + 1.0f };
    v = normalize(v);
    uint64_t h = 0;
    for (int i = 0; i < 16; ++i) h = h * 3 + fb(a.m[i]);
    return h ^ fb(v.x) ^ ((uint64_t)fb(v.y) << 8) ^ ((uint64_t)fb(v.z) << 16) ^ ((uint64_t)fb(v.w) << 24);
}

TEST(f_physics) {
    float px = 0, py = 100, vx = 3.5f, vy = 0;
    const float g = -9.81f, dt = 1.0f / 60.0f, bounce = 0.8f;
    int bounces = 0;
    for (int i = 0; i < 2000; ++i) {
        vy += g * dt;
        px += vx * dt; py += vy * dt;
        if (py < 0) { py = -py; vy = -vy * bounce; ++bounces; }
        vx *= 0.999f;
    }
    return ((uint64_t)fb(px) << 32) ^ fb(py) ^ ((uint64_t)bounces << 16);
}

TEST(f_mandelbrot) {
    uint32_t total = 0;
    for (int yi = 0; yi < 24; ++yi)
        for (int xi = 0; xi < 32; ++xi) {
            double cr = -2.0 + xi * (2.5 / 32), ci = -1.0 + yi * (2.0 / 24);
            double zr = 0, zi = 0;
            int it = 0;
            while (it < 100 && zr * zr + zi * zi < 4.0) { double t = zr * zr - zi * zi + cr; zi = 2 * zr * zi + ci; zr = t; ++it; }
            total = total * 3 + (uint32_t)it;
        }
    return total;
}

TEST(f_horner_double) {
    static const double c[] = { 1.0, -0.5, 0.041666666666666664, -0.001388888888888889, 2.48015873015873e-05 };
    uint64_t h = 0;
    for (int i = -50; i <= 50; ++i) {
        double x = i * 0.0625, x2 = x * x, r = c[4];
        for (int k = 3; k >= 0; --k) r = r * x2 + c[k];
        h = h * 31 + db(r);
    }
    return h;
}

TEST(f_signs_consts) {
    volatile float z = 0.0f, one = 1.0f;
    float nz = -z;
    uint64_t h = fb(nz);
    h = h * 3 + fb(__builtin_fabsf(-2.75f * one)) + fb(-(0.5f * one)) + fb(2.0f * one + 0.125f);
    h = h * 3 + db(-(double)one) + db(1.0 / 3.0 * one);
    h = h * 3 + fb(__builtin_copysignf(3.0f, nz));
    return h;
}

typedef struct { float r, g, b; } color;
static NOINLINE color blend(color a, color b, float t) {
    color c = { a.r + (b.r - a.r) * t, a.g + (b.g - a.g) * t, a.b + (b.b - a.b) * t };
    return c;
}
TEST(f_struct_args) {
    color a = { 1.0f, 0.25f, 0.0f }, b = { 0.0f, 0.75f, 1.0f }, c = a;
    for (int i = 0; i < 50; ++i) c = blend(c, (i & 1) ? a : b, 0.3f);
    return ((uint64_t)fb(c.r) << 32) ^ ((uint64_t)fb(c.g) << 16) ^ fb(c.b);
}

TEST(f_fixed_point) {
    uint64_t h = 0;
    for (int i = -40; i < 40; ++i) {
        float f = (float)i * 0.7f;
        int32_t fx = (int32_t)(f * 65536.0f);
        float back = (float)fx / 65536.0f;
        int16_t q15 = (int16_t)(int32_t)(f * 0.01f * 32768.0f);
        h = h * 31 + (uint32_t)fx + fb(back) + (uint16_t)q15;
    }
    return h;
}

// ------------------------------------------------- vectorisable integers ---

#define N 256
static int32_t A32[N], B32[N], C32[N];
static int16_t A16[N], B16[N];
static uint8_t U8a[N], U8b[N], U8c[N];

static void fill(void) {
    uint32_t x = 7;
    for (int i = 0; i < N; ++i) {
        x = x * 1103515245u + 12345u;
        A32[i] = (int32_t)(x >> 3) - (1 << 27); B32[i] = (int32_t)(x % 2001) - 1000; C32[i] = i * 3 - 100;
        A16[i] = (int16_t)(x >> 16); B16[i] = (int16_t)((x >> 7) & 0x3FFF) - 0x2000;
        U8a[i] = (uint8_t)(x >> 11); U8b[i] = (uint8_t)(x >> 19); U8c[i] = 0;
    }
}
static uint64_t hash32(const int32_t* a, int n) { uint64_t h = 1; for (int i = 0; i < n; ++i) h = h * 1000003u + (uint32_t)a[i]; return h; }
static uint64_t hash16(const int16_t* a, int n) { uint64_t h = 1; for (int i = 0; i < n; ++i) h = h * 1000003u + (uint16_t)a[i]; return h; }
static uint64_t hash8(const uint8_t* a, int n) { uint64_t h = 1; for (int i = 0; i < n; ++i) h = h * 131u + a[i]; return h; }

TEST(v_mla32) {
    fill();
    int32_t r[N];
    for (int i = 0; i < N; ++i) r[i] = A32[i] + B32[i] * C32[i];
    for (int i = 0; i < N; ++i) r[i] -= (B32[i] << 3) ^ (A32[i] >> 5);
    return hash32(r, N);
}

TEST(v_sat_u8) {
    fill();
    for (int i = 0; i < N; ++i) { int s = U8a[i] + U8b[i]; U8c[i] = (uint8_t)(s > 255 ? 255 : s); }
    uint64_t h = hash8(U8c, N);
    for (int i = 0; i < N; ++i) { int s = U8a[i] - U8b[i]; U8c[i] = (uint8_t)(s < 0 ? 0 : s); }
    return h ^ (hash8(U8c, N) << 1);
}

TEST(v_avg_minmax_u8) {
    fill();
    uint8_t avg[N], mn[N], mx[N], ad[N];
    for (int i = 0; i < N; ++i) {
        avg[i] = (uint8_t)((U8a[i] + U8b[i] + 1) >> 1);
        mn[i] = U8a[i] < U8b[i] ? U8a[i] : U8b[i];
        mx[i] = U8a[i] > U8b[i] ? U8a[i] : U8b[i];
        ad[i] = (uint8_t)(U8a[i] > U8b[i] ? U8a[i] - U8b[i] : U8b[i] - U8a[i]);
    }
    return hash8(avg, N) ^ (hash8(mn, N) << 1) ^ (hash8(mx, N) << 2) ^ (hash8(ad, N) << 3);
}

TEST(v_dot16) {
    fill();
    int32_t acc = 0;
    for (int i = 0; i < N; ++i) acc += (int32_t)A16[i] * B16[i];
    int64_t acc64 = 0;
    for (int i = 0; i < N; ++i) acc64 += (int64_t)A32[i] * B32[i];
    return (uint32_t)acc ^ (uint64_t)acc64;
}

TEST(v_abs_shift16) {
    fill();
    int16_t r[N];
    for (int i = 0; i < N; ++i) {
        int16_t v = A16[i];
        int16_t a = (int16_t)(v < 0 ? -v : v);
        r[i] = (int16_t)((a >> 2) + (int16_t)(B16[i] << 1));
    }
    return hash16(r, N);
}

TEST(v_widen_narrow) {
    fill();
    int16_t w[N];
    int32_t s[N];
    uint8_t nb[N];
    for (int i = 0; i < N; ++i) w[i] = (int16_t)(U8a[i] * 3 - U8b[i]);
    for (int i = 0; i < N; ++i) s[i] = (int32_t)w[i] * 1000;
    for (int i = 0; i < N; ++i) nb[i] = (uint8_t)(s[i] >> 4);
    return hash16(w, N) ^ (hash32(s, N) << 1) ^ (hash8(nb, N) << 2);
}

TEST(v_bswap32) {
    fill();
    uint32_t r[N];
    for (int i = 0; i < N; ++i) r[i] = __builtin_bswap32((uint32_t)A32[i]);
    return hash32((int32_t*)r, N);
}

TEST(v_deinterleave_rgb) {
    uint8_t rgb[3 * 128], gray[128];
    uint8_t rg[2 * 128], sum2[128];
    uint8_t rgba[4 * 64], a4[64];
    for (int i = 0; i < 3 * 128; ++i) rgb[i] = (uint8_t)(i * 37 + 11);
    for (int i = 0; i < 2 * 128; ++i) rg[i] = (uint8_t)(i * 91 + 3);
    for (int i = 0; i < 4 * 64; ++i) rgba[i] = (uint8_t)(i * 53 + 7);
    for (int i = 0; i < 128; ++i)
        gray[i] = (uint8_t)((rgb[3 * i] * 77 + rgb[3 * i + 1] * 150 + rgb[3 * i + 2] * 29) >> 8);
    for (int i = 0; i < 128; ++i) sum2[i] = (uint8_t)(rg[2 * i] + 2 * rg[2 * i + 1]);
    for (int i = 0; i < 64; ++i) a4[i] = (uint8_t)((rgba[4 * i] + rgba[4 * i + 1] + rgba[4 * i + 2]) / 3 ^ rgba[4 * i + 3]);
    return hash8(gray, 128) ^ (hash8(sum2, 128) << 1) ^ (hash8(a4, 64) << 2);
}

TEST(v_compare_select) {
    fill();
    int32_t r[N];
    for (int i = 0; i < N; ++i) r[i] = A32[i] > B32[i] * 1000 ? A32[i] : B32[i] - 1;
    int16_t q[N];
    for (int i = 0; i < N; ++i) q[i] = A16[i] == B16[i] ? 7 : (A16[i] < B16[i] ? A16[i] : B16[i]);
    return hash32(r, N) ^ (hash16(q, N) << 1);
}

TEST(v_count_bytes) {
    fill();
    uint32_t eq = 0, pc = 0;
    for (int i = 0; i < N; ++i) eq += (U8a[i] & 7) == (U8b[i] & 7);
    for (int i = 0; i < N; ++i) pc += (uint32_t)__builtin_popcount(U8a[i]);
    uint32_t sum = 0;
    for (int i = 0; i < N; ++i) sum += U8a[i];
    return ((uint64_t)eq << 40) | ((uint64_t)pc << 20) | sum;
}

TEST(v_shift_var) {
    fill();
    uint32_t r[N];
    for (int i = 0; i < N; ++i) r[i] = ((uint32_t)A32[i] << (i & 15)) | ((uint32_t)B32[i] >> (i & 7));
    int32_t s[N];
    for (int i = 0; i < N; ++i) s[i] = A32[i] >> (i & 31);
    return hash32((int32_t*)r, N) ^ (hash32(s, N) << 1);
}

TEST(v_u16_mix) {
    uint16_t a[N], b[N], r[N];
    for (int i = 0; i < N; ++i) { a[i] = (uint16_t)(i * 2654435761u >> 16); b[i] = (uint16_t)(i * 40503u); }
    for (int i = 0; i < N; ++i) {
        uint32_t m = (uint32_t)a[i] * b[i];
        r[i] = (uint16_t)(m >> 16);
    }
    uint64_t h = 1;
    for (int i = 0; i < N; ++i) h = h * 1000003u + r[i];
    return h;
}

TEST(v_sum_squares64) {
    fill();
    uint64_t s = 0;
    for (int i = 0; i < N; ++i) s += (uint64_t)((int64_t)B32[i] * B32[i]);
    uint64_t t = 0;
    for (int i = 0; i < N; ++i) t += (uint64_t)(uint32_t)A32[i];
    return s ^ (t << 1);
}
