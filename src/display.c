#include "display.h"
#include "seeds.h"
#include <stdio.h>
#include <string.h>
#include <stdarg.h>
#include <i86.h>

/* Direct VGA text-mode video buffer at 0xB800:0000.
 * Each cell is two bytes: ASCII then attribute (bg<<4 | fg). */
#define ATTR_NORMAL  0x07   /* light gray on black                 */
#define ATTR_TITLE   0x0F   /* bright white                         */
#define ATTR_LABEL   0x08   /* dark gray                            */
#define ATTR_VALUE   0x0E   /* yellow                               */
#define ATTR_GRID    0x07
#define ATTR_KICK    0x0C   /* light red                            */
#define ATTR_SNARE   0x0D   /* magenta                              */
#define ATTR_HAT     0x0B   /* light cyan                           */
#define ATTR_MEL     0x0A   /* light green                          */
#define ATTR_BASS    0x09   /* light blue                           */
#define ATTR_BAR     0x0F
#define ATTR_BARLINE 0x08
#define ATTR_CURSOR  0x70   /* black on light gray (inverse)        */
#define ATTR_CURBAR  0x1F   /* white on blue                        */

static unsigned char __far *vga = (unsigned char __far *)0;

static void hide_cursor(void) {
    union REGS r;
    r.h.ah = 0x01;
    r.h.ch = 0x20;   /* cursor scan start with bit 5 set = invisible */
    r.h.cl = 0x00;
    int86(0x10, &r, &r);
}

static void show_cursor(void) {
    union REGS r;
    r.h.ah = 0x01;
    r.h.ch = 0x06;
    r.h.cl = 0x07;
    int86(0x10, &r, &r);
}

void display_vga_clear(unsigned char attr) {
    int i;
    for (i = 0; i < 80 * 25; i++) {
        vga[i * 2]     = ' ';
        vga[i * 2 + 1] = attr;
    }
}

void display_vga_putc(int row, int col, unsigned char attr, char ch) {
    int off;
    if (row < 0 || row >= 25 || col < 0 || col >= 80) return;
    off = (row * 80 + col) * 2;
    vga[off]     = (unsigned char)ch;
    vga[off + 1] = attr;
}

void display_vga_puts(int row, int col, unsigned char attr, const char *s) {
    while (*s && col < 80) {
        display_vga_putc(row, col, attr, *s);
        col++;
        s++;
    }
}

void display_vga_printf(int row, int col, unsigned char attr,
                        const char *fmt, ...) {
    char buf[128];
    va_list ap;
    va_start(ap, fmt);
    vsprintf(buf, fmt, ap);
    va_end(ap);
    display_vga_puts(row, col, attr, buf);
}

void display_vga_begin(void) {
    vga = (unsigned char __far *)MK_FP(0xB800, 0x0000);
    hide_cursor();
    display_vga_clear(ATTR_NORMAL);
}

void display_vga_end(void) {
    union REGS r;
    show_cursor();
    /* Move cursor to row 1 so the DOS prompt comes back below the
     * banner most players print on exit. */
    r.h.ah = 0x02;
    r.h.bh = 0x00;
    r.h.dh = 1;
    r.h.dl = 0;
    int86(0x10, &r, &r);
}

/* Back-compat shims for the RNG-specific layout (it uses the same
 * short names everywhere; keeping them avoids a noisy churn). */
#define vga_clear   display_vga_clear
#define vga_putc    display_vga_putc
#define vga_puts    display_vga_puts
#define vga_printf  display_vga_printf

static const char *NOTE_NAMES[12] = {
    "C ", "C#", "D ", "D#", "E ", "F ", "F#", "G ", "G#", "A ", "A#", "B "
};

static void note_str(int midi, char *out) {
    if (midi < 0) {
        strcpy(out, "-- ");
        return;
    }
    sprintf(out, "%s%d", NOTE_NAMES[midi % 12], (midi / 12) - 1);
}

