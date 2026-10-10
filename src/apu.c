/* 2A03 APU, stepped once per CPU cycle.
 * Every APU cycle is two CPU cycles: a "get" half (odd CPU cycle numbers in this
 * emulator) and a "put" half. The DMC timer, its DMA scheduling and the
 * $4015-read acknowledge of the frame IRQ follow that cadence exactly, because
 * DMA-timing-sensitive software can observe all of it (see the NESdev "DMA" page). */
#include <string.h>
#include "nes.h"

static const uint8_t LEN[32] = { 10,254,20,2,40,4,80,6,160,8,60,10,14,12,26,14,12,16,24,18,48,20,96,22,192,24,72,26,16,28,32,30 };
static const uint8_t DUTY[4][8] = { {0,0,0,0,0,0,0,1}, {0,0,0,0,0,0,1,1}, {0,0,0,0,1,1,1,1}, {1,1,1,1,1,1,0,0} };
static const uint16_t NOISE_P[16] = { 4,8,16,32,64,96,128,160,202,254,380,508,762,1016,2034,4068 };
static const uint16_t DMC_P[16] = { 428,380,340,320,286,254,226,214,190,160,142,128,106,84,72,54 };   /* in CPU cycles */
static const int32_t FC_STEPS[2][6] = { {7457,14913,22371,29828,29829,29830}, {7457,14913,22371,29829,37281,37282} };
static const uint8_t FC_TYPE[2][6] = { {1,2,1,0,2,0}, {1,2,1,0,2,0} };   /* 1 = quarter, 2 = half (+quarter) */
static float pulse_tab[31], tnd_tab[203];

static void lc_load(LenCtr *l, uint8_t enabled, uint8_t v) { if (enabled) { l->reload = LEN[v >> 3]; l->prev = l->len; } }
static void lc_tick(LenCtr *l) { if (l->len && !l->halt) l->len--; }
static void lc_apply(LenCtr *l) {
    if (l->reload) { if (l->len == l->prev) l->len = l->reload; l->reload = 0; }
    l->halt = l->new_halt;
}
static void env_tick(Env *e) {
    if (e->start) { e->start = 0; e->decay = 15; e->div = e->vol; }
    else if (e->div == 0) { e->div = e->vol; if (e->decay) e->decay--; else if (e->loop) e->decay = 15; }
    else e->div--;
}
static uint8_t env_out(Env *e) { return e->constant ? e->vol : e->decay; }
static int sweep_target(Pulse *q) {
    int ch = q->period >> q->sw_shift;
    if (q->sw_neg) return q->period - ch - (q->ch1 ? 1 : 0);
    return q->period + ch;
}
static int pulse_muted(Pulse *q) { int t = sweep_target(q); return q->period < 8 || (!q->sw_neg && t > 0x7FF); }
static void sweep_tick(Pulse *q) {
    if (q->sw_div == 0 && q->sw_en && q->sw_shift && !pulse_muted(q)) {
        int t = sweep_target(q);
        if (t >= 0) q->period = (uint16_t)t;
    }
    if (q->sw_div == 0 || q->sw_reload) { q->sw_div = q->sw_per; q->sw_reload = 0; } else q->sw_div--;
}
static void quarter(Apu *a) {
    env_tick(&a->sq[0].env); env_tick(&a->sq[1].env); env_tick(&a->noise.env);
    Tri *t = &a->tri;
    if (t->lin_flag) t->lin = t->lin_val; else if (t->lin) t->lin--;
    if (!t->control) t->lin_flag = 0;
}
static void half(Apu *a) {
    lc_tick(&a->sq[0].lc); lc_tick(&a->sq[1].lc); lc_tick(&a->tri.lc); lc_tick(&a->noise.lc);
    sweep_tick(&a->sq[0]); sweep_tick(&a->sq[1]);
}

static void dmc_restart(Dmc *d) { d->addr = d->samp_addr; d->remain = d->samp_len; }

