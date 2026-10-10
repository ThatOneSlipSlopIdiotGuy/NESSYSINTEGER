/* 2C02 PPU. One ppu_tick() == one dot; (scanline, dot) is the dot executed most
 * recently, so CPU register accesses observe the PPU exactly mid-cycle.
 *  - Every fetch is an address dot plus a data dot, so the cartridge sees the PPU
 *    address bus the way hardware drives it (MMC3 A12, MMC2 latches, tilt glitches).
 *  - Sprite evaluation is an 8-bit OAMADDR state machine with the misaligned-OAM
 *    and overflow-scan quirks; secondary OAM is read during sprite fetch through
 *    its own address counter, whose overflow flag can freeze it.
 *  - Sprites are drawn by eight real output units (shift registers + X counters),
 *    so stale or partially loaded units behave like the hardware's.
 * Scanline 261 is the pre-render line. */
#include <string.h>
#include "nes.h"

#define DECAY_FRAMES 36   /* ~600 ms open-bus decay */

static uint8_t latch_get(Ppu *p) {
    for (int i = 0; i < 8; i++)
        if ((p->latch >> i & 1) && p->frame - p->latch_age[i] > DECAY_FRAMES) p->latch &= (uint8_t)~(1 << i);
    return p->latch;
}
static void latch_set(Ppu *p, uint8_t v, uint8_t mask) {
    p->latch = (uint8_t)((p->latch & ~mask) | (v & mask));
    for (int i = 0; i < 8; i++) if (mask >> i & 1) p->latch_age[i] = p->frame;
}
static int pal_idx(uint16_t a) { a &= 0x1F; if ((a & 0x13) == 0x10) a &= 0x0F; return a; }
static void nmi_update(Nes *n) { Ppu *p = &n->s.ppu; n->s.cpu.nmi_line = (p->status & 0x80) && (p->ctrl & 0x80); }
static void set_addr(Nes *n, uint16_t a) {
    uint16_t old = n->s.ppu.bus_addr;
    n->s.ppu.bus_addr = a & 0x3FFF;
    if ((old ^ a) & 0x1000) cart_ppu_addr(n, a & 0x3FFF);
}
static uint8_t fetch(Nes *n) { return cart_ppu_read(n, n->s.ppu.bus_addr); }
/* The fetch cadence: odd dots latch an address (ALE), even dots read. A $2007 read's
 * state machine reads memory 4 dots after the CPU read ends (sm_rd marks that dot). On a
 * cadence read dot the buffer takes whatever the fetch read; on an ALE dot the octal latch
 * can't take the new low byte while the read is in progress, so it keeps the value on the
 * AD pins, and both the buffer and the next fetch use that address. */
static void inc_hori(Ppu *p);
static void inc_vert(Ppu *p);
static void sm_pop(Ppu *p) {
    if (p->rq_addr[0] & 0x8000) { inc_hori(p); inc_vert(p); }   /* a $2007 read issued during rendering */
    for (int i = 1; i < p->rq_n; i++) { p->rq_addr[i - 1] = p->rq_addr[i]; p->rq_due[i - 1] = p->rq_due[i]; }
    p->rq_n--; p->sm_rd = 0;
}
static void cad_ale(Nes *n, uint16_t a) {
    Ppu *p = &n->s.ppu;
    if (p->sm_rd) {
        uint16_t eff = (uint16_t)((a & 0x3F00) | p->ad_data);
        set_addr(n, eff);
        p->rbuf = cart_ppu_read(n, eff);
        sm_pop(p);
        return;
    }
    set_addr(n, a);
    p->ad_data = (uint8_t)a;
}
static uint8_t cad_read(Nes *n) {
    Ppu *p = &n->s.ppu;
    uint8_t v = fetch(n);
    p->ad_data = v;
    if (p->sm_rd) { p->rbuf = v; sm_pop(p); }
    return v;
}
static int render_line(Ppu *p) { return p->scanline < 240 || p->scanline == 261; }
static int busy(Ppu *p) { return render_line(p) && p->render; }   /* "rendering" as the PPU sees it */
static uint8_t rev8(uint8_t b) {
    b = (uint8_t)((b & 0xF0) >> 4 | (b & 0x0F) << 4);
    b = (uint8_t)((b & 0xCC) >> 2 | (b & 0x33) << 2);
    return (uint8_t)((b & 0xAA) >> 1 | (b & 0x55) << 1);
}

