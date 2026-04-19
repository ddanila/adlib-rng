#include "music.h"
#include "rng.h"
#include <string.h>

/* A natural minor scale: semitone offsets from the tonic. */
static const int8_t SCALE_MINOR[] = { 0, 2, 3, 5, 7, 8, 10 };
#define SCALE_LEN 7

/* Standard 12-bar minor blues compressed to 6 bars (each chord = half
 * a bar) and looped twice across our 12-bar window — 24 chord changes
 * total. One round, written as half-bars:
 *   i  i | i  i | iv iv | i  i | V  iv | i  V
 *  bar1   bar2   bar3    bar4   bar5    bar6
 */
#define BLUES_ROUND_HALFBARS 12
static const int8_t CHORD_PROGRESSION[BARS * CHORDS_PER_BAR] = {
    0, 0,  0, 0,  5, 5,  0, 0,  7, 5,  0, 7,    /* round 1 — bars 1-6  */
    0, 0,  0, 0,  5, 5,  0, 0,  7, 5,  0, 7     /* round 2 — bars 7-12 */
};

#define BASS_BASE_MIDI    33   /* A1 (≈ 55 Hz) */
#define MELODY_BASE_MIDI  57   /* A3 */
/* Melody stays in the key (A) regardless of chord — that's the blues
 * idiom. Only the bass tracks the chord root. */
#define KEY_ROOT          0

static void gen_chord_half(bar_t *bar, int half, int8_t chord_root) {
    int base = half * STEPS_PER_CHORD;   /* 0 or 32 */
    int mid  = base + STEPS_PER_CHORD / 2;

    bar->bass[base] = (int8_t)(BASS_BASE_MIDI + chord_root);
    if (rng_range(0, 1)) {
        /* Optional fifth on the back-beat of this half-bar */
        bar->bass[mid] = (int8_t)(BASS_BASE_MIDI + chord_root + 7);
    }
    bar->chord_root_midi[half] = (uint8_t)(BASS_BASE_MIDI + chord_root);
}

static void gen_bar(bar_t *bar, int8_t chord_a, int8_t chord_b) {
    int s;

    memset(bar, 0, sizeof(*bar));
    for (s = 0; s < STEPS_PER_BAR; s++) {
        bar->melody[s] = -1;
        bar->bass[s]   = -1;
    }

    /* Bass + per-half-bar chord index */
    gen_chord_half(bar, 0, chord_a);
    gen_chord_half(bar, 1, chord_b);

    /* Melody: 8th-note grid, 60% chance per slot. Picks from A natural
     * minor (key-anchored), occasional octave jump for movement. */
    for (s = 0; s < STEPS_PER_BAR; s += 8) {
        if (rng_range(0, 9) < 6) {
            int8_t deg = SCALE_MINOR[rng_range(0, SCALE_LEN - 1)];
            int8_t oct = (rng_range(0, 9) < 3) ? 12 : 0;
            bar->melody[s] = (int8_t)(MELODY_BASE_MIDI + KEY_ROOT + deg + oct);
        }
    }

    /* Drums: kick on 1 & 3, snare on 2 & 4, hihat on 8ths, sprinkled
     * ghost kicks for variation. */
    bar->drums[0]  = (uint8_t)(bar->drums[0]  | DRUM_KICK);
    bar->drums[32] = (uint8_t)(bar->drums[32] | DRUM_KICK);
    bar->drums[16] = (uint8_t)(bar->drums[16] | DRUM_SNARE);
    bar->drums[48] = (uint8_t)(bar->drums[48] | DRUM_SNARE);
    for (s = 0; s < STEPS_PER_BAR; s += 8) {
        bar->drums[s] = (uint8_t)(bar->drums[s] | DRUM_HAT);
    }
    if (rng_range(0, 9) < 3) bar->drums[24] = (uint8_t)(bar->drums[24] | DRUM_KICK);
    if (rng_range(0, 9) < 3) bar->drums[40] = (uint8_t)(bar->drums[40] | DRUM_KICK);
}

void music_generate(bar_t *bars, int num_bars) {
    int b;
    if (num_bars > BARS) num_bars = BARS;
    for (b = 0; b < num_bars; b++) {
        gen_bar(&bars[b],
                CHORD_PROGRESSION[b * CHORDS_PER_BAR + 0],
                CHORD_PROGRESSION[b * CHORDS_PER_BAR + 1]);
    }
}
