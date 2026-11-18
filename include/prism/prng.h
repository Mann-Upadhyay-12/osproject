#ifndef PRISM_PRNG_H
#define PRISM_PRNG_H

#include <stdint.h>

static inline uint64_t prng_next(uint64_t *state) {
    uint64_t x = *state;
    if (x == 0) x = 0x8a5cd789635d2dffULL;
    x ^= x >> 12;
    x ^= x << 25;
    x ^= x >> 27;
    *state = x;
    return x * 0x2545F4914F6CDD1DULL;
}

static inline uint32_t prng_range(uint64_t *state, uint32_t min, uint32_t max) {
    if (min >= max) return min;
    uint64_t r = prng_next(state);
    return min + (uint32_t)(r % (uint64_t)(max - min + 1));
}

#endif
