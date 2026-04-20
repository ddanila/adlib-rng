#ifndef DISPLAY_H
#define DISPLAY_H

#include <stdint.h>
#include "music.h"

/* Universal colour attributes every player uses. Byte layout:
 * (bg << 4) | fg; 0x0-0xF foreground palette. */
#define ATTR_NORMAL  0x07   /* light gray on black   */
#define ATTR_TITLE   0x0F   /* bright white          */
#define ATTR_LABEL   0x08   /* dark gray             */
#define ATTR_VALUE   0x0E   /* yellow                */

/* Low-level VGA text-mode primitives (80x25 colour buffer at
 * B800:0000). Shared across all players — each player composes its
 * own layout on top of these. Row/col are 0-based. */
void display_vga_begin(void);                        /* hide cursor, clear */
void display_vga_end(void);                          /* show cursor, home  */
void display_vga_clear(unsigned char attr);
void display_vga_putc(int row, int col, unsigned char attr, char ch);
void display_vga_puts(int row, int col, unsigned char attr, const char *s);
void display_vga_printf(int row, int col, unsigned char attr,
                        const char *fmt, ...);

/* RNG-player layout on top of the primitives. */
void display_init(uint32_t seed);
void display_frame(int cur_bar, int cur_step, const bar_t *bar);
void display_cleanup(void);

#endif
