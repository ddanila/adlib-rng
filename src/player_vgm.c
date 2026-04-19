#include "player.h"
#include "opl2.h"
#include "display.h"

#include <stdio.h>
#include <string.h>
#include <stdint.h>

/* Stub VGM player. Opens the file, parses just enough of the header
 * to identify the source chip, and displays a status banner. It does
 * not emit audio yet — real playback waits on the host-side OPL3->OPL2
 * transcoder (see README roadmap, stage #2). Tick is a no-op; the user
 * returns to DOS with ESC. */

#define ATTR_NORMAL  0x07
#define ATTR_TITLE   0x0F
#define ATTR_LABEL   0x08
#define ATTR_VALUE   0x0E
#define ATTR_WARN    0x0C
#define ATTR_OK      0x0A

static uint32_t le32(const uint8_t *p) {
    return (uint32_t)p[0]
         | ((uint32_t)p[1] <<  8)
         | ((uint32_t)p[2] << 16)
         | ((uint32_t)p[3] << 24);
}

static int read_header(const char *path, uint8_t *hdr, size_t hdr_size) {
    FILE  *f = fopen(path, "rb");
    size_t got;
    if (!f) return -1;
    got = fread(hdr, 1, hdr_size, f);
    fclose(f);
    return (got >= 0x40) ? 0 : -2;
}

static void draw_chrome(void) {
    display_vga_begin();
    display_vga_puts(0,  0, ATTR_TITLE, "adlib-rng  [VGM mode]");
    display_vga_puts(24, 0, ATTR_LABEL, "ESC: quit");
}

static int player_vgm_init(const char *arg) {
    uint8_t  hdr[0x80];
    uint32_t version, ym3812_clk, ym3526_clk, ymf262_clk;

    draw_chrome();

    if (!arg) {
        display_vga_puts(3, 0, ATTR_WARN,  "error: no VGM file given");
        display_vga_puts(5, 0, ATTR_NORMAL, "usage: ADLIB <FILE>.VGM");
        return 0;
    }

    memset(hdr, 0, sizeof(hdr));
    if (read_header(arg, hdr, sizeof(hdr)) != 0) {
        display_vga_printf(3, 0, ATTR_WARN, "error: cannot open %s", arg);
        return 0;
    }

    if (memcmp(hdr, "Vgm ", 4) != 0) {
        display_vga_printf(3, 0, ATTR_WARN, "error: %s is not a VGM file", arg);
        return 0;
    }

    version    = le32(&hdr[0x08]);
    ym3812_clk = le32(&hdr[0x50]);
    ym3526_clk = le32(&hdr[0x54]);
    ymf262_clk = le32(&hdr[0x5C]);

    display_vga_printf(3, 0, ATTR_LABEL, "file:     ");
    display_vga_printf(3, 10, ATTR_VALUE, "%s", arg);
    display_vga_printf(4, 0, ATTR_LABEL, "version:  ");
    display_vga_printf(4, 10, ATTR_VALUE, "%lu.%02lu",
                       (unsigned long)((version >> 8) & 0xFF),
                       (unsigned long)(version & 0xFF));
    display_vga_printf(6, 0, ATTR_LABEL,  "YM3812 (OPL2):");
    display_vga_printf(6, 16, ATTR_VALUE, "%lu Hz", (unsigned long)ym3812_clk);
    display_vga_printf(7, 0, ATTR_LABEL,  "YM3526 (OPL1):");
    display_vga_printf(7, 16, ATTR_VALUE, "%lu Hz", (unsigned long)ym3526_clk);
    display_vga_printf(8, 0, ATTR_LABEL,  "YMF262 (OPL3):");
    display_vga_printf(8, 16, ATTR_VALUE, "%lu Hz", (unsigned long)ymf262_clk);

    if (ymf262_clk != 0) {
        display_vga_puts(11, 0, ATTR_WARN,
            "This is an OPL3 dump.");
        display_vga_puts(12, 0, ATTR_NORMAL,
            "AdLib hardware is OPL2 -- the port-1 register bank");
        display_vga_puts(13, 0, ATTR_NORMAL,
            "(channels 9-17, 4-op, stereo) has no equivalent here.");
        display_vga_puts(15, 0, ATTR_VALUE,
            "TODO stage #2: host-side transcoder to fold OPL3 -> OPL2.");
    } else if (ym3812_clk != 0 || ym3526_clk != 0) {
        display_vga_puts(11, 0, ATTR_OK,
            "OPL2-compatible dump detected.");
        display_vga_puts(12, 0, ATTR_NORMAL,
            "Playback engine is pending -- see TODO stage #2.");
    } else {
        display_vga_puts(11, 0, ATTR_WARN,
            "No OPL chip declared in this VGM.");
        display_vga_puts(12, 0, ATTR_NORMAL,
            "adlib-rng only handles OPL family chips.");
    }

    return 0;
}

static void player_vgm_tick(uint32_t now_ms) {
    /* Stub: no audio yet. */
    (void)now_ms;
}

static void player_vgm_cleanup(void) {
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
