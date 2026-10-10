/* boards_mmc5.c - Nintendo MMC5 (ExROM, mapper 5).
 * MMC5 watches the PPU bus: three consecutive reads of one nametable address mark a
 * new scanline, and counting the 170 reads that follow tells background fetches from
 * sprite fetches (for the 8x16 "A"/"B" CHR sets), drives extended attributes and the
 * vertical split. Register file in CartState.mreg, ExRAM in CartState.exram. */
#include <string.h>
#include "board.h"

enum {
    M_PRGMODE = 0, M_CHRMODE, M_P1, M_P2, M_EXMODE, M_NTMAP, M_FILLT, M_FILLA,
    M_PRG = 16,                    /* 16..20: $5113-$5117 */
    M_CHRLO = 32, M_CHRHI = 44,    /* 12 CHR registers $5120-$512B (10 bits) */
    M_UPPER = 56, M_SPLIT, M_SPLITY, M_SPLITB, M_IRQT, M_IRQE, M_MULA, M_MULB,
    M_LASTSET, M_PPUCTRL, M_PPUMASK, M_INFRAME, M_LINE, M_IRQP, M_MATCH, M_EXT, M_SPLITTILE, M_SPLITON
};
#define REG(i) (n->s.cart.mreg[i])
static int chr_reg(Nes *n, int i) { return REG(M_CHRLO + i) | (REG(M_CHRHI + i) << 8); }

