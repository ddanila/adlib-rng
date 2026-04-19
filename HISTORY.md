# adlib-rng — iteration history

A rough record of what we tried, what we kept, and what we dropped —
kept here so the musical decisions don't have to be re-derived from
the git log next time. `git log --oneline` has the full story; this
is the listening-session summary.

## Round 1 — bootstrap

Goal: get a toolchain + OPL2 driver + rhythm-ish music end-to-end.

- OpenWatcom v2 cross-compile on macOS to a 16-bit DOS .EXE
- PIT reprogrammed to 1 kHz for sub-BIOS-tick step resolution
- 12 bars × 64 substeps per bar @ 120 BPM (`STEP_US = 31250`)
- `Am - Dm - G - C` loop (vi-ii-V-I in C-major terms, 4 bars per
  chord)
- Random scale-degree pick per 8th, A natural minor
- FM-voice drums (ch 6/7/8) and OPL2 native rhythm mode, switchable
  with `R`

## Round 2 — harmony iterations

- **Tried** 12-bar minor blues `i-i-i-i / iv-iv-i-i / V-iv-i-V`.
  Better shape than the 4-chord loop, but melody sounded off over
  V — picking from each chord's *local* minor scale clashed with the
  key.
- **Fixed** by anchoring the melody to the key root (A) regardless
  of the chord. Bass still follows the chord root.
- **Tried** switching the key from minor to major — same chord roots
  but A major scale on top. Sounded less dark.
- **User feedback**: "still sounds a bit minor, not the generic
  progression, but notes inside" → full A major scale's 4th and 7th
  degrees were sounding modal.
- **Tried** quick changes: 2 chords per bar. Standard 12-bar blues
  compressed to 6 bars and looped twice.
- **Bug hit**: `bar_t.chord_root_midi` grew from single byte to a
  2-element array; header-dep tracking was missing so `main.c` kept
  the old layout → drums spilled into the next bar's data, kicks
  multiplied across bars. Fixed with `$(HEADERS)` deps in the
  Makefile.

## Round 3 — A/B framework + phrase-based melody

Added `1-4` keys to switch variations live with the RNG re-seeded so
only the variation itself changes (clean A/B). First 4 slots tried:

1. baseline — random scale, full major, blues
2. `+pentatonic` — same, minus 4th/7th (addresses the "modal notes"
   issue)
3. `+phrases` — pick from a fixed 8-entry bank of 1-bar melodic
   phrases, not slot-by-slot random — gives the solo actual contour
4. `+50s pop` — phrases over `I-vi-IV-V`

**User verdict**: rhythm-mode drums > FM drums; phrase-based melody
(3, 4) clearly > random-per-slot (1, 2). FM drum code was cut
entirely and the random variations dropped.

## Round 4 — drop FM, new walking / 2-bar experiments

With FM mode gone and the random slots free, four variations became:

1. `blues+phr` — phrases over 12-bar blues
2. `50s+phr` — phrases over `I-vi-IV-V`
3. `50s+walk` — as 2, plus walking bass (root on down-beat → RNG
   chord-tone on back-beat)
4. `50s+2bar` — as 2, but melody picks 2-bar phrase *pairs* for
   multi-bar arcs

Phrase bank grew 8 → 16 entries (denser/more varied shapes).

- **Bug hit**: V3's walking bass used fixed intervals `{+4, +7, +9}`
  (major 3rd / 5th / 6th from root). Over `vi` (F#m) that put +4 on
  A# and +9 on D# — both outside A major pentatonic. User noticed
  "4th bass note is out of tune all the time". Fixed by picking the
  offset so `root + offset` lands on an A-pentatonic note regardless
  of the chord's quality.

## Round 5 — all-major + 90s electronic

- **User**: "let's also always play in major".
- **Took**: I-III-IV-V (A - C# - D - E). All-major, no vi chord at
  all. Same rising/resolving shape as the 50s progression.
- **Added** 90s-electronic variants to replace the rock-flavoured
  V1/V2 slots (user's pick: "drop 1 and 2", "focus on 3 and 4",
  "Aphrodite is a perfect example"):
  - `BASS_ROLLING` — 16th-note sub-bass roll with 10% octave bumps
  - `DRUMS_DANCE` — 4-on-the-floor kick, clap on 2/4, off-beat 8th
    hats plus random 16th ghost hats

## Round 6 — more notes + second lead voice

- **User**: "the solo is very static and not changing much… let's
  try a bit more notes around and instruments".
- **Added** `OPL_INSTR_LEAD_BRIGHT` (quarter-sine waveform, faster
  decay, more feedback) on OPL channel 2 as a second melodic voice.
- V3 became `90s+harm` — second voice doubles the melody an octave
  up (classic layered-lead thickening).
- V4 became `90s+fills` — second voice plays 16th-note ornamental
  fills between the main phrase notes (50% per gap).

## Round 7 — genre playground

- **User**: "leave number 4 as an example" (reference); V1-V3 become
  three different genres.
- New ingredients:
  - `BASS_PUMP` — root / octave alternating on 8ths (funk / French
    house bass)
  - `DRUMS_TECHNO` — 4-on-the-floor + off-beat hats, no snare
  - `DRUMS_BREAK` — syncopated kicks, snare + ghost, 16th ghost hats
- Variations:
  1. `techno` — minimal: `DRUMS_TECHNO` + `BASS_ROOTS`, no harmony
  2. `dnb` — `DRUMS_BREAK` + `BASS_ROLLING` + octave-up harmony
  3. `daft` — `DRUMS_DANCE` + `BASS_PUMP` + 2-bar phrases + harmony
  4. `90s+fills` — unchanged reference

## Round 8 — melody goes per-genre

- **User**: "the main melody for the solo instrument is basically
  very similar for all of those. why?"
- **Why it was**: all variations called the same
  `gen_melody_from_phrase` against the same 16-entry `PHRASE_BANK`
  on the same 8th-note grid in the same A-pent range. Variations
  only differed in bass / drums / second-voice layer — the solo line
  itself was style-blind.
- **Fix**: introduce `melody_mode_t` on the variation and give each
  genre its own strategy. `gen_bar` split into a skeleton
  (bass + drums) + separate melody + lead2 passes so music_generate
  can drive the melody mode from the top.
  - `MEL_MINIMAL_LOOP` (techno) — pick ONE phrase from a new
    `MINIMAL_BANK` (6 sparse phrases, 1-3 notes each) at the top of
    the loop, reuse for all 12 bars.
  - `MEL_STAB_16TH` (dnb) — separate `STAB_BANK` of 16-slot
    phrases on the 16th-note grid (step = s*4). Fresh per bar.
  - `MEL_HOOK_2BAR` (daft) — pick ONE pair from
    `PHRASE_PAIR_BANK` and lock it across all six pair-slots in the
    12-bar loop.
  - `MEL_FRESH_PHRASE` (ref) — unchanged, current 16-entry bank
    fresh per bar.
- `phr_mode` / `PHR_*` dropped — subsumed by `melody_mode`.

## Invariants that settled along the way

Things that got decided early-ish and haven't moved since:

- **Key**: A
- **Scale**: A major pentatonic only (full major's 4th and 7th were
  too modal; minor feels wrong with the major progressions)
- **Tempo**: 120 BPM, 64 substeps/bar, 12 bars then loop
- **Drums**: always OPL2 native rhythm mode (FM drums were cut)
- **Melody**: key-anchored to A regardless of chord — stays in key
  over every chord
- **Progression**: `I-III-IV-V` in A (all-major)
- **Determinism**: same seed → identical output; RNG re-seeds on
  every variation switch so A/B is clean
