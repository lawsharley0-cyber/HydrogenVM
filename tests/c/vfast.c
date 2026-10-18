// Element-wise float array loops. The NEON guest builds compile this with
// -ffast-math so clang will use NEON for float vectors; every expression is a
// single operation per element (or a multiply-add, which ARMv7 VMLA does
// unfused), so reassociation can't change results versus the host.
#include <stdint.h>

#define TEST(name) uint64_t name(void)
#define N 128

static inline uint32_t fb(float f) { union { float f; uint32_t u; } x = { f }; return x.u; }
static float X[N], Y[N], Z[N];

static void fill(void) {
    for (int i = 0; i < N; ++i) {
        X[i] = (float)(i - 64) * 0.375f;
        Y[i] = (float)((i * 37) % 101) * 0.0625f - 3.0f;
        Z[i] = 0;
    }
}
static uint64_t hashf(const float* a) { uint64_t h = 1; for (int i = 0; i < N; ++i) h = h * 1000003u + fb(a[i]); return h; }

TEST(vf_add_mul) {
    fill();
    for (int i = 0; i < N; ++i) Z[i] = X[i] + Y[i];
    uint64_t h = hashf(Z);
    for (int i = 0; i < N; ++i) Z[i] = X[i] * Y[i];
    h ^= hashf(Z) << 1;
    for (int i = 0; i < N; ++i) Z[i] = X[i] - Y[i];
    return h ^ (hashf(Z) << 2);
}

TEST(vf_saxpy) {
    fill();
    const float a = 1.75f;
    for (int i = 0; i < N; ++i) Z[i] = a * X[i] + Y[i];
    return hashf(Z);
}

TEST(vf_clamp_abs) {
    fill();
    for (int i = 0; i < N; ++i) {
        float v = X[i];
        v = v < -5.0f ? -5.0f : v;
        v = v > 7.5f ? 7.5f : v;
        Z[i] = v;
    }
    uint64_t h = hashf(Z);
    for (int i = 0; i < N; ++i) Z[i] = __builtin_fabsf(Y[i]);
    h ^= hashf(Z) << 1;
    for (int i = 0; i < N; ++i) Z[i] = -X[i];
    return h ^ (hashf(Z) << 2);
}

TEST(vf_select) {
    fill();
    for (int i = 0; i < N; ++i) Z[i] = X[i] > Y[i] ? X[i] : Y[i] * 2.0f;
    return hashf(Z);
}

TEST(vf_int_conv) {
    fill();
    int32_t r[N];
    for (int i = 0; i < N; ++i) r[i] = (int32_t)(X[i] * 4.0f);
    for (int i = 0; i < N; ++i) Z[i] = (float)r[i] * 0.5f;
    uint64_t h = hashf(Z);
    for (int i = 0; i < N; ++i) h = h * 31 + (uint32_t)r[i];
    return h;
}
