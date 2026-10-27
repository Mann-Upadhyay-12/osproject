#ifndef PRISM_HASH_H
#define PRISM_HASH_H

#include <stdint.h>
#include <stddef.h>

#define FNV_OFFSET_BASIS_64 0xcbf29ce484222325ULL
#define FNV_PRIME_64        0x100000001b3ULL

static inline uint64_t fnv1a_init(void) {
    return FNV_OFFSET_BASIS_64;
}

static inline uint64_t fnv1a_update(uint64_t hash, const void *data, size_t len) {
    const uint8_t *ptr = (const uint8_t *)data;
    for (size_t i = 0; i < len; ++i) {
        hash ^= (uint64_t)ptr[i];
        hash *= FNV_PRIME_64;
    }
    return hash;
}

#endif /* PRISM_HASH_H */
