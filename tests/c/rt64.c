// 64-bit division helpers for guest test programs (no '/' or '%' on 64-bit
// values here, so the compiler can't turn these back into helper calls).
#include <stdint.h>

uint64_t rt_udivmod64(uint64_t n, uint64_t d, uint64_t* rem) {
    if (d == 0) { *rem = n; return 0; }
    uint64_t q = 0, r = 0;
    for (int i = 63; i >= 0; --i) {
        r = (r << 1) | ((n >> i) & 1);
        if (r >= d) { r -= d; q |= (uint64_t)1 << i; }
    }
    *rem = r;
    return q;
}

int64_t rt_sdivmod64(int64_t n, int64_t d, int64_t* rem) {
    int neg_q = (n < 0) != (d < 0), neg_r = n < 0;
    uint64_t un = n < 0 ? (uint64_t)0 - (uint64_t)n : (uint64_t)n;
    uint64_t ud = d < 0 ? (uint64_t)0 - (uint64_t)d : (uint64_t)d;
    uint64_t r;
    uint64_t q = rt_udivmod64(un, ud, &r);
    *rem = neg_r ? -(int64_t)r : (int64_t)r;
    return neg_q ? -(int64_t)q : (int64_t)q;
}
