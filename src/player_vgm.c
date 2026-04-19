#include "player.h"
#include "opl2.h"
#include "display.h"
#include "timer.h"

#include <stdio.h>
#include <string.h>
#include <stdint.h>

/* VGM player. Streams an OPL2-native VGM off disk against timer_ms,
 * reflects every register write into an on-screen OPL2 channel grid,
 * and exposes a handful of runtime knobs (keys 1-4) so we can try
 * different fold workarounds without rebuilding the transcoder.
 *
 * OPL3 files are refused here; folding 18 channels down to 9 is done
 * offline by scripts/transcode_vgm.py. */

#define ATTR_NORMAL   0x07
#define ATTR_TITLE    0x0F
#define ATTR_LABEL    0x08
#define ATTR_VALUE    0x0E
#define ATTR_ON       0x0A
#define ATTR_OFF      0x08
#define ATTR_WARN     0x0C
#define ATTR_BAR      0x0B
#define ATTR_KNOB     0x0B
#define PROGRESS_W    60

enum { MODE_ERROR = 0, MODE_PLAYING, MODE_ENDED };

/* ------------------------------------------------------------------ *
 *  Per-channel shadow state (what the OPL2 currently holds).         *
 * ------------------------------------------------------------------ */

typedef struct {
    uint8_t  key_on;            /* bit 5 of last 0xB0 write           */
    uint8_t  fnum_hi_raw;       /* last 0xB0 value (incl. key bit)    */
    uint8_t  block;             /* bits 2-4 of last 0xB0              */
    uint16_t fnum;              /* full 10-bit fnum                   */
    uint8_t  mod_tl;            /* bits 0-5 of last 0x40 + mod_off    */
    uint8_t  car_tl;            /* bits 0-5 of last 0x40 + car_off    */
    uint8_t  fb_conn;           /* last 0xC0 value                    */
} ch_state_t;

static ch_state_t g_ch[OPL_CHANNELS];

/* ------------------------------------------------------------------ *
 *  VGM streaming state.                                              *
 * ------------------------------------------------------------------ */

static FILE     *g_f;
static int       g_mode;
static long      g_data_start;
static long      g_loop_start;
static uint32_t  g_total_samples;
static uint32_t  g_samples_played;
static uint32_t  g_start_ms;
static uint32_t  g_next_event_ms;
static uint32_t  g_last_display_ms;
static uint32_t  g_writes;
static uint8_t   g_last_reg;
static uint8_t   g_last_val;

/* Bitmap of which OPL2 registers have been written at least once.
 * Used by the "env freeze" knob to skip re-writes after the first. */
static uint8_t   g_reg_written[32];

#define REG_SEEN(r)    (g_reg_written[(r) >> 3] & (1u << ((r) & 7)))
#define MARK_REG(r)    (g_reg_written[(r) >> 3] |= (uint8_t)(1u << ((r) & 7)))

/* ------------------------------------------------------------------ *
 *  Knob state.                                                       *
 * ------------------------------------------------------------------ */

/* TL boost subtracts from the 6-bit TL on every 0x40-0x55 write.
 * Higher value = louder (OPL2 TL is attenuation: 0 loudest, 63 min). */
static const uint8_t TL_BOOSTS[] = { 0, 4, 8, 12, 16, 24, 32 };
#define TL_BOOSTS_N  (sizeof(TL_BOOSTS) / sizeof(TL_BOOSTS[0]))
static int g_k_tl_idx    = 0;

/* Bank filter lets us solo/mute groups of OPL2 channels so we can
 * hear where the quiet line is actually being allocated. */
enum {
    BANK_ALL = 0,
    BANK_SOLO_LOW,      /* keep 0-2, mute 3-8 */
    BANK_SOLO_MID,      /* keep 3-5           */
    BANK_SOLO_HIGH,     /* keep 6-8           */
    BANK_MUTE_LOW,
    BANK_MUTE_MID,
    BANK_MUTE_HIGH,
    BANK_N
};
static int g_k_bank      = BANK_ALL;

static int g_k_skip_keyoff = 0;   /* 0=normal, 1=drop key-off writes */
static int g_k_freeze_env  = 0;   /* 0=normal, 1=freeze 0x60/0x80 after first */

