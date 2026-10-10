/* boards_misc.c - Sunsoft-3/4/FME-7/5B, Namco 163/175/340, Bandai FCG + EEPROM,
 * Jaleco SS8806, Irem G-101/H-3001/78, Taito TC0190/X1-005/X1-017, CPROM,
 * NTDEC 2722, Tengen RAMBO-1, CNROM protection. */
#include <math.h>
#include <string.h>
#include "board.h"

static const int MIR4_VHAB[4] = { MIR_V, MIR_H, MIR_A, MIR_B };
static int rd_prg(Nes *n, uint16_t a, uint8_t *v) { if (a >= 0x8000) { *v = prg_read(n, a); return 0xFF; } return 0; }
static int rd_wram(Nes *n, uint16_t a, int m2, uint8_t *v, int en) {
    if (a >= 0x8000) { *v = prg_read(n, a); return 0xFF; }
    if (m2 && a >= 0x6000 && wram_ok(n) && en) { *v = *wram_at(n, a & 0x1FFF); return 0xFF; }
    return 0;
}

/* ======================= mapper 13: CPROM ======================= */
static void m13_power(Nes *n) { if (n->chr_ram_size < 0x4000) n->chr_ram_size = 0x4000; prg32(n, 0); chr4(n, 0, 0); chr4(n, 4, 0); }
static void m13_wr(Nes *n, uint16_t a, int m2, uint8_t v) { if (a >= 0x8000) { v &= prg_read(n, a); chr4(n, 4, v & 3); } }
static int m13_rd(Nes *n, uint16_t a, int m2, uint8_t *v) { return rd_prg(n, a, v); }
const Board B_13 = { 13, m13_power, m13_rd, m13_wr, NULL, NULL, NULL, NULL, NULL };

/* ======================= Bandai FCG / LZ93D50 (16, 159) + serial EEPROM ======================= */
enum { E_IDLE, E_RECV, E_ACKOUT, E_SEND, E_ACKIN };
/* I2C serial EEPROM. 24C01: 128 bytes, the word address is in the control byte.
 * 24C02: 256 bytes, device byte then word address; repeated START for random reads.
 * The contents live in the battery-save area (wram), so they persist like PRG-RAM. */
static void eep_io(Nes *n, int size, int scl, int sda) {
    CS; Eeprom *e = &cs->eep; uint8_t *mem = cs->wram;
    if (e->scl && scl && e->sda != sda) {                       /* START / STOP while SCL is high */
        if (!sda) { e->state = E_RECV; e->dev = 1; e->bit = 0; e->byte = 0; }
        else e->state = E_IDLE;
        e->sda_out = 1;
    } else if (!e->scl && scl) {                                /* rising edge: sample */
        switch (e->state) {
        case E_RECV:
            e->byte = (uint8_t)((e->byte << 1) | (sda & 1));
            if (++e->bit == 8) {
                if (e->dev == 1) { e->rw = e->byte & 1; if (size == 128) e->addr = (uint8_t)((e->byte >> 1) & 0x7F); }
                else if (e->dev == 2) e->addr = e->byte;
                else { mem[e->addr % size] = e->byte; e->addr = (uint8_t)((e->addr + 1) % size); }
                e->state = E_ACKOUT;
            }
            break;
        case E_ACKOUT:
            e->bit = 0; e->byte = 0;
            if (e->dev == 1 && e->rw) { e->dev = 0; e->state = E_SEND; e->byte = mem[e->addr % size]; }
            else { e->dev = (uint8_t)((e->dev == 1 && size == 256) ? 2 : 0); e->state = E_RECV; }
            break;
        case E_SEND: if (++e->bit == 8) e->state = E_ACKIN; break;
        case E_ACKIN:
            if (!sda) { e->addr = (uint8_t)((e->addr + 1) % size); e->byte = mem[e->addr % size]; e->bit = 0; e->state = E_SEND; }
            else e->state = E_IDLE;
            break;
        }
    } else if (e->scl && !scl) {                                /* falling edge: drive SDA */
        e->sda_out = e->state == E_ACKOUT ? 0 : e->state == E_SEND ? (uint8_t)((e->byte >> (7 - e->bit)) & 1) : 1;
    }
    e->scl = (uint8_t)scl; e->sda = (uint8_t)sda;
}
static void fcg_update(Nes *n) {
    CS; uint8_t *r = cs->mreg;
    for (int i = 0; i < 8; i++) chr1(n, i, r[i]);
    prg16(n, 0, r[8] & 15); prg16(n, 2, nprg8(n) / 2 - 1);
    mirror(n, MIR4_VHAB[r[9] & 3]);
}
static int fcg_lz(Nes *n) { return n->mapper == 159 || n->submapper == 5; }       /* LZ93D50: IRQ latch, regs at $8000 */
static int fcg_eep_size(Nes *n) { return n->s.cart.mreg[95] * 128; }
static void fcg_power(Nes *n) {
    CS;
    int eep = n->mapper == 159 ? 128 : (n->submapper == 4 ? 0 : 256);   /* 24C01 / none (FCG-1/2) / 24C02 */
    cs->mreg[95] = (uint8_t)(eep / 128);
    n->wram_size = (uint32_t)eep;
    memset(&cs->eep, 0, sizeof cs->eep); cs->eep.sda_out = 1; cs->eep.sda = 1;
    fcg_update(n);
}
static int fcg_rd(Nes *n, uint16_t a, int m2, uint8_t *v) {
    CS;
    if (a >= 0x8000) { *v = prg_read(n, a); return 0xFF; }
    if (m2 && a >= 0x6000 && fcg_eep_size(n)) { *v = (uint8_t)(((cs->eep.sda_out & cs->eep.sda) & 1) << 4); return 0x10; }
    return 0;
}
static void fcg_wr(Nes *n, uint16_t a, int m2, uint8_t v) {
    CS; uint8_t *r = cs->mreg;
    int at6 = m2 && a >= 0x6000 && a < 0x8000, at8 = a >= 0x8000;
    if (!((at6 && !fcg_lz(n)) || (at8 && (fcg_lz(n) || n->submapper == 0)))) return;
    int reg = a & 15;
    switch (reg) {
    case 10: r[10] = v & 1; irq(n, 0); if (fcg_lz(n)) cs->mw[1] = cs->mw[0]; break;
    case 11: if (fcg_lz(n)) cs->mw[0] = (uint16_t)((cs->mw[0] & 0xFF00) | v); else cs->mw[1] = (uint16_t)((cs->mw[1] & 0xFF00) | v); break;
    case 12: if (fcg_lz(n)) cs->mw[0] = (uint16_t)((cs->mw[0] & 0xFF) | v << 8); else cs->mw[1] = (uint16_t)((cs->mw[1] & 0xFF) | v << 8); break;
    case 13: if (fcg_eep_size(n)) eep_io(n, fcg_eep_size(n), (v >> 5) & 1, (v >> 6) & 1); break;
    case 14: case 15: break;
    default: r[reg] = v; fcg_update(n); break;
    }
}
static void fcg_m2(Nes *n) {
    CS;
    if (!(cs->mreg[10] & 1)) return;
    if (cs->mw[1] == 0) irq(n, 1);
    cs->mw[1]--;
}
const Board B_16 = { 16, fcg_power, fcg_rd, fcg_wr, NULL, NULL, NULL, fcg_m2, NULL };
const Board B_159 = { 159, fcg_power, fcg_rd, fcg_wr, NULL, NULL, NULL, fcg_m2, NULL };

