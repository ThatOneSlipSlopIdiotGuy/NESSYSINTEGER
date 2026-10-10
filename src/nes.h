/* NESSYSINTEGER - cycle-stepped NES (NTSC 2A03 + 2C02) emulator core.
 * Everything that is emulated hardware lives in NesState (plain data, so save
 * states are a straight memcpy). ROM data, the cartridge-connector "physics"
 * and front-end settings live outside it in Nes. */
#ifndef NESSYS_NES_H
#define NESSYS_NES_H
#include <stdint.h>
#include "chips.h"
#include <stddef.h>

#define NES_W 256
#define NES_H 240
#define NES_CPU_HZ 1789773.0
#define AUDIO_RATE 44100
#define AUDIO_MAX 4096

#define IRQ_FRAME 0x01
#define IRQ_DMC   0x02

#define FC 0x01
#define FZ 0x02
#define FI 0x04
#define FD 0x08
#define FB 0x10
#define FU 0x20
#define FV 0x40
#define FN 0x80

typedef struct Nes Nes;

typedef struct {
    uint16_t pc; uint8_t a, x, y, s, p, jammed;
    uint64_t cycles; int64_t mclk;
    uint8_t irq_src;
    uint8_t nmi_line, prev_nmi_line, need_nmi, prev_need_nmi, nmi_s, run_irq, prev_run_irq;
    uint8_t need_halt, need_dummy, dmc_running, abort_dmc, spr_dma, spr_page, dma_hit;
    uint8_t bus;                 /* external data bus (cartridge-side pins) */
    uint8_t ibus;                /* 2A03-internal data bus */
} Cpu;

typedef struct {
    int16_t scanline, dot;       /* the dot executed most recently (0..261, 0..340) */
    uint64_t frame; int64_t mclk;
    uint8_t ctrl, mask, status, oamaddr, fx, w, rbuf;
    uint16_t v, t, v_pending, bus_addr;
    uint8_t v_delay, render, render_next, render_prev, vbl_suppress, ignore_writes, frame_done, held;
    uint8_t latch; uint64_t latch_age[8];
    uint8_t oam[256], sec[32], pal[32];
    uint16_t line_v[240]; uint8_t line_fx[240];   /* each scanline's scroll origin (v and fine X; bit 7 = rendering on) */
    uint32_t line_chr[240][8]; uint8_t line_src[240];   /* and the CHR banks it was drawn with */
    uint8_t nt, at, at_latch, plo, phi;
    uint16_t bglo, bghi, atlo, athi;
    uint8_t oam_buf, sec_addr, in_range, copy_done, ovf_ctr, spr0_added, spr0_vis, eval_first; /* evaluation */
    uint8_t oam2a, oam2f, oam2_rst;                         /* OAM2 address counter, overflow flag, armed reset */
    uint8_t f_y, f_tile, f_attr, f_x, f_lo;                  /* sprite fetch latches */
    uint8_t sp_lo[8], sp_hi[8], sp_at[8], sp_x[8], spr0_unit; /* the 8 sprite output units */
    uint8_t corrupt[32], corrupt_any;                       /* armed OAM row corruption */
    uint8_t rq_n; uint16_t rq_addr[4]; int64_t rq_due[4];   /* $2007 state-machine reads in flight */
    uint8_t sm_rd, ad_data, skip340;                                 /* state machine reads on this dot; PPU AD pins */
} Ppu;

typedef struct { uint8_t len, halt, new_halt, reload, prev, enabled; } LenCtr;
typedef struct { uint8_t start, loop, constant, vol, div, decay; } Env;
typedef struct { Env env; LenCtr lc; uint8_t duty, pos, ch1; uint16_t period, timer;
                 uint8_t sw_en, sw_per, sw_neg, sw_shift, sw_reload, sw_div; } Pulse;