/* TL clamp pulls loud-but-attenuated writes up to a common loudness
 * floor. Any 0x40-0x55 write whose TL field is higher (= quieter)
 * than the clamp gets pinned at the clamp — so quiet voices get
 * normalised up, but already-loud voices aren't pushed past their
 * current level. 0xFF means "off" (no clamping). */
static const uint8_t TL_CLAMPS[] = { 0xFF, 0x20, 0x10, 0x08, 0x00 };
#define TL_CLAMPS_N  (sizeof(TL_CLAMPS) / sizeof(TL_CLAMPS[0]))
static int g_k_tl_clamp_idx = 0;

/* Click-experiment knob: cycles between the transcoder-default behaviour
 * and two alternative fixes, so they can be A/B'd at runtime. */
enum {
    EXP_OFF = 0,
    EXP_SOFT_ATTACK,   /* a) subtract 2 from AR on every 0x60-0x75 write —
                          softens the envelope ramp-up, which is one
                          suspect for the attack-edge click.            */
    EXP_SILENT_23,     /* b) swallow key-off writes on dst 2 and 3 only —
                          per-dst surgical version of knob [3].         */
    EXP_N
};
static int g_k_experiment = EXP_OFF;

/* ------------------------------------------------------------------ *
 *  OPL register-geometry helpers.                                    *
 * ------------------------------------------------------------------ */

static const uint16_t FNUMS[12] = {
    0x158, 0x16B, 0x181, 0x198, 0x1B0, 0x1CA,
    0x1E5, 0x202, 0x220, 0x241, 0x263, 0x287
};

static const char * const NOTE_NAMES[12] = {
    "C ", "C#", "D ", "D#", "E ", "F ", "F#", "G ", "G#", "A ", "A#", "B "
};

/* Operator offset (0x00..0x15) -> channel, is_carrier. -1 means
 * "not a valid operator slot" (the gaps 0x06/0x07, 0x0E/0x0F). */
static const int16_t OP_MAP[22] = {
    0, 1, 2,
    0x100 | 0, 0x100 | 1, 0x100 | 2,
    -1, -1,
    3, 4, 5,
    0x100 | 3, 0x100 | 4, 0x100 | 5,
    -1, -1,
    6, 7, 8,
    0x100 | 6, 0x100 | 7, 0x100 | 8
};

/* If `off` maps to a (channel, is_carrier), fill them and return 0. */
static int decode_op_offset(int off, int *out_ch, int *out_is_car) {
    int16_t m;
    if (off < 0 || off >= 22) return -1;
    m = OP_MAP[off];
    if (m < 0) return -1;
    *out_ch = m & 0x0F;
    *out_is_car = (m >> 8) & 1;
    return 0;
}

static int fnum_to_semi(uint16_t fnum) {
    int i, best = 0;
    int best_diff = 32767;
    for (i = 0; i < 12; i++) {
        int diff = (int)fnum - (int)FNUMS[i];
        if (diff < 0) diff = -diff;
        if (diff < best_diff) { best_diff = diff; best = i; }
    }
    return best;
}

/* ------------------------------------------------------------------ *
 *  Knob helpers.                                                     *
 * ------------------------------------------------------------------ */

static int is_channel_muted(int ch) {
    int bank;
    if (ch < 3)      bank = 0;
    else if (ch < 6) bank = 1;
    else             bank = 2;
    switch (g_k_bank) {
    case BANK_ALL:       return 0;
    case BANK_SOLO_LOW:  return bank != 0;
    case BANK_SOLO_MID:  return bank != 1;
    case BANK_SOLO_HIGH: return bank != 2;
    case BANK_MUTE_LOW:  return bank == 0;
    case BANK_MUTE_MID:  return bank == 1;
    case BANK_MUTE_HIGH: return bank == 2;
    }
    return 0;
}

static const char *bank_label(void) {
    switch (g_k_bank) {
    case BANK_ALL:       return "all      ";
    case BANK_SOLO_LOW:  return "solo 0-2 ";
    case BANK_SOLO_MID:  return "solo 3-5 ";
    case BANK_SOLO_HIGH: return "solo 6-8 ";
    case BANK_MUTE_LOW:  return "mute 0-2 ";
    case BANK_MUTE_MID:  return "mute 3-5 ";
    case BANK_MUTE_HIGH: return "mute 6-8 ";
    }
    return "?        ";
}

