/* boards_konami.c - Konami VRC1, VRC2/VRC4, VRC3, VRC6 (+audio), VRC7 (+FM audio).
 * Register files live in CartState.mreg / mw / mc. */
#include <math.h>
#include <string.h>
#include "board.h"

/* ---------- shared: $6000-$7FFF work RAM / VRC2 microwire latch, PRG reads ---------- */
static int rd_common(Nes *n, uint16_t a, int m2, uint8_t *v, int ram_en) {
    if (a >= 0x8000) { *v = prg_read(n, a); return 0xFF; }
    if (m2 && a >= 0x6000) {
        if (wram_ok(n) && ram_en) { *v = *wram_at(n, a & 0x1FFF); return 0xFF; }
        if (!wram_ok(n) && a < 0x7000) { *v = n->s.cart.mreg[90] & 1; return 0x01; }   /* VRC2 latch: bit 0 only */
    }
    return 0;
}
static void wr_ram(Nes *n, uint16_t a, uint8_t v, int ram_en) {
    if (wram_ok(n) && ram_en) *wram_at(n, a & 0x1FFF) = v;
    else if (!wram_ok(n) && a < 0x7000) n->s.cart.mreg[90] = v & 1;
}

/* ---------- VRC4/VRC6/VRC7 IRQ: 8-bit up counter, scanline prescaler of 341/3 ---------- */
enum { IRQ_LATCH = 80, IRQ_CTR, IRQ_CTL };
static void virq_ctl(Nes *n, uint8_t v) {
    CS; cs->mreg[IRQ_CTL] = v & 7;
    if (v & 2) { cs->mreg[IRQ_CTR] = cs->mreg[IRQ_LATCH]; cs->mc[0] = 341; }
    irq(n, 0);
}
static void virq_ack(Nes *n) { CS; irq(n, 0); cs->mreg[IRQ_CTL] = (uint8_t)((cs->mreg[IRQ_CTL] & ~2) | ((cs->mreg[IRQ_CTL] & 1) << 1)); }
static void virq_tick(Nes *n) { CS; if (cs->mreg[IRQ_CTR] == 0xFF) { cs->mreg[IRQ_CTR] = cs->mreg[IRQ_LATCH]; irq(n, 1); } else cs->mreg[IRQ_CTR]++; }
static void virq_m2(Nes *n) {
    CS; uint8_t c = cs->mreg[IRQ_CTL];
    if (!(c & 2)) return;
    if (c & 4) virq_tick(n);                                  /* cycle mode */
    else if ((cs->mc[0] -= 3) <= 0) { cs->mc[0] += 341; virq_tick(n); }
}

/* ======================= VRC1 (mapper 75) ======================= */
static void vrc1_update(Nes *n) {
    CS; uint8_t *r = cs->mreg;
    prg8(n, 0, r[0] & 15); prg8(n, 1, r[1] & 15); prg8(n, 2, r[2] & 15); prg8(n, 3, nprg8(n) - 1);
    chr4(n, 0, (r[4] & 15) | ((r[3] & 2) << 3)); chr4(n, 4, (r[5] & 15) | ((r[3] & 4) << 2));
    mirror(n, (r[3] & 1) ? MIR_H : MIR_V);
}
static void vrc1_power(Nes *n) { vrc1_update(n); }
static int vrc1_rd(Nes *n, uint16_t a, int m2, uint8_t *v) { return rd_common(n, a, m2, v, 1); }
static void vrc1_wr(Nes *n, uint16_t a, int m2, uint8_t v) {
    CS; uint8_t *r = cs->mreg;
    if (a < 0x8000) { if (m2 && a >= 0x6000) wr_ram(n, a, v, 1); return; }
    switch (a & 0xF000) {
    case 0x8000: r[0] = v; break; case 0x9000: r[3] = v; break; case 0xA000: r[1] = v; break;
    case 0xC000: r[2] = v; break; case 0xE000: r[4] = v; break; case 0xF000: r[5] = v; break;
    default: return;
    }
    vrc1_update(n);
}
const Board B_VRC1 = { 75, vrc1_power, vrc1_rd, vrc1_wr, NULL, NULL, NULL, NULL, NULL };