/* ======================= Jaleco SS8806 (18) ======================= */
static void ss_update(Nes *n) {
    CS; uint8_t *r = cs->mreg;
    prg8(n, 0, r[0]); prg8(n, 1, r[1]); prg8(n, 2, r[2]); prg8(n, 3, nprg8(n) - 1);
    for (int i = 0; i < 8; i++) chr1(n, i, r[8 + i]);
    static const int M[4] = { MIR_H, MIR_V, MIR_A, MIR_B };
    mirror(n, M[r[4] & 3]);
}
static void ss_power(Nes *n) { ss_update(n); }
static int ss_rd(Nes *n, uint16_t a, int m2, uint8_t *v) { return rd_wram(n, a, m2, v, n->s.cart.mreg[3] & 1); }
static void ss_wr(Nes *n, uint16_t a, int m2, uint8_t v) {
    CS; uint8_t *r = cs->mreg;
    if (a < 0x8000) { if (m2 && a >= 0x6000 && wram_ok(n) && (r[3] & 1) && !(r[3] & 2)) *wram_at(n, a & 0x1FFF) = v; return; }
    int sel = a & 3, lo = !(sel & 1);
    uint8_t *t = NULL;
    switch (a & 0xF000) {
    case 0x8000: t = &r[sel >> 1]; break;
    case 0x9000: if (sel < 2) t = &r[2]; else if (sel == 2) { r[3] = v & 3; return; } else return; break;
    case 0xA000: case 0xB000: case 0xC000: case 0xD000: t = &r[8 + (((a >> 12) - 0xA) << 1) + (sel >> 1)]; break;
    case 0xE000: cs->mw[0] = (uint16_t)((cs->mw[0] & ~(0xF << (sel * 4))) | ((v & 15) << (sel * 4))); return;
    case 0xF000:
        if (sel == 0) { cs->mw[1] = cs->mw[0]; irq(n, 0); }
        else if (sel == 1) { r[5] = v & 15; irq(n, 0); }
        else if (sel == 2) { r[4] = v & 3; ss_update(n); }
        return;
    }
    if (!t) return;
    *t = lo ? (uint8_t)((*t & 0xF0) | (v & 15)) : (uint8_t)((*t & 0x0F) | ((v & 15) << 4));
    ss_update(n);
}
static void ss_m2(Nes *n) {
    CS; uint8_t c = cs->mreg[5];
    if (!(c & 1)) return;
    uint16_t mask = (c & 8) ? 0x000F : (c & 4) ? 0x00FF : (c & 2) ? 0x0FFF : 0xFFFF;
    if ((cs->mw[1] & mask) == 0) irq(n, 1);
    cs->mw[1] = (uint16_t)((cs->mw[1] & ~mask) | ((cs->mw[1] - 1) & mask));
}
const Board B_18 = { 18, ss_power, ss_rd, ss_wr, NULL, NULL, NULL, ss_m2, NULL };