/* ------------------------------------------------------------------ *
 *  Write interception — shadow + knobs + real opl_write.             *
 * ------------------------------------------------------------------ */

static void update_shadow(uint8_t reg, uint8_t val) {
    int ch = -1, is_car;
    uint8_t base = reg & 0xF0;

    if (reg >= 0xA0 && reg <= 0xA8) {
        ch = reg - 0xA0;
        g_ch[ch].fnum = (uint16_t)((g_ch[ch].fnum & 0x0300) | val);
        return;
    }
    if (reg >= 0xB0 && reg <= 0xB8) {
        ch = reg - 0xB0;
        g_ch[ch].fnum_hi_raw = val;
        g_ch[ch].key_on      = (val >> 5) & 1;
        g_ch[ch].block       = (val >> 2) & 0x07;
        g_ch[ch].fnum        = (uint16_t)(((uint16_t)(val & 0x03) << 8) | (g_ch[ch].fnum & 0xFF));
        return;
    }
    if (reg >= 0xC0 && reg <= 0xC8) {
        ch = reg - 0xC0;
        g_ch[ch].fb_conn = val;
        return;
    }
    if (base == 0x20 || base == 0x40 || base == 0x60 || base == 0x80 || base == 0xE0) {
        int off = reg - base;
        if (decode_op_offset(off, &ch, &is_car) == 0) {
            if (base == 0x40) {
                if (is_car) g_ch[ch].car_tl = val & 0x3F;
                else        g_ch[ch].mod_tl = val & 0x3F;
            }
        }
    }
}

static uint8_t apply_tl_transforms(uint8_t val) {
    uint8_t ksl = val & 0xC0;
    uint8_t tl  = val & 0x3F;
    uint8_t boost = TL_BOOSTS[g_k_tl_idx];
    uint8_t clamp = TL_CLAMPS[g_k_tl_clamp_idx];
    int t = (int)tl;
    if (boost != 0) {
        t -= (int)boost;
        if (t < 0) t = 0;
    }
    if (clamp != 0xFF && t > (int)clamp) t = (int)clamp;
    return (uint8_t)(ksl | (uint8_t)t);
}

/* Direct OPL write with shadow update but without re-running knobs —
 * used by the event loop after knobs have already transformed the
 * value, and by knob-change handlers that need to emit silence. */
static void raw_write(uint8_t reg, uint8_t val) {
    opl_write((uint16_t)reg, val);
    update_shadow(reg, val);
    MARK_REG(reg);
}

/* Process one incoming VGM write: apply knobs, possibly drop, and
 * emit to the chip. This is the only path used by the event loop. */
static void stream_write(uint8_t reg, uint8_t val) {
    uint8_t out = val;

    /* [1] TL boost + [5] TL clamp — both act on 0x40-0x55 writes. */
    if (reg >= 0x40 && reg <= 0x55) {
        out = apply_tl_transforms(out);
    }

    /* [2] Bank filter — strip key-on bit on muted channels. */
    if (reg >= 0xB0 && reg <= 0xB8) {
        int ch = reg - 0xB0;
        if (is_channel_muted(ch)) {
            out = (uint8_t)(out & 0x1F);
        }
    }

    /* [3] Skip key-off — swallow 0xB0 writes whose key bit is 0. */
    if (g_k_skip_keyoff && reg >= 0xB0 && reg <= 0xB8 && !(out & 0x20)) {
        /* Still update the shadow so the UI reflects what the stream
         * tried to do. Don't touch the chip. */
        update_shadow(reg, out);
        MARK_REG(reg);
        g_writes++;
        g_last_reg = reg;
        g_last_val = out;
        return;
    }

    /* [4] Env freeze — skip subsequent writes to 0x60-0x75 / 0x80-0x95. */
    if (g_k_freeze_env &&
        ((reg >= 0x60 && reg <= 0x75) || (reg >= 0x80 && reg <= 0x95))) {
        if (REG_SEEN(reg)) {
            g_writes++;
            g_last_reg = reg;
            g_last_val = val;
            return;
        }
    }

    /* [6] Click experiments. */
    if (g_k_experiment == EXP_SOFT_ATTACK &&
        reg >= 0x60 && reg <= 0x75) {
        /* Lower the AR (upper nibble) by 2, keep DR (lower nibble).
         * OPL AR=15 is instant, AR=0 is slowest — subtracting 2 makes
         * the ramp-up noticeably gentler, which softens the edge that
         * reads as a click on loud notes. */
        uint8_t ar = (out >> 4) & 0x0F;
        uint8_t dr = out & 0x0F;
        ar = (ar >= 2) ? (uint8_t)(ar - 2) : 0;
        out = (uint8_t)((ar << 4) | dr);
    }
    if (g_k_experiment == EXP_SILENT_23 &&
        reg >= 0xB0 && reg <= 0xB8) {
        int ch = reg - 0xB0;
        if ((ch == 2 || ch == 3) && !(out & 0x20)) {
            /* Swallow the key-off on exactly the two dsts the listener
             * reports as click-prone. Track the intent in the shadow
             * so the HUD stays honest. */
            update_shadow(reg, out);
            MARK_REG(reg);
            g_writes++;
            g_last_reg = reg;
            g_last_val = out;
            return;
        }
    }

    opl_write((uint16_t)reg, out);
    update_shadow(reg, out);
    MARK_REG(reg);
    g_writes++;
    g_last_reg = reg;
    g_last_val = out;
}

