#ifndef SEEDS_H
#define SEEDS_H

#include <stdint.h>

#define SEED_COUNT 6

/* Curated seed bank bound to keys a..f at runtime. */
extern const uint32_t   SEED_BANK[SEED_COUNT];
extern const char *const SEED_LABEL[SEED_COUNT];

#endif
