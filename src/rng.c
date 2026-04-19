#include "rng.h"

/* xorshift32 — small, deterministic, plenty random for picking
 * scale degrees and pattern bits. */
static uint32_t state = 0x12345678UL;

void rng_seed(uint32_t s) {
    state = s ? s : 0xDEADBEEFUL;   /* xorshift can't have zero state */
}

uint32_t rng_next(void) {
    uint32_t x = state;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    state = x;
    return x;
}

int rng_range(int min, int max) {
    if (max <= min) return min;
    return min + (int)(rng_next() % (uint32_t)(max - min + 1));
}