static void inc_hori(Ppu *p) { if ((p->v & 0x1F) == 31) { p->v &= ~0x1F; p->v ^= 0x400; } else p->v++; }
static void inc_vert(Ppu *p) {
    if ((p->v & 0x7000) != 0x7000) { p->v += 0x1000; return; }
    p->v &= ~0x7000;
    int y = (p->v >> 5) & 31;
    if (y == 29) { y = 0; p->v ^= 0x800; } else if (y == 31) y = 0; else y++;
    p->v = (uint16_t)((p->v & ~0x3E0) | (y << 5));
}
static void inc_2007(Nes *n) {
    Ppu *p = &n->s.ppu;
    if (busy(p)) { inc_hori(p); inc_vert(p); }          /* glitchy coarse-X + Y increment while rendering */
    else { p->v = (p->v + ((p->ctrl & 4) ? 32 : 1)) & 0x7FFF; set_addr(n, p->v); }
}

void ppu_power(Nes *n, int soft) {
    static const uint8_t pal0[32] = { 0x09,0x01,0x00,0x01,0x00,0x02,0x02,0x0D,0x08,0x10,0x08,0x24,0x00,0x00,0x04,0x2C,
                                      0x09,0x01,0x34,0x03,0x00,0x04,0x00,0x14,0x08,0x3A,0x00,0x02,0x00,0x20,0x2C,0x08 };
    Ppu *p = &n->s.ppu;
    if (!soft) {
        uint64_t fr = p->frame;
        memset(p, 0, sizeof *p);
        p->frame = fr + 1;
        if (n->ram_init == 4) {      /* like a real console: this PPU's own picture, a few unstable bits */
            nes_fill_power_on(n, p->pal, 32, 6, pal0);          /* base: blargg's dump from a real NTSC PPU */
            for (int i = 0; i < 32; i++) p->pal[i] &= 0x3F;
            nes_fill_power_on(n, p->oam, 256, 5, NULL);
            for (int i = 2; i < 256; i += 4) p->oam[i] &= 0xE3;
        } else {
            memcpy(p->pal, pal0, 32);
            for (int i = 0; i < 256; i++) p->oam[i] = (uint8_t)((i * 37 + 11) & ((i & 3) == 2 ? 0xE3 : 0xFF));
        }
        memset(p->sec, 0xFF, 32);
    }
    p->ctrl = p->mask = 0; p->w = 0; p->rbuf = 0; p->fx = 0; p->t = 0; p->v_delay = 0; p->rq_n = 0;
    p->render = p->render_next = p->render_prev = 0;
    p->ignore_writes = 1;   /* NES front-loader: $2000/1/5/6 writes ignored until the end of the first VBL */
    nmi_update(n);
}

uint8_t ppu_reg_peek(Nes *n, uint16_t a) {
    Ppu *p = &n->s.ppu;
    switch (a & 7) {
    case 2: return (uint8_t)((p->status & 0xE0) | (p->latch & 0x1F));
    case 4: return p->oam[p->oamaddr];
    case 7: return p->rbuf;
    default: return p->latch;
    }
}

uint8_t ppu_reg_read(Nes *n, uint16_t a) {
    Ppu *p = &n->s.ppu;
    uint8_t r;
    switch (a & 7) {
    case 2:
        r = (uint8_t)((p->status & 0xE0) | (latch_get(p) & 0x1F));
        if (p->scanline == 241 && p->dot == 0) p->vbl_suppress = 1;   /* one dot before VBL: never set */
        p->status &= 0x7F; nmi_update(n);
        p->w = 0;
        latch_set(p, r, 0xE0);
        return r;
    case 4:
        r = (p->scanline < 240 && p->render) ? p->oam_buf : p->oam[p->oamaddr];
        latch_set(p, r, 0xFF);
        return r;
    case 7: {
        uint16_t va = busy(p) ? p->bus_addr : (uint16_t)(p->v & 0x3FFF);
        r = p->rbuf;
        if (!busy(p)) set_addr(n, va);
        if ((va & 0x3F00) == 0x3F00) {
            r = (uint8_t)((p->pal[pal_idx(va)] & ((p->mask & 1) ? 0x30 : 0x3F)) | (latch_get(p) & 0xC0));
            latch_set(p, r, 0x3F);
        } else latch_set(p, r, 0xFF);
        if (p->rq_n < 4) {   /* the read buffer refills shortly after the read */
            p->rq_addr[p->rq_n] = (uint16_t)(va | (busy(p) ? 0x8000 : 0));   /* bit 15: increment v later */
            p->rq_due[p->rq_n] = p->mclk + 16;      /* 4 dots after the CPU read cycle ends */
            p->rq_n++;
        }
        if (!busy(p)) inc_2007(n);   /* while rendering, v steps once the state machine has read */
        return r;
    }
    default: return latch_get(p);
    }
}

