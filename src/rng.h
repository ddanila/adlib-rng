#ifndef RNG_H
#define RNG_H

#include <stdint.h>

void     rng_seed(uint32_t s);
uint32_t rng_next(void);
int      rng_range(int min, int max);   /* inclusive */

#endif