/* ---- PRG: per 8 KB window, ROM or RAM ---- */
static int prg_win(Nes *n, int w, int *ram) {          /* returns 8 KB bank number; *ram = RAM window */
    int mode = REG(M_PRGMODE) & 3, r;
    *ram = 0;
    switch (mode) {
    case 0: return ((REG(M_PRG + 4) & 0x7C) | w);
    case 1: if (w < 2) { r = REG(M_PRG + 2); *ram = !(r & 0x80); return ((r & 0x7E) | w); }
            return ((REG(M_PRG + 4) & 0x7E) | (w - 2));
    case 2: if (w < 2) { r = REG(M_PRG + 2); *ram = !(r & 0x80); return ((r & 0x7E) | w); }
            if (w == 2) { r = REG(M_PRG + 3); *ram = !(r & 0x80); return r & 0x7F; }
            return REG(M_PRG + 4) & 0x7F;
    default: if (w < 3) { r = REG(M_PRG + 1 + w); *ram = !(r & 0x80); return r & 0x7F; }
             return REG(M_PRG + 4) & 0x7F;
    }
}
static int ram_writable(Nes *n) { return (REG(M_P1) & 3) == 2 && (REG(M_P2) & 3) == 1; }
static int m5_rd(Nes *n, uint16_t a, int m2, uint8_t *v) {
    CS;
    if (a >= 0x8000) {
        int ram, b = prg_win(n, (a >> 13) & 3, &ram);
        *v = ram ? *wram_at(n, (uint32_t)(b & 7) * 0x2000 + (a & 0x1FFF)) : prg_abs(n, (uint32_t)b * 0x2000 + (a & 0x1FFF));
        if (!ram && (cs->mmc5snd.pcm_mode & 1) && a < 0xC000) cs->mmc5snd.pcm = *v;   /* PCM read mode */
        return 0xFF;
    }
    if (!m2) return 0;
    if (a >= 0x6000) { *v = *wram_at(n, (uint32_t)(REG(M_PRG) & 7) * 0x2000 + (a & 0x1FFF)); return 0xFF; }
    if (a >= 0x5C00) { if ((REG(M_EXMODE) & 3) >= 2) { *v = cs->exram[a & 0x3FF]; return 0xFF; } return 0; }
    switch (a) {
    case 0x5204: *v = (uint8_t)(REG(M_IRQP) << 7 | REG(M_INFRAME) << 6); REG(M_IRQP) = 0; irq(n, 0); return 0xC0;
    case 0x5205: *v = (uint8_t)(REG(M_MULA) * REG(M_MULB)); return 0xFF;
    case 0x5206: *v = (uint8_t)((REG(M_MULA) * REG(M_MULB)) >> 8); return 0xFF;
    case 0x5015: *v = (uint8_t)((cs->mmc5snd.p[0].len ? 1 : 0) | (cs->mmc5snd.p[1].len ? 2 : 0)); return 0x03;
    case 0x5010: *v = (uint8_t)(cs->mmc5snd.pcm_irq << 7); cs->mmc5snd.pcm_irq = 0; return 0x80;
    }
    return 0;
}
static const uint8_t LEN[32] = { 10,254,20,2,40,4,80,6,160,8,60,10,14,12,26,14,12,16,24,18,48,20,96,22,192,24,72,26,16,28,32,30 };
static void m5_wr(Nes *n, uint16_t a, int m2, uint8_t v) {
    CS; Mmc5Snd *s = &cs->mmc5snd;
    if (a >= 0x8000) {                                          /* RAM mapped into $8000-$DFFF */
        int ram, b = prg_win(n, (a >> 13) & 3, &ram);
        if (ram && ram_writable(n)) *wram_at(n, (uint32_t)(b & 7) * 0x2000 + (a & 0x1FFF)) = v;
        return;
    }
    if (!m2) return;
    if ((a & 0xE007) == 0x2000) { REG(M_PPUCTRL) = v; return; } /* snooped PPU registers */
    if ((a & 0xE007) == 0x2001) { REG(M_PPUMASK) = v; if (!(v & 0x18)) REG(M_INFRAME) = 0; return; }
    if (a >= 0x6000) { if (ram_writable(n)) *wram_at(n, (uint32_t)(REG(M_PRG) & 7) * 0x2000 + (a & 0x1FFF)) = v; return; }
    if (a >= 0x5C00) {
        int mode = REG(M_EXMODE) & 3;
        if (mode == 2) cs->exram[a & 0x3FF] = v;
        else if (mode < 2) cs->exram[a & 0x3FF] = REG(M_INFRAME) ? v : 0;   /* modes 0/1: only while rendering */
        return;
    }
    if (a >= 0x5000 && a <= 0x5007) {
        Mmc5Pulse *p = &s->p[(a >> 2) & 1];
        switch (a & 3) {
        case 0: p->duty = v >> 6; p->loop = (v >> 5) & 1; p->cvol = (v >> 4) & 1; p->vol = v & 15; break;
        case 2: p->period = (uint16_t)((p->period & 0x700) | v); break;
        case 3: p->period = (uint16_t)((p->period & 0xFF) | ((v & 7) << 8)); if (p->en) p->len = LEN[v >> 3]; p->estart = 1; p->step = 0; break;
        }
        return;
    }
    switch (a) {
    case 0x5010: s->pcm_mode = v & 1; s->pcm_irq_en = v >> 7; return;
    case 0x5011: if (!(s->pcm_mode & 1) && v) s->pcm = v; return;
    case 0x5015: s->p[0].en = v & 1; s->p[1].en = (v >> 1) & 1; if (!s->p[0].en) s->p[0].len = 0; if (!s->p[1].en) s->p[1].len = 0; return;
    case 0x5100: REG(M_PRGMODE) = v & 3; return;
    case 0x5101: REG(M_CHRMODE) = v & 3; return;
    case 0x5102: REG(M_P1) = v; return;
    case 0x5103: REG(M_P2) = v; return;
    case 0x5104: REG(M_EXMODE) = v & 3; return;
    case 0x5105: REG(M_NTMAP) = v; return;
    case 0x5106: REG(M_FILLT) = v; return;
    case 0x5107: REG(M_FILLA) = v & 3; return;
    case 0x5130: REG(M_UPPER) = v & 3; return;
    case 0x5200: REG(M_SPLIT) = v; return;
    case 0x5201: REG(M_SPLITY) = v; return;
    case 0x5202: REG(M_SPLITB) = v; return;
    case 0x5203: REG(M_IRQT) = v; return;
    case 0x5204: REG(M_IRQE) = v >> 7; irq(n, REG(M_IRQE) && REG(M_IRQP)); return;
    case 0x5205: REG(M_MULA) = v; return;
    case 0x5206: REG(M_MULB) = v; return;
    }
    if (a >= 0x5113 && a <= 0x5117) { REG(M_PRG + (a - 0x5113)) = v; return; }
    if (a >= 0x5120 && a <= 0x512B) { int i = a - 0x5120; REG(M_CHRLO + i) = v; REG(M_CHRHI + i) = REG(M_UPPER); REG(M_LASTSET) = i >= 8; return; }
}

