#include "player.h"
#include "opl2.h"
#include "display.h"
#include "timer.h"

#include <stdio.h>
#include <string.h>
#include <stdint.h>

/* VGM player. Streams an OPL2-native VGM off disk, schedules register
 * writes against timer_ms, and handles the loop point.
 *
 * OPL3 files are refused here: folding 18 OPL3 channels down to 9 OPL2
 * voices is a musical decision that belongs in the host-side transcoder
 * (stage #2b). Run that script first to produce an OPL2-native file. */

#define ATTR_NORMAL 0x07
#define ATTR_TITLE  0x0F
#define ATTR_LABEL  0x08
#define ATTR_VALUE  0x0E
#define ATTR_WARN   0x0C
#define ATTR_OK     0x0A
#define ATTR_BAR    0x0B
#define PROGRESS_W  60

enum { MODE_ERROR = 0, MODE_PLAYING, MODE_ENDED };

static FILE     *g_f;
static int       g_mode;
static long      g_data_start;
static long      g_loop_start;       /* absolute; 0 if no loop point */
static uint32_t  g_total_samples;    /* from header; for progress bar */
static uint32_t  g_samples_played;   /* accumulated across wait cmds  */
static uint32_t  g_start_ms;
static uint32_t  g_next_event_ms;    /* when the next command fires   */
static uint32_t  g_last_display_ms;
/* Running counters the user can reference in bug reports ("it clicked
 * at write N" / "at reg 0xBn"). Displayed live on the HUD. */
static uint32_t  g_writes;
static uint8_t   g_last_reg;
static uint8_t   g_last_val;

static uint32_t le32(const uint8_t *p) {
    return (uint32_t)p[0]
         | ((uint32_t)p[1] <<  8)
         | ((uint32_t)p[2] << 16)
         | ((uint32_t)p[3] << 24);
}

/* 441 samples = 10 ms exactly at 44100 Hz. Rounded to nearest ms.
 * Called on per-wait values (≤ 65535 samples ≈ 1.486 s) so `samples*10`
 * never overflows 32 bits. */
static uint32_t samples_to_ms(uint32_t samples) {
    return (samples * 10u + 220u) / 441u;
}

static void fmt_mm_ss(uint32_t ms, char *out) {
    uint32_t total_s = ms / 1000u;
    sprintf(out, "%lu:%02lu",
            (unsigned long)(total_s / 60u),
            (unsigned long)(total_s % 60u));
}

static void draw_chrome(const char *arg) {
    display_vga_begin();
    display_vga_puts(0,  0, ATTR_TITLE, "adlib-rng  [VGM mode]");
    display_vga_puts(24, 0, ATTR_LABEL, "ESC: quit");
    if (arg) {
        display_vga_puts  (2, 0, ATTR_LABEL, "file:");
        display_vga_printf(2, 7, ATTR_VALUE, "%s", arg);
    }
}

static void draw_status(const char *txt, unsigned char attr) {
    display_vga_puts  (13, 0, ATTR_LABEL, "status:");
    display_vga_printf(13, 8, attr, "%-20s", txt);
}

static void draw_progress(uint32_t now_ms) {
    uint32_t elapsed = now_ms - g_start_ms;
    uint32_t total_ms = samples_to_ms(g_total_samples);
    char cur_buf[16], tot_buf[16];
    int  filled, i;

    if (elapsed > total_ms) elapsed = total_ms;
    fmt_mm_ss(elapsed,  cur_buf);
    fmt_mm_ss(total_ms, tot_buf);
    display_vga_puts  (10, 0, ATTR_LABEL, "time:");
    display_vga_printf(10, 8, ATTR_VALUE, "%-6s / %-6s", cur_buf, tot_buf);

    filled = total_ms ? (int)((uint32_t)PROGRESS_W * elapsed / total_ms) : 0;
    if (filled > PROGRESS_W) filled = PROGRESS_W;
    display_vga_putc(11, 0, ATTR_LABEL, '[');
    for (i = 0; i < PROGRESS_W; i++) {
        int  on = (i < filled);
        display_vga_putc(11, 1 + i,
                         on ? ATTR_BAR : ATTR_LABEL,
                         on ? '#' : '-');
    }
    display_vga_putc(11, 1 + PROGRESS_W, ATTR_LABEL, ']');

    /* Live write counter + last-written register. If you hear an
     * artefact, jot down the number shown here — it's a deterministic
     * timestamp that survives across playback. */
    display_vga_puts  (15, 0, ATTR_LABEL, "writes:");
    display_vga_printf(15, 8, ATTR_VALUE, "%-10lu", (unsigned long)g_writes);
    display_vga_puts  (15, 22, ATTR_LABEL, "last reg:");
    display_vga_printf(15, 32, ATTR_VALUE, "0x%02X = 0x%02X",
                       (unsigned)g_last_reg, (unsigned)g_last_val);
}

static void silence_all(void) {
    int ch;
    for (ch = 0; ch < OPL_CHANNELS; ch++) opl_note_off(ch);
    opl_write(0xBD, 0x00);   /* rhythm enable + triggers off */
}

/* Classify a one-byte VGM command. Returns the number of operand bytes
 * to consume, or -1 for end-of-stream. Covers the common cases; rare
 * variable-length commands (0x67 data block, 0x90-0x95 DAC streams) are
 * treated as stream-end — an OPL-only file won't contain them. */