void display_init(uint32_t seed) {
    int i;
    display_vga_begin();

    vga_puts(0, 0, ATTR_TITLE, "adlib-rng");
    vga_printf(0, 24, ATTR_LABEL, "seed=");
    vga_printf(0, 29, ATTR_VALUE, "0x%08lX", (unsigned long)seed);

    vga_puts(2, 0, ATTR_LABEL, "Bar:");
    vga_puts(2, 16, ATTR_LABEL, "Step:");
    vga_puts(2, 32, ATTR_LABEL, "Chord:");
    vga_puts(4, 0, ATTR_LABEL, "Mel:");
    vga_puts(4, 14, ATTR_LABEL, "Bass:");
    vga_puts(4, 28, ATTR_LABEL, "Drum:");

    vga_puts(6, 0, ATTR_LABEL, "Current bar (64 steps):");
    vga_putc(7, 0, ATTR_LABEL, '[');
    vga_putc(7, 65, ATTR_LABEL, ']');
    /* Step ruler row 8: tick marks every 8 steps */
    for (i = 0; i < 64; i++) {
        char c = ' ';
        if ((i & 15) == 0) c = '|';
        else if ((i & 7) == 0) c = '.';
        vga_putc(8, 1 + i, ATTR_BARLINE, c);
    }

    vga_puts(10, 0, ATTR_LABEL, "Bars:");
    for (i = 0; i < BARS; i++) {
        vga_putc(10, 6 + i * 4, ATTR_LABEL, '[');
        vga_printf(10, 7 + i * 4, ATTR_NORMAL, "%02d", i + 1);
        vga_putc(10, 9 + i * 4, ATTR_LABEL, ']');
    }

    {
        int cur = music_get_variation();
        vga_puts(12, 0, ATTR_LABEL, "Variations:");
        for (i = 0; i < NUM_VARIATIONS; i++) {
            int col = 13 + i * 17;
            unsigned char a = (i == cur) ? ATTR_CURBAR : ATTR_NORMAL;
            unsigned char ab = (i == cur) ? ATTR_CURBAR : ATTR_LABEL;
            vga_printf(12, col,     ab, "[%d]", i + 1);
            vga_printf(12, col + 4, a,  "%-12s", music_variation_name(i));
        }
        vga_puts(14, 0, ATTR_LABEL, "Now:");
        vga_printf(14, 5, ATTR_VALUE, "%-73s", music_variation_desc(cur));
    }

    vga_puts(16, 0, ATTR_LABEL, "Always:");
    vga_puts(17, 2, ATTR_NORMAL, "A major pentatonic only (no 4th/7th); melody key-anchored to A");
    vga_puts(18, 2, ATTR_NORMAL, "120 BPM, 64 substeps/bar, 12 bars then loop forever");
    vga_puts(19, 2, ATTR_NORMAL, "Drums via OPL2 rhythm mode. Pattern varies per variation.");

    /* Seed bank — letter-bound quick picks. Highlight whichever one
     * matches the currently-active seed (if any). */
    {
        int col = 7;
        int j;
        vga_puts(21, 0, ATTR_LABEL, "Seeds:");
        for (j = 0; j < SEED_COUNT; j++) {
            unsigned char a  = (SEED_BANK[j] == seed) ? ATTR_CURBAR : ATTR_NORMAL;
            unsigned char ab = (SEED_BANK[j] == seed) ? ATTR_CURBAR : ATTR_LABEL;
            vga_printf(21, col,     ab, "[%c]", 'a' + j);
            vga_printf(21, col + 3, a,  "%s ", SEED_LABEL[j]);
            col += 3 + (int)strlen(SEED_LABEL[j]) + 2;
        }
    }

    vga_puts(24, 0, ATTR_LABEL, "ESC: quit    1-4: variation    a-f: seed");
}

static void draw_step_grid(int cur_step, const bar_t *bar) {
    int i;
    for (i = 0; i < STEPS_PER_BAR; i++) {
        char ch;
        unsigned char attr;
        uint8_t d = (uint8_t)bar->drums[i];
        int has_mel   = bar->melody[i] >= 0;
        int has_lead2 = bar->lead2[i]  >= 0;
        int has_bass  = bar->bass[i]   >= 0;

        if      (d & DRUM_KICK)  { ch = 'K'; attr = ATTR_KICK;  }
        else if (d & DRUM_SNARE) { ch = 'S'; attr = ATTR_SNARE; }
        else if (d & DRUM_HAT)   { ch = 'h'; attr = ATTR_HAT;   }
        else if (has_mel)        { ch = 'M'; attr = ATTR_MEL;   }
        else if (has_lead2)      { ch = 'm'; attr = ATTR_MEL;   }
        else if (has_bass)       { ch = 'b'; attr = ATTR_BASS;  }
        else if ((i & 15) == 0)  { ch = ':'; attr = ATTR_BARLINE; }
        else                     { ch = '.'; attr = ATTR_BARLINE; }

        if (i == cur_step) attr = ATTR_CURSOR;
        vga_putc(7, 1 + i, attr, ch);
    }
}

static void draw_bar_index(int cur_bar) {
    int i;
    for (i = 0; i < BARS; i++) {
        unsigned char a = (i == cur_bar) ? ATTR_CURBAR : ATTR_NORMAL;
        unsigned char ab = (i == cur_bar) ? ATTR_CURBAR : ATTR_LABEL;
        char buf[3];
        sprintf(buf, "%02d", i + 1);
        vga_putc(10, 6 + i * 4, ab, '[');
        vga_putc(10, 7 + i * 4, a, buf[0]);
        vga_putc(10, 8 + i * 4, a, buf[1]);
        vga_putc(10, 9 + i * 4, ab, ']');
    }
}

void display_frame(int cur_bar, int cur_step, const bar_t *bar) {
    char m[8], b[8], c[8];
    uint8_t d = bar->drums[cur_step];
    int chord_idx = cur_step / STEPS_PER_CHORD;

    note_str(bar->melody[cur_step],            m);
    note_str(bar->bass[cur_step],              b);
    note_str(bar->chord_root_midi[chord_idx],  c);

    vga_printf(2,  5, ATTR_VALUE, "%2d/12", cur_bar + 1);
    vga_printf(2, 22, ATTR_VALUE, "%2d/64", cur_step);
    vga_printf(2, 39, ATTR_VALUE, "%-3s",   c);

    vga_printf(4,  5, ATTR_MEL,  "%-3s",  m);
    vga_printf(4, 20, ATTR_BASS, "%-3s",  b);
    vga_putc(4, 34, (d & DRUM_KICK)  ? ATTR_KICK  : ATTR_BARLINE,
                    (d & DRUM_KICK)  ? 'K' : '.');
    vga_putc(4, 35, (d & DRUM_SNARE) ? ATTR_SNARE : ATTR_BARLINE,
                    (d & DRUM_SNARE) ? 'S' : '.');
    vga_putc(4, 36, (d & DRUM_HAT)   ? ATTR_HAT   : ATTR_BARLINE,
                    (d & DRUM_HAT)   ? 'H' : '.');

    draw_step_grid(cur_step, bar);
    draw_bar_index(cur_bar);
}

void display_cleanup(void) {
    vga_clear(ATTR_NORMAL);
    vga_puts(0, 0, ATTR_NORMAL, "adlib-rng: bye.");
    display_vga_end();
}
