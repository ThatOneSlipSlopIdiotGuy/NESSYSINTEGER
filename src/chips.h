/* chips.h - state of mapper chips and cartridge sound hardware (part of CartState,
 * so it is captured by save states) */
#ifndef CHIPS_H
#define CHIPS_H
#include <stdint.h>

typedef struct { uint16_t period, timer; uint8_t step, duty, vol, mode, en; } Vrc6Pulse;
typedef struct {                       /* Konami VRC6: 2 pulse + sawtooth */
    Vrc6Pulse p[2];
    uint16_t speriod, stimer; uint8_t srate, sacc, sstep, sen, freqctl;
} Vrc6Snd;

typedef struct {                       /* Konami VRC7: OPLL-derived 6-channel FM */
    uint8_t reg[64], sel, div, muted;
    uint32_t phase[6][2];              /* 20-bit phase accumulators (modulator, carrier) */
    float env[6][2];                   /* attenuation in dB */
    uint8_t eg[6][2];                  /* envelope state */
    float fb[6][2];                    /* modulator output history (feedback) */
    uint32_t lfo;                      /* AM/PM LFO counter */
    float out;
} Vrc7Snd;

typedef struct {                       /* Sunsoft 5B: AY-3-8910-style PSG */
    uint8_t reg[16], sel;
    uint16_t tcnt[3]; uint8_t tout[3];
    uint16_t ncnt; uint32_t lfsr; uint8_t nout;
    uint32_t ecnt; uint8_t estep, ehold, edir, div;
    float out;
} S5bSnd;

typedef struct { uint8_t addr, ch, tick; float out; } N163Snd;   /* wave RAM lives in CartState.exram */

typedef struct { uint8_t duty, vol, cvol, loop, en, len, estart, ediv, edecay, step; uint16_t period, timer; } Mmc5Pulse;
typedef struct { Mmc5Pulse p[2]; uint8_t pcm, pcm_mode, pcm_irq, pcm_irq_en; uint16_t frame; } Mmc5Snd;

typedef struct {                       /* FDS RAM adapter sound */
    uint8_t wave[64], mod[32], regs[16];
    uint32_t wpos, mpos;               /* wave/mod accumulators */
    uint8_t vgain, mgain, venv_ctr, menv_ctr, mtab_pos;
    int8_t mcounter;
    uint16_t env_clk;
    float out, lp;
} FdsSnd;

typedef struct {                       /* FDS RAM adapter + drive */
    uint16_t timer_reload, timer; uint8_t timer_ctl, timer_irq, disk_irq, io_en, ctl, ext;
    uint8_t rdata, status, wdata, xfer, crc_ctl, gap_end, motor, side, inserted;
    uint32_t pos, delay;               /* position in the raw side stream; cycles until next byte */
    uint8_t scan, dirty;
} FdsDrive;

typedef struct { uint8_t state, bit, byte, addr, rw, sda_out, scl, sda, dev, size; } Eeprom;   /* 24C01/24C02 */
#endif
