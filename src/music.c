#include "music.h"
#include "rng.h"
#include <string.h>

/* A major pentatonic — the only scale in use. */
static const int8_t SCALE_MAJ_PENT[] = { 0, 2, 4, 7, 9 };
#define SCALE_LEN 5

#define PROG_LEN (BARS * CHORDS_PER_BAR)

/* All-major progression in A: I-III-IV-V (A - C# - D - E). The
 * 4-chord cycle spans 2 bars; repeats six times across 12 bars. */
static const int8_t PROG_MAJ4[PROG_LEN] = {
    0, 4,  5, 7,   0, 4,  5, 7,   0, 4,  5, 7,
    0, 4,  5, 7,   0, 4,  5, 7,   0, 4,  5, 7
};

/* 12-bar blues (I-IV-V in A = A-D-E, all major) compressed to half-bar
 * resolution so one round fits in 6 bars, looped twice. Half-bar layout
 * of one round:  I I | I I | IV IV | I I | V IV | I V. */
static const int8_t PROG_BLUES[PROG_LEN] = {
    0, 0,  0, 0,  5, 5,  0, 0,  7, 5,  0, 7,
    0, 0,  0, 0,  5, 5,  0, 0,  7, 5,  0, 7
};

/* Minimal house vamp: 1 bar of I, 1 bar of IV, loop six times. Slowest
 * harmonic motion of the three — the locked hook and pumping bass carry
 * the piece. */
static const int8_t PROG_VAMP[PROG_LEN] = {
    0, 0,  5, 5,  0, 0,  5, 5,  0, 0,  5, 5,
    0, 0,  5, 5,  0, 0,  5, 5,  0, 0,  5, 5
};

/* Melody phrase bank. Each phrase is 8 eighth-note slots holding
 * scale-degree indices (0..SCALE_LEN-1) or -1 for a rest. */
#define PHRASE_LEN   8
#define PHRASE_COUNT 20
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
    { 0,  2,  4,  3,  2,  4,  3,  2 },   /* 15: busy eights           */
    { 4, -1,  2,  0,  2,  4,  3,  2 },   /* 16: high start, settle    */
    { 0,  3,  2,  4,  3,  0,  2,  4 },   /* 17: zigzag                */
    { 2,  4,  3, -1,  0,  2,  4,  3 },   /* 18: offbeat climb         */
    { 4,  2,  0,  2,  4,  3,  2,  0 }    /* 19: descend + land        */
};

/* 2-bar phrase pairs — indices into PHRASE_BANK. Chosen so the pair
 * forms a musical sentence (statement → answer, ascend → descend). */
#define PHRASE_PAIR_COUNT 14
static const int8_t PHRASE_PAIR_BANK[PHRASE_PAIR_COUNT][2] = {
    { 0,  2 },   /* call → arch                    */
    { 1,  4 },   /* ascend → descending run        */
    { 6,  7 },   /* stepwise → lazy answer         */
    { 8,  9 },   /* dense arp → run up+down        */
    { 10, 14 },  /* rolling → call-and-taper       */
    { 11, 12 },  /* stutter → wavey descent        */
    { 3,  5 },   /* sparse → jumpy answer          */
    { 13, 4  },  /* bounce → descending resolution */
    { 15, 7 },   /* busy → lazy answer             */
    { 16, 19 },  /* high-start → descend+land      */
    { 17, 14 },  /* zigzag → call-and-taper        */
    { 18, 5 },   /* offbeat climb → jumpy          */
    { 2,  8 },   /* arch → dense arpeggio          */
    { 5,  16 }   /* jumpy → high-start             */
};

typedef enum {
    BASS_ROLLING = 0,   /* 16th-note root hits + occasional octave bumps */
    BASS_PUMP    = 1    /* octave jumps on 8ths — french-house bass      */
} bass_mode_t;

/* melody_mode:
 *   FRESH_PHRASE — random 1-bar phrase from PHRASE_BANK every bar.
 *   HOOK_2BAR    — pick 6 random phrase pairs (one per pair-slot) plus
 *                  6 independent octave shifts and loop them across the
 *                  12-bar round — locked-hook signature.
 */
typedef enum {
    MEL_FRESH_PHRASE = 0,
    MEL_HOOK_2BAR    = 1
} melody_mode_t;

/* lead2_mode: what the second melodic voice does.
 *   HARMONY — double the melody one octave up.
 *   FILLS   — 16th-note ornaments between main phrase notes.
 */
typedef enum {
    LEAD2_HARMONY = 0,
    LEAD2_FILLS   = 1
} lead2_mode_t;

typedef struct {
    const char    *name;
    const int8_t  *progression;
    bass_mode_t    bass_mode;
    melody_mode_t  melody_mode;
    lead2_mode_t   lead2_mode;
} variation_t;

