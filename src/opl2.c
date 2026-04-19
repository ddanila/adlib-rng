#include "opl2.h"
#include <conio.h>

/* OPL2 spec wait times: 3.3 us after register select, 23 us after data
 * write. On any modern host (and inside QEMU) these are met trivially.
 * Old code used to read the status port a bunch of times; do the same
 * for safety on real 8086s in case anyone tries this on metal. */
static void io_wait_short(void) {
    int i;
    for (i = 0; i < 6; i++) (void)inp(OPL_PORT_REG);
}
static void io_wait_long(void) {
    int i;
    for (i = 0; i < 36; i++) (void)inp(OPL_PORT_REG);
}

void opl_write(uint16_t reg, uint8_t val) {
    outp(OPL_PORT_REG, (uint8_t)reg);
    io_wait_short();
    outp(OPL_PORT_DATA, val);
    io_wait_long();
}

void opl_reset(void) {
    int r;
    for (r = 0; r < 256; r++) opl_write((uint16_t)r, 0);
    /* Enable waveform-select feature so non-sine waveforms work. */
    opl_write(0x01, 0x20);
}

void opl_init(void) {
    opl_reset();
}

/* F-numbers for one octave. The block field of register 0xBn selects
 * the octave separately, so a single 12-entry table suffices for all
 * octaves. Tuned for A=440Hz at block=4. */
static const uint16_t FNUMS[12] = {
    /* C    C#   D    D#   E    F    F#   G    G#   A    A#   B   */
    0x158, 0x16B, 0x181, 0x198, 0x1B0, 0x1CA, 0x1E5, 0x202, 0x220, 0x241, 0x263, 0x287
};

/* Maps voice 0..8 to its two operator register offsets. */
static const uint8_t OP_OFFSETS[9][2] = {
    {0x00, 0x03}, {0x01, 0x04}, {0x02, 0x05},
    {0x08, 0x0B}, {0x09, 0x0C}, {0x0A, 0x0D},
    {0x10, 0x13}, {0x11, 0x14}, {0x12, 0x15},
};

static uint8_t rhythm_bd_reg = 0x00;

void opl_note_on(int ch, int midi_note) {
    int octave, semi, block;
    uint16_t fnum;
    uint8_t low, high;

    if (midi_note < 0)  midi_note = 0;
    if (midi_note > 95) midi_note = 95;

    /* MIDI 60 = C4 → octave 4. */
    octave = (midi_note / 12) - 1;
    semi   = midi_note % 12;
    if (octave < 0) octave = 0;
    if (octave > 7) octave = 7;
    block = octave;
    fnum  = FNUMS[semi];

    low  = (uint8_t)(fnum & 0xFF);
    high = (uint8_t)(((fnum >> 8) & 0x03) | (block << 2) | 0x20);  /* key-on */

    opl_write((uint16_t)(0xA0 + ch), low);
    opl_write((uint16_t)(0xB0 + ch), high);
}

void opl_note_off(int ch) {
    /* Clear key-on bit; leave the envelope to release naturally. */
    opl_write((uint16_t)(0xB0 + ch), 0);
}

static void opl_write_op(int ch, int which, const opl_op_t *op) {
    uint8_t off = OP_OFFSETS[ch][which];
    opl_write((uint16_t)(0x20 + off), op->am_vib_eg_ksr_mult);
    opl_write((uint16_t)(0x40 + off), op->ksl_tl);
    opl_write((uint16_t)(0x60 + off), op->ar_dr);
    opl_write((uint16_t)(0x80 + off), op->sl_rr);
    opl_write((uint16_t)(0xE0 + off), op->waveform);
}

void opl_set_instrument(int ch, const opl_instr_t *ins) {
    opl_write_op(ch, 0, &ins->mod);
    opl_write_op(ch, 1, &ins->car);
    opl_write((uint16_t)(0xC0 + ch), ins->feedback_conn);
}

void opl_rhythm_mode(int enable) {
    rhythm_bd_reg = (uint8_t)(enable ? OPL_RHY_ENABLE : 0x00);
    opl_write(0xBD, rhythm_bd_reg);
}

void opl_rhythm_trigger(uint8_t bits) {
    rhythm_bd_reg = (uint8_t)((rhythm_bd_reg & 0xE0) | (bits & 0x1F));
    opl_write(0xBD, rhythm_bd_reg);
}

void opl_rhythm_stop(void) {
    rhythm_bd_reg = (uint8_t)(rhythm_bd_reg & 0xE0);
    opl_write(0xBD, rhythm_bd_reg);
}

void opl_rhythm_setup_pitches(void) {
    /* Voice 6 (BD) gets a normal note via Bn register. Pitch a low C. */
    opl_write(0xA6, 0x40);
    opl_write(0xB6, 0x01);   /* block=0, fnum_high=0, key-off (rhythm bit drives it) */
    /* Voice 7 hosts SD (carrier) and HH (modulator). Mid-high pitch. */
    opl_write(0xA7, 0x05);
    opl_write(0xB7, 0x09);   /* block=2, low fnum */
    /* Voice 8 hosts TT (modulator) and TC (carrier). */
    opl_write(0xA8, 0x05);
    opl_write(0xB8, 0x09);
}

/* === Instrument patches.
 * Hand-tuned starting points; expect to iterate by ear. === */

const opl_instr_t OPL_INSTR_LEAD = {
    /* mod */ { 0x01, 0x10, 0xF1, 0x53, 0x00 },
    /* car */ { 0x01, 0x00, 0xF1, 0x53, 0x00 },
    0x08
};

/* Brighter, pluckier lead for the second melodic voice. Quarter-sine
 * waveform (reg 0xE0 value 3) adds high harmonics, faster decay cuts
 * the tail so rapid 16ths don't mush together, and more feedback
 * gives it edge. */
const opl_instr_t OPL_INSTR_LEAD_BRIGHT = {
    /* mod */ { 0x01, 0x00, 0xF8, 0xA3, 0x03 },
    /* car */ { 0x01, 0x00, 0xF8, 0xA3, 0x03 },
    0x0C
};

const opl_instr_t OPL_INSTR_BASS = {
    /* mod */ { 0x21, 0x1A, 0xF0, 0x33, 0x00 },
    /* car */ { 0x01, 0x00, 0xF0, 0x33, 0x00 },
    0x06
};

const opl_instr_t OPL_INSTR_RHY_BD = {
    /* mod */ { 0x01, 0x00, 0xF8, 0x66, 0x00 },
    /* car */ { 0x01, 0x00, 0xF8, 0x66, 0x00 },
    0x00
};

/* Voice 7 in rhythm mode: mod operator = HH, car operator = SD.
 * Tuned to be percussive (fast attack, fast release). */
const opl_instr_t OPL_INSTR_RHY_SD_HH = {
    /* mod (HH) */ { 0x01, 0x00, 0xF8, 0x37, 0x03 },
    /* car (SD) */ { 0x01, 0x00, 0xF8, 0x47, 0x00 },
    0x00
};

/* Voice 8: mod = TT, car = TC. Currently we don't trigger TT/TC. */
const opl_instr_t OPL_INSTR_RHY_TT_TC = {
    /* mod (TT) */ { 0x05, 0x00, 0xF6, 0x47, 0x00 },
    /* car (TC) */ { 0x01, 0x00, 0xF6, 0x47, 0x00 },
    0x00
};
