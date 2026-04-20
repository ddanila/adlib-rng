# Notes

Longer-form detail that doesn't fit in the README. Read the README
first for the 60-second overview.

## Build targets

```sh
make           # build/adlib.exe only
make vgms      # (re)transcode sources/*.vgm → assets/*.vgm and
               # regenerate assets/testopl.vgm via the test helper
make floppy    # build/adlib.img — DOS floppy with the EXE + the VGMs
make run       # everything above + boot QEMU (default target for use)
make clean     # rm -rf build/
```

Override the DOS boot image:

```sh
make floppy FLOPPY_SRC=/path/to/other.img
```

Override the Open Watcom location:

```sh
make WATCOM_DIR=/path/to/other/openwatcom
```

## Source layout

```
src/
  main.c            thin dispatcher — picks a player from argv, runs
                    the tick/key loop
  player.{h}        vtable shared by every playback engine
                    (init / tick / on_key / cleanup)
  player_rng.c      the algorithmic chiptune generator (default player)
  player_vgm.c      streaming VGM reader — plays OPL2-native files
  opl2.{h,c}        OPL2 register driver and instrument patches
  timer.{h,c}       IRQ0 hook for sub-BIOS-tick timing (1 ms resolution)
  rng.{h,c}         deterministic xorshift32
  music.{h,c}       scale, chord progression, pattern generator
  display.{h,c}     VGA text-mode primitives + RNG-player layout
sources/
  *.vgm             OPL3 originals (not shipped on the floppy). The
                    transcoder folds these into assets/.
assets/
  *.vgm             OPL2-native VGMs ready for the floppy. Auto-copied
                    as 8.3 uppercase (assets/foo.vgm -> FOO.VGM;
                    longer basenames are truncated to 8 chars).
scripts/
  run.sh                 QEMU launcher with AdLib audio device
  make_test_vgm.py       emits assets/testopl.vgm for smoke-testing
                         the VGM player
  transcode_vgm.py       OPL3 -> OPL2 transcoder (voice-allocating fold)
  vendor_openwatcom.sh   refresh the vendored OpenWatcom snapshot
vendor/
  openwatcom-v2/         OpenWatcom v2 host tools (see its README.md)
  msdos/                 MS-DOS 4.0 boot floppy image (see its README.md)
Makefile                 OpenWatcom build + floppy packaging + vgms target
```

## RNG player

Key is A major, pentatonic only. Harmony across all variations is
`I-III-IV-V` (A - C# - D - E, all major) at half-bar resolution —
the 4-chord cycle spans 2 bars and loops six times per 12-bar round.
Melody is key-anchored, so it always stays in A pentatonic.
Drums hit a basic kick / snare / hat pattern with a sprinkle of
RNG-driven extra kicks. Tempo is fixed at 120 BPM, 64 substeps/bar.
Drums use OPL2's native rhythm mode (FM-voice drums were cut in an
earlier listening round).

### Keys while playing

RNG is re-seeded on every switch, so only the variation itself
changes — clean A/B.

- `ESC` — quit
- `a`-`f` — swap the RNG seed to one of six predefined values
  (`0x1337`, `42`, `0xDEADBEEF`, `0xCAFEBABE`, `0xBADC0DE`, `0x8086`)
  and regenerate with the current variation still selected. Lets you
  A/B seeds without changing style.

### Variations

All three "daft" variations share the same bass (octave pump), drums
(house), melody mode (locked 2-bar hook with arch-shape octave lift),
and harmony (octave up). Only the **progression** changes — so you
can A/B what the harmonic structure alone does to the same musical
material.

- `1` — `daft-blues` — daft style over a **12-bar blues** (`I-IV-V`
  compressed to half-bar resolution, looped twice). More harmonic
  movement, classic blues shape.
- `2` — `daft-vamp` — daft style over a **1-bar I-IV vamp**. Slowest
  harmonic motion of the three — the locked hook and pumping bass
  carry the piece, like a minimal-house loop.
- `3` — `daft` — daft style over `I-III-IV-V` (reference).
- `4` — `90s+fills` — rolling bass + dance drums + fresh-per-bar
  phrase from the main bank + 16th-note ornamental fills on channel 2.

All progressions stay all-major (I, III, IV, V in A are all major
chords).

## VGM mode