/* ======================= Namco 163 (19) + wavetable audio, Namco 175/340 (210) ======================= */
static void n163_update(Nes *n) {
    CS; uint8_t *r = cs->mreg;
    prg8(n, 0, r[16] & 63); prg8(n, 1, r[17] & 63); prg8(n, 2, r[18] & 63); prg8(n, 3, nprg8(n) - 1);
    for (int i = 0; i < 8; i++) chr1(n, i, r[i]);
    if (n->mapper == 210 && n->submapper == 2) { static const int M[4] = { MIR_A, MIR_V, MIR_B, MIR_H }; mirror(n, M[(r[16] >> 6) & 3]); }
}
static void n163_power(Nes *n) { CS; for (int i = 0; i < 4; i++) cs->mreg[8 + i] = (uint8_t)(0xE0 | (i & 1)); n163_update(n); }
static int n163_rd(Nes *n, uint16_t a, int m2, uint8_t *v) {
    CS;
    if (a >= 0x8000) { *v = prg_read(n, a); return 0xFF; }
    if (n->mapper == 19 && m2) {
        if ((a & 0xF800) == 0x4800) { *v = cs->exram[cs->n163.addr & 0x7F]; if (cs->n163.addr & 0x80) cs->n163.addr = (uint8_t)(0x80 | ((cs->n163.addr + 1) & 0x7F)); return 0xFF; }
        if ((a & 0xF800) == 0x5000) { *v = (uint8_t)cs->mw[0]; return 0xFF; }
        if ((a & 0xF800) == 0x5800) { *v = (uint8_t)(cs->mw[0] >> 8); return 0xFF; }
    }
    if (m2 && a >= 0x6000 && wram_ok(n) && (n->mapper == 19 || (cs->mreg[8] & 1) || n->submapper != 1)) { *v = *wram_at(n, a & 0x1FFF); return 0xFF; }
    return 0;
}
static void n163_wr(Nes *n, uint16_t a, int m2, uint8_t v) {
    CS; uint8_t *r = cs->mreg;
    if (a < 0x8000) {
        if (!m2) return;
        if (n->mapper == 19) {
            if ((a & 0xF800) == 0x4800) { cs->exram[cs->n163.addr & 0x7F] = v; if (cs->n163.addr & 0x80) cs->n163.addr = (uint8_t)(0x80 | ((cs->n163.addr + 1) & 0x7F)); return; }
            if ((a & 0xF800) == 0x5000) { cs->mw[0] = (uint16_t)((cs->mw[0] & 0xFF00) | v); irq(n, 0); return; }
            if ((a & 0xF800) == 0x5800) { cs->mw[0] = (uint16_t)((cs->mw[0] & 0x00FF) | v << 8); irq(n, 0); return; }
        }
        if (a >= 0x6000 && wram_ok(n)) *wram_at(n, a & 0x1FFF) = v;
        return;
    }
    int k = (a >> 11) & 15;                                      /* $8000-$FFFF in $800 steps */
    if (k < 8) r[k] = v;                                        /* CHR banks */
    else if (k < 12) r[k] = v;                                  /* nametable banks ($C000-$D800) */
    else if (k == 12) r[16] = v;                                /* $E000: PRG 0 + sound disable */
    else if (k == 13) r[17] = v;                                /* $E800: PRG 1 + CIRAM-as-CHR disables */
    else if (k == 14) r[18] = v;                                /* $F000: PRG 2 */
    else cs->n163.addr = v;                                     /* $F800: sound RAM address */
    n163_update(n);
}
static int n163_ppu(Nes *n, uint16_t a, uint8_t *v) {
    CS; uint8_t *r = cs->mreg;
    if (n->mapper != 19) return PPU_DEFAULT;
    if (a < 0x2000) {
        uint8_t b = r[(a >> 10) & 7];
        int ciram_off = (a & 0x1000) ? (r[17] & 0x80) : (r[17] & 0x40);
        if (b >= 0xE0 && !ciram_off) return (b & 1) ? PPU_CIRAM1 : PPU_CIRAM0;
        return PPU_DEFAULT;
    }
    if (a >= 0x3F00) return PPU_DEFAULT;
    uint8_t b = r[8 + ((a >> 10) & 3)];
    if (b >= 0xE0) return (b & 1) ? PPU_CIRAM1 : PPU_CIRAM0;
    *v = chr_abs(n, (uint32_t)b * 0x400 + (a & 0x3FF));
    return PPU_DRIVE;
}
static int n163_ppu_w(Nes *n, uint16_t a, uint8_t v) {
    CS; uint8_t *r = cs->mreg;
    if (n->mapper != 19) return PPU_DEFAULT;
    if (a < 0x2000) {
        uint8_t b = r[(a >> 10) & 7];
        int ciram_off = (a & 0x1000) ? (r[17] & 0x80) : (r[17] & 0x40);
        if (b >= 0xE0 && !ciram_off) return (b & 1) ? PPU_CIRAM1 : PPU_CIRAM0;
        return PPU_OPEN;                                         /* CHR-ROM */
    }
    if (a >= 0x3F00) return PPU_DEFAULT;
    uint8_t b = r[8 + ((a >> 10) & 3)];
    if (b >= 0xE0) return (b & 1) ? PPU_CIRAM1 : PPU_CIRAM0;
    (void)v; return PPU_OPEN;
}
static void n163_m2(Nes *n) {
    CS;
    if ((cs->mw[0] & 0x8000) && (cs->mw[0] & 0x7FFF) != 0x7FFF) { cs->mw[0]++; if ((cs->mw[0] & 0x7FFF) == 0x7FFF) irq(n, 1); }
    N163Snd *s = &cs->n163;
    if (++s->tick < 15) return;
    s->tick = 0;
    if (cs->mreg[16] & 0x40) { s->out = 0; return; }            /* sound disabled */
    uint8_t *ram = cs->exram;
    int num = ((ram[0x7F] >> 4) & 7) + 1;
    if (s->ch >= num) s->ch = 0;
    int base = 0x78 - s->ch * 8;                                /* channel 8 first, then 7, ... */
    uint32_t freq = ram[base] | (uint32_t)ram[base + 2] << 8 | (uint32_t)(ram[base + 4] & 3) << 16;
    uint32_t phase = ram[base + 1] | (uint32_t)ram[base + 3] << 8 | (uint32_t)ram[base + 5] << 16;
    uint32_t len = 256u - (ram[base + 4] & 0xFC);
    phase = (phase + freq) % (len << 16);
    ram[base + 1] = (uint8_t)phase; ram[base + 3] = (uint8_t)(phase >> 8); ram[base + 5] = (uint8_t)(phase >> 16);
    uint32_t idx = ((phase >> 16) + ram[base + 6]) & 0xFF;
    int smp = (ram[idx >> 1] >> ((idx & 1) * 4)) & 15;
    s->out = (float)((smp - 8) * (ram[base + 7] & 15));        /* the DAC shows one channel at a time */
    s->ch = (uint8_t)((s->ch + 1) % num);
}
static float n163_audio(Nes *n) { return n->s.cart.n163.out * 0.0011f; }
const Board B_19 = { 19, n163_power, n163_rd, n163_wr, n163_ppu, n163_ppu_w, NULL, n163_m2, n163_audio };
const Board B_210 = { 210, n163_power, n163_rd, n163_wr, NULL, NULL, NULL, NULL, NULL };

