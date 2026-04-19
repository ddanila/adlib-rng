#include "music.h"
#include "rng.h"
#include <string.h>

/* A natural minor scale: semitone offsets from the tonic.
 * Indexes 0,2,3,4,5,6 form the pentatonic emphasis we lean on. */
static const int8_t SCALE_MINOR[] = { 0, 2, 3, 5, 7, 8, 10 };
#define SCALE_LEN 7

/* Chord roots in semitones above A. Cycle: Am — Dm — G — C, three
 * times across 12 bars. All chords are diatonic to A natural minor
 * so any scale note works on top. */
static const int8_t CHORD_PROGRESSION[BARS] = {
    0, 5, 10, 3,
    0, 5, 10, 3,
    0, 5, 10, 3
};

#define BASS_BASE_MIDI    33   /* A1 (≈ 55 Hz) */
#define MELODY_BASE_MIDI  57   /* A3 */

static void gen_bar(bar_t *bar, int8_t chord_root) {
    int s;

    memset(bar, 0, sizeof(*bar));
    for (s = 0; s < STEPS_PER_BAR; s++) {
        bar->melody[s] = -1;
        bar->bass[s]   = -1;
    }
    bar->chord_root_midi = (uint8_t)(BASS_BASE_MIDI + chord_root);

    /* Bass: root on beats 1 and 3, optional fifth on beat 2, optional
     * octave on beat 4. Steps are 16 substeps apart. */
    bar->bass[0]  = (int8_t)(BASS_BASE_MIDI + chord_root);
    bar->bass[32] = (int8_t)(BASS_BASE_MIDI + chord_root);
    if (rng_range(0, 1)) {
        bar->bass[16] = (int8_t)(BASS_BASE_MIDI + chord_root + 7);
    }
    if (rng_range(0, 1)) {
        bar->bass[48] = (int8_t)(BASS_BASE_MIDI + chord_root + 12);
    }

    /* Melody: 8th-note grid, 60% chance of a note per slot. Pick from
     * the minor scale rooted on the chord, sometimes jumping an octave
     * up for movement. */
    for (s = 0; s < STEPS_PER_BAR; s += 8) {
        if (rng_range(0, 9) < 6) {
            int8_t deg = SCALE_MINOR[rng_range(0, SCALE_LEN - 1)];
            int8_t oct = (rng_range(0, 9) < 3) ? 12 : 0;
            bar->melody[s] = (int8_t)(MELODY_BASE_MIDI + chord_root + deg + oct);
        }
    }

    /* Drums: classic kick on 1 & 3, snare on 2 & 4, hihat on 8ths.
     * Plus occasional ghost kicks for variation. */
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
        gen_bar(&bars[b], CHORD_PROGRESSION[b]);
    }
}
