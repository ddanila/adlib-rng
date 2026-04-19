#ifndef OPL2_H
#define OPL2_H

#include <stdint.h>

#define OPL_CHANNELS    9
#define OPL_PORT_REG    0x388
#define OPL_PORT_DATA   0x389

/* Rhythm-mode bits in register 0xBD */
#define OPL_RHY_ENABLE  0x20
#define OPL_RHY_BD      0x10
#define OPL_RHY_SD      0x08
#define OPL_RHY_TT      0x04
#define OPL_RHY_TC      0x02
#define OPL_RHY_HH      0x01

typedef struct {
    uint8_t am_vib_eg_ksr_mult;  /* register 0x20+off */
    uint8_t ksl_tl;              /* register 0x40+off */
    uint8_t ar_dr;               /* register 0x60+off */
    uint8_t sl_rr;               /* register 0x80+off */
    uint8_t waveform;            /* register 0xE0+off */
} opl_op_t;

typedef struct {
    opl_op_t mod;                /* operator 1 (modulator) */
    opl_op_t car;                /* operator 2 (carrier)   */
    uint8_t  feedback_conn;      /* register 0xC0+ch       */
} opl_instr_t;

void opl_init(void);
void opl_reset(void);
void opl_write(uint16_t reg, uint8_t val);

void opl_set_instrument(int ch, const opl_instr_t *ins);
void opl_note_on(int ch, int midi_note);
void opl_note_off(int ch);

void opl_rhythm_mode(int enable);
void opl_rhythm_trigger(uint8_t bits);
void opl_rhythm_stop(void);

/* Set fixed pitches for the rhythm-mode percussion voices.
 * Must be called after enabling rhythm mode. */
void opl_rhythm_setup_pitches(void);

extern const opl_instr_t OPL_INSTR_LEAD;
extern const opl_instr_t OPL_INSTR_LEAD_BRIGHT;
extern const opl_instr_t OPL_INSTR_BASS;
extern const opl_instr_t OPL_INSTR_RHY_BD;
extern const opl_instr_t OPL_INSTR_RHY_SD_HH;
extern const opl_instr_t OPL_INSTR_RHY_TT_TC;

#endif