/* Knob-change side effects that need to hit the chip immediately. */
static void apply_bank_filter_now(void) {
    int ch;
    for (ch = 0; ch < OPL_CHANNELS; ch++) {
        if (is_channel_muted(ch) && g_ch[ch].key_on) {
            /* Force a key-off without touching the fnum bits. */
            raw_write((uint8_t)(0xB0 + ch),
                      (uint8_t)(g_ch[ch].fnum_hi_raw & 0x1F));
        }
    }
}

/* ------------------------------------------------------------------ *
 *  Timing helpers.                                                   *
 * ------------------------------------------------------------------ */

static uint32_t le32(const uint8_t *p) {
    return (uint32_t)p[0]
         | ((uint32_t)p[1] <<  8)
         | ((uint32_t)p[2] << 16)
         | ((uint32_t)p[3] << 24);
}

static uint32_t samples_to_ms(uint32_t samples) {
    return (samples * 10u + 220u) / 441u;
}

static void fmt_mm_ss(uint32_t ms, char *out) {
    uint32_t total_s = ms / 1000u;
    sprintf(out, "%lu:%02lu",
            (unsigned long)(total_s / 60u),
            (unsigned long)(total_s % 60u));
}

/* ------------------------------------------------------------------ *
 *  Display.                                                          *
 * ------------------------------------------------------------------ */

static void draw_chrome(const char *arg) {
    display_vga_begin();
    display_vga_puts(0, 0, ATTR_TITLE, "adlib-rng  [VGM mode]");
    if (arg) {
        display_vga_puts  (2, 0, ATTR_LABEL, "file:");
        display_vga_printf(2, 6, ATTR_VALUE, "%s", arg);
    }
}

static void draw_status(const char *txt, unsigned char attr) {
    display_vga_puts  (8, 0, ATTR_LABEL, "status:");
    display_vga_printf(8, 8, attr, "%-20s", txt);
}

static void draw_channel_grid(void) {
    int ch;
    display_vga_puts(10, 0, ATTR_LABEL,
        "ch on note  blk fnum    mtl ctl fb/c");
    for (ch = 0; ch < OPL_CHANNELS; ch++) {
        int row = 11 + ch;
        int semi;
        unsigned char attr_row = g_ch[ch].key_on ? ATTR_ON : ATTR_OFF;
        unsigned char attr_mute = is_channel_muted(ch) ? ATTR_WARN : attr_row;

        display_vga_printf(row, 0, attr_mute, "%d ", ch);
        display_vga_printf(row, 3, attr_row,  "%s ",
                           g_ch[ch].key_on ? "* " : ". ");

        if (g_ch[ch].key_on || g_ch[ch].fnum != 0) {
            semi = fnum_to_semi(g_ch[ch].fnum);
            display_vga_printf(row, 6, attr_row, "%s%d   ",
                               NOTE_NAMES[semi], (int)g_ch[ch].block);
            display_vga_printf(row, 12, attr_row, "%d   ",
                               (int)g_ch[ch].block);
            display_vga_printf(row, 16, attr_row, "0x%03X   ",
                               (unsigned)g_ch[ch].fnum);
            display_vga_printf(row, 24, attr_row, "%02X  ",
                               (unsigned)g_ch[ch].mod_tl);
            display_vga_printf(row, 28, attr_row, "%02X  ",
                               (unsigned)g_ch[ch].car_tl);
            display_vga_printf(row, 32, attr_row, "%02X",
                               (unsigned)g_ch[ch].fb_conn);
        } else {
            display_vga_puts(row, 6, attr_row, "--  - ---     -- -- --");
        }
    }
}

