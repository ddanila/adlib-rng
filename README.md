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
  main.c       entry, step loop, key handling
  opl2.{h,c}   OPL2 register driver and instrument patches
  timer.{h,c}  IRQ0 hook for sub-BIOS-tick timing (1 ms resolution)
  rng.{h,c}    deterministic xorshift32
  music.{h,c}  scale, chord progression, pattern generator
  display.{h,c} VGA text-mode UI (bar / step grid, current notes)
scripts/
  run.sh       QEMU launcher with AdLib audio device
Makefile       OpenWatcom build + floppy packaging
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
A:> ADLIB          default seed 0x1337
A:> ADLIB 42       seed = 42
```

Drums always use OPL2 native rhythm mode (FM-voice drums were cut in
the last listening round).

Keys while playing (RNG re-seeds on every switch, so only the
variation itself changes — clean A/B):

- `ESC` — quit
- `1` — `blues+phr` — phrase-bank melody over 12-bar blues
  (`I-IV-V`), root bass
- `2` — `50s+phr` — same melody style over the `I-vi-IV-V` doo-wop
  progression, root bass
- `3` — `50s+walk` — as **2**, but bass walks (root → chord tone)
  on every half-bar for more motion
- `4` — `50s+2bar` — as **2**, but melody picks 2-bar phrase *pairs*
  (statement → answer) instead of independent per-bar phrases, for
  multi-bar arcs

## What you'll hear

Key is A major (pentatonic throughout — the 4th and 7th degrees got
cut for sounding too modal). Harmony is one of two progressions at
half-bar resolution depending on variation: the 12-bar blues (looped
twice) or the `I-vi-IV-V` "50s pop" cycle (looped six times). Melody
is key-anchored, so the solo stays in key over every chord.
Drums hit a basic kick / snare / hat pattern with a sprinkle of
RNG-driven extra kicks. Tempo is fixed at 120 BPM, 64 substeps/bar.

## License

MIT — see [LICENSE](LICENSE).