/* ======================= Irem G-101 (32), H-3001 (65), 74HC161 (78) ======================= */
static void g101_update(Nes *n) {
    CS; uint8_t *r = cs->mreg; int last = nprg8(n) - 1;
    int swap = (r[2] & 2) && n->submapper != 1;
    prg8(n, swap ? 2 : 0, r[0] & 31); prg8(n, swap ? 0 : 2, last - 1); prg8(n, 1, r[1] & 31); prg8(n, 3, last);
    for (int i = 0; i < 8; i++) chr1(n, i, r[8 + i]);
    mirror(n, n->submapper == 1 ? MIR_A : (r[2] & 1) ? MIR_H : MIR_V);
}
static void g101_power(Nes *n) { g101_update(n); }
static int g101_rd(Nes *n, uint16_t a, int m2, uint8_t *v) { return rd_wram(n, a, m2, v, 1); }
static void g101_wr(Nes *n, uint16_t a, int m2, uint8_t v) {
    CS; uint8_t *r = cs->mreg;
    if (a < 0x8000) { if (m2 && a >= 0x6000 && wram_ok(n)) *wram_at(n, a & 0x1FFF) = v; return; }
    switch (a & 0xF000) {
    case 0x8000: r[0] = v; break; case 0x9000: r[2] = v; break; case 0xA000: r[1] = v; break;
    case 0xB000: r[8 + (a & 7)] = v; break; default: return;
    }
    g101_update(n);
}
const Board B_32 = { 32, g101_power, g101_rd, g101_wr, NULL, NULL, NULL, NULL, NULL };

static void h3001_update(Nes *n) {
    CS; uint8_t *r = cs->mreg;
    prg8(n, 0, r[0]); prg8(n, 1, r[1]); prg8(n, 2, r[2]); prg8(n, 3, nprg8(n) - 1);
    for (int i = 0; i < 8; i++) chr1(n, i, r[8 + i]);
    mirror(n, (r[3] & 0x80) ? MIR_H : MIR_V);
}
static void h3001_power(Nes *n) { CS; cs->mreg[0] = 0; cs->mreg[1] = 1; cs->mreg[2] = 0xFE; h3001_update(n); }
static void h3001_wr(Nes *n, uint16_t a, int m2, uint8_t v) {
    CS; uint8_t *r = cs->mreg;
    if (a < 0x8000) { if (m2 && a >= 0x6000 && wram_ok(n)) *wram_at(n, a & 0x1FFF) = v; return; }
    switch (a & 0xF007) {
    case 0x8000: case 0x8001: case 0x8002: case 0x8003: case 0x8004: case 0x8005: case 0x8006: case 0x8007: r[0] = v; break;
    case 0x9001: r[3] = v; break;
    case 0x9003: r[4] = v & 0x80; irq(n, 0); return;
    case 0x9004: cs->mw[1] = cs->mw[0]; irq(n, 0); return;
    case 0x9005: cs->mw[0] = (uint16_t)((cs->mw[0] & 0xFF) | v << 8); return;
    case 0x9006: cs->mw[0] = (uint16_t)((cs->mw[0] & 0xFF00) | v); return;
    default:
        if ((a & 0xF000) == 0xA000) r[1] = v;
        else if ((a & 0xF000) == 0xB000) r[8 + (a & 7)] = v;
        else if ((a & 0xF000) == 0xC000) r[2] = v;
        else return;
    }
    h3001_update(n);
}
static void h3001_m2(Nes *n) { CS; if (cs->mreg[4] && cs->mw[1]) { if (--cs->mw[1] == 0) irq(n, 1); } }
const Board B_65 = { 65, h3001_power, g101_rd, h3001_wr, NULL, NULL, NULL, h3001_m2, NULL };

static int m78_hv(Nes *n) { return n->submapper == 3 || (n->submapper == 0 && n->s.cart.mreg[90]); }
static void m78_update(Nes *n) {
    CS; uint8_t v = cs->mreg[0];
    prg16(n, 0, v & 7); prg16(n, 2, nprg8(n) / 2 - 1); chr8(n, v >> 4);
    mirror(n, m78_hv(n) ? ((v & 8) ? MIR_V : MIR_H) : ((v & 8) ? MIR_B : MIR_A));
}
static void m78_power(Nes *n) { CS; cs->mreg[90] = (uint8_t)n->four_screen; n->four_screen = 0; m78_update(n); }   /* iNES: 4-screen bit marks Holy Diver */
static void m78_wr(Nes *n, uint16_t a, int m2, uint8_t v) { CS; if (a >= 0x8000) { cs->mreg[0] = v & prg_read(n, a); m78_update(n); } }
const Board B_78 = { 78, m78_power, m13_rd, m78_wr, NULL, NULL, NULL, NULL, NULL };