/* ======================= VRC2 / VRC4 (mappers 21, 22, 23, 25) ======================= */
/* Register-select lines differ per board; NES 2.0 submappers pick one wiring,
 * otherwise both candidate wirings are decoded. */
static int vrc24_sel(Nes *n, uint16_t a) {
    int s = n->submapper, A0, A1;
    switch (n->mapper) {
    case 21: A0 = s == 1 ? (a >> 1) : s == 2 ? (a >> 6) : ((a >> 1) | (a >> 6));
             A1 = s == 1 ? (a >> 2) : s == 2 ? (a >> 7) : ((a >> 2) | (a >> 7)); break;   /* VRC4a / VRC4c */
    case 22: A0 = a >> 1; A1 = a; break;                                                     /* VRC2a */
    case 23: A0 = s == 2 ? (a >> 2) : (s == 1 || s == 3) ? a : (a | (a >> 2));
             A1 = s == 2 ? (a >> 3) : (s == 1 || s == 3) ? (a >> 1) : ((a >> 1) | (a >> 3)); break;   /* VRC4f / VRC4e / VRC2b */
    default: A0 = s == 2 ? (a >> 3) : (s == 1 || s == 3) ? (a >> 1) : ((a >> 1) | (a >> 3));
             A1 = s == 2 ? (a >> 2) : (s == 1 || s == 3) ? a : (a | (a >> 2)); break;     /* 25: VRC4b / VRC4d / VRC2c */
    }
    return (A0 & 1) | ((A1 & 1) << 1);
}
static int is_vrc2(Nes *n) { return n->mapper == 22 || ((n->mapper == 23 || n->mapper == 25) && n->submapper == 3); }
static void vrc24_update(Nes *n) {
    CS; uint8_t *r = cs->mreg; int last = nprg8(n) - 1;
    if (!is_vrc2(n) && (r[3] & 2)) { prg8(n, 0, last - 1); prg8(n, 2, r[0] & 31); }
    else { prg8(n, 0, r[0] & 31); prg8(n, 2, last - 1); }
    prg8(n, 1, r[1] & 31); prg8(n, 3, last);
    for (int i = 0; i < 8; i++) chr1(n, i, n->mapper == 22 ? cs->mw[i] >> 1 : cs->mw[i]);
    static const int M4[4] = { MIR_V, MIR_H, MIR_A, MIR_B };
    mirror(n, is_vrc2(n) ? ((r[2] & 1) ? MIR_H : MIR_V) : M4[r[2] & 3]);
}
static void vrc24_power(Nes *n) { CS; for (int i = 0; i < 8; i++) cs->mw[i] = (uint16_t)i; vrc24_update(n); }
static int vrc24_rd(Nes *n, uint16_t a, int m2, uint8_t *v) { return rd_common(n, a, m2, v, 1); }
static void vrc24_wr(Nes *n, uint16_t a, int m2, uint8_t v) {
    CS; uint8_t *r = cs->mreg;
    if (a < 0x8000) { if (m2 && a >= 0x6000) wr_ram(n, a, v, 1); return; }
    int sel = vrc24_sel(n, a), vrc2 = is_vrc2(n);
    switch (a & 0xF000) {
    case 0x8000: r[0] = v; break;
    case 0x9000: if (vrc2 || sel < 2) r[2] = v; else r[3] = v; break;
    case 0xA000: r[1] = v; break;
    case 0xB000: case 0xC000: case 0xD000: case 0xE000: {
        int k = (((a >> 12) - 0xB) << 1) | (sel >> 1);
        cs->mw[k] = (sel & 1) ? (uint16_t)((cs->mw[k] & 0x0F) | ((v & 0x1F) << 4)) : (uint16_t)((cs->mw[k] & 0x1F0) | (v & 0x0F));
    } break;
    case 0xF000:
        if (vrc2) return;
        switch (sel) {
        case 0: cs->mreg[IRQ_LATCH] = (uint8_t)((cs->mreg[IRQ_LATCH] & 0xF0) | (v & 0x0F)); break;
        case 1: cs->mreg[IRQ_LATCH] = (uint8_t)((cs->mreg[IRQ_LATCH] & 0x0F) | (v << 4)); break;
        case 2: virq_ctl(n, v); break;
        default: virq_ack(n); break;
        }
        return;
    }
    vrc24_update(n);
}
const Board B_VRC2_4_21 = { 21, vrc24_power, vrc24_rd, vrc24_wr, NULL, NULL, NULL, virq_m2, NULL };
const Board B_VRC2_4_22 = { 22, vrc24_power, vrc24_rd, vrc24_wr, NULL, NULL, NULL, NULL, NULL };
const Board B_VRC2_4_23 = { 23, vrc24_power, vrc24_rd, vrc24_wr, NULL, NULL, NULL, virq_m2, NULL };
const Board B_VRC2_4_25 = { 25, vrc24_power, vrc24_rd, vrc24_wr, NULL, NULL, NULL, virq_m2, NULL };

