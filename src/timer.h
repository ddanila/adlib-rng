#ifndef TIMER_H
#define TIMER_H

#include <stdint.h>

void timer_install(void);
void timer_restore(void);
uint32_t timer_ms(void);

#endif
