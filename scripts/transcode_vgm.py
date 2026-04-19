#!/usr/bin/env python3
"""Fold an OPL3 VGM down to OPL2 while preserving as many notes as
possible.

Strategy:
  - Parse the source stream once, keeping a virtual OPL3 model (all 18
    channels, full per-operator patch, fnum, key state).
  - At every key-on transition in the source, dynamically allocate an
    OPL2 channel: take a free one; if none is free, LRU-steal the
    channel whose current note started longest ago. Reserve channels
    6/7/8 when the source runs in OPL2 rhythm mode.
  - On the key-on, emit the full patch (2 ops × 5 regs + feedback/
    connection + fnum_lo + fnum_hi-with-key-on) to the allocated
    destination — simpler and safer than register diffing.
  - Hold a live src→dst mapping; route subsequent pitch/patch/fnum
    writes during the note through it. Drop writes when unmapped.
  - Mask OPL3-only bits: stereo (C0 bits 4/5), 4-op carrier connection
    (C0 bit 6), waveforms 4-7 (E0 bit 2 cleared). Silently drop the
    OPL3 mode enable (port-1 0x05) and 4-op register (port-1 0x04).
  - Emit a clean v1.51 YM3812 VGM with the same wait timing as input.

Loop offsets are preserved *if* the source's loop point lands exactly
on a command boundary we could identify — otherwise the output has no
loop and plays once. Good enough for the first cut.

Usage:
    scripts/transcode_vgm.py SRC.vgm DST.vgm
"""
from __future__ import annotations

import struct
import sys
from pathlib import Path
from typing import Dict, List, Optional, Tuple

# ---------------------------------------------------------------- #
# VGM parsing
# ---------------------------------------------------------------- #

class VGMHeader:
    def __init__(self, hdr: bytes) -> None:
        if hdr[0:4] != b"Vgm ":
            raise ValueError("Not a VGM file")
        self.version       = struct.unpack_from("<I", hdr, 0x08)[0]
        self.total_samples = struct.unpack_from("<I", hdr, 0x18)[0]
        loop_off_rel       = struct.unpack_from("<I", hdr, 0x1C)[0]
        self.loop_offset   = (0x1C + loop_off_rel) if loop_off_rel else 0
        self.loop_samples  = struct.unpack_from("<I", hdr, 0x20)[0]
        if self.version >= 0x150:
            data_off_rel = struct.unpack_from("<I", hdr, 0x34)[0]
            self.data_offset = (0x34 + data_off_rel) if data_off_rel else 0x40
        else:
            self.data_offset = 0x40
        self.ym3812_clk = struct.unpack_from("<I", hdr, 0x50)[0] if self.version >= 0x151 else 0
        self.ym3526_clk = struct.unpack_from("<I", hdr, 0x54)[0] if self.version >= 0x151 else 0
        self.ymf262_clk = struct.unpack_from("<I", hdr, 0x5C)[0] if self.version >= 0x151 else 0


def parse_commands(data: bytes, start: int):
    """Yield (kind, *args, byte_pos) for each command.
    Kinds: 'w' (port, reg, val), 'wait' (samples), 'end', 'data_block'."""
    i = start
    n = len(data)
    while i < n:
        pos = i
        c = data[i]
        if c == 0x5A:   # YM3812 write
            yield ("w", 0, data[i + 1], data[i + 2], pos); i += 3
        elif c == 0x5B:   # YM3526 (OPL1) — treat as OPL2-compat
            yield ("w", 0, data[i + 1], data[i + 2], pos); i += 3
        elif c == 0x5E:   # YMF262 port 0
            yield ("w", 0, data[i + 1], data[i + 2], pos); i += 3
        elif c == 0x5F:   # YMF262 port 1
            yield ("w", 1, data[i + 1], data[i + 2], pos); i += 3
        elif c == 0x61:
            w = data[i + 1] | (data[i + 2] << 8)
            yield ("wait", w, pos); i += 3
        elif c == 0x62:
            yield ("wait", 735, pos); i += 1
        elif c == 0x63:
            yield ("wait", 882, pos); i += 1
        elif 0x70 <= c <= 0x7F:
            yield ("wait", c - 0x6F, pos); i += 1
        elif c == 0x66:
            yield ("end", pos); return
        elif c == 0x67:
            # 0x67 0x66 tt ss ss ss ss [data...]
            sz = struct.unpack_from("<I", data, i + 3)[0]
            i += 7 + sz
        elif 0x30 <= c <= 0x3F: i += 2
        elif 0x40 <= c <= 0x4E: i += 3
        elif 0x50 <= c <= 0x5F: i += 3
        elif 0xA0 <= c <= 0xBF: i += 3
        elif 0xC0 <= c <= 0xDF: i += 4
        elif 0xE0 <= c <= 0xFF: i += 5
        else:
            raise ValueError(f"Unknown VGM command 0x{c:02X} at 0x{i:X}")


