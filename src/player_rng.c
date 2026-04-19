#include "player.h"
#include "opl2.h"
#include "timer.h"
#include "rng.h"
#include "music.h"
#include "display.h"
#include "seeds.h"

#include <stdlib.h>

#define MELODY_CH  0
#define BASS_CH    1
#define LEAD2_CH   2   /* harmony / fills voice (see music.c) */

static bar_t    bars[BARS];
static uint32_t seed = 0x1337UL;
static int      cur_bar;
static int      cur_step;
static int      total_steps;
static uint32_t loop_start_ms;

static void setup_voices(void) {
    opl_reset();
    opl_set_instrument(MELODY_CH, &OPL_INSTR_LEAD);
    opl_set_instrument(BASS_CH,   &OPL_INSTR_BASS);
    opl_set_instrument(LEAD2_CH,  &OPL_INSTR_LEAD_BRIGHT);
    opl_set_instrument(6, &OPL_INSTR_RHY_BD);
    opl_set_instrument(7, &OPL_INSTR_RHY_SD_HH);
    opl_set_instrument(8, &OPL_INSTR_RHY_TT_TC);
    opl_rhythm_mode(1);
    opl_rhythm_setup_pitches();
}

static void silence_all(void) {
    int ch;
    for (ch = 0; ch < OPL_CHANNELS; ch++) opl_note_off(ch);
    opl_rhythm_stop();
}

static void play_step(int cb, int cs) {
    const bar_t *bar = &bars[cb];
    int m  = bar->melody[cs];
    int l2 = bar->lead2[cs];
    int b  = bar->bass[cs];
    uint8_t d = bar->drums[cs];

    if (m >= 0) {
        opl_note_off(MELODY_CH);
        opl_note_on(MELODY_CH, m);
    }
    if (l2 >= 0) {
        opl_note_off(LEAD2_CH);
        opl_note_on(LEAD2_CH, l2);
    }
    if (b >= 0) {
        opl_note_off(BASS_CH);
        opl_note_on(BASS_CH, b);
    }

    if (d) {
        uint8_t bits = 0;
        if (d & DRUM_KICK)  bits |= OPL_RHY_BD;
        if (d & DRUM_SNARE) bits |= OPL_RHY_SD;
        if (d & DRUM_HAT)   bits |= OPL_RHY_HH;
        /* Stop-then-trigger forces a fresh attack on each hit. */
        opl_rhythm_stop();
        opl_rhythm_trigger(bits);
    }
}

static void regenerate(void) {
    rng_seed(seed);
    music_generate(bars, BARS);
}

static void restart_from_top(void) {
    silence_all();
    cur_bar       = 0;
    cur_step      = 0;
    total_steps   = 1;
    loop_start_ms = timer_ms();
    display_init(seed);
    play_step(cur_bar, cur_step);
    display_frame(cur_bar, cur_step, &bars[cur_bar]);
}

static int player_rng_init(const char *arg) {
    if (arg) seed = strtoul(arg, NULL, 0);
    regenerate();
    setup_voices();
    display_init(seed);
    cur_bar       = 0;
    cur_step      = 0;
    total_steps   = 1;
    loop_start_ms = timer_ms();
    play_step(cur_bar, cur_step);
    display_frame(cur_bar, cur_step, &bars[cur_bar]);
    return 0;
}

static void player_rng_tick(uint32_t now_ms) {
    uint32_t now_us = (now_ms - loop_start_ms) * 1000UL;
    uint32_t target = (uint32_t)total_steps * STEP_US;

    if (now_us < target) return;

    cur_step++;
    if (cur_step >= STEPS_PER_BAR) {
        cur_step = 0;
        cur_bar++;
        if (cur_bar >= BARS) {
            /* Loop. Reset the time/step accumulators so we never
             * overflow on long playback sessions. */
            cur_bar       = 0;
            total_steps   = 0;
            loop_start_ms = now_ms;
        }
    }
    play_step(cur_bar, cur_step);
    display_frame(cur_bar, cur_step, &bars[cur_bar]);
    total_steps++;
}

static void player_rng_on_key(int k) {
    if (k >= '1' && k <= '0' + NUM_VARIATIONS) {
        int v = k - '1';
        if (v != music_get_variation()) {
            /* Re-seed before regenerating so the only thing that
             * changes between variations is the variation itself,
             * not the random draws. */
            music_set_variation(v);
            regenerate();
            restart_from_top();
        }
        return;
    }

    {
        int sidx = -1;
        if (k >= 'a' && k <= 'a' + SEED_COUNT - 1) sidx = k - 'a';
        if (k >= 'A' && k <= 'A' + SEED_COUNT - 1) sidx = k - 'A';
        if (sidx >= 0 && SEED_BANK[sidx] != seed) {
            /* Swap seed and regenerate with the current variation
             * intact — lets the user A/B seeds without changing style. */
            seed = SEED_BANK[sidx];
            regenerate();
            restart_from_top();
        }
    }
}

static void player_rng_cleanup(void) {
    silence_all();
    display_cleanup();
}

const player_t PLAYER_RNG = {
    "rng",
    player_rng_init,
    player_rng_tick,
    player_rng_on_key,
    player_rng_cleanup,
};
