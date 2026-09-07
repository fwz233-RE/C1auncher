#include "rng.h"

void rng_seed(rng_t *r, uint64_t entropy) {
    uint32_t s = (uint32_t)(entropy ^ (entropy >> 32));
    if (s == 0) s = 0x9e3779b9u;
    r->s = s;
}

uint32_t rng_next(rng_t *r) {
    uint32_t x = r->s;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    r->s = x;
    return x;
}

uint32_t rng_range(rng_t *r, uint32_t n) {
    if (n <= 1) return 0;
    /* 拒绝采样消除模偏置 */
    uint32_t limit = (uint32_t)(-1) / n * n;
    uint32_t v;
    do { v = rng_next(r); } while (v >= limit);
    return v % n;
}