static void draw_knobs(void) {
    uint8_t clamp = TL_CLAMPS[g_k_tl_clamp_idx];
    display_vga_puts  (21, 0, ATTR_LABEL, "knobs:");
    display_vga_printf(22, 2, ATTR_KNOB, "[1] tl boost: +%-4u",
                       (unsigned)TL_BOOSTS[g_k_tl_idx]);
    display_vga_printf(22, 30, ATTR_KNOB, "[4] env:      %s",
                       g_k_freeze_env ? "FREEZE " : "normal ");
    display_vga_printf(23, 2, ATTR_KNOB, "[2] bank:     %s",
                       bank_label());
    if (clamp == 0xFF) {
        display_vga_puts(23, 30, ATTR_KNOB, "[5] tl clamp: off      ");
    } else {
        display_vga_printf(23, 30, ATTR_KNOB, "[5] tl clamp: <=0x%02X   ",
                           (unsigned)clamp);
    }
    display_vga_printf(24, 2, ATTR_KNOB, "[3] key-off:  %s",
                       g_k_skip_keyoff ? "SKIP   " : "normal ");
    {
        const char *exp;
        switch (g_k_experiment) {
        case EXP_SOFT_ATTACK: exp = "soft attack"; break;
        case EXP_SILENT_23:   exp = "silent 2-3 "; break;
        default:              exp = "off        "; break;
        }
        display_vga_printf(24, 30, ATTR_KNOB, "[6] exp: %s", exp);
    }
    display_vga_puts(24, 55, ATTR_LABEL, "ESC quit  1-6");
}

static void draw_progress(uint32_t now_ms) {
    uint32_t elapsed = now_ms - g_start_ms;
    uint32_t total_ms = samples_to_ms(g_total_samples);
    char cur_buf[16], tot_buf[16];
    int  filled, i;

    if (elapsed > total_ms) elapsed = total_ms;
    fmt_mm_ss(elapsed,  cur_buf);
    fmt_mm_ss(total_ms, tot_buf);
    display_vga_puts  (5, 0, ATTR_LABEL, "time:");
    display_vga_printf(5, 6, ATTR_VALUE, "%-6s / %-6s", cur_buf, tot_buf);
    display_vga_puts  (5, 24, ATTR_LABEL, "writes:");
    display_vga_printf(5, 32, ATTR_VALUE, "%-10lu", (unsigned long)g_writes);
    display_vga_puts  (5, 46, ATTR_LABEL, "last:");
    display_vga_printf(5, 52, ATTR_VALUE, "0x%02X=0x%02X",
                       (unsigned)g_last_reg, (unsigned)g_last_val);

    filled = total_ms ? (int)((uint32_t)PROGRESS_W * elapsed / total_ms) : 0;
    if (filled > PROGRESS_W) filled = PROGRESS_W;
    display_vga_putc(6, 0, ATTR_LABEL, '[');
    for (i = 0; i < PROGRESS_W; i++) {
        int  on = (i < filled);
        display_vga_putc(6, 1 + i,
                         on ? ATTR_BAR : ATTR_LABEL,
                         on ? '#' : '-');
    }
    display_vga_putc(6, 1 + PROGRESS_W, ATTR_LABEL, ']');
}

static void redraw_all(uint32_t now_ms) {
    draw_progress(now_ms);
    draw_channel_grid();
    draw_knobs();
}

/* ------------------------------------------------------------------ *
 *  Stream decoding.                                                  *
 * ------------------------------------------------------------------ */

