/* xorshift32 PRNG — 无堆分配, 同种子同序列(可测试) */
#ifndef CCG_RNG_H
#define CCG_RNG_H

#include <stdint.h>

typedef struct { uint32_t s; } rng_t;

void rng_seed(rng_t *r, uint64_t entropy);
uint32_t rng_next(rng_t *r);
/* [0, n) 均匀; n>0 */
uint32_t rng_range(rng_t *r, uint32_t n);

#endif
