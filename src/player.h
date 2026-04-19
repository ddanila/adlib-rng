#ifndef PLAYER_H
#define PLAYER_H

#include <stdint.h>

/* A player is anything that, given time, drives the OPL2. The RNG
 * generator and the VGM file reader are both players — main.c owns
 * the event loop and hands off to the selected player via this vtable. */
typedef struct player {
    const char *name;
    /* Returns 0 on success. `arg` is whatever main pulled off argv[1]
     * (a seed string for RNG, a filename for VGM, possibly NULL). The
     * player must leave a readable error on the display if it returns
     * non-zero, because main just exits. */
    int  (*init)(const char *arg);
    /* Called as often as main can spin. `now_ms` is monotonic from
     * timer_ms(). The player is responsible for its own scheduling. */
    void (*tick)(uint32_t now_ms);
    /* Optional; may be NULL. ESC is intercepted by main. */
    void (*on_key)(int k);
    void (*cleanup)(void);
} player_t;

extern const player_t PLAYER_RNG;
extern const player_t PLAYER_VGM;

#endif
