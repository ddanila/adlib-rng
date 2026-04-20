# adlib-rng

A tiny algorithmic chiptune generator for MS-DOS that drives a real
AdLib (OPL2 / Yamaha YM3812), plus a VGM player with a host-side
OPL3 → OPL2 transcoder. Runs in QEMU against an MS-DOS 4.0 boot
floppy.

Not a real music engine — an excuse to wire up a 16-bit DOS toolchain
end-to-end on a modern Mac.

## Requirements

- **`mtools`** (`mcopy`, `mformat`)
- **`qemu-system-i386`**

Open Watcom v2 and the MS-DOS boot floppy are both vendored under
`vendor/` — no external install or network needed to build. See
[NOTES.md](NOTES.md#refreshing-vendor-bundles) for how to refresh
them.

## Build & run

```sh
make           # build/adlib.exe
make vgms      # (re)transcode sources/*.vgm → assets/*.vgm
make floppy    # build/adlib.img — DOS floppy with the EXE + the VGMs
make run       # boot in QEMU; AUTOEXEC.BAT starts ADLIB SUSPENSE.VGM
make clean
```

Override the DOS image with `make floppy FLOPPY_SRC=/path/to/other.img`.

## Usage

From the DOS prompt (hit `ESC` after boot to drop out of the
auto-started VGM):

```
A:> ADLIB                default seed 0x1337 (RNG player)
A:> ADLIB 42             seed = 42 (RNG player)
A:> ADLIB SUSPENSE.VGM   VGM player
```

A filename contains a dot, a seed doesn't — that's how `main.c`
picks the player.

**RNG player keys:** `ESC` quit, `a`-`f` swap seed, `1`-`4` pick
variation. **VGM player keys:** `ESC` quit, `1`-`6` cycle runtime
knobs (volume boost, bank solo/mute, key-off skip, envelope freeze,
TL clamp, click-reduction experiment). Knob states are shown live
on the in-DOS HUD.

See [NOTES.md](NOTES.md) for the source layout, the four RNG
variations, the VGM player internals, the transcoder's auto-fixes
(TL clamp + OPL3 voice-allocation fold), and how to add a new
player backend.

## License

MIT — see [LICENSE](LICENSE).
