#include "music.h"
#include "rng.h"
#include <string.h>

/* A major pentatonic: semitone offsets from the tonic.
 * We only ever use the pentatonic now — the full major scale sounded
 * too modal because of the 4th and 7th degrees. */
static const int8_t SCALE_MAJ_PENT[] = { 0, 2, 4, 7, 9 };
#define SCALE_LEN 5

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
 * scale-degree indices (0..SCALE_LEN-1) or -1 for a rest. With the
 * pentatonic (5 degrees) the indices 0..4 map to root, 2, 3, 5, 6.
 * Mix of sparse/dense and ascending/descending to give the RNG real
 * options. */
#define PHRASE_LEN   8
#define PHRASE_COUNT 16
static const int8_t PHRASE_BANK[PHRASE_COUNT][PHRASE_LEN] = {
    { 0, -1,  2, -1,  4, -1,  2, -1 },   /* 00: call — root-3-5-3     */
    { 0,  2,  4,  2,  0, -1, -1, -1 },   /* 01: ascend + trail off    */
    { 4,  2,  0, -1,  2,  4,  4, -1 },   /* 02: arch                  */
    { 0, -1, -1,  4,  2, -1,  0, -1 },   /* 03: sparse                */
    { 4,  3,  2,  1,  0, -1, -1, -1 },   /* 04: descending run        */
    { 0,  4,  2,  0, -1,  2,  4, -1 },   /* 05: jumpy                 */
    { 0,  0,  2,  2,  4,  4,  2,  0 },   /* 06: stepwise climb+fall   */
    { -1, 2,  4,  3, -1,  2,  0, -1 },   /* 07: lazy syncopated       */
    { 0,  2,  4,  2,  0,  4,  2,  0 },   /* 08: dense arpeggio        */
    { 0,  2,  3,  4,  3,  2,  0, -1 },   /* 09: full run up + down    */
    { 2,  4,  2,  0,  2,  4,  2,  0 },   /* 10: rolling pattern       */
    { 0, -1,  0,  2,  4,  2,  0, -1 },   /* 11: stutter intro         */
    { 4,  3,  4,  2,  3,  2,  0, -1 },   /* 12: wavey descent         */
    { 0,  4,  0,  4,  0,  2,  4, -1 },   /* 13: octave-ish bounce     */
    { 2,  0,  2,  4,  2,  0, -1, -1 },   /* 14: call-and-taper        */
    { 0,  2,  4,  3,  2,  4,  3,  2 }    /* 15: busy eights           */
};

/* 2-bar phrase pairs — indices into PHRASE_BANK. Chosen so the pair
 * forms a musical sentence (statement → answer, ascend → descend). */
#define PHRASE_PAIR_COUNT 8
static const int8_t PHRASE_PAIR_BANK[PHRASE_PAIR_COUNT][2] = {
    { 0,  2 },   /* call → arch                   */
    { 1,  4 },   /* ascend → descending run       */
    { 6,  7 },   /* stepwise → lazy answer        */
    { 8,  9 },   /* dense arp → run up+down       */
    { 10, 14 },  /* rolling → call-and-taper      */
    { 11, 12 },  /* stutter → wavey descent       */
    { 3,  5 },   /* sparse → jumpy answer         */
    { 13, 4  }   /* bounce → descending resolution*/
};

typedef enum { BASS_ROOTS = 0, BASS_WALKING = 1 } bass_mode_t;
typedef enum { PHR_BAR   = 0, PHR_2BAR      = 1 } phrase_mode_t;

typedef struct {
    const char   *name;
    const int8_t *progression;
    phrase_mode_t phr_mode;
    bass_mode_t   bass_mode;
} variation_t;

/* V1-V2 are the survivors of the last listening session (both used
 * phrases on pentatonic); V3-V4 are new experiments that vary one
 * dimension each vs V2. */
