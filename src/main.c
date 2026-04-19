#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <conio.h>

#include "opl2.h"
#include "timer.h"
#include "rng.h"
#include "music.h"
#include "display.h"

#define MELODY_CH  0
#define BASS_CH    1
#define KICK_CH    6
#define SNARE_CH   7
#define HAT_CH     8

/* Drum pitches when running FM-voice drums. Tuned by ear-ish; expect
 * to iterate. */
#define KICK_NOTE  36   /* C2 */
#define SNARE_NOTE 60   /* C4 */
#define HAT_NOTE   84   /* C6 */

static bar_t       bars[BARS];
static drum_mode_t drum_mode = DRUM_FM;
static uint32_t    seed = 0x1337UL;

static void setup_voices(void) {
    opl_reset();
    opl_set_instrument(MELODY_CH, &OPL_INSTR_LEAD);
    opl_set_instrument(BASS_CH,   &OPL_INSTR_BASS);

    if (drum_mode == DRUM_FM) {
        opl_rhythm_mode(0);
        opl_set_instrument(KICK_CH,  &OPL_INSTR_KICK_FM);
        opl_set_instrument(SNARE_CH, &OPL_INSTR_SNARE_FM);
        opl_set_instrument(HAT_CH,   &OPL_INSTR_HAT_FM);
    } else {
        opl_set_instrument(6, &OPL_INSTR_RHY_BD);
        opl_set_instrument(7, &OPL_INSTR_RHY_SD_HH);
        opl_set_instrument(8, &OPL_INSTR_RHY_TT_TC);
        opl_rhythm_mode(1);
        opl_rhythm_setup_pitches();
    }
}

static void silence_all(void) {
    int ch;
    for (ch = 0; ch < OPL_CHANNELS; ch++) opl_note_off(ch);
    opl_rhythm_stop();
}

static void play_step(int cur_bar, int cur_step) {
    const bar_t *bar = &bars[cur_bar];
    int m = bar->melody[cur_step];
    int b = bar->bass[cur_step];
    uint8_t d = bar->drums[cur_step];

    if (m >= 0) {
        opl_note_off(MELODY_CH);
        opl_note_on(MELODY_CH, m);
    }
    if (b >= 0) {
        opl_note_off(BASS_CH);
        opl_note_on(BASS_CH, b);
    }

    if (!d) return;

    if (drum_mode == DRUM_FM) {
        if (d & DRUM_KICK)  { opl_note_off(KICK_CH);  opl_note_on(KICK_CH,  KICK_NOTE);  }
        if (d & DRUM_SNARE) { opl_note_off(SNARE_CH); opl_note_on(SNARE_CH, SNARE_NOTE); }
        if (d & DRUM_HAT)   { opl_note_off(HAT_CH);   opl_note_on(HAT_CH,   HAT_NOTE);   }
    } else {
        uint8_t bits = 0;
        if (d & DRUM_KICK)  bits |= OPL_RHY_BD;
        if (d & DRUM_SNARE) bits |= OPL_RHY_SD;
        if (d & DRUM_HAT)   bits |= OPL_RHY_HH;
        /* Stop-then-trigger forces a fresh attack on each hit. */
        opl_rhythm_stop();
        opl_rhythm_trigger(bits);
    }
}

int main(int argc, char **argv) {
    int      cur_bar = 0;
    int      cur_step = 0;
    int      total_steps = 0;
    uint32_t loop_start_ms = 0;
    int      quit = 0;

    if (argc >= 2) {
        seed = strtoul(argv[1], NULL, 0);
    }
    if (argc >= 3 && (strcmp(argv[2], "rhythm") == 0 || strcmp(argv[2], "RHYTHM") == 0)) {
        drum_mode = DRUM_RHYTHM;
    }

    rng_seed(seed);
    music_generate(bars, BARS);

    opl_init();
    setup_voices();
    timer_install();
    display_init(seed, drum_mode);

    loop_start_ms = timer_ms();
    play_step(cur_bar, cur_step);
    display_frame(cur_bar, cur_step, &bars[cur_bar]);
    total_steps = 1;

    while (!quit) {
        uint32_t now_us = (timer_ms() - loop_start_ms) * 1000UL;
        uint32_t target = (uint32_t)total_steps * STEP_US;

        if (now_us >= target) {
            cur_step++;
            if (cur_step >= STEPS_PER_BAR) {
                cur_step = 0;
                cur_bar++;
                if (cur_bar >= BARS) {
                    /* Loop. Reset the time/step accumulators so we
                     * never overflow on long playback sessions. */
                    cur_bar       = 0;
                    total_steps   = 0;
                    loop_start_ms = timer_ms();
                }
            }
            play_step(cur_bar, cur_step);
            display_frame(cur_bar, cur_step, &bars[cur_bar]);
            total_steps++;
        }

        if (kbhit()) {
            int k = getch();
            if (k == 27) {
                quit = 1;
            } else if (k == 'r' || k == 'R') {
                drum_mode = (drum_mode == DRUM_FM) ? DRUM_RHYTHM : DRUM_FM;
                silence_all();
                setup_voices();
                display_init(seed, drum_mode);
            } else if (k >= '1' && k <= '0' + NUM_VARIATIONS) {
                int v = k - '1';
                if (v != music_get_variation()) {
                    /* Re-seed before regenerating so the only thing
                     * that changes between variations is the variation
                     * itself, not the random draws. */
                    music_set_variation(v);
                    rng_seed(seed);
                    music_generate(bars, BARS);
                    silence_all();
                    cur_bar       = 0;
                    cur_step      = 0;
                    total_steps   = 1;
                    loop_start_ms = timer_ms();
                    display_init(seed, drum_mode);
                    play_step(cur_bar, cur_step);
                    display_frame(cur_bar, cur_step, &bars[cur_bar]);
                }
            }
        }
    }

    silence_all();
    opl_reset();
    timer_restore();
    display_cleanup();
    return 0;
}