void ppu_reg_write(Nes *n, uint16_t a, uint8_t v) {
    Ppu *p = &n->s.ppu;
    latch_set(p, v, 0xFF);
    switch (a & 7) {
    case 0: if (p->ignore_writes) break;
        p->ctrl = v; p->t = (uint16_t)((p->t & 0x73FF) | (v & 3) << 10); nmi_update(n); break;
    case 1: if (p->ignore_writes) break;
        p->mask = v; p->render_next = (v & 0x18) != 0; break;
    case 3: p->oamaddr = v; break;
    case 4:
        if (busy(p)) p->oamaddr = (uint8_t)((p->oamaddr + 4) & 0xFC);   /* no write: +4, realigned */
        else { if ((p->oamaddr & 3) == 2) v &= 0xE3; p->oam[p->oamaddr++] = v; }
        break;
    case 5: if (p->ignore_writes) break;
        if (!p->w) { p->t = (uint16_t)((p->t & ~0x1F) | (v >> 3)); p->fx = v & 7; }
        else p->t = (uint16_t)((p->t & ~0x73E0) | (v & 7) << 12 | (v & 0xF8) << 2);
        p->w ^= 1; break;
    case 6: if (p->ignore_writes) break;
        if (!p->w) p->t = (uint16_t)((p->t & 0x00FF) | (v & 0x3F) << 8);
        else { p->t = (uint16_t)((p->t & 0x7F00) | v); p->v_pending = p->t; p->v_delay = 3; }
        p->w ^= 1; break;
    case 7: {
        uint16_t va = busy(p) ? p->bus_addr : (uint16_t)(p->v & 0x3FFF);
        if ((va & 0x3F00) == 0x3F00) p->pal[pal_idx(va)] = v & 0x3F;
        else { set_addr(n, va); cart_ppu_write(n, va, busy(p) ? (uint8_t)va : v); }  /* mid-render: writes addr LSB */
        inc_2007(n);
    } break;
    }
}

/* ---------------- sprites ---------------- */
static void oam2_inc(Ppu *p) { if (!p->oam2f && ++p->oam2a >= 32) { p->oam2a = 0; p->oam2f = 1; } }

/* Evaluation and sprite fetch share the OAM2 address counter. Reaching 32 wraps it
 * to 0 and raises the "overflowed" flag, which freezes it until a reset (dots 63,
 * 255 and 339 - each skipped if rendering is off on that dot). */
static void sprite_eval(Ppu *p, int sl, int dot) {
    if (dot <= 64) { p->oam_buf = 0xFF; p->sec[(dot - 1) >> 1] = 0xFF; return; }   /* OAM2 clear */
    if (dot == 65) { p->in_range = 0; p->copy_done = 0; p->ovf_ctr = 0; p->spr0_added = 0; p->eval_first = 1; }
    if (dot & 1) p->oam_buf = p->oam[p->oamaddr];                /* odd dots read OAM */
    else {                                                       /* even dots act on that byte */
        uint8_t b = p->oam_buf;
        int h = (p->ctrl & 0x20) ? 16 : 8, d = sl - b, in = d >= 0 && d < h, a = p->oamaddr;
        if (p->copy_done) {
            a += 4;
            p->oam_buf = p->sec[p->oam2a];                       /* once OAMADDR has wrapped, OAM2 writes become reads */
        } else if (!p->oam2f) {
            p->sec[p->oam2a] = b;
            if (p->in_range) {                                   /* bytes 2-4 of an object being copied */
                oam2_inc(p); a++;
                if (!(p->oam2a & 3)) { p->in_range = 0; if (!in) a &= 0x1FC; }   /* "X out of range" realigns */
            } else if (in) {                                     /* Y in range: start copying */
                if (p->eval_first) p->spr0_added = 1;            /* the first object processed is "sprite zero" */
                p->in_range = 1; oam2_inc(p); a++;
            } else a = (a + 4) & 0x1FC;                          /* next object, realigned */
        } else {                                                 /* OAM2 full: overflow scan, writes become reads */
            p->oam_buf = p->sec[p->oam2a];
            if (p->in_range) { a++; if (--p->ovf_ctr == 0) { p->copy_done = 1; a &= 0x1FC; } }
            else if (in) { p->status |= 0x20; p->in_range = 1; p->ovf_ctr = 3; a++; }
            else a = (((a >> 2) + 1) << 2) | ((a + 1) & 3);      /* diagonal-scan bug: n and m both +1, no carry */
        }
        p->eval_first = 0;
        if (a > 0xFF) p->copy_done = 1;
        p->oamaddr = (uint8_t)a;
    }
    if (dot == 256) p->spr0_vis = p->spr0_added;
}