/* ======================= Taito TC0190 (33), X1-005 (80), X1-017 (82) ======================= */
static void tc_update(Nes *n) {
    CS; uint8_t *r = cs->mreg;
    prg8(n, 0, r[0] & 63); prg8(n, 1, r[1] & 63); prg8(n, 2, nprg8(n) - 2); prg8(n, 3, nprg8(n) - 1);
    chr2(n, 0, r[2]); chr2(n, 2, r[3]); chr1(n, 4, r[4]); chr1(n, 5, r[5]); chr1(n, 6, r[6]); chr1(n, 7, r[7]);
    mirror(n, (r[0] & 0x40) ? MIR_H : MIR_V);
}
static void tc_power(Nes *n) { tc_update(n); }
static void tc_wr(Nes *n, uint16_t a, int m2, uint8_t v) {
    CS;
    if (a < 0x8000) return;
    switch (a & 0xA003) {
    case 0x8000: cs->mreg[0] = v; break; case 0x8001: cs->mreg[1] = v; break;
    case 0x8002: cs->mreg[2] = v; break; case 0x8003: cs->mreg[3] = v; break;
    default: if ((a & 0xA000) == 0xA000) cs->mreg[4 + (a & 3)] = v; else return;
    }
    tc_update(n);
}
const Board B_33 = { 33, tc_power, m13_rd, tc_wr, NULL, NULL, NULL, NULL, NULL };

static void x1_update(Nes *n) {
    CS; uint8_t *r = cs->mreg; int x17 = n->mapper == 82, sh = x17 ? 2 : 0;
    prg8(n, 0, r[10] >> sh); prg8(n, 1, r[12] >> sh); prg8(n, 2, r[14] >> sh); prg8(n, 3, nprg8(n) - 1);
    int inv = (x17 && (r[6] & 2)) ? 4 : 0;
    chr2(n, 0 ^ inv, r[0] >> 1); chr2(n, 2 ^ inv, r[1] >> 1);
    for (int i = 0; i < 4; i++) chr1(n, (4 + i) ^ inv, r[2 + i]);
    mirror(n, (r[6] & 1) ? MIR_V : MIR_H);
}
static void x1_power(Nes *n) { if (n->mapper == 82 && n->wram_size < 0x1400) n->wram_size = 0x1400; x1_update(n); }
static int x1_ram_ok(Nes *n, uint16_t a) {
    CS; uint8_t *r = cs->mreg;
    if (n->mapper == 80) return r[8] == 0xA3;
    if (a < 0x6800) return r[7] == 0xCA;
    if (a < 0x7000) return r[8] == 0x69;
    if (a < 0x7400) return r[9] == 0x84;
    return 0;
}
static int x1_rd(Nes *n, uint16_t a, int m2, uint8_t *v) {
    CS;
    if (a >= 0x8000) { *v = prg_read(n, a); return 0xFF; }
    if (!m2) return 0;
    if (n->mapper == 80 && a >= 0x7F00) { if (!x1_ram_ok(n, a)) return 0; *v = cs->exram[a & 0x7F]; return 0xFF; }
    if (n->mapper == 82 && a >= 0x6000 && a < 0x7400 && x1_ram_ok(n, a)) { *v = cs->wram[a - 0x6000]; return 0xFF; }
    return 0;
}
static void x1_wr(Nes *n, uint16_t a, int m2, uint8_t v) {
    CS;
    if (a >= 0x8000 || !m2) return;
    if (a >= 0x7EF0 && a <= 0x7EFF) {
        int reg = a - 0x7EF0;
        if (n->mapper == 80) {
            if (reg <= 5) cs->mreg[reg] = v;
            else if (reg <= 7) cs->mreg[6] = v;
            else if (reg <= 9) cs->mreg[8] = v;
            else cs->mreg[10 + ((reg - 10) & ~1)] = v;
        } else {
            if (reg <= 6) cs->mreg[reg] = v;
            else if (reg <= 9) cs->mreg[reg] = v;
            else if (reg <= 12) cs->mreg[10 + (reg - 10) * 2] = v;
            else return;
        }
        x1_update(n); return;
    }
    if (n->mapper == 80 && a >= 0x7F00) { if (x1_ram_ok(n, a)) cs->exram[a & 0x7F] = v; return; }
    if (n->mapper == 82 && a >= 0x6000 && a < 0x7400 && x1_ram_ok(n, a)) cs->wram[a - 0x6000] = v;
}
const Board B_80 = { 80, x1_power, x1_rd, x1_wr, NULL, NULL, NULL, NULL, NULL };
const Board B_82 = { 82, x1_power, x1_rd, x1_wr, NULL, NULL, NULL, NULL, NULL };

/* ======================= NTDEC 2722 SMB2J conversion (40) ======================= */
static void m40_power(Nes *n) { prg8(n, 0, 4); prg8(n, 1, 5); prg8(n, 2, 0); prg8(n, 3, 7); chr8(n, 0); }
static int m40_rd(Nes *n, uint16_t a, int m2, uint8_t *v) {
    if (a >= 0x8000) { *v = prg_read(n, a); return 0xFF; }
    if (m2 && a >= 0x6000) { *v = prg_abs(n, 6u * 0x2000 + (a & 0x1FFF)); return 0xFF; }
    return 0;
}
static void m40_wr(Nes *n, uint16_t a, int m2, uint8_t v) {
    CS;
    switch (a & 0xE000) {
    case 0x8000: cs->mreg[0] = 0; cs->mw[0] = 0; irq(n, 0); break;
    case 0xA000: cs->mreg[0] = 1; break;
    case 0xE000: prg8(n, 2, v & 7); break;
    }
}
/* CD4020 used as a 13-bit counter, held in reset while disabled; /IRQ follows bit 12, so the
 * IRQ appears 4096 M2 cycles after enabling and acknowledges itself 4096 cycles later */
static void m40_m2(Nes *n) { CS; if (cs->mreg[0]) { cs->mw[0] = (uint16_t)((cs->mw[0] + 1) & 0x1FFF); irq(n, (cs->mw[0] & 0x1000) != 0); } }
const Board B_40 = { 40, m40_power, m40_rd, m40_wr, NULL, NULL, NULL, m40_m2, NULL };