/* ======================= VRC3 (mapper 73) ======================= */
static void vrc3_update(Nes *n) { CS; prg16(n, 0, cs->mreg[1] & 7); prg16(n, 2, nprg8(n) / 2 - 1); chr8(n, 0); }
static void vrc3_power(Nes *n) { vrc3_update(n); }
static int vrc3_rd(Nes *n, uint16_t a, int m2, uint8_t *v) { return rd_common(n, a, m2, v, 1); }
static void vrc3_wr(Nes *n, uint16_t a, int m2, uint8_t v) {
    CS;
    if (a < 0x8000) { if (m2 && a >= 0x6000) wr_ram(n, a, v, 1); return; }
    int sh = ((a >> 12) - 8) * 4;
    switch (a & 0xF000) {
    case 0x8000: case 0x9000: case 0xA000: case 0xB000:
        cs->mw[0] = (uint16_t)((cs->mw[0] & ~(0xF << sh)) | ((v & 0xF) << sh)); break;
    case 0xC000: cs->mreg[0] = v & 7; if (v & 2) cs->mw[1] = cs->mw[0]; irq(n, 0); break;
    case 0xD000: irq(n, 0); cs->mreg[0] = (uint8_t)((cs->mreg[0] & ~2) | ((cs->mreg[0] & 1) << 1)); break;
    case 0xF000: cs->mreg[1] = v; vrc3_update(n); break;
    }
}
static void vrc3_m2(Nes *n) {
    CS; uint8_t c = cs->mreg[0];
    if (!(c & 2)) return;
    if (c & 4) {                                            /* 8-bit mode: only the low byte counts */
        uint8_t lo = (uint8_t)(cs->mw[1] + 1);
        if (!lo) { lo = (uint8_t)cs->mw[0]; irq(n, 1); }
        cs->mw[1] = (uint16_t)((cs->mw[1] & 0xFF00) | lo);
    } else if (++cs->mw[1] == 0) { cs->mw[1] = cs->mw[0]; irq(n, 1); }
}
const Board B_VRC3 = { 73, vrc3_power, vrc3_rd, vrc3_wr, NULL, NULL, NULL, vrc3_m2, NULL };