# ---------------------------------------------------------------- #
# OPL register geometry
# ---------------------------------------------------------------- #

# (mod_op_offset, car_op_offset) per channel 0..8 within a 9-channel bank.
OP_OFFSETS = [
    (0x00, 0x03), (0x01, 0x04), (0x02, 0x05),
    (0x08, 0x0B), (0x09, 0x0C), (0x0A, 0x0D),
    (0x10, 0x13), (0x11, 0x14), (0x12, 0x15),
]
OP_TO_CH: Dict[int, Tuple[int, bool]] = {}
for _ch, (_mod, _car) in enumerate(OP_OFFSETS):
    OP_TO_CH[_mod] = (_ch, False)   # (channel, is_carrier)
    OP_TO_CH[_car] = (_ch, True)


def classify_reg(reg: int):
    """Decode a per-channel register into (kind, bank_ch, slot, is_car).
    kind ∈ {'op','fnum_lo','fnum_hi','fb_conn'}. Returns None if the
    register isn't per-channel."""
    slot_by_base = {0x20: 0, 0x40: 1, 0x60: 2, 0x80: 3, 0xE0: 4}
    for base, slot in slot_by_base.items():
        if base <= reg <= base + 0x15:
            off = reg - base
            if off in OP_TO_CH:
                ch, car = OP_TO_CH[off]
                return ("op", ch, slot, car)
    if 0xA0 <= reg <= 0xA8: return ("fnum_lo", reg - 0xA0, -1, False)
    if 0xB0 <= reg <= 0xB8: return ("fnum_hi", reg - 0xB0, -1, False)
    if 0xC0 <= reg <= 0xC8: return ("fb_conn", reg - 0xC0, -1, False)
    return None


# ---------------------------------------------------------------- #
# Transcoder
# ---------------------------------------------------------------- #

class SrcChannel:
    __slots__ = ("mod", "car", "fb_conn", "fnum_lo", "fnum_hi", "key_on")
    def __init__(self) -> None:
        self.mod = [0, 0, 0, 0, 0]   # 0x20, 0x40, 0x60, 0x80, 0xE0
        self.car = [0, 0, 0, 0, 0]
        self.fb_conn = 0
        self.fnum_lo = 0
        self.fnum_hi = 0              # raw value incl. block + key bit
        self.key_on = False