static uint16_t spr_addr(Ppu *p) {
    uint8_t row = (uint8_t)((p->scanline & 0xFF) - p->f_y), tile = p->f_tile;
    if (p->ctrl & 0x20) {
        if (p->f_attr & 0x80) row = (uint8_t)(15 - row);
        return (uint16_t)((tile & 1) << 12 | (tile & 0xFE) << 4 | (row & 8) << 1 | (row & 7));
    }
    if (p->f_attr & 0x80) row = (uint8_t)(7 - row);
    return (uint16_t)((p->ctrl & 0x08) << 9 | tile << 4 | (row & 7));
}

/* dots 257-320: 8 slots x (NT, NT, pattern low, pattern high), OAM2 read through its counter */
static void sprite_fetch(Nes *n, int dot) {
    Ppu *p = &n->s.ppu;
    int i = (dot - 257) >> 3, ph = (dot - 257) & 7;
    uint8_t b = p->sec[p->oam2a];
    p->oam_buf = b;
    switch (ph) {
    case 0: p->f_y = b; cad_ale(n, 0x2000 | (p->v & 0x0FFF)); break;
    case 1: p->f_tile = b; cad_read(n); break;
    case 2: p->f_attr = b; cad_ale(n, 0x2000 | (p->v & 0x0FFF)); break;
    case 3: p->f_x = b; cad_read(n); break;
    case 4: cad_ale(n, spr_addr(p)); break;
    case 5: p->f_lo = cad_read(n); break;
    case 6: cad_ale(n, (uint16_t)(spr_addr(p) + 8)); break;
    case 7: {
        uint8_t hi = cad_read(n), lo = p->f_lo;
        /* the Y range check uses the low 8 bits of the line counter, so the pre-render
         * line (261) acts as line 5 and stale OAM2 data can put sprites on line 0 */
        int h = (p->ctrl & 0x20) ? 16 : 8, d = (p->scanline & 0xFF) - p->f_y;
        if (d < 0 || d >= h) lo = hi = 0;                       /* out of range: the unit is loaded transparent */
        if (p->f_attr & 0x40) { lo = rev8(lo); hi = rev8(hi); }
        p->sp_lo[i] = lo; p->sp_hi[i] = hi; p->sp_at[i] = p->f_attr; p->sp_x[i] = p->f_x;
        if (i == 0) p->spr0_unit = p->spr0_vis;
    } break;
    }
}

static uint8_t sprite_pixel(Ppu *p, int *s0) {
    uint8_t out = 0;
    *s0 = 0;
    for (int i = 0; i < 8; i++) {
        if (p->sp_x[i]) { p->sp_x[i]--; continue; }               /* X counters run even in forced blank */
        if (!p->render) continue;                                  /* ...but the shifters only shift while rendering */
        uint8_t c = (uint8_t)((p->sp_lo[i] >> 7) | ((p->sp_hi[i] >> 6) & 2));
        p->sp_lo[i] = (uint8_t)(p->sp_lo[i] << 1); p->sp_hi[i] = (uint8_t)(p->sp_hi[i] << 1);
        if (c && !out) {
            uint8_t at = p->sp_at[i];
            out = (uint8_t)(0x80 | c | (at & 3) << 2 | ((at & 0x20) ? 0x10 : 0));
            *s0 = i == 0 && p->spr0_unit;
        }
    }
    return out;
}

/* ---------------- background ---------------- */
static void bg_reload(Ppu *p) {
    p->bglo = (uint16_t)((p->bglo & 0xFF00) | p->plo);
    p->bghi = (uint16_t)((p->bghi & 0xFF00) | p->phi);
    p->atlo = (uint16_t)((p->atlo & 0xFF00) | ((p->at & 1) ? 0xFF : 0));
    p->athi = (uint16_t)((p->athi & 0xFF00) | ((p->at & 2) ? 0xFF : 0));
    p->at_latch = p->at;
}
static void bg_shift(Ppu *p) {
    p->bglo = (uint16_t)(p->bglo << 1);
    p->bghi = (uint16_t)(p->bghi << 1 | 1);                     /* the high bit-plane shifter brings in 1s */
    p->atlo = (uint16_t)(p->atlo << 1 | (p->at_latch & 1));      /* attribute shifters are fed from their latch */
    p->athi = (uint16_t)(p->athi << 1 | (p->at_latch >> 1 & 1));
}