typedef struct { LenCtr lc; uint16_t period, timer; uint8_t pos, lin, lin_val, lin_flag, control; } Tri;
typedef struct { Env env; LenCtr lc; uint16_t period, timer, lfsr; uint8_t mode; } Noise;
typedef struct { uint8_t irq_en, loop, level, buf, buf_empty, shift, bits, silence, start_delay, disable_delay;
                 uint16_t period, timer, samp_addr, samp_len, addr, remain; uint8_t act_prev; uint64_t wrap_empty_cyc; } Dmc;
typedef struct { int32_t cycle; uint8_t step, mode, inhibit, block; int16_t new_val; int8_t delay; } FrameCtr;
typedef struct {
    Pulse sq[2]; Tri tri; Noise noise; Dmc dmc; FrameCtr fc; uint8_t fc_clear;
    double acc, phase, hp1_in, hp1_out, hp2_in, hp2_out, lp_out; uint32_t acc_n;
} Apu;

typedef struct {
    uint8_t regs[16];
    uint32_t prg_off[4];         /* 8 KB windows at $8000/$A000/$C000/$E000 */
    uint32_t chr_off[8];         /* 1 KB windows */
    uint8_t nt_map[4];           /* CIRAM page (0/1) per nametable, 2/3 = cart VRAM */
    uint8_t wram_on, wram_wp; uint32_t wram_off;
    uint8_t irq_latch, irq_ctr, irq_reload, irq_en, irq_line, a12, a12_low;
    uint8_t latch0, latch1, shift, shift_n;
    uint64_t last_write;
    uint8_t chr_src;             /* bit k: 1 KB CHR window k comes from CHR-RAM on a CHR-ROM board */
    uint8_t mreg[96];            /* newer boards' registers */
    uint16_t mw[8];
    int32_t mc[8];
    uint8_t wram[65536], chr_ram[32768], vram[4096], exram[1024];
    Vrc6Snd vrc6; Vrc7Snd vrc7; S5bSnd s5b; N163Snd n163; Mmc5Snd mmc5snd; FdsSnd fdssnd; FdsDrive fds; Eeprom eep;
} CartState;

typedef struct {
    Cpu cpu; Ppu ppu; Apu apu; CartState cart;
    uint8_t ram[2048], ciram[2048];
    uint8_t strobe, out_latch, pad_shift[2];
} NesState;

/* ---------- cartridge connector ("tilt") ---------- */
#define PINS 72
enum { PIN_AUTO = 0, PIN_OFF, PIN_ON, PIN_FLAKY };
enum { FLOAT_HIGH = 0, FLOAT_LOW, FLOAT_NOISE, FLOAT_HOLD, FLOAT_REAL };   /* REAL: hold + drift + neighbour coupling */
enum {  /* signals carried by the pins */
    S_NONE, S_GND, S_VCC, S_CLK, S_M2, S_RW, S_IRQ, S_ROMSEL,
    S_CA0, S_CD0 = S_CA0 + 15, S_PA0 = S_CD0 + 8, S_PD0 = S_PA0 + 14,
    S_RD = S_PD0 + 8, S_WR, S_NA13, S_CICE, S_CIA10, S_CIC0, S_EXP0 = S_CIC0 + 4,
    S_AIN = S_EXP0 + 10, S_AOUT, S_COUNT          /* Famicom: 2A03 audio out to the cart, mixed audio back */
};
enum { SLOT_NES = 0, SLOT_NES_TOP, SLOT_FC };    /* NES-001 72-pin tray, NES-101 72-pin vertical, Famicom 60-pin */
enum { KEY_LICENSED = 0, KEY_OTHER_REGION, KEY_NONE, KEY_STUN };   /* what the cartridge offers the 10NES lock */
enum { COND_NEW = 0, COND_USED, COND_WORN, COND_AFTERMARKET, COND_REFURB };
typedef struct {
    float seat, roll, pitch, dirt;           /* pose of the cart; dirt = loose dust on the contacts */
    float tray, press, moist;                /* NES-001 tray (1 = pushed down and latched); pressing on the cart; breath moisture */
    float tension[PINS + 1];                 /* the console connector's pin springs: 1 = factory, <0.6 worn, >1.3 aftermarket */
    float cgrime[PINS + 1], fgrime[PINS + 1];/* oxidation and grime on the connector pins / on the cartridge fingers */
    uint8_t mode[PINS + 1];                  /* per-pin override */
    float tol[PINS + 1];                     /* manufacturing variation per contact */
    uint8_t conn[PINS + 1], flaky[PINS + 1]; /* computed */
    float flaky_p[PINS + 1], quality[PINS + 1];
    float sig_p[S_COUNT];                    /* per-signal chance an access gets through (1 = solid contact) */
    uint8_t slot, npins, vcc_flaky;
    uint16_t ctx_ca, ctx_pa;                 /* address of the access in progress (neighbour coupling) */
    uint8_t nb[S_COUNT][2];                  /* signals on the adjacent contacts */
    uint8_t float_mode, perfect, powered, cic_ok, nflaky, flaky_list[PINS];
    uint8_t sig_ok[S_COUNT];                 /* per-signal connection state used by the bus */
    uint16_t ca_mask, pa_mask; uint8_t cd_mask, pd_mask;
    uint16_t hold_ca, hold_pa; uint8_t hold_cd, hold_pd, hold_ctl[S_COUNT];
    uint32_t rng;
} Connector;

