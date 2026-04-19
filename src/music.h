#ifndef MUSIC_H
#define MUSIC_H

#include <stdint.h>

#define BARS             12
#define STEPS_PER_BAR    64
#define CHORDS_PER_BAR   2
#define STEPS_PER_CHORD  (STEPS_PER_BAR / CHORDS_PER_BAR)   /* = 32 */
#define BPM              120
/* 120 BPM, 4 beats/bar, 16 substeps/beat → 2000 ms/bar → 31.25 ms/step.
 * Stored as microseconds for cumulative scheduling without drift. */
#define STEP_US          31250UL

typedef enum { DRUM_FM = 0, DRUM_RHYTHM = 1 } drum_mode_t;

typedef enum {
    DRUM_NONE  = 0,
    DRUM_KICK  = 1,
    DRUM_SNARE = 2,
    DRUM_HAT   = 4
} drum_bits_t;

typedef struct {
    int8_t  melody[STEPS_PER_BAR];   /* MIDI note, -1 = rest */
    int8_t  bass[STEPS_PER_BAR];
    uint8_t drums[STEPS_PER_BAR];    /* OR of drum_bits_t */
    uint8_t chord_root_midi[CHORDS_PER_BAR];
} bar_t;

#define NUM_VARIATIONS 4

void        music_set_variation(int idx);
int         music_get_variation(void);
const char *music_variation_name(int idx);
void        music_generate(bar_t *bars, int num_bars);

#endif