/* the DMA unit delivers a sample byte (always on a "get" cycle) */
void apu_dmc_fill(Nes *n, uint8_t v) {
    Dmc *d = &n->s.apu.dmc;
    d->buf = v; d->buf_empty = 0;
    if (!d->remain) return;                       /* playback stopped while this fetch was in flight */
    d->addr = (uint16_t)(d->addr + 1);
    if (!d->addr) d->addr = 0x8000;
    if (--d->remain == 0) {
        if (d->loop) dmc_restart(d);
        else if (d->irq_en) n->s.cpu.irq_src |= IRQ_DMC;
    }
    if (d->samp_len == 1 && d->wrap_empty_cyc == n->s.cpu.cycles) {
        /* RP2A03H / late-G: a 1-byte sample (looping or not) fetched on the APU cycle in which
         * the output unit found the buffer empty is taken at once and fetched again */
        d->shift = d->buf; d->silence = 0; d->buf_empty = 1;
        dmc_restart(d);
        cpu_start_dmc_dma(n);
    }
}

void apu_power(Nes *n, int soft) {
    Apu *a = &n->s.apu;
    if (!pulse_tab[1]) {
        for (int i = 1; i < 31; i++) pulse_tab[i] = (float)(95.52 / (8128.0 / i + 100.0));
        for (int i = 1; i < 203; i++) tnd_tab[i] = (float)(163.67 / (24329.0 / i + 100.0));
    }
    uint8_t mode = a->fc.mode;
    uint64_t now = soft ? n->s.cpu.cycles : (uint64_t)-1;
    if (!soft) { memset(a, 0, sizeof *a); a->noise.lfsr = 1; a->sq[0].ch1 = 1; a->dmc.period = DMC_P[0] / 2 - 1; }
    a->sq[0].lc.len = a->sq[1].lc.len = a->tri.lc.len = a->noise.lc.len = 0;
    Dmc *d = &a->dmc;
    d->remain = 0; d->irq_en = 0; d->buf_empty = 1; d->bits = 8; d->silence = 1;
    d->start_delay = d->disable_delay = 0; d->act_prev = 0; d->wrap_empty_cyc = ~(uint64_t)0;
    d->level &= 1;
    a->fc_clear = 0;
    n->s.cpu.irq_src &= (uint8_t)~(IRQ_FRAME | IRQ_DMC);
    FrameCtr *f = &a->fc;   /* acts as if $4017 was written just before the reset vector is fetched */
    f->cycle = 0; f->step = 0; f->mode = soft ? mode : 0;
    f->new_val = (int16_t)(f->mode ? 0x80 : 0); f->delay = (int8_t)((now & 1) ? 4 : 3);
    f->inhibit = 0; f->block = 0;
}

static void fc_run(Nes *n) {
    FrameCtr *f = &n->s.apu.fc;
    f->cycle++;
    if (f->cycle >= FC_STEPS[f->mode][f->step]) {
        if (f->mode == 0 && f->step >= 3) {
            /* the flag is raised on two cycles even when inhibited; the third raise only
             * happens with IRQs enabled - otherwise the inhibit clears it right away */
            if (f->step < 5 || !f->inhibit) n->s.cpu.irq_src |= IRQ_FRAME;
            else n->s.cpu.irq_src &= (uint8_t)~IRQ_FRAME;
        }
        uint8_t t = FC_TYPE[f->mode][f->step];
        if (t && !f->block) { quarter(&n->s.apu); if (t == 2) half(&n->s.apu); f->block = 2; }
        if (++f->step == 6) { f->step = 0; f->cycle = 0; }
    }
    if (f->new_val >= 0 && --f->delay == 0) {
        f->mode = (f->new_val & 0x80) ? 1 : 0;
        f->delay = -1; f->step = 0; f->cycle = 0; f->new_val = -1;
        if (f->mode && !f->block) { quarter(&n->s.apu); half(&n->s.apu); f->block = 2; }
    }
    if (f->block) f->block--;
}