/* ======================= Kaiser KS7032 / KS202 FDS conversions (142) ======================= */
/* The KS202 is an upgraded VRC3: four 8 KB PRG banks (one of them at $6000) set through a
 * select/data register pair, the last bank fixed, 8 KB CHR-RAM, and VRC3's 16-bit M2 up-counter
 * that reloads and raises /IRQ when it passes $FFFF. Unlike a VRC3, acknowledging does not copy
 * bit 0 into the enable - Kaiser's SMB2J freezes if it does. (nesdev wiki: INES Mapper 142) */
static void m142_sync(Nes *n) { CS; prg8(n, 0, cs->mreg[1]); prg8(n, 1, cs->mreg[2]); prg8(n, 2, cs->mreg[3]); prg8(n, 3, nprg8(n) - 1); chr8(n, 0); }
static void m142_power(Nes *n) { CS; memset(cs->mreg, 0, 8); cs->mw[0] = cs->mw[1] = 0; m142_sync(n); }
static int m142_rd(Nes *n, uint16_t a, int m2, uint8_t *v) {
    CS;
    if (a >= 0x8000) { *v = prg_read(n, a); return 0xFF; }
    if (m2 && a >= 0x6000) { *v = prg_abs(n, (uint32_t)cs->mreg[4] * 0x2000u + (a & 0x1FFF)); return 0xFF; }
    return 0;
}
static void m142_wr(Nes *n, uint16_t a, int m2, uint8_t v) {
    CS;
    if (a < 0x8000) return;
    switch (a & 0xF000) {
    case 0x8000: case 0x9000: case 0xA000: case 0xB000: {           /* reload value, four nibbles */
        int sh = ((a >> 12) - 8) * 4;
        cs->mw[1] = (uint16_t)((cs->mw[1] & ~(0xF << sh)) | ((v & 0xF) << sh)); break;
    }
    case 0xC000: cs->mreg[5] = (v & 7) != 0; if (cs->mreg[5]) cs->mw[0] = cs->mw[1]; irq(n, 0); break;
    case 0xD000: irq(n, 0); break;
    case 0xE000: cs->mreg[0] = (uint8_t)(v & 7); break;
    case 0xF000: if (cs->mreg[0] >= 1 && cs->mreg[0] <= 4) { cs->mreg[cs->mreg[0]] = v; m142_sync(n); } break;
    }
}
static void m142_m2(Nes *n) {
    CS;
    if (!cs->mreg[5]) return;
    if (cs->mw[0] == 0xFFFF) { cs->mw[0] = cs->mw[1]; irq(n, 1); }
    else cs->mw[0]++;
}
const Board B_142 = { 142, m142_power, m142_rd, m142_wr, NULL, NULL, NULL, m142_m2, NULL };

/* ======================= Tengen RAMBO-1 (64) ======================= */
static void rambo_update(Nes *n) {
    CS; uint8_t *r = cs->mreg, c = r[16];
    int inv = (c & 0x80) ? 4 : 0, last = nprg8(n) - 1;
    if (c & 0x20) { chr1(n, 0 ^ inv, r[0]); chr1(n, 1 ^ inv, r[8]); chr1(n, 2 ^ inv, r[1]); chr1(n, 3 ^ inv, r[9]); }
    else { chr1(n, 0 ^ inv, r[0] & 0xFE); chr1(n, 1 ^ inv, r[0] | 1); chr1(n, 2 ^ inv, r[1] & 0xFE); chr1(n, 3 ^ inv, r[1] | 1); }
    for (int i = 0; i < 4; i++) chr1(n, (4 + i) ^ inv, r[2 + i]);
    if (c & 0x40) { prg8(n, 0, r[15]); prg8(n, 1, r[6]); prg8(n, 2, r[7]); }
    else { prg8(n, 0, r[6]); prg8(n, 1, r[7]); prg8(n, 2, r[15]); }
    prg8(n, 3, last);
    mirror(n, (r[17] & 1) ? MIR_H : MIR_V);
}
static void rambo_power(Nes *n) { rambo_update(n); }
static void rambo_clock(Nes *n) {
    CS;
    if (cs->mreg[20]) { cs->mreg[19] = (uint8_t)(cs->mreg[18] + (cs->mreg[18] ? 1 : 0)); cs->mreg[20] = 0; }
    else if (cs->mreg[19] == 0) cs->mreg[19] = cs->mreg[18];
    else cs->mreg[19]--;
    if (cs->mreg[19] == 0 && cs->mreg[21]) cs->mreg[22] = 2;    /* asserted after a short delay */
}
static void rambo_wr(Nes *n, uint16_t a, int m2, uint8_t v) {
    CS; uint8_t *r = cs->mreg;
    if (a < 0x8000) { if (m2 && a >= 0x6000 && wram_ok(n)) *wram_at(n, a & 0x1FFF) = v; return; }
    switch (a & 0xE001) {
    case 0x8000: r[16] = v; break;
    case 0x8001: { int k = r[16] & 15; if (k < 8) r[k] = v; else if (k < 10) r[k] = v; else if (k == 15) r[15] = v; } break;
    case 0xA000: r[17] = v; break;
    case 0xC000: r[18] = v; return;
    case 0xC001: r[23] = v & 1; r[20] = 1; cs->mc[0] = 0; return;
    case 0xE000: r[21] = 0; r[22] = 0; irq(n, 0); return;
    case 0xE001: r[21] = 1; return;
    default: return;
    }
    rambo_update(n);
}
static void rambo_ppu_addr(Nes *n, uint16_t a) {
    CS; int a12 = (a >> 12) & 1;
    if (!cs->mreg[23] && a12 && !cs->a12 && cs->a12_low >= 3) rambo_clock(n);
    if (!a12 && cs->a12) cs->a12_low = 0;
    cs->a12 = (uint8_t)a12;
}
static void rambo_m2(Nes *n) {
    CS;
    if (cs->mreg[23] && ++cs->mc[0] >= 4) { cs->mc[0] = 0; rambo_clock(n); }
    if (cs->mreg[22] && --cs->mreg[22] == 0 && cs->mreg[21]) irq(n, 1);
}
static int rambo_rd(Nes *n, uint16_t a, int m2, uint8_t *v) { return rd_wram(n, a, m2, v, 1); }
const Board B_64 = { 64, rambo_power, rambo_rd, rambo_wr, NULL, NULL, rambo_ppu_addr, rambo_m2, NULL };