class Transcoder:
    def __init__(self, trace: bool = False) -> None:
        self.src: List[SrcChannel] = [SrcChannel() for _ in range(18)]
        self.dst_key    = [False] * 9
        self.dst_owner: List[Optional[int]] = [None] * 9
        self.dst_last_used = [0] * 9
        self.map: Dict[int, int] = {}
        self.counter = 0
        self.rhythm_mode = False
        self.out: List[tuple] = []
        # Shadow of the OPL2 register file so we can diff-suppress
        # redundant writes. DOS player opl_reset()'s to all-zeros before
        # streaming, so that matches our starting state.
        self.dst_regs: Dict[int, int] = {}
        self.sample_time = 0
        self.trace = trace
        self.stats = {
            "key_on": 0, "stolen": 0, "dropped": 0, "p1_mode_hits": 0,
            "writes_kept": 0, "writes_suppressed": 0,
        }

    # --- Output buffer helpers ----------------------------------- #

    def emit(self, reg: int, val: int) -> None:
        """Always emit. Use for the key-on edge, where the envelope
        needs the transition even when the numeric value matches."""
        reg &= 0xFF
        val &= 0xFF
        self.out.append(("w", reg, val))
        self.dst_regs[reg] = val
        self.stats["writes_kept"] += 1

    def emit_maybe(self, reg: int, val: int) -> None:
        """Emit only if the value differs from what's already in the dst
        register. Safe for all patch writes and for fnum/release paths
        — never use it for do_note_on's key-on write."""
        reg &= 0xFF
        val &= 0xFF
        if self.dst_regs.get(reg) == val:
            self.stats["writes_suppressed"] += 1
            return
        self.out.append(("w", reg, val))
        self.dst_regs[reg] = val
        self.stats["writes_kept"] += 1

    def wait(self, n: int) -> None:
        self.out.append(("wait", n))
        self.sample_time += n

    def end(self) -> None:
        self.out.append(("end",))

    def log(self, msg: str) -> None:
        if not self.trace:
            return
        s = self.sample_time
        sys.stderr.write(f"[{s // 44100:3d}:{(s % 44100) * 1000 // 44100:03d}] {msg}\n")

    # --- Voice allocation ---------------------------------------- #

    def allocate(self, src_ch: int) -> Optional[int]:
        pool = list(range(6)) if self.rhythm_mode else list(range(9))
        for d in pool:
            if not self.dst_key[d]:
                return d
        # All busy in the pool — LRU-steal.
        self.stats["stolen"] += 1
        victim = min(pool, key=lambda x: self.dst_last_used[x])
        prev_owner = self.dst_owner[victim]
        self.log(f"steal dst {victim} from src {prev_owner} for src {src_ch}")
        if prev_owner is not None and self.map.get(prev_owner) == victim:
            del self.map[prev_owner]
        # Force a key-off edge on the stolen voice so its envelope
        # releases cleanly before the new note lands on top of it.
        self.emit(0xB0 + victim, 0x00)
        self.dst_key[victim]   = False
        self.dst_owner[victim] = None
        return victim

    # --- Emission primitives ------------------------------------- #

    def install_patch(self, dst: int, src_ch: int) -> None:
        """Configure dst with src_ch's current patch. Diff-suppressed —
        if the dst already holds the exact values, we skip those writes
        so consecutive notes on the same voice don't re-tickle the
        envelope generator (that's what caused the bass click)."""
        src = self.src[src_ch]
        mod_off, car_off = OP_OFFSETS[dst]
        for slot_idx, base in enumerate([0x20, 0x40, 0x60, 0x80]):
            self.emit_maybe(base + mod_off, src.mod[slot_idx])
            self.emit_maybe(base + car_off, src.car[slot_idx])
        # Waveforms 4-7 exist only on OPL3; mask to OPL2's 2-bit field.
        self.emit_maybe(0xE0 + mod_off, src.mod[4] & 0x03)
        self.emit_maybe(0xE0 + car_off, src.car[4] & 0x03)
        # C0 has stereo (bits 4-5) and 4-op carrier flag (bit 6) on
        # OPL3; OPL2 only cares about bits 0-3 (FB + connection).
        self.emit_maybe(0xC0 + dst, src.fb_conn & 0x0F)
        self.emit_maybe(0xA0 + dst, src.fnum_lo)

    # --- Source event handlers ----------------------------------- #

    def do_note_on(self, src_ch: int, fnum_hi_val: int) -> None:
        dst = self.allocate(src_ch)
        if dst is None:
            self.stats["dropped"] += 1
            self.log(f"DROP note on src {src_ch}: no free dst")
            return
        self.install_patch(dst, src_ch)
        # Key-on MUST always emit — the envelope needs the rising edge
        # even when fnum_hi happens to match what's already in the reg.
        self.emit(0xB0 + dst, fnum_hi_val & 0x3F)
        self.map[src_ch]        = dst
        self.dst_owner[dst]     = src_ch
        self.dst_key[dst]       = True
        self.counter           += 1
        self.dst_last_used[dst] = self.counter
        self.stats["key_on"]   += 1
        src = self.src[src_ch]
        self.log(f"keyon  src {src_ch:2d} -> dst {dst}  fnum=0x{fnum_hi_val & 0x3F:02X}{src.fnum_lo:02X}  car_tl=0x{src.car[1] & 0x3F:02X} mod_tl=0x{src.mod[1] & 0x3F:02X}")

    def do_note_off(self, src_ch: int) -> None:
        dst = self.map.get(src_ch)
        if dst is None:
            return
        self.emit_maybe(0xB0 + dst, self.src[src_ch].fnum_hi & 0x1F)
        self.dst_key[dst] = False
        del self.map[src_ch]

    def do_freq_change(self, src_ch: int) -> None:
        dst = self.map.get(src_ch)
        if dst is None:
            return
        s = self.src[src_ch]
        self.emit_maybe(0xA0 + dst, s.fnum_lo)
        self.emit_maybe(0xB0 + dst, s.fnum_hi & 0x3F)

    def do_fnum_lo(self, src_ch: int) -> None:
        dst = self.map.get(src_ch)
        if dst is None:
            return
        self.emit_maybe(0xA0 + dst, self.src[src_ch].fnum_lo)

    def do_op_write(self, src_ch: int, slot: int, is_car: bool) -> None:
        dst = self.map.get(src_ch)
        if dst is None:
            return
        mod_off, car_off = OP_OFFSETS[dst]
        base = [0x20, 0x40, 0x60, 0x80, 0xE0][slot]
        off  = car_off if is_car else mod_off
        val  = (self.src[src_ch].car if is_car else self.src[src_ch].mod)[slot]
        if slot == 4:
            val &= 0x03
        self.emit_maybe(base + off, val)

    def do_fb_conn(self, src_ch: int) -> None:
        dst = self.map.get(src_ch)
        if dst is None:
            return
        self.emit_maybe(0xC0 + dst, self.src[src_ch].fb_conn & 0x0F)

    # --- Main dispatch ------------------------------------------- #

    def write(self, port: int, reg: int, val: int) -> None:
        # --- Global / pass-through registers ---
        if port == 0 and reg == 0xBD:
            self.rhythm_mode = (val & 0x20) != 0
            self.emit(0xBD, val)
            return
        if port == 0 and reg in (0x01, 0x02, 0x03, 0x04, 0x08):
            self.emit(reg, val)
            return
        # --- OPL3-only control (port 1) ---
        if port == 1 and reg in (0x04, 0x05, 0x08):
            self.stats["p1_mode_hits"] += 1
            return

        cls = classify_reg(reg)
        if cls is None:
            return
        kind, bank_ch, slot, is_car = cls
        src_ch = bank_ch + (9 if port == 1 else 0)
        src = self.src[src_ch]

        if kind == "op":
            if is_car:
                src.car[slot] = val
            else:
                src.mod[slot] = val
            self.do_op_write(src_ch, slot, is_car)
        elif kind == "fnum_lo":
            src.fnum_lo = val
            self.do_fnum_lo(src_ch)
        elif kind == "fnum_hi":
            old_key = src.key_on
            src.fnum_hi = val
            new_key = (val & 0x20) != 0
            src.key_on = new_key
            # Rhythm-mode drum channels don't use key-on bits on 0xB6/7/8.
            if self.rhythm_mode and port == 0 and bank_ch in (6, 7, 8):
                # Still forward the pitch/block to dst if currently
                # mapped — but since rhythm mode owns dst 6/7/8, skip.
                return
            if new_key and not old_key:
                self.do_note_on(src_ch, val)
            elif old_key and not new_key:
                self.do_note_off(src_ch)
            elif new_key and old_key:
                self.do_freq_change(src_ch)
        elif kind == "fb_conn":
            src.fb_conn = val
            self.do_fb_conn(src_ch)