static int operand_len(int c) {
    if (c == 0x66) return -1;
    if (c == 0x61) return 2;
    if (c == 0x62 || c == 0x63) return 0;
    if (c >= 0x70 && c <= 0x7F) return 0;
    if (c >= 0x30 && c <= 0x3F) return 1;
    if (c >= 0x40 && c <= 0x4E) return 2;
    if (c >= 0x50 && c <= 0x5F) return 2;
    if (c >= 0xA0 && c <= 0xBF) return 2;
    if (c >= 0xC0 && c <= 0xDF) return 3;
    if (c >= 0xE0 && c <= 0xFF) return 4;
    return -1;
}

static int step_one(void) {
    int c = fgetc(g_f);
    int n, reg, val, i;

    if (c == EOF) return 1;
    if (c == 0x5A) {
        reg = fgetc(g_f);
        val = fgetc(g_f);
        if (reg == EOF || val == EOF) return 1;
        stream_write((uint8_t)reg, (uint8_t)val);
        return 0;
    }
    if (c == 0x61) {
        int lo = fgetc(g_f);
        int hi = fgetc(g_f);
        uint32_t w;
        if (lo == EOF || hi == EOF) return 1;
        w = (uint32_t)lo | ((uint32_t)hi << 8);
        g_samples_played += w;
        g_next_event_ms  += samples_to_ms(w);
        return 0;
    }
    if (c == 0x62) { g_samples_played += 735; g_next_event_ms += samples_to_ms(735); return 0; }
    if (c == 0x63) { g_samples_played += 882; g_next_event_ms += samples_to_ms(882); return 0; }
    if (c >= 0x70 && c <= 0x7F) {
        uint32_t w = (uint32_t)(c - 0x6F);
        g_samples_played += w;
        g_next_event_ms  += samples_to_ms(w);
        return 0;
    }

    n = operand_len(c);
    if (n < 0) return 1;
    for (i = 0; i < n; i++) {
        if (fgetc(g_f) == EOF) return 1;
    }
    return 0;
}

static void silence_all(void) {
    int ch;
    for (ch = 0; ch < OPL_CHANNELS; ch++) opl_note_off(ch);
    opl_write(0xBD, 0x00);
}

static void do_loop_or_end(void) {
    if (g_loop_start != 0) {
        fseek(g_f, g_loop_start, SEEK_SET);
        g_samples_played  = 0;
        g_start_ms        = timer_ms();
        g_next_event_ms   = g_start_ms;
        g_last_display_ms = g_start_ms;
        return;
    }
    silence_all();
    g_mode = MODE_ENDED;
    draw_status("done (ESC to quit)", ATTR_LABEL);
}

/* ------------------------------------------------------------------ *
 *  Init / cleanup / key handling.                                    *
 * ------------------------------------------------------------------ */

static int parse_header(const char *arg) {
    uint8_t  hdr[0x80];
    size_t   got;
    uint32_t version, ym3812_clk, ym3526_clk, ymf262_clk;
    uint32_t data_off_rel, loop_off_rel;

    if (!arg) {
        display_vga_puts(4, 0, ATTR_WARN,   "error: no VGM file given");
        display_vga_puts(6, 0, ATTR_NORMAL, "usage: ADLIB <FILE>.VGM");
        return -1;
    }
    g_f = fopen(arg, "rb");
    if (!g_f) {
        display_vga_printf(4, 0, ATTR_WARN, "error: cannot open %s", arg);
        return -1;
    }
    memset(hdr, 0, sizeof(hdr));
    got = fread(hdr, 1, sizeof(hdr), g_f);
    if (got < 0x40 || memcmp(hdr, "Vgm ", 4) != 0) {
        display_vga_printf(4, 0, ATTR_WARN, "error: %s is not a VGM file", arg);
        fclose(g_f); g_f = NULL;
        return -1;
    }

    version         = le32(&hdr[0x08]);
    g_total_samples = le32(&hdr[0x18]);
    loop_off_rel    = le32(&hdr[0x1C]);
    ym3812_clk      = le32(&hdr[0x50]);
    ym3526_clk      = le32(&hdr[0x54]);
    ymf262_clk      = le32(&hdr[0x5C]);
    data_off_rel    = (version >= 0x150) ? le32(&hdr[0x34]) : 0;
    g_data_start    = (data_off_rel != 0) ? (long)(0x34 + data_off_rel) : 0x40;
    g_loop_start    = (loop_off_rel != 0) ? (long)(0x1C + loop_off_rel) : 0;

    display_vga_printf(3, 0, ATTR_LABEL, "v%lu.%02lu  ",
                       (unsigned long)((version >> 8) & 0xFF),
                       (unsigned long)(version & 0xFF));
    if (ymf262_clk != 0) {
        display_vga_printf(3, 8, ATTR_WARN,
                           "YMF262 (OPL3) @ %lu Hz", (unsigned long)ymf262_clk);
        display_vga_puts(11, 0, ATTR_WARN,
            "This is an OPL3 dump. Run the host-side transcoder first:");
        display_vga_puts(12, 0, ATTR_NORMAL,
            "  python3 scripts/transcode_vgm.py SRC DST");
        fclose(g_f); g_f = NULL;
        return -1;
    }
    if (ym3812_clk == 0 && ym3526_clk == 0) {
        display_vga_puts(3, 8, ATTR_WARN, "(no OPL clock declared)");
        fclose(g_f); g_f = NULL;
        return -1;
    }
    display_vga_printf(3, 8, ATTR_ON,
                       "YM3812 @ %lu Hz  (OPL2 native)",
                       (unsigned long)ym3812_clk);
    return 0;
}