struct Nes {
    NesState s;
    uint8_t *prg, *chr; uint32_t prg_size, chr_size, wram_size, chr_ram_size;
    int mapper, submapper, mirror_v, four_screen, battery, chr_is_ram, has_rom;
    const struct Board *board;     /* newer mappers (board.h); NULL = built-in boards */
    int exp_audio;                 /* cartridge audio reaches the output (Famicom / EXP6-modded NES) */
    int console_setting, console;  /* CON_* : setting (CON_AUTO allowed) and the model in effect */
    uint8_t *fds_disk; uint32_t fds_disk_size; int fds_sides; uint8_t fds_bios[8192]; int fds_bios_ok;
    uint32_t rom_crc;
    Connector conn;
    int ppu_offset, cpu_align, ram_init, cic_enabled, bus_conflicts, allow_opposite;
    uint32_t seed, console_id;      /* per-power-on noise; this console's power-up fingerprint */
    struct { uint16_t addr; uint8_t val, cmp, has_cmp; } genie[16]; int ngenie;   /* Game Genie */
    struct { uint16_t addr; uint8_t val; } freeze[32]; int nfreeze;               /* RAM freeze */
    uint8_t pad[2];
    uint16_t fb[NES_W * NES_H];
    int16_t audio[AUDIO_MAX]; int audio_len;
    int cic_timer, was_powered;
    int cic_key, lock_dead, lock_frames, lock_low, stun_ok;   /* 10NES lock state (NES-001) */
    int reset_button, reset_pulse, in_reset, led, btn_prev;/* reset line: held button, momentary press; power LED */
    int brownout; uint32_t brown_p, brown_rng, brown_max;   /* power-off: unreliable reads while the rail collapses */
    uint8_t *flat_mem;                       /* CPU test mode: flat 64K RAM */
    void (*read_hook)(Nes *n, uint16_t addr, uint8_t val);   /* debugging: every CPU read (NULL = off) */
    void (*trace)(Nes *n, char kind, uint16_t addr, uint8_t val);   /* optional bus-cycle tracer */
};

