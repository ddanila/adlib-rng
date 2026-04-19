#include "timer.h"
#include <dos.h>
#include <conio.h>
#include <i86.h>

/* Reprogram PIT counter 0 to 1 kHz so we get 1 ms resolution.
 * 1193182 Hz / 1000 Hz ≈ 1193 reload value. The default BIOS rate is
 * 18.2 Hz; we still need to call the original ISR at that rate so
 * BIOS clock and disk motor timeouts keep working, so we chain every
 * 55th tick. */
#define PIT_HZ        1000U
#define PIT_RELOAD    (1193182UL / PIT_HZ)
#define BIOS_DIVISOR  55                    /* 1000 / 18.2 ≈ 55 */

static volatile uint32_t ms_count;
static volatile uint16_t bios_count;
static void (__interrupt __far *old_isr)(void);

static void __interrupt __far new_isr(void) {
    ms_count++;
    bios_count++;
    if (bios_count >= BIOS_DIVISOR) {
        bios_count = 0;
        _chain_intr(old_isr);   /* original ISR sends EOI on its own */
    } else {
        outp(0x20, 0x20);       /* EOI to master PIC */
    }
}

static void pit_set_reload(uint16_t reload) {
    _disable();
    outp(0x43, 0x36);                       /* counter 0, mode 3, binary */
    outp(0x40, (uint8_t)(reload & 0xFF));
    outp(0x40, (uint8_t)((reload >> 8) & 0xFF));
    _enable();
}

void timer_install(void) {
    ms_count   = 0;
    bios_count = 0;
    old_isr = _dos_getvect(0x08);
    _dos_setvect(0x08, new_isr);
    pit_set_reload((uint16_t)PIT_RELOAD);
}

void timer_restore(void) {
    pit_set_reload(0);                      /* 0 = 65536 → default 18.2 Hz */
    _dos_setvect(0x08, old_isr);
}

uint32_t timer_ms(void) {
    uint32_t v;
    _disable();
    v = ms_count;
    _enable();
    return v;
}