static void render_dot(Nes *n, int sl, int dot) {
    Ppu *p = &n->s.ppu;
    int on = p->render, inc = p->render_prev;   /* fetches use the 1-dot-late flag, scrolling the 2-dot-late one */
    if (on) {
        if ((dot >= 2 && dot <= 257) || (dot >= 322 && dot <= 337)) bg_shift(p);
        if ((dot >= 1 && dot <= 256) || (dot >= 321 && dot <= 336)) {
            uint16_t pt = (uint16_t)((p->ctrl & 0x10) << 8 | p->nt << 4 | (p->v >> 12));
            switch ((dot - 1) & 7) {
            case 0: bg_reload(p); cad_ale(n, 0x2000 | (p->v & 0x0FFF)); break;
            case 1: p->nt = cad_read(n); break;
            case 2: cad_ale(n, 0x23C0 | (p->v & 0x0C00) | ((p->v >> 4) & 0x38) | ((p->v >> 2) & 7)); break;
            case 3: p->at = (cad_read(n) >> (((p->v >> 4) & 4) | (p->v & 2))) & 3; break;
            case 4: cad_ale(n, pt); break;
            case 5: p->plo = cad_read(n); break;
            case 6: cad_ale(n, pt + 8); break;
            case 7: p->phi = cad_read(n); break;
            }
        } else if (dot == 337 || dot == 339) { if (dot == 337) bg_reload(p); cad_ale(n, 0x2000 | (p->v & 0x0FFF)); }
        else if (dot == 338 || dot == 340) { uint8_t t = cad_read(n); if (dot == 340) p->nt = t; }   /* NT, not AT */
        else if (dot == 0 && sl < 240) {
            if (p->skip340) p->nt = cad_read(n);   /* odd frame: dot 0 performs the skipped dot-340 nametable read */
            else cad_ale(n, (uint16_t)((p->ctrl & 0x10) << 8 | p->nt << 4 | (p->v >> 12)));   /* first half of a pattern fetch */
        }
    }
    if (inc && ((dot <= 256 && !(dot & 7) && dot) || dot == 328 || dot == 336)) { inc_hori(p); if (dot == 256) inc_vert(p); }
    /* the OAM2 address steps during sprite fetch on the 2-dot-late rendering flag, like v;
     * dot 321's step is the one that overflows a full OAM2 */
    if (inc && ((dot >= 258 && dot <= 320 && ((dot - 257) & 7) <= 3) || dot == 321)) oam2_inc(p);
    if (on) {
        if (sl == 261 && dot >= 280 && dot <= 304) p->v = (uint16_t)((p->v & ~0x7BE0) | (p->t & 0x7BE0));
        if (sl < 240 && dot >= 1 && dot <= 256) sprite_eval(p, sl, dot);
        if (dot >= 257 && dot <= 320) { p->oamaddr = 0; sprite_fetch(n, dot); }
        if (dot == 63 || dot == 255) p->oam2_rst = 1;           /* reset armed; lands after the next dot's OAM2 access */
        if (dot == 339) { p->oam2a = 0; p->oam2f = 0; }
        if (dot >= 321) p->oam_buf = p->sec[p->oam2a];
    }
    if ((dot == 64 || dot == 256) && p->oam2_rst) { p->oam2a = 0; p->oam2f = 0; p->oam2_rst = 0; }
    if (inc && dot == 257) p->v = (uint16_t)((p->v & ~0x041F) | (p->t & 0x041F));   /* after the dot-257 NT fetch */
}

static void output_pixel(Nes *n, int sl, int x, uint8_t sp, int s0) {
    Ppu *p = &n->s.ppu;
    uint8_t m = p->mask, bg = 0, c;
    if ((m & 0x08) && (x >= 8 || (m & 0x02))) {
        int b = 15 - p->fx;
        uint8_t px = (uint8_t)((p->bglo >> b & 1) | (p->bghi >> b & 1) << 1);
        if (px) bg = (uint8_t)(px | ((p->atlo >> b & 1) | (p->athi >> b & 1) << 1) << 2);
    }
    if (!((m & 0x10) && (x >= 8 || (m & 0x04)))) sp = 0;
    if (sp && bg && s0 && x != 255) p->status |= 0x40;                     /* sprite 0 hit */
    uint8_t idx = (sp && (!bg || !(sp & 0x10))) ? (uint8_t)(0x10 | (sp & 0x0F)) : bg;
    if (!p->render && (p->v & 0x3F00) == 0x3F00) c = p->pal[pal_idx(p->v)];   /* forced blank shows palette[v] */
    else c = p->pal[pal_idx(idx)];
    if (m & 1) c &= 0x30;
    n->fb[sl * 256 + x] = (uint16_t)(c | (m & 0xE0) << 1);
}

