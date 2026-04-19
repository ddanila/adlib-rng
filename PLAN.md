# adlib-rng — next-step plan

## Decisions from listening session

- **Drums**: rhythm mode (OPL2 native) sounds better than FM voices.
  → Drop FM drum mode. Make rhythm the only path. Remove the `R`
    key, the FM_* instrument patches, the FM drum channel routing,
    and the `rhythm` CLI arg (it becomes the default and only mode).

- **Variations**: 3 (`+phrases`) and 4 (`+50s pop`) are the keepers;
  1 (`baseline`) and 2 (`+pentatonic`) are not interesting enough to
  keep.
  → Drop variations 1 and 2. Keep the phrase-based ones. New `1` and
    `2` keys map to what are currently `3` and `4`. Slot 3 and 4 are
    open for further experiments.

## Continue iterating from here

With FM and the random-pick variations gone, the next round explores
the phrase-based melody under rhythm-mode drums:

- More / better phrase patterns in `PHRASE_BANK` — current 8 are quite
  sparse and short.
- Phrase concatenation across bars instead of fully independent per-
  bar pick — gives multi-bar arcs.
- Try other progressions for the new free slots (e.g. `I-V-vi-IV`,
  `vi-IV-I-V`, `I-IV-vi-V`).
- Bass: walking patterns vs root-only (currently very static).
- Phrase rhythm variation — currently fixed 8th-note grid; try 16ths
  or syncopation in the phrase encoding.