/* ======================= VRC6 (mappers 24, 26) + audio ======================= */
static int vrc6_sel(Nes *n, uint16_t a) { return n->mapper == 26 ? (((a & 1) << 1) | ((a >> 1) & 1)) : (a & 3); }
static void vrc6_update(Nes *n) {
    CS; uint8_t *r = cs->mreg, b3 = r[3];
    prg16(n, 0, r[0] & 15); prg8(n, 2, r[1] & 31); prg8(n, 3, nprg8(n) - 1);
    uint8_t *R = &r[8];
    switch (b3 & 3) {
    case 0: for (int i = 0; i < 8; i++) chr1(n, i, R[i]); break;
    case 1: for (int k = 0; k < 4; k++) {                     /* 2 KB windows; A10 from PPU A10 unless bit 5 */
                chr1(n, 2 * k, (b3 & 0x20) ? R[k] : (R[k] & ~1)); chr1(n, 2 * k + 1, (b3 & 0x20) ? R[k] : (R[k] | 1)); } break;
    default: for (int i = 0; i < 4; i++) chr1(n, i, R[i]);
             chr1(n, 4, (b3 & 0x20) ? R[4] : (R[4] & ~1)); chr1(n, 5, (b3 & 0x20) ? R[4] : (R[4] | 1));
             chr1(n, 6, (b3 & 0x20) ? R[5] : (R[5] & ~1)); chr1(n, 7, (b3 & 0x20) ? R[5] : (R[5] | 1)); break;
    }
    static const int M[4] = { MIR_V, MIR_H, MIR_A, MIR_B };
    mirror(n, M[(b3 >> 2) & 3]);
}
static void vrc6_power(Nes *n) { CS; for (int i = 0; i < 8; i++) cs->mreg[8 + i] = (uint8_t)i; vrc6_update(n); }
static int vrc6_rd(Nes *n, uint16_t a, int m2, uint8_t *v) { return rd_common(n, a, m2, v, (n->s.cart.mreg[3] & 0x80) != 0); }
static void vrc6_wr(Nes *n, uint16_t a, int m2, uint8_t v) {
    CS; uint8_t *r = cs->mreg; Vrc6Snd *s = &cs->vrc6;
    if (a < 0x8000) { if (m2 && a >= 0x6000) wr_ram(n, a, v, (r[3] & 0x80) != 0); return; }
    int sel = vrc6_sel(n, a);
    switch (a & 0xF000) {
    case 0x8000: r[0] = v; break;
    case 0x9000: case 0xA000: {
        if ((a & 0xF000) == 0x9000 && sel == 3) { s->freqctl = v & 7; return; }
        Vrc6Pulse *p = &s->p[(a >> 12) - 9];
        if (sel == 0) { p->vol = v & 15; p->duty = (v >> 4) & 7; p->mode = v >> 7; }
        else if (sel == 1) p->period = (uint16_t)((p->period & 0xF00) | v);
        else if (sel == 2) { p->period = (uint16_t)((p->period & 0xFF) | ((v & 15) << 8)); p->en = v >> 7; if (!p->en) p->step = 15; }
        return;
    }
    case 0xB000:
        if (sel == 0) s->srate = v & 0x3F;
        else if (sel == 1) s->speriod = (uint16_t)((s->speriod & 0xF00) | v);
        else if (sel == 2) { s->speriod = (uint16_t)((s->speriod & 0xFF) | ((v & 15) << 8)); s->sen = v >> 7; if (!s->sen) { s->sacc = 0; s->sstep = 0; } }
        else r[3] = v;
        break;
    case 0xC000: r[1] = v; break;
    case 0xD000: r[8 + sel] = v; break;
    case 0xE000: r[12 + sel] = v; break;
    case 0xF000:
        if (sel == 0) cs->mreg[IRQ_LATCH] = v; else if (sel == 1) virq_ctl(n, v); else if (sel == 2) virq_ack(n);
        return;
    }
    vrc6_update(n);
}
static uint16_t vrc6_reload(uint16_t period, uint8_t fc) { return (uint16_t)((fc & 4) ? (period >> 8) : (fc & 2) ? (period >> 4) : period); }
static void vrc6_m2(Nes *n) {
    virq_m2(n);
    Vrc6Snd *s = &n->s.cart.vrc6;
    if (s->freqctl & 1) return;                                /* halt */
    for (int i = 0; i < 2; i++) {
        Vrc6Pulse *p = &s->p[i];
        if (!p->en) continue;
        if (p->timer == 0) { p->timer = vrc6_reload(p->period, s->freqctl); p->step = (uint8_t)((p->step - 1) & 15); }
        else p->timer--;
    }
    if (s->sen) {
        if (s->stimer == 0) {
            s->stimer = vrc6_reload(s->speriod, s->freqctl);
            if (++s->sstep >= 14) { s->sstep = 0; s->sacc = 0; }
            else if (!(s->sstep & 1)) s->sacc = (uint8_t)(s->sacc + s->srate);
        } else s->stimer--;
    }
}
static float vrc6_audio(Nes *n) {
    Vrc6Snd *s = &n->s.cart.vrc6; int o = 0;
    for (int i = 0; i < 2; i++) { Vrc6Pulse *p = &s->p[i]; if (p->en && (p->mode || p->step <= p->duty)) o += p->vol; }
    if (s->sen) o += s->sacc >> 3;
    return (float)o * 0.00996f;                                /* a VRC6 pulse at 15 ~ an APU pulse at 15 */
}
const Board B_VRC6_24 = { 24, vrc6_power, vrc6_rd, vrc6_wr, NULL, NULL, NULL, vrc6_m2, vrc6_audio };
const Board B_VRC6_26 = { 26, vrc6_power, vrc6_rd, vrc6_wr, NULL, NULL, NULL, vrc6_m2, vrc6_audio };