/* nes.c */
Nes *nes_create(void);
void nes_destroy(Nes *n);
int nes_load_rom(Nes *n, const uint8_t *data, size_t size, int hotswap, char *err, size_t errlen);
void nes_power(Nes *n);
void nes_reset(Nes *n);
void nes_run_frame(Nes *n);
size_t nes_state_size(void);
void nes_save_state(Nes *n, uint8_t *buf);
int nes_load_state(Nes *n, const uint8_t *buf, size_t len);
void nes_palette(uint32_t out[512]);
uint8_t nes_peek(Nes *n, uint16_t a);
uint8_t nes_pad_peek(Nes *n, int port);
void nes_fill_power_on(Nes *n, uint8_t *m, size_t len, uint32_t chip, const uint8_t *ref);
void nes_poke(Nes *n, uint16_t a, uint8_t v);
uint8_t nes_ppu_peek(Nes *n, uint16_t a);
void nes_ppu_poke(Nes *n, uint16_t a, uint8_t v);
int nes_genie_decode(const char *code, uint16_t *addr, uint8_t *val, uint8_t *cmp, int *has_cmp);
const char *nes_mapper_name(int mapper);
uint8_t cpu_bus_read(Nes *n, uint16_t a);
void cpu_bus_write(Nes *n, uint16_t a, uint8_t v);
void nes_cpu_clock(Nes *n);
int nes_irq_line(Nes *n);
/* cpu.c */
void cpu_power(Nes *n, int soft);
void cpu_step(Nes *n);
void cpu_start_dmc_dma(Nes *n);
void cpu_stop_dmc_dma(Nes *n);
/* ppu.c */
void ppu_power(Nes *n, int soft);
void ppu_run_to(Nes *n, int64_t target);
uint8_t ppu_reg_read(Nes *n, uint16_t a);
uint8_t ppu_reg_peek(Nes *n, uint16_t a);
void ppu_reg_write(Nes *n, uint16_t a, uint8_t v);
/* apu.c */
void apu_power(Nes *n, int soft);
void apu_cycle(Nes *n);
uint8_t apu_status(Nes *n, int clear);
void apu_write(Nes *n, uint16_t a, uint8_t v);
void apu_dmc_fill(Nes *n, uint8_t v);
/* cart.c */
int cart_setup(Nes *n);
void cart_power(Nes *n);
uint8_t cart_cpu_read(Nes *n, uint16_t a, uint8_t *val);  /* returns mask of driven bits */
void cart_cpu_read_post(Nes *n, uint16_t a, uint8_t busval);
void cart_cpu_write(Nes *n, uint16_t a, uint8_t v);
uint8_t cart_ppu_read(Nes *n, uint16_t a);
void cart_ppu_write(Nes *n, uint16_t a, uint8_t v);
void cart_ppu_addr(Nes *n, uint16_t a);
void cart_m2(Nes *n);
int cart_irq(Nes *n);
float cart_audio(Nes *n);
enum { CON_AUTO = 0, CON_NES, CON_NES101, CON_FAMICOM };
int nes_console_resolve(const Nes *n);
const char *nes_console_name(int console);
uint8_t cart_chr_peek(Nes *n, uint16_t a);
uint8_t *cart_vram_ptr(Nes *n, uint16_t a);
/* PPU viewer (ppuview.c): one picture of the PPU's memory, read without bus side effects */
#define VIEW_W 776
#define VIEW_H 480
#define VIEW_CHR_X 520
#define VIEW_CHR_Y 0
#define VIEW_PAL_X 520
#define VIEW_PAL_Y 140
#define VIEW_OAM_X 520
#define VIEW_OAM_Y 184
void nes_view_render(Nes *n, uint32_t *rgb, const uint32_t pal_rgb[512], int chr_pal, int grid, int end_banks);
int nes_view_describe(Nes *n, int x, int y, int end_banks, char *buf, int len);
uint8_t cart_chr_peek_banks(Nes *n, uint16_t a, const uint32_t off[8], uint8_t src);
void conn_init(Connector *c);
void conn_set_condition(Connector *c, int cond, uint32_t seed);
void conn_wear_pushdown(Connector *c);
void conn_pin_geom(const Connector *c, int pin, int *row, float *x);
int conn_slot_for(int console);
float cart_audio_path(Nes *n, float s);
void nes_power_off(Nes *n);
void cpu_idle_cycle(Nes *n);
void conn_reseat(Connector *c);
void conn_update(Nes *n);
const char *conn_pin_name(const Connector *c, int pin);
int conn_pin_signal(const Connector *c, int pin);

#endif