static void reset_state(void) {
    memset(g_ch, 0, sizeof(g_ch));
    memset(g_reg_written, 0, sizeof(g_reg_written));
    g_writes   = 0;
    g_last_reg = 0;
    g_last_val = 0;
}

static int player_vgm_init(const char *arg) {
    g_mode = MODE_ERROR;
    g_f    = NULL;
    reset_state();

    draw_chrome(arg);
    draw_knobs();

    if (parse_header(arg) != 0) {
        draw_status("error", ATTR_WARN);
        return 0;
    }

    if (fseek(g_f, g_data_start, SEEK_SET) != 0) {
        display_vga_puts(11, 0, ATTR_WARN, "error: cannot seek to data start");
        fclose(g_f); g_f = NULL;
        draw_status("error", ATTR_WARN);
        return 0;
    }

    opl_reset();
    opl_write(0x01, 0x20);
    MARK_REG(0x01);

    g_mode            = MODE_PLAYING;
    g_samples_played  = 0;
    g_start_ms        = timer_ms();
    g_next_event_ms   = g_start_ms;
    g_last_display_ms = g_start_ms;

    draw_status("playing", ATTR_ON);
    redraw_all(g_start_ms);
    return 0;
}

static void player_vgm_tick(uint32_t now_ms) {
    int budget;

    if (g_mode != MODE_PLAYING) return;

    budget = 1024;
    while (now_ms >= g_next_event_ms && budget-- > 0) {
        if (step_one()) {
            do_loop_or_end();
            if (g_mode != MODE_PLAYING) break;
        }
    }

    if (now_ms - g_last_display_ms >= 150u) {
        g_last_display_ms = now_ms;
        redraw_all(now_ms);
    }
}

static void player_vgm_on_key(int k) {
    switch (k) {
    case '1':
        g_k_tl_idx = (g_k_tl_idx + 1) % (int)TL_BOOSTS_N;
        break;
    case '2':
        g_k_bank = (g_k_bank + 1) % BANK_N;
        apply_bank_filter_now();
        break;
    case '3':
        g_k_skip_keyoff = !g_k_skip_keyoff;
        break;
    case '4':
        g_k_freeze_env = !g_k_freeze_env;
        break;
    case '5':
        g_k_tl_clamp_idx = (g_k_tl_clamp_idx + 1) % (int)TL_CLAMPS_N;
        break;
    case '6':
        g_k_experiment = (g_k_experiment + 1) % EXP_N;
        break;
    default:
        return;
    }
    draw_knobs();
}

static void player_vgm_cleanup(void) {
    silence_all();
    opl_reset();
    if (g_f) { fclose(g_f); g_f = NULL; }
    display_vga_clear(ATTR_NORMAL);
    display_vga_puts(0, 0, ATTR_NORMAL, "adlib-rng: bye.");
    display_vga_end();
}

const player_t PLAYER_VGM = {
    "vgm",
    player_vgm_init,
    player_vgm_tick,
    player_vgm_on_key,
    player_vgm_cleanup,
};