/* ======================= VRC7 (mapper 85) + FM audio ======================= */
/* The VRC7's 15 built-in instruments (dumped from the chip). */
static const uint8_t VRC7_PATCH[16][8] = {
    { 0 },
    { 0x03, 0x21, 0x05, 0x06, 0xE8, 0x81, 0x42, 0x27 }, { 0x13, 0x41, 0x14, 0x0D, 0xD8, 0xF6, 0x23, 0x12 },
    { 0x11, 0x11, 0x08, 0x08, 0xFA, 0xB2, 0x20, 0x12 }, { 0x31, 0x61, 0x0C, 0x07, 0xA8, 0x64, 0x61, 0x27 },
    { 0x32, 0x21, 0x1E, 0x06, 0xE1, 0x76, 0x01, 0x28 }, { 0x02, 0x01, 0x06, 0x00, 0xA3, 0xE2, 0xF4, 0xF4 },
    { 0x21, 0x61, 0x1D, 0x07, 0x82, 0x81, 0x11, 0x07 }, { 0x23, 0x21, 0x22, 0x17, 0xA2, 0x72, 0x01, 0x17 },
    { 0x35, 0x11, 0x25, 0x00, 0x40, 0x73, 0x72, 0x01 }, { 0xB5, 0x01, 0x0F, 0x0F, 0xA8, 0xA5, 0x51, 0x02 },
    { 0x17, 0xC1, 0x24, 0x07, 0xF8, 0xF8, 0x22, 0x12 }, { 0x71, 0x23, 0x11, 0x06, 0x65, 0x74, 0x18, 0x16 },
    { 0x01, 0x02, 0xD3, 0x05, 0xC9, 0x95, 0x03, 0x02 }, { 0x61, 0x63, 0x0C, 0x00, 0x94, 0xC0, 0x33, 0xF6 },
    { 0x21, 0x72, 0x0D, 0x00, 0xC1, 0xD5, 0x56, 0x06 },
};
enum { EG_OFF, EG_ATTACK, EG_DECAY, EG_SUSTAIN, EG_RELEASE };
#define FM_RATE 49716.0f                                       /* 3.58 MHz / 72: one sample every 36 CPU cycles */
static const float FM_MULT2[16] = { 1, 2, 4, 6, 8, 10, 12, 14, 16, 18, 20, 20, 24, 24, 30, 30 };   /* 2 x multiplier */
static const float FM_KSL[16] = { 0, 18, 24, 27.75f, 30, 32.25f, 33.75f, 35.25f, 36, 37.5f, 38.25f, 39, 39.75f, 40.5f, 41.25f, 42 };