/* ======================= Sunsoft-3 (67), Sunsoft-4 (68) ======================= */
static void s3_update(Nes *n) {
    CS; uint8_t *r = cs->mreg;
    chr2(n, 0, r[0]); chr2(n, 2, r[1]); chr2(n, 4, r[2]); chr2(n, 6, r[3]);
    prg16(n, 0, r[5] & 15); prg16(n, 2, nprg8(n) / 2 - 1);
    mirror(n, MIR4_VHAB[r[4] & 3]);
}
static void s3_power(Nes *n) { s3_update(n); }
static void s3_wr(Nes *n, uint16_t a, int m2, uint8_t v) {
    CS; uint8_t *r = cs->mreg;
    if (a < 0x8000) { if (m2 && a >= 0x6000 && wram_ok(n)) *wram_at(n, a & 0x1FFF) = v; return; }
    switch (a & 0xF800) {
    case 0x8800: r[0] = v; break; case 0x9800: r[1] = v; break; case 0xA800: r[2] = v; break; case 0xB800: r[3] = v; break;
    case 0xC800: if (!r[6]) cs->mw[0] = (uint16_t)((cs->mw[0] & 0x00FF) | v << 8); else cs->mw[0] = (uint16_t)((cs->mw[0] & 0xFF00) | v); r[6] ^= 1; return;
    case 0xD800: r[7] = (v >> 4) & 1; r[6] = 0; irq(n, 0); return;
    case 0xE800: r[4] = v; break;
    case 0xF800: r[5] = v; break;
    default: return;
    }
    s3_update(n);
}
static void s3_m2(Nes *n) { CS; if (cs->mreg[7]) { if (cs->mw[0] == 0) { cs->mreg[7] = 0; irq(n, 1); } cs->mw[0]--; } }
const Board B_67 = { 67, s3_power, g101_rd, s3_wr, NULL, NULL, NULL, s3_m2, NULL };

static void s4_update(Nes *n) {
    CS; uint8_t *r = cs->mreg;
    chr2(n, 0, r[0]); chr2(n, 2, r[1]); chr2(n, 4, r[2]); chr2(n, 6, r[3]);
    prg16(n, 0, r[7] & 15); prg16(n, 2, nprg8(n) / 2 - 1);
    mirror(n, MIR4_VHAB[r[6] & 3]);
}
static void s4_power(Nes *n) { s4_update(n); }
static int s4_rd(Nes *n, uint16_t a, int m2, uint8_t *v) { return rd_wram(n, a, m2, v, (n->s.cart.mreg[7] & 0x10) != 0); }
static void s4_wr(Nes *n, uint16_t a, int m2, uint8_t v) {
    CS; uint8_t *r = cs->mreg;
    if (a < 0x8000) { if (m2 && a >= 0x6000 && wram_ok(n) && (r[7] & 0x10)) *wram_at(n, a & 0x1FFF) = v; return; }
    r[(a >> 12) - 8] = v;                                       /* $8000-$B000 CHR, $C000/$D000 NT, $E000 mode, $F000 PRG */
    s4_update(n);
}
static int s4_ppu(Nes *n, uint16_t a, uint8_t *v) {             /* nametables from CHR-ROM */
    CS; uint8_t *r = cs->mreg;
    if (a < 0x2000 || a >= 0x3F00 || !(r[6] & 0x10)) return PPU_DEFAULT;
    uint8_t page = cs->nt_map[(a >> 10) & 3] & 1;
    *v = chr_abs(n, (uint32_t)((r[4 + page] | 0x80) & 0xFF) * 0x400 + (a & 0x3FF));
    return PPU_DRIVE;
}
static int s4_ppu_w(Nes *n, uint16_t a, uint8_t v) { (void)v; return (a >= 0x2000 && a < 0x3F00 && (n->s.cart.mreg[6] & 0x10)) ? PPU_OPEN : PPU_DEFAULT; }
const Board B_68 = { 68, s4_power, s4_rd, s4_wr, s4_ppu, s4_ppu_w, NULL, NULL, NULL };