static const variation_t VARIATIONS[NUM_VARIATIONS] = {
    { "blues+phr",  PROG_BLUES, PHR_BAR,  BASS_ROOTS   },
    { "50s+phr",    PROG_50S,   PHR_BAR,  BASS_ROOTS   },
    { "50s+walk",   PROG_50S,   PHR_BAR,  BASS_WALKING },
    { "50s+2bar",   PROG_50S,   PHR_2BAR, BASS_ROOTS   }
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

static void place_bass(bar_t *bar, int step, int8_t chord_root, int8_t semi) {
    bar->bass[step] = (int8_t)(BASS_BASE_MIDI + chord_root + semi);
}

static void gen_chord_half_roots(bar_t *bar, int half, int8_t chord_root) {
    int base = half * STEPS_PER_CHORD;   /* 0 or 32 */
    int mid  = base + STEPS_PER_CHORD / 2;

    place_bass(bar, base, chord_root, 0);
    if (rng_range(0, 1)) {
        place_bass(bar, mid, chord_root, 7);     /* optional fifth */
    }
    bar->chord_root_midi[half] = (uint8_t)(BASS_BASE_MIDI + chord_root);
}

/* Walking bass: root on the down-beat, an RNG-picked chord tone on
 * the back-beat. Every half-bar gets two bass notes — more motion. */
static void gen_chord_half_walking(bar_t *bar, int half, int8_t chord_root) {
    static const int8_t WALK_CHOICES[] = { 4, 7, 9 };  /* 3rd, 5th, 6th */
    int base = half * STEPS_PER_CHORD;
    int mid  = base + STEPS_PER_CHORD / 2;
    int8_t walk = WALK_CHOICES[rng_range(0, 2)];

    place_bass(bar, base, chord_root, 0);
    place_bass(bar, mid,  chord_root, walk);
    bar->chord_root_midi[half] = (uint8_t)(BASS_BASE_MIDI + chord_root);
}

static void gen_melody_from_phrase(bar_t *bar, int phrase_idx) {
    int oct = (rng_range(0, 4) == 0) ? 12 : 0;
    const int8_t *p = PHRASE_BANK[phrase_idx];
    int s;
    for (s = 0; s < PHRASE_LEN; s++) {
        int8_t d = p[s];
        if (d < 0) continue;
        if (d >= SCALE_LEN) d = (int8_t)(SCALE_LEN - 1);
        bar->melody[s * 8] = (int8_t)(MELODY_BASE_MIDI + KEY_ROOT + SCALE_MAJ_PENT[d] + oct);
    }
}

static void gen_bar(bar_t *bar, const variation_t *v, int bar_idx, int phrase_idx) {
    int s;
    int8_t chord_a = v->progression[bar_idx * CHORDS_PER_BAR + 0];
    int8_t chord_b = v->progression[bar_idx * CHORDS_PER_BAR + 1];

    memset(bar, 0, sizeof(*bar));
    for (s = 0; s < STEPS_PER_BAR; s++) {
        bar->melody[s] = -1;
        bar->bass[s]   = -1;
    }

    if (v->bass_mode == BASS_WALKING) {
        gen_chord_half_walking(bar, 0, chord_a);
        gen_chord_half_walking(bar, 1, chord_b);
    } else {
        gen_chord_half_roots(bar, 0, chord_a);
        gen_chord_half_roots(bar, 1, chord_b);
    }

    gen_melody_from_phrase(bar, phrase_idx);

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

    if (v->phr_mode == PHR_2BAR) {
        for (b = 0; b < num_bars; b += 2) {
            int pair_idx = rng_range(0, PHRASE_PAIR_COUNT - 1);
            gen_bar(&bars[b], v, b, PHRASE_PAIR_BANK[pair_idx][0]);
            if (b + 1 < num_bars) {
                gen_bar(&bars[b + 1], v, b + 1, PHRASE_PAIR_BANK[pair_idx][1]);
            }
        }
    } else {
        for (b = 0; b < num_bars; b++) {
            int phrase_idx = rng_range(0, PHRASE_COUNT - 1);
            gen_bar(&bars[b], v, b, phrase_idx);
        }
    }
}
