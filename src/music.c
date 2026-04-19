#include "music.h"
#include "rng.h"
#include <string.h>

/* Scales — semitone offsets from the tonic. */
static const int8_t SCALE_MAJOR[]    = { 0, 2, 4, 5, 7, 9, 11 };
static const int8_t SCALE_MAJ_PENT[] = { 0, 2, 4, 7, 9 };

#define PROG_LEN (BARS * CHORDS_PER_BAR)

/* Standard 12-bar blues compressed to 6 bars (each chord = half a
 * bar) and looped twice — 24 chord changes total. Half-bar layout:
 *   I  I | I  I | IV IV | I  I | V  IV | I  V    (× 2)
 */
static const int8_t PROG_BLUES[PROG_LEN] = {
    0, 0,  0, 0,  5, 5,  0, 0,  7, 5,  0, 7,
    0, 0,  0, 0,  5, 5,  0, 0,  7, 5,  0, 7
};

/* "50s pop" I-vi-IV-V: A → F#m → D → E. One chord per half-bar so the
 * 4-chord cycle spans 2 bars; repeats six times across 12 bars. */
static const int8_t PROG_50S[PROG_LEN] = {
    0, 9,  5, 7,   0, 9,  5, 7,   0, 9,  5, 7,
    0, 9,  5, 7,   0, 9,  5, 7,   0, 9,  5, 7
};

/* Melody phrase bank. Each phrase is 8 eighth-note slots holding
 * scale-degree indices (0..4 only — the lowest 5, valid for both
 * pentatonic and full major scales). -1 = rest. The same indices
 * map to different intervals depending on the active scale, so the
 * phrases sound subtly different per scale (a feature, not a bug). */
#define PHRASE_LEN   8
#define PHRASE_COUNT 8
static const int8_t PHRASE_BANK[PHRASE_COUNT][PHRASE_LEN] = {
    { 0, -1,  2, -1,  4, -1,  2, -1 },   /* 0: simple call, root-3-5-3 */
    { 0,  2,  4,  2,  0, -1, -1, -1 },   /* 1: ascending then rest    */
    { 4,  2,  0, -1,  2,  4,  4, -1 },   /* 2: arch shape             */
    { 0, -1, -1,  4,  2, -1,  0, -1 },   /* 3: sparse, mostly rests   */
    { 4,  3,  2,  1,  0, -1, -1, -1 },   /* 4: descending run         */
    { 0,  4,  2,  0, -1,  2,  4, -1 },   /* 5: jumpy                  */
    { 0,  0,  2,  2,  4,  4,  2,  0 },   /* 6: dense, stepwise        */
    { -1, 2,  4,  3, -1,  2,  0, -1 }    /* 7: lazy syncopated        */
};

typedef enum { MEL_RANDOM = 0, MEL_PHRASE = 1 } mel_mode_t;

typedef struct {
    const char   *name;
    const int8_t *progression;
    const int8_t *scale;
    int           scale_len;
    mel_mode_t    mel_mode;
} variation_t;

/* Variations are intentionally additive: each one introduces one
 * change relative to the previous so you can hear what that change
 * actually buys you. */
static const variation_t VARIATIONS[NUM_VARIATIONS] = {
    { "baseline",     PROG_BLUES, SCALE_MAJOR,    7, MEL_RANDOM },
    { "+pentatonic",  PROG_BLUES, SCALE_MAJ_PENT, 5, MEL_RANDOM },
    { "+phrases",     PROG_BLUES, SCALE_MAJ_PENT, 5, MEL_PHRASE },
    { "+50s pop",     PROG_50S,   SCALE_MAJ_PENT, 5, MEL_PHRASE }
};

static int current_variation = 0;

void music_set_variation(int idx) {
    if (idx < 0 || idx >= NUM_VARIATIONS) return;
    current_variation = idx;
}

int music_get_variation(void) {
    return current_variation;
}

const char *music_variation_name(int idx) {
    if (idx < 0 || idx >= NUM_VARIATIONS) return "?";
    return VARIATIONS[idx].name;
}

#define BASS_BASE_MIDI    33   /* A1 (≈ 55 Hz) */
#define MELODY_BASE_MIDI  57   /* A3 */
/* Melody stays in the key (A) regardless of chord. Only the bass
 * tracks the chord root. */
#define KEY_ROOT          0

static void gen_chord_half(bar_t *bar, int half, int8_t chord_root) {
    int base = half * STEPS_PER_CHORD;       /* 0 or 32 */
    int mid  = base + STEPS_PER_CHORD / 2;

    bar->bass[base] = (int8_t)(BASS_BASE_MIDI + chord_root);
    if (rng_range(0, 1)) {
        bar->bass[mid] = (int8_t)(BASS_BASE_MIDI + chord_root + 7);
    }
    bar->chord_root_midi[half] = (uint8_t)(BASS_BASE_MIDI + chord_root);
}

static void gen_melody_random(bar_t *bar, const int8_t *scale, int scale_len) {
    int s;
    for (s = 0; s < STEPS_PER_BAR; s += 8) {
        if (rng_range(0, 9) < 6) {
            int8_t deg = scale[rng_range(0, scale_len - 1)];
            int8_t oct = (rng_range(0, 9) < 3) ? 12 : 0;
            bar->melody[s] = (int8_t)(MELODY_BASE_MIDI + KEY_ROOT + deg + oct);
        }
    }
}

static void gen_melody_phrase(bar_t *bar, const int8_t *scale, int scale_len) {
    int idx = rng_range(0, PHRASE_COUNT - 1);
    int oct = (rng_range(0, 4) == 0) ? 12 : 0;
    const int8_t *p = PHRASE_BANK[idx];
    int s;
    for (s = 0; s < PHRASE_LEN; s++) {
        int8_t d = p[s];
        if (d < 0) continue;
        if (d >= scale_len) d = (int8_t)(scale_len - 1);
        bar->melody[s * 8] = (int8_t)(MELODY_BASE_MIDI + KEY_ROOT + scale[d] + oct);
    }
}

static void gen_bar(bar_t *bar, int8_t chord_a, int8_t chord_b,
                    const int8_t *scale, int scale_len, mel_mode_t mel) {
    int s;

    memset(bar, 0, sizeof(*bar));
    for (s = 0; s < STEPS_PER_BAR; s++) {
        bar->melody[s] = -1;
        bar->bass[s]   = -1;
    }

    gen_chord_half(bar, 0, chord_a);
    gen_chord_half(bar, 1, chord_b);

    if (mel == MEL_PHRASE) {
        gen_melody_phrase(bar, scale, scale_len);
    } else {
        gen_melody_random(bar, scale, scale_len);
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
    const variation_t *v = &VARIATIONS[current_variation];
    int b;
    if (num_bars > BARS) num_bars = BARS;
    for (b = 0; b < num_bars; b++) {
        gen_bar(&bars[b],
                v->progression[b * CHORDS_PER_BAR + 0],
                v->progression[b * CHORDS_PER_BAR + 1],
                v->scale, v->scale_len, v->mel_mode);
    }
}