static const uint8_t *vrc7_patch(Vrc7Snd *s, int ch) { int i = s->reg[0x30 + ch] >> 4; return i ? VRC7_PATCH[i] : s->reg; }
static float eg_seconds(int rs, int attack) {                 /* full-swing time for an effective rate 0-63 */
    if (rs >= 60 && attack) return 0.0f;
    float base = attack ? 2.826f : 39.28f;
    return base * powf(2.0f, -(float)(rs - 4) / 4.0f);
}
static void vrc7_key(Vrc7Snd *s, int ch, int on) {
    for (int o = 0; o < 2; o++) {
        if (on) { s->eg[ch][o] = EG_ATTACK; s->phase[ch][o] = 0; if (s->env[ch][o] > 48.0f) s->env[ch][o] = 48.0f; }
        else if (s->eg[ch][o] != EG_OFF) s->eg[ch][o] = EG_RELEASE;
    }
}
static void vrc7_write(Vrc7Snd *s, uint8_t reg, uint8_t v) {
    reg &= 0x3F;
    if (reg >= 0x20 && reg <= 0x25) {
        int was = s->reg[reg] & 0x10, now = v & 0x10;
        s->reg[reg] = v;
        if (now && !was) vrc7_key(s, reg - 0x20, 1); else if (!now && was) vrc7_key(s, reg - 0x20, 0);
        return;
    }
    s->reg[reg] = v;
}
static void vrc7_sample(Vrc7Snd *s) {
    s->lfo++;
    float am = 4.875f * 0.5f * (1.0f - cosf(2.0f * 3.14159265f * 3.7f * (float)s->lfo / FM_RATE));     /* tremolo, dB */
    float pm = 1.0f + 0.004f * sinf(2.0f * 3.14159265f * 6.4f * (float)s->lfo / FM_RATE);             /* vibrato */
    float mix = 0.0f;
    for (int ch = 0; ch < 6; ch++) {
        const uint8_t *P = vrc7_patch(s, ch);
        int fnum = s->reg[0x10 + ch] | ((s->reg[0x20 + ch] & 1) << 8), block = (s->reg[0x20 + ch] >> 1) & 7;
        int sus = (s->reg[0x20 + ch] >> 5) & 1, vol = s->reg[0x30 + ch] & 15;
        int kcode = (block << 1) | (fnum >> 8);
        float kslbase = FM_KSL[fnum >> 5] - 6.0f * (float)(7 - block); if (kslbase < 0) kslbase = 0;
        float out = 0.0f, modout = 0.0f;
        for (int o = 0; o < 2; o++) {
            uint8_t c0 = P[o], ar = P[4 + o] >> 4, dr = P[4 + o] & 15, sl = P[6 + o] >> 4, rr = P[6 + o] & 15;
            int egtype = (c0 >> 5) & 1, ksr = (c0 >> 4) & 1, rks = ksr ? kcode : (kcode >> 2);
            /* phase */
            float inc = (float)(fnum << block) * FM_MULT2[c0 & 15] * ((c0 & 0x40) ? pm : 1.0f);
            s->phase[ch][o] += (uint32_t)inc;
            /* envelope (attenuation in dB) */
            float *e = &s->env[ch][o];
            int rate = 0;
            switch (s->eg[ch][o]) {
            case EG_ATTACK:  rate = ar ? ar * 4 + rks : 0; break;
            case EG_DECAY:   rate = dr ? dr * 4 + rks : 0; break;
            case EG_SUSTAIN: rate = egtype ? 0 : (rr ? rr * 4 + rks : 0); break;
            case EG_RELEASE: rate = o == 1 ? (sus ? 5 * 4 + rks : (rr ? rr * 4 + rks : 0)) : (egtype ? 0 : (rr ? rr * 4 + rks : 0)); break;
            default: break;
            }
            if (rate > 63) rate = 63;
            if (s->eg[ch][o] == EG_ATTACK) {
                if (rate) {
                    float t = eg_seconds(rate, 1);
                    if (t <= 0.0f) *e = 0.0f;
                    else { float lin = powf(10.0f, -*e / 20.0f); lin += (1.02f - lin) * (3.0f / (t * FM_RATE)); *e = lin >= 1.0f ? 0.0f : -20.0f * log10f(lin); }
                    if (*e <= 0.05f) { *e = 0.0f; s->eg[ch][o] = EG_DECAY; }
                }
            } else if (s->eg[ch][o] != EG_OFF && rate) {
                *e += 48.0f / (eg_seconds(rate, 0) * FM_RATE);
                if (s->eg[ch][o] == EG_DECAY && *e >= 3.0f * sl) { *e = 3.0f * sl; s->eg[ch][o] = EG_SUSTAIN; }
                if (*e >= 48.0f) { *e = 48.0f; s->eg[ch][o] = EG_OFF; }
            }
            if (s->eg[ch][o] == EG_OFF) { if (o == 0) modout = 0; continue; }
            /* level */
            int kslv = (o ? P[3] : P[2]) >> 6;
            float att = *e + kslbase * (kslv == 0 ? 0.0f : kslv == 1 ? 0.5f : kslv == 2 ? 1.0f : 2.0f)
                      + (o ? 3.0f * vol : 0.75f * (P[2] & 0x3F)) + ((c0 & 0x80) ? am : 0.0f);
            float amp = att >= 48.0f ? 0.0f : powf(10.0f, -att / 20.0f);
            /* operator */
            float ph = (float)(s->phase[ch][o] & 0xFFFFF) / 1048576.0f;
            if (o == 0) { static const float FBK[8] = { 0, 1.0f / 32, 1.0f / 16, 1.0f / 8, 1.0f / 4, 1.0f / 2, 1, 2 };
                          ph += (s->fb[ch][0] + s->fb[ch][1]) * 0.5f * FBK[P[3] & 7]; }
            else ph += modout * 2.0f;                          /* modulator swings the carrier by up to +/- 4 pi */
            float w = sinf(2.0f * 3.14159265f * ph);
            if ((P[3] >> (o ? 4 : 3)) & 1) { if (w < 0) w = 0; }   /* rectified (half) sine */
            if (o == 0) { modout = w * amp; s->fb[ch][1] = s->fb[ch][0]; s->fb[ch][0] = modout; }
            else out = w * amp;
        }
        mix += out;
    }
    s->out = mix;
}
static void vrc7_update(Nes *n) {
    CS; uint8_t *r = cs->mreg;
    prg8(n, 0, r[0] & 63); prg8(n, 1, r[1] & 63); prg8(n, 2, r[2] & 63); prg8(n, 3, nprg8(n) - 1);
    for (int i = 0; i < 8; i++) chr1(n, i, r[8 + i]);
    static const int M[4] = { MIR_V, MIR_H, MIR_A, MIR_B };
    mirror(n, M[r[3] & 3]);
}
static void vrc7_power(Nes *n) { vrc7_update(n); }
static int vrc7_rd(Nes *n, uint16_t a, int m2, uint8_t *v) { return rd_common(n, a, m2, v, (n->s.cart.mreg[3] & 0x80) != 0); }   /* $E000 bit 7: WRAM enable */
static void vrc7_wr(Nes *n, uint16_t a, int m2, uint8_t v) {
    CS; uint8_t *r = cs->mreg;
    if (a < 0x8000) { if (m2 && a >= 0x6000) wr_ram(n, a, v, (r[3] & 0x80) != 0); return; }
    int s = n->submapper, hi = (a & (s == 1 ? 0x08 : s == 2 ? 0x10 : 0x18)) ? 1 : 0;   /* VRC7b: A3, VRC7a: A4 */
    if ((a & 0xF030) == 0x9010) { if (!cs->vrc7.muted) cs->vrc7.sel = v; return; }   /* audio register select (ignored while silenced) */
    if ((a & 0xF030) == 0x9030) { if (!cs->vrc7.muted) vrc7_write(&cs->vrc7, cs->vrc7.sel, v); return; }
    switch (a & 0xF000) {
    case 0x8000: r[hi] = v; break;
    case 0x9000: if (!hi) r[2] = v; break;
    case 0xA000: case 0xB000: case 0xC000: case 0xD000: r[8 + (((a >> 12) - 0xA) << 1) + hi] = v; break;
    case 0xE000:
        if (!hi) {                                       /* RS.. ..MM: R = WRAM enable, S = silence + reset the sound chip */
            r[3] = v; cs->vrc7.muted = (v >> 6) & 1;
            if (v & 0x40) { memset(cs->vrc7.reg, 0, sizeof cs->vrc7.reg); cs->vrc7.lfo = 0; cs->vrc7.out = 0;
                            for (int c = 0; c < 6; c++) cs->vrc7.eg[c][0] = cs->vrc7.eg[c][1] = EG_OFF; }
        }
        else cs->mreg[IRQ_LATCH] = v;
        break;
    case 0xF000: if (!hi) virq_ctl(n, v); else virq_ack(n); return;
    }
    vrc7_update(n);
}
static void vrc7_m2(Nes *n) {
    virq_m2(n);
    Vrc7Snd *s = &n->s.cart.vrc7;
    if (++s->div >= 36) { s->div = 0; if (!s->muted) vrc7_sample(s); else s->out = 0; }
}
static float vrc7_audio(Nes *n) { return n->s.cart.vrc7.out * 0.11f; }
const Board B_VRC7 = { 85, vrc7_power, vrc7_rd, vrc7_wr, NULL, NULL, NULL, vrc7_m2, vrc7_audio };