static void ppu_tick(Nes *n) {
    Ppu *p = &n->s.ppu;
    if (p->dot < 340) p->dot++;
    else { p->dot = 0; if (++p->scanline > 261) { p->scanline = 0; p->frame++; } }
    int sl = p->scanline, dot = p->dot;
    if (sl < 240 || sl == 261) {
        if (dot == 1 && p->corrupt_any && p->render) {             /* armed OAM row corruption lands when rendering resumes */
            for (int i = 1; i < 32; i++) if (p->corrupt[i]) memcpy(p->oam + i * 8, p->oam, 8);
            memset(p->corrupt, 0, sizeof p->corrupt); p->corrupt_any = 0;
        }
        if (p->render || p->render_prev) render_dot(n, sl, dot);
        if (dot == 321 && (sl == 261 || sl < 239)) {                /* the next line's first tile is at v now */
            int nl = sl == 261 ? 0 : sl + 1;
            p->line_v[nl] = p->v; p->line_fx[nl] = (uint8_t)(p->fx | (p->render ? 0x80 : 0));
            memcpy(p->line_chr[nl], n->s.cart.chr_off, sizeof p->line_chr[nl]); p->line_src[nl] = n->s.cart.chr_src;
        }
        if (dot == 0) p->skip340 = 0;
        if (sl < 240 && dot >= 1 && dot <= 256) { int s0; uint8_t sp = sprite_pixel(p, &s0); output_pixel(n, sl, dot - 1, sp, s0); }
        if (dot == 339 && !p->render && !p->render_prev) memset(p->sp_x, 0, sizeof p->sp_x);  /* F-blank: X counters reset */
        if (sl == 261) {
            if (dot == 0) p->status &= (uint8_t)~0x60;                  /* sprite 0 hit + overflow clear a dot before VBL */
            if (dot == 1) { p->status &= 0x7F; nmi_update(n); p->ignore_writes = 0; }
            if (dot == 339 && (p->frame & 1) && p->render) { p->dot = 340; p->skip340 = 1; }   /* odd frame: jump to (0,0) */
        }
    } else if (sl == 240 && dot == 0) { p->frame_done = 1; set_addr(n, p->v); }
    else if (sl == 241 && dot == 1) {
        if (!p->vbl_suppress) { p->status |= 0x80; nmi_update(n); }
        p->vbl_suppress = 0;
    }
    /* end-of-dot state updates */
    if (p->v_delay && --p->v_delay == 0) { p->v = p->v_pending; if (!busy(p)) set_addr(n, p->v); }
    if (p->render_prev != p->render) {
        p->render_prev = p->render;
        if (render_line(p) && !p->render_prev) {                    /* rendering switched off mid-frame */
            set_addr(n, p->v);
            if (sl < 240 && dot >= 65 && dot <= 256) p->oamaddr++;
            if (dot >= 1 && dot <= 64) { p->corrupt[(dot - 1) >> 1] = 1; p->corrupt_any = 1; }
            else if (dot >= 257 && dot <= 320) {
                int o = (dot - 257) & 7;
                p->corrupt[((dot - 257) >> 3) * 4 + (o > 3 ? 3 : o)] = 1; p->corrupt_any = 1;
            }
        }
    }
    p->render = p->render_next;
}

void ppu_run_to(Nes *n, int64_t target) {
    Ppu *p = &n->s.ppu;
    if (p->held) { while (p->mclk + 4 <= target) p->mclk += 4; return; }   /* NES-001 reset held: counters frozen, no picture */
    while (p->mclk + 4 <= target) {
        p->sm_rd = p->rq_n && p->rq_due[0] <= p->mclk;   /* the $2007 state machine reads on this dot */
        ppu_tick(n);
        p->mclk += 4;
        if (p->sm_rd) {                                   /* no fetch on this dot: a plain read */
            p->rbuf = cart_ppu_read(n, p->rq_addr[0] & 0x3FFF);
            sm_pop(p);
        }
    }
}