/* ---- CHR: which 1 KB bank backs a pattern address ---- */
static int chr_bank(Nes *n, uint16_t a, int set_b) {
    int slot = (a >> 10) & 7, mode = REG(M_CHRMODE) & 3;
    if (!set_b) {
        switch (mode) {
        case 0: return chr_reg(n, 7) * 8 + slot;
        case 1: return chr_reg(n, slot < 4 ? 3 : 7) * 4 + (slot & 3);
        case 2: return chr_reg(n, (slot & 6) + 1) * 2 + (slot & 1);
        default: return chr_reg(n, slot);
        }
    }
    int s4 = slot & 3;                                          /* the B set covers $0000-$0FFF, mirrored to $1000 */
    switch (mode) {
    case 0: return chr_reg(n, 11) * 8 + s4 + (slot & 4);
    case 1: return chr_reg(n, 11) * 4 + s4;
    case 2: return chr_reg(n, s4 < 2 ? 9 : 11) * 2 + (s4 & 1);
    default: return chr_reg(n, 8 + s4);
    }
}
static void scanline_start(Nes *n) {
    if (!REG(M_INFRAME)) { REG(M_INFRAME) = 1; REG(M_LINE) = 0; }
    else {
        REG(M_LINE)++;
        if (REG(M_LINE) == REG(M_IRQT) && REG(M_IRQT)) { REG(M_IRQP) = 1; if (REG(M_IRQE)) irq(n, 1); }
    }
    n->s.cart.mw[1] = 0;
}
static int nt_source(Nes *n, uint16_t a, uint8_t *v) {         /* plain nametable mapping via $5105 */
    CS; int sel = (REG(M_NTMAP) >> (((a >> 10) & 3) * 2)) & 3;
    if (sel < 2) return sel ? PPU_CIRAM1 : PPU_CIRAM0;
    if (sel == 2) { *v = (REG(M_EXMODE) & 3) < 2 ? cs->exram[a & 0x3FF] : 0; return PPU_DRIVE; }
    *v = (a & 0x3FF) >= 0x3C0 ? (uint8_t)(REG(M_FILLA) * 0x55) : REG(M_FILLT);
    return PPU_DRIVE;
}
static int m5_ppu(Nes *n, uint16_t a, uint8_t *v) {
    CS;
    cs->mc[0] = 0;                                              /* the PPU is reading: not idle */
    if (a >= 0x2000 && a < 0x3000) {
        if (a == cs->mw[0]) { if (++REG(M_MATCH) == 2) scanline_start(n); }
        else REG(M_MATCH) = 0;
        cs->mw[0] = a;
    } else { REG(M_MATCH) = 0; cs->mw[0] = 0xFFFF; }        /* the reads must be strictly consecutive */
    int r = cs->mw[1]++;
    int rendering = REG(M_INFRAME) && (REG(M_PPUMASK) & 0x18);
    int sprite = rendering && r >= 128 && r < 160;
    int tile = r < 128 ? (r >> 2) + 2 : (r >= 160 && r < 168) ? ((r - 160) >> 2) : -1;
    int bg = rendering && tile >= 0;
    int phase = r & 3;                                          /* 0 NT, 1 AT, 2/3 pattern */
    /* vertical split */
    if (bg && (REG(M_SPLIT) & 0x80) && (REG(M_EXMODE) & 3) < 2) {
        if (phase == 0) {
            int t = REG(M_SPLIT) & 31, right = (REG(M_SPLIT) >> 6) & 1;
            REG(M_SPLITON) = (uint8_t)(right ? tile >= t : tile < t);
            REG(M_SPLITTILE) = (uint8_t)tile;
        }
        if (REG(M_SPLITON)) {
            int line = REG(M_LINE) + (r >= 160 ? 1 : 0);
            int y = (REG(M_SPLITY) + line) % 240, row = y >> 3, col = REG(M_SPLITTILE) & 31;
            if (phase == 0) { *v = cs->exram[(row * 32 + col) & 0x3FF]; REG(M_EXT) = *v; return PPU_DRIVE; }
            if (phase == 1) { uint8_t at = cs->exram[0x3C0 + (row >> 2) * 8 + (col >> 2)];
                              *v = (uint8_t)(((at >> (((row & 2) << 1) | (col & 2))) & 3) * 0x55); return PPU_DRIVE; }
            *v = chr_abs(n, (uint32_t)REG(M_SPLITB) * 0x1000 + REG(M_EXT) * 16 + (y & 7) + (a & 8));
            return PPU_DRIVE;
        }
    }
    if (a >= 0x2000 && a < 0x3F00) {
        if (bg && (REG(M_EXMODE) & 3) == 1) {                   /* extended attributes */
            if (phase == 0) { int k = nt_source(n, a, v); REG(M_EXT) = cs->exram[a & 0x3FF]; return k; }
            if (phase == 1) { *v = (uint8_t)((REG(M_EXT) >> 6) * 0x55); return PPU_DRIVE; }
        }
        return nt_source(n, a, v);
    }
    if (a >= 0x2000) return PPU_DEFAULT;
    if (bg && (REG(M_EXMODE) & 3) == 1 && phase >= 2) {
        *v = chr_abs(n, (uint32_t)((REG(M_EXT) & 0x3F) | (REG(M_UPPER) << 6)) * 0x1000 + (a & 0xFFF));
        return PPU_DRIVE;
    }
    int set_b = (REG(M_PPUCTRL) & 0x20) ? (rendering ? !sprite : REG(M_LASTSET)) : 0;
    *v = chr_abs(n, (uint32_t)chr_bank(n, a, set_b) * 0x400 + (a & 0x3FF));
    return PPU_DRIVE;
}
static int m5_ppu_w(Nes *n, uint16_t a, uint8_t v) {
    CS;
    if (a < 0x2000) return PPU_OPEN;                            /* CHR-ROM */
    if (a >= 0x3F00) return PPU_DEFAULT;
    int sel = (REG(M_NTMAP) >> (((a >> 10) & 3) * 2)) & 3;
    if (sel < 2) return sel ? PPU_CIRAM1 : PPU_CIRAM0;
    if (sel == 2 && (REG(M_EXMODE) & 3) < 2) cs->exram[a & 0x3FF] = v;
    return PPU_OPEN;
}
static void m5_power(Nes *n) {
    CS;
    if (n->wram_size < 0x10000) n->wram_size = 0x10000;        /* up to 64 KB of PRG-RAM */
    REG(M_PRGMODE) = 3; REG(M_CHRMODE) = 3; REG(M_PRG + 4) = 0xFF;
    memset(cs->exram, 0, sizeof cs->exram);
}
static void m5_m2(Nes *n) {
    CS; Mmc5Snd *s = &cs->mmc5snd;
    if (++cs->mc[0] >= 3) { cs->mc[0] = 3; REG(M_INFRAME) = 0; }   /* PPU stopped reading: out of frame */
    if (++s->frame >= 7457) {                                   /* 240 Hz: envelopes and length counters */
        s->frame = 0;
        for (int i = 0; i < 2; i++) {
            Mmc5Pulse *p = &s->p[i];
            if (p->estart) { p->estart = 0; p->edecay = 15; p->ediv = p->vol; }
            else if (p->ediv) p->ediv--;
            else { p->ediv = p->vol; if (p->edecay) p->edecay--; else if (p->loop) p->edecay = 15; }
            if (p->len && !p->loop) p->len--;
        }
    }
    if (cs->mc[1]++ & 1) {                                      /* pulse timers run every other cycle */
        for (int i = 0; i < 2; i++) {
            Mmc5Pulse *p = &s->p[i];
            if (p->timer == 0) { p->timer = p->period; p->step = (uint8_t)((p->step + 1) & 7); } else p->timer--;
        }
    }
}
static float m5_audio(Nes *n) {
    static const uint8_t DUTY[4][8] = { {0,1,0,0,0,0,0,0}, {0,1,1,0,0,0,0,0}, {0,1,1,1,1,0,0,0}, {1,0,0,1,1,1,1,1} };
    Mmc5Snd *s = &n->s.cart.mmc5snd; int o = 0;
    for (int i = 0; i < 2; i++) {
        Mmc5Pulse *p = &s->p[i];
        if (p->len && p->period >= 8 && DUTY[p->duty][p->step]) o += p->cvol ? p->vol : p->edecay;
    }
    return (float)o * 0.00996f + (float)s->pcm * 0.0008f;
}
const Board B_MMC5 = { 5, m5_power, m5_rd, m5_wr, m5_ppu, m5_ppu_w, NULL, m5_m2, m5_audio };
