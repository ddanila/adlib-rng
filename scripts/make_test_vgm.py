#!/usr/bin/env python3
"""Generate a small OPL2-native VGM for testing the DOS playback engine.

Emits an A-major pentatonic scale on a single instrument across ~8 s
of audio. Writes to assets/testopl.vgm (named short so the DOS 8.3
filesystem on our boot floppy keeps it readable as `TESTOPL.VGM`).
Useful until the OPL3->OPL2 transcoder (stage #2b) can produce real
test material.
"""
from __future__ import annotations

import os
import struct
import sys

SAMPLE_RATE = 44100

# F-numbers for one octave at block=4 (A=440 Hz).
FNUMS = [0x158, 0x16B, 0x181, 0x198, 0x1B0, 0x1CA,
         0x1E5, 0x202, 0x220, 0x241, 0x263, 0x287]

# Operator offsets for OPL2 channel N. Match src/opl2.c.
OP_OFFSETS = [
    (0x00, 0x03), (0x01, 0x04), (0x02, 0x05),
    (0x08, 0x0B), (0x09, 0x0C), (0x0A, 0x0D),
    (0x10, 0x13), (0x11, 0x14), (0x12, 0x15),
]


class VGMBuilder:
    def __init__(self) -> None:
        self.data = bytearray()
        self.samples = 0

    def opl2(self, reg: int, val: int) -> None:
        self.data += bytes([0x5A, reg & 0xFF, val & 0xFF])

    def wait(self, samples: int) -> None:
        while samples > 65535:
            self.data += bytes([0x61]) + struct.pack("<H", 65535)
            samples -= 65535
            self.samples += 65535
        if samples > 0:
            self.data += bytes([0x61]) + struct.pack("<H", samples)
            self.samples += samples

    def wait_ms(self, ms: int) -> None:
        self.wait(round(ms * SAMPLE_RATE / 1000))

    def end(self) -> None:
        self.data += bytes([0x66])


def set_instrument(v: VGMBuilder, ch: int,
                   mod: tuple[int, int, int, int, int],
                   car: tuple[int, int, int, int, int],
                   fb_conn: int) -> None:
    """Write one 2-op patch: (am_vib_eg_ksr_mult, ksl_tl, ar_dr, sl_rr, wf)."""
    mod_off, car_off = OP_OFFSETS[ch]
    for base, off, op in ((0x20, mod_off, mod), (0x20, car_off, car)):
        v.opl2(base + off,     op[0])
        v.opl2(0x40 + off,     op[1])
        v.opl2(0x60 + off,     op[2])
        v.opl2(0x80 + off,     op[3])
        v.opl2(0xE0 + off,     op[4])
    v.opl2(0xC0 + ch, fb_conn)


def note_on(v: VGMBuilder, ch: int, midi: int) -> None:
    semi = midi % 12
    octave = max(0, min(7, midi // 12 - 1))
    fnum = FNUMS[semi]
    v.opl2(0xA0 + ch, fnum & 0xFF)
    v.opl2(0xB0 + ch, ((fnum >> 8) & 0x03) | (octave << 2) | 0x20)


def note_off(v: VGMBuilder, ch: int) -> None:
    v.opl2(0xB0 + ch, 0x00)


def build_header(v: VGMBuilder) -> bytes:
    """VGM v1.51 header, YM3812 only. Data starts at 0x80."""
    hdr = bytearray(0x80)
    hdr[0:4] = b"Vgm "
    # version 1.51 (BCD-ish in LE)
    struct.pack_into("<I", hdr, 0x08, 0x151)
    # GD3: none
    # total samples
    struct.pack_into("<I", hdr, 0x18, v.samples)
    # loop offset (rel to 0x1C): 0 = no loop
    # loop samples: 0
    # data offset (rel to 0x34): 0x4C so data starts at 0x80
    struct.pack_into("<I", hdr, 0x34, 0x80 - 0x34)
    # YM3812 clock — standard AdLib
    struct.pack_into("<I", hdr, 0x50, 3_579_545)
    # EoF offset (rel to 0x04) = filesize - 4
    struct.pack_into("<I", hdr, 0x04, len(hdr) + len(v.data) - 4)
    return bytes(hdr)


def main() -> int:
    v = VGMBuilder()

    # Waveform-select enable so our square waves are legal.
    v.opl2(0x01, 0x20)

    # Bright lead voice on channel 0.
    set_instrument(
        v, ch=0,
        mod=(0x01, 0x10, 0xF1, 0x53, 0x00),
        car=(0x01, 0x00, 0xF1, 0x53, 0x00),
        fb_conn=0x08,
    )

    # A-major pentatonic scale up two octaves, plus a couple of
    # sustained high notes so the instrument tail is audible.
    scale = [57, 59, 61, 64, 66, 69, 71, 73, 76, 78, 81, 84]
    for midi in scale:
        note_on(v, 0, midi)
        v.wait_ms(250)
        note_off(v, 0)
        v.wait_ms(60)

    note_on(v, 0, 69)   # land on A
    v.wait_ms(1200)
    note_off(v, 0)
    v.wait_ms(400)

    v.end()

    out_dir = os.path.join(os.path.dirname(os.path.abspath(__file__)),
                           "..", "assets")
    os.makedirs(out_dir, exist_ok=True)
    out_path = os.path.join(out_dir, "testopl.vgm")
    with open(out_path, "wb") as f:
        f.write(build_header(v))
        f.write(v.data)

    size = os.path.getsize(out_path)
    secs = v.samples / SAMPLE_RATE
    print(f"wrote {out_path}: {size} bytes, {v.samples} samples ({secs:.2f} s)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
