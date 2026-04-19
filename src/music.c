#include "music.h"
#include "rng.h"
#include <string.h>

/* A major pentatonic — the only scale in use. */
static const int8_t SCALE_MAJ_PENT[] = { 0, 2, 4, 7, 9 };
#define SCALE_LEN 5

#define PROG_LEN (BARS * CHORDS_PER_BAR)

/* All-major progression in A: I-III-IV-V (A - C# - D - E). Replaces
 * the earlier I-vi-IV-V, whose vi (F#m) implied a minor chord the
 * ear kept hearing under the A-pent melody. I-III-IV-V keeps the
 * same rising/resolving shape with zero minor implication. One chord
 * per half-bar so the 4-chord cycle spans 2 bars; repeats six times
 * across the 12-bar window. */
static const int8_t PROG_MAJ4[PROG_LEN] = {
    0, 4,  5, 7,   0, 4,  5, 7,   0, 4,  5, 7,
    0, 4,  5, 7,   0, 4,  5, 7,   0, 4,  5, 7
};

/* Melody phrase bank. Each phrase is 8 eighth-note slots holding
 * scale-degree indices (0..SCALE_LEN-1) or -1 for a rest. Mix of
 * sparse/dense and ascending/descending to give the RNG real options. */
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
    { 0,  2 },   /* call → arch                    */
    { 1,  4 },   /* ascend → descending run        */
    { 6,  7 },   /* stepwise → lazy answer         */
    { 8,  9 },   /* dense arp → run up+down        */
    { 10, 14 },  /* rolling → call-and-taper       */
    { 11, 12 },  /* stutter → wavey descent        */
    { 3,  5 },   /* sparse → jumpy answer          */
    { 13, 4  }   /* bounce → descending resolution */
};

typedef enum { BASS_ROOTS = 0, BASS_WALKING = 1, BASS_ROLLING = 2 } bass_mode_t;
typedef enum { PHR_BAR   = 0, PHR_2BAR      = 1 }                   phrase_mode_t;
typedef enum { DRUMS_ROCK = 0, DRUMS_DANCE  = 1 }                   drum_pattern_t;

/* lead2_mode: what the second melodic voice (ch 2) does.
 *   NONE     — silent (V1, V2)
 *   HARMONY  — doubles the melody one octave up (V3)
 *   FILLS    — 16th-note ornaments between main phrase notes (V4) */
typedef enum { LEAD2_NONE = 0, LEAD2_HARMONY = 1, LEAD2_FILLS = 2 } lead2_mode_t;

typedef struct {
    const char    *name;
    const int8_t  *progression;
    phrase_mode_t  phr_mode;
    bass_mode_t    bass_mode;
    drum_pattern_t drum_pattern;
    lead2_mode_t   lead2_mode;
} variation_t;

/* V1/V2 are the 90s-electronic slots the user committed to as the
 * baseline. V3/V4 stack a second melodic voice on top of V1 to
 * experiment with the solo feeling less static. */
static const variation_t VARIATIONS[NUM_VARIATIONS] = {
    { "90s+phr",   PROG_MAJ4, PHR_BAR,  BASS_ROLLING, DRUMS_DANCE, LEAD2_NONE    },
    { "90s+2bar",  PROG_MAJ4, PHR_2BAR, BASS_ROLLING, DRUMS_DANCE, LEAD2_NONE    },
    { "90s+harm",  PROG_MAJ4, PHR_BAR,  BASS_ROLLING, DRUMS_DANCE, LEAD2_HARMONY },
    { "90s+fills", PROG_MAJ4, PHR_BAR,  BASS_ROLLING, DRUMS_DANCE, LEAD2_FILLS   }
};