# ---------------------------------------------------------------- #
# Output writer
# ---------------------------------------------------------------- #

def encode_wait(n: int) -> bytes:
    out = bytearray()
    while n > 0:
        if n <= 16:
            out.append(0x6F + n)
            n = 0
        elif n == 735:
            out.append(0x62); n = 0
        elif n == 882:
            out.append(0x63); n = 0
        elif n <= 65535:
            out.append(0x61)
            out += struct.pack("<H", n)
            n = 0
        else:
            out.append(0x61)
            out += struct.pack("<H", 65535)
            n -= 65535
    return bytes(out)


def encode_events(events: List[tuple]) -> bytes:
    """Collapse runs of waits and emit a packed VGM body."""
    out = bytearray()
    pending = 0
    for ev in events:
        if ev[0] == "wait":
            pending += ev[1]
            continue
        if pending:
            out += encode_wait(pending)
            pending = 0
        if ev[0] == "w":
            out += bytes([0x5A, ev[1], ev[2]])
        elif ev[0] == "end":
            out.append(0x66)
    if pending:
        out += encode_wait(pending)
    return bytes(out)


def build_header(total_samples: int, data_len: int) -> bytes:
    hdr = bytearray(0x80)
    hdr[0:4] = b"Vgm "
    struct.pack_into("<I", hdr, 0x08, 0x151)
    struct.pack_into("<I", hdr, 0x18, total_samples)
    # Loop: not preserved in this pass (see module docstring).
    struct.pack_into("<I", hdr, 0x34, 0x80 - 0x34)          # data offset
    struct.pack_into("<I", hdr, 0x50, 3_579_545)            # YM3812 clock
    struct.pack_into("<I", hdr, 0x04, len(hdr) + data_len - 4)
    return bytes(hdr)