static int operand_len(int c) {
    if (c == 0x66) return -1;                   /* end of sound data */
    if (c == 0x61) return 2;                    /* wait nnnn         */
    if (c == 0x62 || c == 0x63) return 0;       /* fixed waits       */
    if (c >= 0x70 && c <= 0x7F) return 0;       /* short wait        */
    if (c >= 0x30 && c <= 0x3F) return 1;       /* 1-arg reserved    */
    if (c >= 0x40 && c <= 0x4E) return 2;       /* 2-arg reserved    */
    if (c >= 0x50 && c <= 0x5F) return 2;       /* chip writes       */
    if (c >= 0xA0 && c <= 0xBF) return 2;
    if (c >= 0xC0 && c <= 0xDF) return 3;
    if (c >= 0xE0 && c <= 0xFF) return 4;
    return -1;
}

/* Returns 1 when the stream hit its terminator, 0 otherwise. */
static int step_one(void) {
    int c = fgetc(g_f);
    int n, reg, val, i;

    if (c == EOF) return 1;
    if (c == 0x5A) {
        reg = fgetc(g_f);
        val = fgetc(g_f);
        if (reg == EOF || val == EOF) return 1;
        opl_write((uint16_t)reg, (uint8_t)val);
        g_writes++;
        g_last_reg = (uint8_t)reg;
        g_last_val = (uint8_t)val;
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
        uint32_t w = (uint32_t)(c - 0x6F);      /* 1..16 */
        g_samples_played += w;
        g_next_event_ms  += samples_to_ms(w);
        return 0;
    }

    /* Unknown but length-predictable: skip its operands. */
    n = operand_len(c);
    if (n < 0) return 1;
    for (i = 0; i < n; i++) {
        if (fgetc(g_f) == EOF) return 1;
    }
    return 0;
}

static void do_loop_or_end(void) {
    if (g_loop_start != 0) {
        fseek(g_f, g_loop_start, SEEK_SET);
        /* Reset the wall clock so the progress bar restarts cleanly. */
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

    display_vga_puts  (4, 0, ATTR_LABEL, "version:");
    display_vga_printf(4, 9, ATTR_VALUE, "%lu.%02lu",
                       (unsigned long)((version >> 8) & 0xFF),
                       (unsigned long)(version & 0xFF));
    display_vga_puts  (5, 0, ATTR_LABEL, "chip:");

    if (ymf262_clk != 0) {
        display_vga_printf(5, 8, ATTR_WARN,
                           "YMF262 (OPL3) @ %lu Hz", (unsigned long)ymf262_clk);
        display_vga_puts(7, 0, ATTR_WARN,
            "This is an OPL3 dump. Run the host-side transcoder first:");
        display_vga_puts(8, 0, ATTR_NORMAL,
            "  python3 scripts/transcode_vgm.py  (TODO stage #2b)");
        fclose(g_f); g_f = NULL;
        return -1;
    }
    if (ym3812_clk == 0 && ym3526_clk == 0) {
        display_vga_puts(5, 8, ATTR_WARN, "(none declared)");
        display_vga_puts(7, 0, ATTR_WARN, "No OPL clock in header; refusing to play.");
        fclose(g_f); g_f = NULL;
        return -1;
    }
    display_vga_printf(5, 8, ATTR_OK,
                       "YM3812 @ %lu Hz (OPL2 native)",
                       (unsigned long)ym3812_clk);
    return 0;
}

static int player_vgm_init(const char *arg) {
    g_mode = MODE_ERROR;
    g_f = NULL;

    draw_chrome(arg);

    if (parse_header(arg) != 0) {
        draw_status("error", ATTR_WARN);
        return 0;      /* stay running so the user can read the banner */
    }

    if (fseek(g_f, g_data_start, SEEK_SET) != 0) {
        display_vga_puts(7, 0, ATTR_WARN, "error: cannot seek to data start");
        fclose(g_f); g_f = NULL;
        draw_status("error", ATTR_WARN);
        return 0;
    }

    opl_reset();
    opl_write(0x01, 0x20);   /* waveform-select enable (harmless if the
                                stream also sets it) */
    g_mode            = MODE_PLAYING;
    g_samples_played  = 0;
    g_start_ms        = timer_ms();
    g_next_event_ms   = g_start_ms;
    g_last_display_ms = g_start_ms;
    g_writes          = 0;
    g_last_reg        = 0;
    g_last_val        = 0;

    draw_status("playing", ATTR_OK);
    draw_progress(g_start_ms);
    return 0;
}

static void player_vgm_tick(uint32_t now_ms) {
    int budget;

    if (g_mode != MODE_PLAYING) return;

    /* Cap the inner loop so key polling stays responsive even if we
     * need to chew through a large burst of back-to-back writes. Any
     * backlog just carries into the next tick. */
    budget = 1024;
    while (now_ms >= g_next_event_ms && budget-- > 0) {
        if (step_one()) {
            do_loop_or_end();
            if (g_mode != MODE_PLAYING) break;
        }
    }

    if (now_ms - g_last_display_ms >= 250u) {
        g_last_display_ms = now_ms;
        draw_progress(now_ms);
    }
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
    NULL,                /* no key handling yet */
    player_vgm_cleanup,
};