static const char *VARIATION_DESC[NUM_VARIATIONS] = {
    "I-III-IV-V, rolling 16th bass + 4-on-floor, phrase melody",
    "I-III-IV-V, rolling 16th bass + 4-on-floor, 2-bar phrase pairs",
    "V1 + 2nd lead voice doubling melody an octave up (bright patch)",
    "V1 + 2nd lead voice playing 16th ornaments between phrase notes"
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

static void gen_chord_half_roots(bar_t *bar, int half, int8_t chord_root) {
    int base = half * STEPS_PER_CHORD;
    int mid  = base + STEPS_PER_CHORD / 2;

    place_bass(bar, base, chord_root, 0);
    if (rng_range(0, 1)) {
        place_bass(bar, mid, chord_root, 7);     /* optional fifth */
    }
    bar->chord_root_midi[half] = (uint8_t)(BASS_BASE_MIDI + chord_root);
}

/* Pick a pentatonic-safe walking offset above the chord root — works
 * regardless of chord quality (see the commit that introduced this). */
static int8_t pick_pent_walk_offset(int8_t chord_root) {
    static const int8_t PENT_SEMIS[] = { 0, 2, 4, 7, 9, 12, 14, 16, 19, 21 };
    int8_t root_mod = (int8_t)(chord_root % 12);
    int8_t offs[6];
    int n = 0, i;
    for (i = 0; i < (int)(sizeof(PENT_SEMIS) / sizeof(PENT_SEMIS[0])); i++) {
        int8_t d = (int8_t)(PENT_SEMIS[i] - root_mod);
        if (d > 0 && d <= 12 && n < 6) offs[n++] = d;
    }
    if (n == 0) return 7;
    return offs[rng_range(0, n - 1)];
}

static void gen_chord_half_walking(bar_t *bar, int half, int8_t chord_root) {
    int base = half * STEPS_PER_CHORD;
    int mid  = base + STEPS_PER_CHORD / 2;
    int8_t walk = pick_pent_walk_offset(chord_root);

    place_bass(bar, base, chord_root, 0);
    place_bass(bar, mid,  chord_root, walk);
    bar->chord_root_midi[half] = (uint8_t)(BASS_BASE_MIDI + chord_root);
}

/* Rolling 16th-note bass: hit the chord root every 4 steps, with an
 * occasional octave bump for movement. That's 8 hits per half-bar,
 * 16 per bar — the rolling sub-bass figure of late-90s DnB / big beat. */
static void gen_chord_half_rolling(bar_t *bar, int half, int8_t chord_root) {
    int base = half * STEPS_PER_CHORD;
    int i;
    for (i = 0; i < STEPS_PER_CHORD; i += 4) {
        int8_t oct = (rng_range(0, 9) == 0) ? 12 : 0;   /* 10% octave up */
        place_bass(bar, base + i, chord_root, oct);
    }
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

/* Harmony = melody doubled one octave up. Simplest thickening trick;
 * works for any phrase that already fits in the scale. */
static void gen_lead2_harmony(bar_t *bar) {
    int s;
    for (s = 0; s < STEPS_PER_BAR; s++) {
        if (bar->melody[s] >= 0) {
            bar->lead2[s] = (int8_t)(bar->melody[s] + 12);
        }
    }
}

/* Fills = random pentatonic note on the 16th *between* each main
 * phrase note (steps 4, 12, 20, ...). 50% chance per gap; an octave
 * bump now and then. */
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

static void apply_drums_rock(bar_t *bar) {
    int s;
    /* Classic rock beat: kick on 1 & 3, snare on 2 & 4, hats on 8ths,
     * sprinkled ghost kicks. */
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

static void apply_drums_dance(bar_t *bar) {
    /* 4-on-the-floor kick, clap/snare on 2 & 4, open hats on every
     * "and" (off-beat 8ths) — the classic house / big-beat shape. */
    bar->drums[0]  = (uint8_t)(bar->drums[0]  | DRUM_KICK);
    bar->drums[16] = (uint8_t)(bar->drums[16] | DRUM_KICK | DRUM_SNARE);
    bar->drums[32] = (uint8_t)(bar->drums[32] | DRUM_KICK);
    bar->drums[48] = (uint8_t)(bar->drums[48] | DRUM_KICK | DRUM_SNARE);
    bar->drums[8]  = (uint8_t)(bar->drums[8]  | DRUM_HAT);
    bar->drums[24] = (uint8_t)(bar->drums[24] | DRUM_HAT);
    bar->drums[40] = (uint8_t)(bar->drums[40] | DRUM_HAT);
    bar->drums[56] = (uint8_t)(bar->drums[56] | DRUM_HAT);
    /* Occasional ghost hat on a 16th for some hyperactivity */
    if (rng_range(0, 9) < 3) bar->drums[4]  = (uint8_t)(bar->drums[4]  | DRUM_HAT);
    if (rng_range(0, 9) < 3) bar->drums[36] = (uint8_t)(bar->drums[36] | DRUM_HAT);
}

static void gen_bar(bar_t *bar, const variation_t *v, int bar_idx, int phrase_idx) {
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
    case BASS_WALKING:
        gen_chord_half_walking(bar, 0, chord_a);
        gen_chord_half_walking(bar, 1, chord_b);
        break;
    case BASS_ROLLING:
        gen_chord_half_rolling(bar, 0, chord_a);
        gen_chord_half_rolling(bar, 1, chord_b);
        break;
    case BASS_ROOTS:
    default:
        gen_chord_half_roots(bar, 0, chord_a);
        gen_chord_half_roots(bar, 1, chord_b);
        break;
    }

    gen_melody_from_phrase(bar, phrase_idx);

    switch (v->lead2_mode) {
    case LEAD2_HARMONY: gen_lead2_harmony(bar); break;
    case LEAD2_FILLS:   gen_lead2_fills(bar);   break;
    case LEAD2_NONE:
    default:            break;
    }

    if (v->drum_pattern == DRUMS_DANCE) {
        apply_drums_dance(bar);
    } else {
        apply_drums_rock(bar);
    }
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