/* ======================= Sunsoft FME-7 / 5A / 5B (69) + 5B PSG ======================= */
static void fme7_update(Nes *n) {
    CS; uint8_t *r = cs->mreg;
    for (int i = 0; i < 8; i++) chr1(n, i, r[i]);
    prg8(n, 0, r[9] & 63); prg8(n, 1, r[10] & 63); prg8(n, 2, r[11] & 63); prg8(n, 3, nprg8(n) - 1);
    mirror(n, MIR4_VHAB[r[12] & 3]);
}
static void fme7_power(Nes *n) { fme7_update(n); }
static int fme7_rd(Nes *n, uint16_t a, int m2, uint8_t *v) {
    CS; uint8_t b = cs->mreg[8];
    if (a >= 0x8000) { *v = prg_read(n, a); return 0xFF; }
    if (!m2 || a < 0x6000) return 0;
    if (!(b & 0x40)) { *v = prg_abs(n, (uint32_t)(b & 63) * 0x2000 + (a & 0x1FFF)); return 0xFF; }   /* ROM at $6000 */
    if ((b & 0x80) && wram_ok(n)) { *v = *wram_at(n, (uint32_t)(b & 63) * 0x2000 + (a & 0x1FFF)); return 0xFF; }
    return 0;
}
static void s5b_write(S5bSnd *s, uint8_t reg, uint8_t v) {
    reg &= 15; s->reg[reg] = v;
    if (reg == 13) { s->estep = 0; s->ehold = 0; s->edir = (v & 4) ? 1 : 0; s->ecnt = 0; }
}
static void fme7_wr(Nes *n, uint16_t a, int m2, uint8_t v) {
    CS; uint8_t *r = cs->mreg; uint8_t b = r[8];
    if (a < 0x8000) {
        if (m2 && a >= 0x6000 && (b & 0xC0) == 0xC0 && wram_ok(n)) *wram_at(n, (uint32_t)(b & 63) * 0x2000 + (a & 0x1FFF)) = v;
        return;
    }
    switch (a & 0xE000) {
    case 0x8000: r[16] = v & 15; return;
    case 0xA000: {
        int c = r[16];
        if (c <= 12) { r[c] = v; fme7_update(n); }
        else if (c == 13) { r[13] = v; irq(n, 0); }
        else if (c == 14) cs->mw[0] = (uint16_t)((cs->mw[0] & 0xFF00) | v);
        else cs->mw[0] = (uint16_t)((cs->mw[0] & 0x00FF) | v << 8);
    } return;
    case 0xC000: cs->s5b.sel = v & 15; return;
    case 0xE000: s5b_write(&cs->s5b, cs->s5b.sel, v); return;
    }
}
static float s5b_level(int v5) { return v5 ? powf(10.0f, (float)(v5 - 31) * 1.5f / 20.0f) : 0.0f; }   /* 1.5 dB per step */
static void fme7_m2(Nes *n) {
    CS; uint8_t c = cs->mreg[13];
    if (c & 0x80) { if (cs->mw[0] == 0 && (c & 1)) irq(n, 1); cs->mw[0]--; }
    S5bSnd *s = &cs->s5b;
    if (++s->div < 16) return;                                  /* the PSG runs at CPU/16 */
    s->div = 0;
    for (int ch = 0; ch < 3; ch++) {
        uint16_t per = (uint16_t)(s->reg[ch * 2] | ((s->reg[ch * 2 + 1] & 15) << 8));
        if (++s->tcnt[ch] >= (per ? per : 1)) { s->tcnt[ch] = 0; s->tout[ch] ^= 1; }
    }
    uint8_t np = s->reg[6] & 31;
    if (++s->ncnt >= 2u * (np ? np : 1)) {
        s->ncnt = 0;
        if (!s->lfsr) s->lfsr = 1;
        s->lfsr = (s->lfsr >> 1) | ((((s->lfsr) ^ (s->lfsr >> 3)) & 1) << 16);
        s->nout = s->lfsr & 1;
    }
    uint16_t ep = (uint16_t)(s->reg[11] | s->reg[12] << 8);
    if (++s->ecnt >= (ep ? ep : 1)) {
        s->ecnt = 0;
        if (!s->ehold && ++s->estep >= 32) {
            uint8_t sh = s->reg[13];
            if (!(sh & 8)) { s->ehold = 1; s->edir = 0; s->estep = 31; }                    /* one-shot: drop to 0 */
            else if (sh & 1) { s->ehold = 1; if (sh & 2) s->edir ^= 1; s->estep = 31; }       /* hold */
            else { if (sh & 2) s->edir ^= 1; s->estep = 0; }                                   /* repeat / alternate */
        }
    }
    int env = s->edir ? s->estep : 31 - s->estep;
    if (s->ehold && !(s->reg[13] & 8)) env = 0;
    float out = 0.0f;
    for (int ch = 0; ch < 3; ch++) {
        int toff = (s->reg[7] >> ch) & 1, noff = (s->reg[7] >> (ch + 3)) & 1;
        if (!((s->tout[ch] | toff) & (s->nout | noff))) continue;
        uint8_t vr = s->reg[8 + ch];
        out += (vr & 0x10) ? s5b_level(env) : s5b_level((vr & 15) ? (vr & 15) * 2 + 1 : 0);
    }
    s->out = out;
}
static float fme7_audio(Nes *n) { return n->s.cart.s5b.out * 0.12f; }
const Board B_69 = { 69, fme7_power, fme7_rd, fme7_wr, NULL, NULL, NULL, fme7_m2, fme7_audio };

/* ======================= CNROM with copy protection (185) ======================= */
static int m185_on(Nes *n, uint8_t v) {
    switch (n->submapper) {
    case 4: return (v & 3) == 0; case 5: return (v & 3) == 1; case 6: return (v & 3) == 2; case 7: return (v & 3) == 3;
    default: return (v & 0x0F) != 0 && v != 0x13;               /* no submapper: the common heuristic */
    }
}
static void m185_power(Nes *n) { CS; prg32(n, 0); chr8(n, 0); cs->mreg[0] = 1; }
static void m185_wr(Nes *n, uint16_t a, int m2, uint8_t v) { CS; if (a >= 0x8000) { v &= prg_read(n, a); cs->mreg[0] = (uint8_t)m185_on(n, v); } }
static int m185_ppu(Nes *n, uint16_t a, uint8_t *v) { (void)v; return (a < 0x2000 && !n->s.cart.mreg[0]) ? PPU_OPEN : PPU_DEFAULT; }   /* CHR disabled: bus floats */
const Board B_185 = { 185, m185_power, m13_rd, m185_wr, m185_ppu, NULL, NULL, NULL, NULL };
