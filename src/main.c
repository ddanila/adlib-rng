#include <conio.h>
#include <string.h>

#include "opl2.h"
#include "timer.h"
#include "player.h"

/* Argument convention: filenames contain a dot (FOO.VGM, FOO.BAR).
 * Anything else is a numeric seed for the RNG player. Keeps DOS
 * usage natural — ADLIB, ADLIB 42, ADLIB SUSPENSE.VGM. */
static const player_t *pick_player(const char *arg) {
    if (arg && strchr(arg, '.') != 0) return &PLAYER_VGM;
    return &PLAYER_RNG;
}

int main(int argc, char **argv) {
    const player_t *player;
    const char     *arg  = NULL;
    int             quit = 0;

    if (argc >= 2) arg = argv[1];
    player = pick_player(arg);

    opl_init();
    timer_install();

    if (player->init(arg) != 0) {
        timer_restore();
        opl_reset();
        return 1;
    }

    while (!quit) {
        player->tick(timer_ms());

        if (kbhit()) {
            int k = getch();
            if (k == 27) {
                quit = 1;
            } else if (player->on_key) {
                player->on_key(k);
            }
        }
    }

    player->cleanup();
    opl_reset();
    timer_restore();
    return 0;
}
