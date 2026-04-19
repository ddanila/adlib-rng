#ifndef DISPLAY_H
#define DISPLAY_H

#include <stdint.h>
#include "music.h"

void display_init(uint32_t seed);
void display_frame(int cur_bar, int cur_step, const bar_t *bar);
void display_cleanup(void);

#endif