static void mix(Nes *n) {
    Apu *a = &n->s.apu;
    int p1 = 0, p2 = 0, t, no = 0;
    for (int i = 0; i < 2; i++) {
        Pulse *q = &a->sq[i];
        int o = (q->lc.len && !pulse_muted(q) && DUTY[q->duty][q->pos]) ? env_out(&q->env) : 0;
        if (i) p2 = o; else p1 = o;
    }
    t = a->tri.pos < 16 ? 15 - a->tri.pos : a->tri.pos - 16;
    if (a->noise.lc.len && !(a->noise.lfsr & 1)) no = env_out(&a->noise.env);
    a->acc += cart_audio_path(n, pulse_tab[p1 + p2] + tnd_tab[3 * t + 2 * no + a->dmc.level]);
    a->acc_n++;
    a->phase += AUDIO_RATE;
    if (a->phase >= NES_CPU_HZ) {
        a->phase -= NES_CPU_HZ;
        double s = a->acc / a->acc_n; a->acc = 0; a->acc_n = 0;
        /* NES output filter chain: 90 Hz + 440 Hz high-pass, 14 kHz low-pass */
        double h1 = 0.98726 * (a->hp1_out + s - a->hp1_in); a->hp1_in = s; a->hp1_out = h1;
        double h2 = 0.93926 * (a->hp2_out + h1 - a->hp2_in); a->hp2_in = h1; a->hp2_out = h2;
        a->lp_out += 0.8653 * (h2 - a->lp_out);
        int v = (int)(a->lp_out * 40000.0);
        if (v > 32767) v = 32767;
        if (v < -32768) v = -32768;
        if (n->audio_len < AUDIO_MAX) n->audio[n->audio_len++] = (int16_t)v;
    }
}

void apu_cycle(Nes *n) {
    Apu *a = &n->s.apu;
    Dmc *d = &a->dmc;
    int get = (int)(n->s.cpu.cycles & 1);
    if (d->disable_delay && --d->disable_delay == 0) cpu_stop_dmc_dma(n);
    if (d->start_delay && --d->start_delay == 0 && d->buf_empty && d->remain) {   /* load DMA */
        if (n->trace) n->trace(n, 'L', d->addr, 0);
        cpu_start_dmc_dma(n);
    }
    if (get && a->fc_clear) { n->s.cpu.irq_src &= (uint8_t)~IRQ_FRAME; a->fc_clear = 0; }      /* put->get edge */
    fc_run(n);
    for (int i = 0; i < 2; i++) {
        Pulse *q = &a->sq[i];
        if (q->timer == 0) { q->timer = (uint16_t)(q->period * 2 + 1); q->pos = (q->pos - 1) & 7; } else q->timer--;
    }
    Tri *t = &a->tri;
    if (t->timer == 0) { t->timer = t->period; if (t->lc.len && t->lin) t->pos = (t->pos + 1) & 31; } else t->timer--;
    Noise *no = &a->noise;
    if (no->timer == 0) {
        no->timer = no->period;
        uint16_t fb = (no->lfsr ^ (no->lfsr >> (no->mode ? 6 : 1))) & 1;
        no->lfsr = (uint16_t)(no->lfsr >> 1 | fb << 14);
    } else no->timer--;
    if (get) {                                    /* the DMC runs on APU cycles */
        uint8_t act = d->act_prev;                /* playback state one APU cycle ago */
        d->act_prev = d->remain != 0;
        if (d->timer == 0) {
            d->timer = d->period;
            if (!d->silence) {
                if (d->shift & 1) { if (d->level <= 125) d->level += 2; } else if (d->level >= 2) d->level -= 2;
                d->shift >>= 1;
            }
            if (--d->bits == 0) {
                d->bits = 8;
                if (d->buf_empty) { d->silence = 1; d->wrap_empty_cyc = n->s.cpu.cycles; if (n->trace) n->trace(n, 'e', 0, 0); }
                else {
                    d->silence = 0; d->shift = d->buf; d->buf_empty = 1;
                    if (n->trace) n->trace(n, 'w', 0, 0);
                    if (act) {   /* reload decided on the status one APU cycle late */
                        if (n->trace) n->trace(n, d->remain ? 'S' : 'A', d->addr, 0);               /* reload DMA: tries to halt on the next (put) cycle */
                        cpu_start_dmc_dma(n);
                        if (!d->remain) d->disable_delay = 1;   /* stopped during the previous APU cycle: aborted after its halt */
                    }
                }
            }
        } else d->timer--;
    }
    lc_apply(&a->sq[0].lc); lc_apply(&a->sq[1].lc); lc_apply(&a->tri.lc); lc_apply(&a->noise.lc);
    mix(n);
}

