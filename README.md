# adlib-rng

A tiny algorithmic chiptune generator for MS-DOS that drives a real
AdLib (OPL2 / Yamaha YM3812). Picks notes from a hardcoded scale and
chord progression, fills in a melody / bass / drum pattern across 12
bars, and loops forever. Same seed always produces the same music.

Built to be compiled with OpenWatcom v2 (cross-compiled from macOS or
Linux to 16-bit DOS) and run inside QEMU with the `-device adlib`
emulation, on top of an MS-DOS 4.0 bootable floppy.

## Why

Curiosity about OPL2 FM synthesis, plus an excuse to wire up a full
DOS toolchain end-to-end on a modern Mac. Not a real music engine.

## Layout

```
src/
  main.c          thin dispatcher — picks a player from argv, runs the
                  tick/key loop
  player.{h}      vtable shared by every playback engine
                  (init / tick / on_key / cleanup)
  player_rng.c    the algorithmic chiptune generator (default player)
  player_vgm.c    VGM file reader — stub so far, see "VGM mode" below
  opl2.{h,c}      OPL2 register driver and instrument patches
  timer.{h,c}     IRQ0 hook for sub-BIOS-tick timing (1 ms resolution)
  rng.{h,c}       deterministic xorshift32
  music.{h,c}     scale, chord progression, pattern generator
  display.{h,c}   VGA text-mode primitives + RNG-player layout
assets/
  *.vgm           tracker dumps; auto-copied onto the floppy as 8.3
                  uppercase filenames (FOO.vgm -> FOO.VGM)
scripts/
  run.sh          QEMU launcher with AdLib audio device
Makefile          OpenWatcom build + floppy packaging
```

## Requirements

Host tools (macOS or Linux):

- **OpenWatcom v2** — used as a 16-bit DOS cross-compiler. The
  Makefile defaults to a build at
  `~/fun/beta_kappa/vendor/openwatcom-v2/current-build-2026-04-04`.
  Override with `WATCOM_DIR=...` if yours lives elsewhere.
- **mtools** (`mcopy`, `mformat`) — to inject `ADLIB.EXE` into a
  bootable DOS floppy image without mounting anything.
- **qemu-system-i386** — emulates the PC and its OPL2.

Plus a bootable **MS-DOS 4.0 floppy image**. By default `make floppy`
fetches `floppy-minimal.img` from the latest
[ddanila/msdos release](https://github.com/ddanila/msdos/releases) via
`gh` and caches it under `build/dos/`. Override to use your own image:

```sh
make floppy FLOPPY_SRC=/path/to/dos.img
```

## Build

```sh
make           # build/adlib.exe
make floppy    # build/adlib.img — DOS floppy with ADLIB.EXE + AUTOEXEC.BAT
make run       # boot floppy in QEMU, AdLib enabled
make clean
```

## Usage

Inside DOS (the floppy's `AUTOEXEC.BAT` runs it automatically with
default args):

```
A:> ADLIB                default seed 0x1337 (RNG player)
A:> ADLIB 42             seed = 42 (RNG player)
A:> ADLIB SUSPENSE.VGM   VGM player (see "VGM mode" below)
```

Argument convention: a filename contains a dot (e.g. `SUSPENSE.VGM`);
anything else is treated as a numeric seed for the RNG player.

Drums always use OPL2 native rhythm mode (FM-voice drums were cut in
the last listening round).

Keys while playing (RNG re-seeds on every switch, so only the
variation itself changes — clean A/B):

- `ESC` — quit
- `a`-`f` — swap the RNG seed to one of six predefined values
  (`0x1337`, `42`, `0xDEADBEEF`, `0xCAFEBABE`, `0xBADC0DE`, `0x8086`)
  and regenerate with the current variation still selected. Lets you
  A/B seeds without changing style.
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
- `3` — `daft` — daft style over `I-III-IV-V` (unchanged reference
  from round 7).
- `4` — `90s+fills` — anchor from round 6: rolling bass + dance drums
  + fresh-per-bar phrase from the main bank + 16th-note ornamental
  fills on channel 2.

All progressions stay all-major (I, III, IV, V in A are all major
chords).

## What you'll hear

Key is A major, pentatonic only. Harmony across all variations is
`I-III-IV-V` (A - C# - D - E, all-major) at half-bar resolution —
the 4-chord cycle spans 2 bars and loops six times per 12-bar round.
Melody is key-anchored, so it always stays in A pentatonic.
Variations differ on drums (rock vs 4-on-the-floor), bass (root /
walking / rolling 16ths) and melody granularity (per-bar phrases vs
2-bar phrase pairs).
Drums hit a basic kick / snare / hat pattern with a sprinkle of
RNG-driven extra kicks. Tempo is fixed at 120 BPM, 64 substeps/bar.

## VGM mode

Drop a `.vgm` file under `assets/`, rebuild the floppy, and launch it
as `ADLIB FOO.VGM` from the DOS prompt. Filenames are shortened to
8.3 upper-case on the way onto the floppy, so `assets/foo-bar.vgm`
lands as `FOO-BAR.VGM` (the basename is truncated if longer than 8).

What works today (**stage #2a**):

- streams **OPL2-native** VGMs (YM3812 clock set; no YMF262) off the
  floppy and clocks the register writes against `timer_ms`,
- handles the header, variable-length waits (`0x61`/`0x62`/`0x63` and
  the `0x70..0x7F` short form), the loop offset, and end-of-stream,
- shows a progress bar, elapsed/total time, and the declared chip
  clocks while it plays.

What doesn't (yet):

- **OPL3 dumps are refused.** AdLib hardware is OPL2-only, and the
  port-1 register bank (channels 9-17, 4-op, stereo) has no
  equivalent on the chip — folding 18 voices down to 9 is a musical
  decision that belongs in the host-side transcoder, not the DOS
  player. See stage #2b below.

### Generating a test VGM

`scripts/make_test_vgm.py` emits `assets/testopl.vgm` — an A-major
pentatonic scale, one voice, 5 s — enough to smoke-test the engine
without waiting on the transcoder.

```sh
python3 scripts/make_test_vgm.py
make floppy && make run
# at the DOS prompt (ESC out of the RNG first):
A:> ADLIB TESTOPL.VGM
```

### Roadmap

- **#2b — OPL3→OPL2 transcoder (TODO).** Host-side Python tool that
  reads an OPL3 VGM, simulates register state, and re-emits an
  OPL2-native VGM. Voice folding is allocation-based rather than
  naive drop-port-1, so notes should survive whenever there's a free
  AdLib voice.

### Adding a new player

Implement the `player_t` vtable in `src/player.h` and wire it into
`main.c`'s argv dispatch. Each player owns its own display layout on
top of the `display_vga_*` primitives in `display.h`.

## License

MIT — see [LICENSE](LICENSE).