/* V1-V3 share bass (pump), drums (dance), melody mode (locked hook),
 * and harmony (octave up); only the progression differs — so the A/B
 * isolates what the harmonic structure alone does to the same musical
 * material. V4 swaps in rolling bass + fresh phrases + 16th-note fills
 * as a reference contrast. */
static const variation_t VARIATIONS[NUM_VARIATIONS] = {
    { "daft-blues", PROG_BLUES, BASS_PUMP,    MEL_HOOK_2BAR,    LEAD2_HARMONY },
    { "daft-vamp",  PROG_VAMP,  BASS_PUMP,    MEL_HOOK_2BAR,    LEAD2_HARMONY },
    { "daft",       PROG_MAJ4,  BASS_PUMP,    MEL_HOOK_2BAR,    LEAD2_HARMONY },
    { "90s+fills",  PROG_MAJ4,  BASS_ROLLING, MEL_FRESH_PHRASE, LEAD2_FILLS   }
};

static const char *VARIATION_DESC[NUM_VARIATIONS] = {
    "daft style over 12-bar blues (I-IV-V)",
    "daft style over a minimal I-IV 1-bar vamp (slow harmonic motion)",
    "daft style over I-III-IV-V (reference)",
    "rolling bass + dance drums + fresh-per-bar phrase + 16th fills"
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

const char *music_variation_desc(int idx) {
    if (idx < 0 || idx >= NUM_VARIATIONS) return "";
    return VARIATION_DESC[idx];
}

#define BASS_BASE_MIDI    33   /* A1 (≈ 55 Hz) */
#define MELODY_BASE_MIDI  57   /* A3 */
/* Melody stays in the key (A) regardless of chord. */
#define KEY_ROOT          0

static void place_bass(bar_t *bar, int step, int8_t chord_root, int8_t semi) {
    bar->bass[step] = (int8_t)(BASS_BASE_MIDI + chord_root + semi);
}

/* Rolling 16th-note bass: chord root every 4 steps, 10% chance of an
 * octave bump for movement. 16 hits per bar — the late-90s sub-bass
 * figure. */
static void gen_chord_half_rolling(bar_t *bar, int half, int8_t chord_root) {
    int base = half * STEPS_PER_CHORD;
    int i;
    for (i = 0; i < STEPS_PER_CHORD; i += 4) {
        int8_t oct = (rng_range(0, 9) == 0) ? 12 : 0;
        place_bass(bar, base + i, chord_root, oct);
    }
    bar->chord_root_midi[half] = (uint8_t)(BASS_BASE_MIDI + chord_root);
}

/* Pumping bass: root on the beat, octave up on the "and" — funky
 * french-house bassline shape, 8 hits per bar. */
static void gen_chord_half_pump(bar_t *bar, int half, int8_t chord_root) {
    int base = half * STEPS_PER_CHORD;
    int i;
    for (i = 0; i < STEPS_PER_CHORD; i += 8) {
        int8_t offset = ((i / 8) & 1) ? 12 : 0;
        place_bass(bar, base + i, chord_root, offset);
    }
    bar->chord_root_midi[half] = (uint8_t)(BASS_BASE_MIDI + chord_root);
}

/* Place a phrase on the 8th-note grid (8 slots, step = s*8). */
static void apply_melody_phrase(bar_t *bar, int phrase_idx, int oct) {
    const int8_t *p = PHRASE_BANK[phrase_idx];
    int s;
    for (s = 0; s < PHRASE_LEN; s++) {
        int8_t d = p[s];
        if (d < 0) continue;
        if (d >= SCALE_LEN) d = (int8_t)(SCALE_LEN - 1);
        bar->melody[s * 8] = (int8_t)(MELODY_BASE_MIDI + KEY_ROOT + SCALE_MAJ_PENT[d] + oct);
    }
}

/* Harmony = melody doubled one octave up. Thickens the line without
 * any chord-quality analysis. */
static void gen_lead2_harmony(bar_t *bar) {
    int s;
    for (s = 0; s < STEPS_PER_BAR; s++) {
        if (bar->melody[s] >= 0) {
            bar->lead2[s] = (int8_t)(bar->melody[s] + 12);
        }
    }
}

/* Fills = random pentatonic note on the 16th *between* each main phrase
 * note (steps 4, 12, 20, ...). 50% chance per gap; an octave bump now
 * and then. */
static void gen_lead2_fills(bar_t *bar) {
    int s;
    for (s = 0; s < PHRASE_LEN; s++) {
        int main_step = s * 8;
        int fill_step = main_step + 4;
        if (bar->melody[main_step] < 0) continue;   /* no main note, no fill */
        if (rng_range(0, 1) == 0) continue;         /* 50% skip */
        {
            int8_t d   = SCALE_MAJ_PENT[rng_range(0, SCALE_LEN - 1)];
            int8_t oct = (rng_range(0, 4) == 0) ? 12 : 0;
            bar->lead2[fill_step] =
                (int8_t)(MELODY_BASE_MIDI + KEY_ROOT + d + oct);
        }
    }
}

/* Dance drums: 4-on-the-floor kick, clap/snare on 2 & 4, open hats on
 * every "and" (off-beat 8ths) + occasional 16th ghosts. */
static void apply_drums_dance(bar_t *bar) {
    bar->drums[0]  = (uint8_t)(bar->drums[0]  | DRUM_KICK);
    bar->drums[16] = (uint8_t)(bar->drums[16] | DRUM_KICK | DRUM_SNARE);
    bar->drums[32] = (uint8_t)(bar->drums[32] | DRUM_KICK);
    bar->drums[48] = (uint8_t)(bar->drums[48] | DRUM_KICK | DRUM_SNARE);
    bar->drums[8]  = (uint8_t)(bar->drums[8]  | DRUM_HAT);
    bar->drums[24] = (uint8_t)(bar->drums[24] | DRUM_HAT);
    bar->drums[40] = (uint8_t)(bar->drums[40] | DRUM_HAT);
    bar->drums[56] = (uint8_t)(bar->drums[56] | DRUM_HAT);
    if (rng_range(0, 9) < 3) bar->drums[4]  = (uint8_t)(bar->drums[4]  | DRUM_HAT);
    if (rng_range(0, 9) < 3) bar->drums[36] = (uint8_t)(bar->drums[36] | DRUM_HAT);
}

/* gen_bar_skeleton fills bass + drums for one bar. Melody and lead2
 * are applied separately by music_generate so the melody mode can
 * decide how to pick phrases (fresh per bar, locked hook). */
static void gen_bar_skeleton(bar_t *bar, const variation_t *v, int bar_idx) {
    int s;
    int8_t chord_a = v->progression[bar_idx * CHORDS_PER_BAR + 0];
    int8_t chord_b = v->progression[bar_idx * CHORDS_PER_BAR + 1];

    memset(bar, 0, sizeof(*bar));
    for (s = 0; s < STEPS_PER_BAR; s++) {
        bar->melody[s] = -1;
        bar->lead2[s]  = -1;
        bar->bass[s]   = -1;
    }

    switch (v->bass_mode) {
    case BASS_ROLLING:
        gen_chord_half_rolling(bar, 0, chord_a);
        gen_chord_half_rolling(bar, 1, chord_b);
        break;
    case BASS_PUMP:
    default:
        gen_chord_half_pump(bar, 0, chord_a);
        gen_chord_half_pump(bar, 1, chord_b);
        break;
    }

    apply_drums_dance(bar);
}

static void apply_lead2(bar_t *bar, lead2_mode_t mode) {
    switch (mode) {
    case LEAD2_FILLS:   gen_lead2_fills(bar);   break;
    case LEAD2_HARMONY:
    default:            gen_lead2_harmony(bar); break;
    }
}

void music_generate(bar_t *bars, int num_bars) {
    const variation_t *v = &VARIATIONS[current_variation];
    /* For HOOK_2BAR: 6 pair-slots × 2 picks (pair + octave) per slot =
     * 12 RNG draws, turning the seed into a composition knob rather
     * than a one-off choice out of 14 pairs. */
    int hook_pair[6] = {0, 0, 0, 0, 0, 0};
    int hook_oct[6]  = {0, 0, 0, 0, 0, 0};
    int b;
    if (num_bars > BARS) num_bars = BARS;

    if (v->melody_mode == MEL_HOOK_2BAR) {
        int i;
        for (i = 0; i < 6; i++) {
            hook_pair[i] = rng_range(0, PHRASE_PAIR_COUNT - 1);
            hook_oct[i]  = rng_range(0, 1) ? 12 : 0;
        }
    }

    for (b = 0; b < num_bars; b++) {
        gen_bar_skeleton(&bars[b], v, b);

        switch (v->melody_mode) {
        case MEL_HOOK_2BAR: {
            int pair_slot = b / 2;
            int idx = PHRASE_PAIR_BANK[hook_pair[pair_slot]][b % 2];
            int oct = hook_oct[pair_slot];
            apply_melody_phrase(&bars[b], idx, oct);
            break;
        }
        case MEL_FRESH_PHRASE:
        default: {
            int oct = (rng_range(0, 4) == 0) ? 12 : 0;
            apply_melody_phrase(&bars[b], rng_range(0, PHRASE_COUNT - 1), oct);
            break;
        }
        }

        apply_lead2(&bars[b], v->lead2_mode);
    }
}