# ---------------------------------------------------------------- #
# CLI
# ---------------------------------------------------------------- #

def main() -> int:
    args = list(sys.argv[1:])
    trace = False
    if "--trace" in args:
        trace = True
        args.remove("--trace")
    if len(args) != 2:
        print(f"usage: {sys.argv[0]} [--trace] SRC.vgm DST.vgm", file=sys.stderr)
        return 2
    src_path = Path(args[0])
    dst_path = Path(args[1])

    data = src_path.read_bytes()
    hdr  = VGMHeader(data)

    print(f"src: {src_path}  version {hdr.version >> 8}.{hdr.version & 0xFF:02X}")
    chip = []
    if hdr.ym3812_clk: chip.append(f"YM3812 @ {hdr.ym3812_clk} Hz")
    if hdr.ym3526_clk: chip.append(f"YM3526 @ {hdr.ym3526_clk} Hz")
    if hdr.ymf262_clk: chip.append(f"YMF262 @ {hdr.ymf262_clk} Hz")
    print("     chips:", ", ".join(chip) or "(none)")

    t = Transcoder(trace=trace)
    total_samples = 0
    for ev in parse_commands(data, hdr.data_offset):
        kind = ev[0]
        if kind == "w":
            _, port, reg, val, _pos = ev
            t.write(port, reg, val)
        elif kind == "wait":
            _, n, _pos = ev
            t.wait(n)
            total_samples += n
        elif kind == "end":
            t.end()
            break

    body   = encode_events(t.out)
    header = build_header(total_samples, len(body))
    dst_path.parent.mkdir(parents=True, exist_ok=True)
    dst_path.write_bytes(header + body)

    print(f"dst: {dst_path}  {len(header) + len(body)} bytes, "
          f"{total_samples} samples ({total_samples / 44100:.1f} s)")
    print(f"  key-on events: {t.stats['key_on']}")
    print(f"  voice steals : {t.stats['stolen']}")
    print(f"  dropped      : {t.stats['dropped']}")
    print(f"  p1 mode regs : {t.stats['p1_mode_hits']} (silently skipped)")
    total_w = t.stats['writes_kept'] + t.stats['writes_suppressed']
    if total_w:
        pct = 100 * t.stats['writes_suppressed'] / total_w
        print(f"  writes       : {t.stats['writes_kept']} kept, "
              f"{t.stats['writes_suppressed']} diff-suppressed ({pct:.0f}%)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