uint8_t apu_status(Nes *n, int clear) {
    Apu *a = &n->s.apu;
    uint8_t r = (uint8_t)((a->sq[0].lc.len ? 1 : 0) | (a->sq[1].lc.len ? 2 : 0) | (a->tri.lc.len ? 4 : 0) |
                          (a->noise.lc.len ? 8 : 0) | (a->dmc.remain ? 0x10 : 0) |
                          ((n->s.cpu.irq_src & IRQ_FRAME) ? 0x40 : 0) | ((n->s.cpu.irq_src & IRQ_DMC) ? 0x80 : 0));
    if (clear) a->fc_clear = 1;                   /* the frame IRQ flag clears on the next put->get edge */
    return r;
}

void apu_write(Nes *n, uint16_t addr, uint8_t v) {
    Apu *a = &n->s.apu;
    switch (addr) {
    case 0x4000: case 0x4004: {
        Pulse *q = &a->sq[(addr >> 2) & 1];
        q->duty = v >> 6; q->lc.new_halt = q->env.loop = (v >> 5) & 1; q->env.constant = (v >> 4) & 1; q->env.vol = v & 15;
    } break;
    case 0x4001: case 0x4005: {
        Pulse *q = &a->sq[(addr >> 2) & 1];
        q->sw_en = v >> 7; q->sw_per = (v >> 4) & 7; q->sw_neg = (v >> 3) & 1; q->sw_shift = v & 7; q->sw_reload = 1;
    } break;
    case 0x4002: case 0x4006: { Pulse *q = &a->sq[(addr >> 2) & 1]; q->period = (uint16_t)((q->period & 0x700) | v); } break;
    case 0x4003: case 0x4007: {
        Pulse *q = &a->sq[(addr >> 2) & 1];
        q->period = (uint16_t)((q->period & 0xFF) | (v & 7) << 8);
        lc_load(&q->lc, q->lc.enabled, v); q->pos = 0; q->env.start = 1;
    } break;
    case 0x4008: a->tri.control = a->tri.lc.new_halt = v >> 7; a->tri.lin_val = v & 0x7F; break;
    case 0x400A: a->tri.period = (uint16_t)((a->tri.period & 0x700) | v); break;
    case 0x400B: a->tri.period = (uint16_t)((a->tri.period & 0xFF) | (v & 7) << 8);
        lc_load(&a->tri.lc, a->tri.lc.enabled, v); a->tri.lin_flag = 1; break;
    case 0x400C: a->noise.lc.new_halt = a->noise.env.loop = (v >> 5) & 1; a->noise.env.constant = (v >> 4) & 1; a->noise.env.vol = v & 15; break;
    case 0x400E: a->noise.mode = v >> 7; a->noise.period = (uint16_t)(NOISE_P[v & 15] - 1); break;
    case 0x400F: lc_load(&a->noise.lc, a->noise.lc.enabled, v); a->noise.env.start = 1; break;
    case 0x4010:
        a->dmc.irq_en = v >> 7; a->dmc.loop = (v >> 6) & 1; a->dmc.period = (uint16_t)(DMC_P[v & 15] / 2 - 1);
        if (!a->dmc.irq_en) n->s.cpu.irq_src &= (uint8_t)~IRQ_DMC;
        break;
    case 0x4011: a->dmc.level = v & 0x7F; break;
    case 0x4012: a->dmc.samp_addr = (uint16_t)(0xC000 | v << 6); break;
    case 0x4013: a->dmc.samp_len = (uint16_t)(v << 4 | 1); break;
    case 0x4015: {
        LenCtr *l[4] = { &a->sq[0].lc, &a->sq[1].lc, &a->tri.lc, &a->noise.lc };
        for (int i = 0; i < 4; i++) { l[i]->enabled = (v >> i) & 1; if (!l[i]->enabled) l[i]->len = 0; }
        n->s.cpu.irq_src &= (uint8_t)~IRQ_DMC;
        Dmc *d = &a->dmc;
        if (!(v & 0x10)) d->remain = 0;
        else if (!d->remain) {   /* load DMA: halts on the get cycle of the 2nd following APU cycle */
            dmc_restart(d);
            d->start_delay = (uint8_t)((n->s.cpu.cycles & 1) ? 3 : 2);
        }
    } break;
    case 0x4017: {
        FrameCtr *f = &a->fc;
        f->new_val = v; f->delay = (int8_t)((n->s.cpu.cycles & 1) ? 4 : 3);
        f->inhibit = (v >> 6) & 1;
        if (f->inhibit) n->s.cpu.irq_src &= (uint8_t)~IRQ_FRAME;
    } break;
    }
}