Drop a `.vgm` file under `assets/`, rebuild the floppy, and launch it
as `ADLIB FOO.VGM` from the DOS prompt. Filenames are shortened to
8.3 upper-case on the way onto the floppy, so `assets/foo-bar.vgm`
lands as `FOO-BAR.VGM` (the basename is truncated to 8 chars).

What the player does:

- streams **OPL2-native** VGMs (YM3812 clock set; no YMF262) off the
  floppy and clocks the register writes against `timer_ms`,
- handles the header, variable-length waits (`0x61`/`0x62`/`0x63` and
  the `0x70..0x7F` short form), the loop offset, and end-of-stream,
- shows a per-channel OPL2 grid, a progress bar, and live register
  activity counters,
- exposes six runtime knobs (keys `1`-`6`) for volume boost, bank
  solo/mute, key-off skip, envelope freeze, TL clamp, and a click-
  reduction experiment cycle.

### OPL3 originals → OPL2 assets

OPL3 dumps (YMF262) are refused by the DOS player — AdLib hardware
is OPL2 only, and the port-1 register bank (channels 9-17, 4-op,
stereo) has no equivalent on the chip. Instead, drop OPL3 files into
`sources/` and run the host-side transcoder:

```sh
make vgms           # rebuilds assets/*.vgm from sources/*.vgm
# or explicitly:
python3 scripts/transcode_vgm.py sources/suspense.vgm assets/suspense.vgm
```

The transcoder walks the source stream with a virtual OPL3 model
(all 18 channels, full per-operator patch, fnum, key state), and at
every key-on transition dynamically allocates an OPL2 voice — taking
a free one when available, else LRU-stealing the dst channel whose
current note started longest ago, so a sustained pad gets sacrificed
before a fresh transient. For `sources/suspense.vgm` it folds 2968
key-ons onto 9 voices with 58 steals and **0 dropped notes**.

### Transcoder auto-fixes

A one-shot pre-pass decides two per-song transforms:

- **Auto TL clamp** (default on). Clamp every carrier-TL write at
  `min(TL_seen) + 10` so the quiet half of the mix is pulled up to a
  common floor without touching the loud peaks. Trade: compresses
  dynamic range from the bottom.
- **Legato** (opt-in, `--legato`). If a source channel's dominant
  carrier SL is ≤ 2, skip its key-off writes — the patch's envelope
  decays to silence on its own, the key-off is cosmetic. Interacts
  badly with the voice allocator though (dst never frees → whole
  song flattens into drones), so it stays off by default.

Flags:

```
--raw               skip the pre-pass entirely
--legato            opt in to the legato rule above
--no-clamp          preserve original TL distribution
--defer-keyoff      hold key-off writes until next key-on on same
                    dst (masks the click under a new attack; tends
                    to expose new clicks on previously-quiet voices)
--trace             per-key-on tracing on stderr
```

### Generating a test VGM

`scripts/make_test_vgm.py` emits `assets/testopl.vgm` — an A-major
pentatonic scale, one voice, ~5 s — for smoke-testing the engine
without going through the transcoder.

## Adding a new player

Implement the `player_t` vtable in `src/player.h` and wire it into
`main.c`'s argv dispatch. Each player owns its own display layout on
top of the `display_vga_*` primitives in `display.h`.

## Refreshing vendor bundles

### Open Watcom v2

```sh
scripts/vendor_openwatcom.sh
```

The script downloads the latest `Current-build` snapshot from the
Open Watcom v2 GitHub release, extracts the subset we need into a
new `vendor/openwatcom-v2/current-build-<date>/` directory, and
prints fresh SHA-256s. After it finishes, bump the `WATCOM_DIR`
default in the `Makefile` to the new date and delete the previous
directory. See the vendor bundle's own `README.md` for the full
per-file mapping.

### MS-DOS boot floppy

The image under `vendor/msdos/floppy-minimal.img` comes from the
[`ddanila/msdos`](https://github.com/ddanila/msdos/releases) release.
To refresh it (only if the upstream image changes):

```sh
gh release download 0.1 --repo ddanila/msdos \
    --pattern floppy-minimal.img --dir vendor/msdos --clobber
shasum -a 256 vendor/msdos/floppy-minimal.img
```

Paste the new checksum into `vendor/msdos/README.md`.
