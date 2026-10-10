/* 2A03 CPU. Every CPU cycle is one bus access (including all dummy reads and
 * writes), and each access advances the PPU/APU/mapper in lockstep using a
 * master-clock model (12 master clocks per CPU cycle, 4 per PPU dot).
 * Interrupts are polled at the end of every cycle and taken based on the
 * state at the end of an instruction's second-to-last cycle. DMA halts the
 * CPU on read cycles only, exactly like the real RDY line. */
#include "nes.h"

/* One CPU cycle is 12 master clocks. The bus access (read data sampled, write data
 * delivered) lands 5 clocks in, and the 6502's NMI edge detector samples the /NMI line
 * 9 clocks in. Calibrated against AccuracyCoin on the real ROM: these are the only
 * values with which every VBlank/NMI and rendering-toggle test passes at all four
 * CPU/PPU clock alignments. */
#define ACCESS_PT 5
#define NMI_PT 9
static void start_cycle(Nes *n, int rd) {
    Cpu *c = &n->s.cpu;
    (void)rd;
    c->mclk += ACCESS_PT;
    c->cycles++;
    ppu_run_to(n, c->mclk - n->ppu_offset);
    nes_cpu_clock(n);
}

/* real == 0 for cycles in which DMA holds the CPU halted: the 6502 core makes
 * no progress then, so its interrupt-poll pipeline does not advance (the NMI
 * edge detector itself still watches the line). */
static void end_cycle(Nes *n, int rd, int real) {
    Cpu *c = &n->s.cpu;
    (void)rd;
    ppu_run_to(n, c->mclk + (NMI_PT - ACCESS_PT) - n->ppu_offset);
    if (!c->prev_nmi_line && c->nmi_line) c->need_nmi = 1;
    c->prev_nmi_line = c->nmi_line;
    c->mclk += 12 - ACCESS_PT;
    ppu_run_to(n, c->mclk - n->ppu_offset);
    if (!real) return;
    c->prev_need_nmi = c->nmi_s;
    c->nmi_s = c->need_nmi;
    c->prev_run_irq = c->run_irq;
    c->run_irq = nes_irq_line(n) && !(c->p & FI);
}
#define TRACE(k, a, v) do { if (n->trace) n->trace(n, (k), (uint16_t)(a), (uint8_t)(v)); } while (0)

/* ---- DMA (OAM + DMC), including the 2A03 internal-register read glitch ---- */
/* The 2A03 decodes its registers from the 6502 core's A15-A5 (still holding the
 * halted read's address) and the DMA unit's A4-A0, so a DMA can hit them. */
static uint8_t dma_read(Nes *n, uint16_t a, uint16_t *prev, int internal, int dmc) {
    Cpu *c = &n->s.cpu;
    uint8_t v;
    if (!internal) {
        if (a >= 0x4000 && a <= 0x401F) v = c->ibus = c->bus;   /* registers inactive: nothing answers */
        else v = cpu_bus_read(n, a);
        *prev = a;
        return v;
    }
    uint16_t ia = 0x4000 | (a & 0x1F);
    int same = ia == a;
    if (ia == 0x4015) {
        /* the 2A03 reads $4015 onto its internal bus (that's what the DMA gets), while the
         * external read still puts the addressed byte on the cartridge-side pins */
        uint8_t st = (uint8_t)((apu_status(n, 1) & 0xDF) | (c->ibus & 0x20));
        if (!same) cpu_bus_read(n, a);
        v = c->ibus = st;
    } else if (ia == 0x4016 || ia == 0x4017) {
        /* the port drivers override D0-D4 on the pins; D5-D7 keep what the external read put
         * there. A contiguous read of the same port doesn't clock it. */
        uint8_t joy = (*prev == ia) ? nes_pad_peek(n, ia & 1) : (uint8_t)(cpu_bus_read(n, ia) & 0x1F);
        uint8_t src = same ? c->bus : cpu_bus_read(n, a);
        uint8_t pins = (uint8_t)((src & 0xE0) | joy);
        c->bus = c->ibus = pins;
        v = dmc ? (uint8_t)(src & (joy | 0xE0)) : pins;   /* the DMC latches a corrupted byte */
    } else v = cpu_bus_read(n, a);                      /* write-only registers: just the external read */
    *prev = ia;
    return v;
}

/* halt / dummy / alignment cycles repeat the CPU's read; repeats of a joypad read keep
 * the port's bit on the bus without clocking it (NES behaviour) */
static void dma_idle_read(Nes *n, uint16_t ra, int pad) {
    Cpu *c = &n->s.cpu;
    if (!pad) { cpu_bus_read(n, ra); return; }
    c->bus = c->ibus = (uint8_t)(nes_pad_peek(n, ra & 1) | (c->bus & 0xE0));
}

static void dma_cycle(Nes *n) {
    Cpu *c = &n->s.cpu;
    if (c->abort_dmc) { c->dmc_running = 0; c->abort_dmc = 0; c->need_dummy = 0; c->need_halt = 0; }
    else if (c->need_halt) c->need_halt = 0;
    else if (c->need_dummy) c->need_dummy = 0;
    start_cycle(n, 1);
}

/* Odd CPU cycle numbers are DMA "get" cycles, even ones "put" cycles. */
static void process_dma(Nes *n, uint16_t ra) {
    Cpu *c = &n->s.cpu;
    uint16_t prev = ra;
    int pad = ra == 0x4016 || ra == 0x4017;
    int internal = (ra & 0xFFE0) == 0x4000, skip_first = 0;
    c->dma_hit = 1;
    /* DMC reading the same register the CPU was reading hides the read from the controller */
    if (internal && c->dmc_running && pad && ((n->s.apu.dmc.addr & 0x1F) == (ra & 0x1F))) skip_first = 1;
    c->need_halt = 0;
    start_cycle(n, 1);                       /* halt cycle: the CPU's read is repeated */
    TRACE('H', ra, 0);
    if (!skip_first) cpu_bus_read(n, ra); else dma_idle_read(n, ra, 1);
    end_cycle(n, 1, 0);
    if (c->abort_dmc) {                      /* DMC DMA aborted during its halt cycle */
        c->dmc_running = 0; c->abort_dmc = 0;
        if (!c->spr_dma) { c->need_dummy = 0; return; }
    }
    uint16_t cnt = 0; uint8_t sa = 0, val = 0;
    while (c->dmc_running || c->spr_dma) {
        if ((c->cycles & 1) == 0) {          /* "get" cycle */
            if (c->dmc_running && !c->need_halt && !c->need_dummy) {
                dma_cycle(n);
                val = dma_read(n, n->s.apu.dmc.addr, &prev, internal, 1);
                TRACE('G', n->s.apu.dmc.addr, val);
                end_cycle(n, 1, 0);
                c->dmc_running = 0; c->abort_dmc = 0;
                apu_dmc_fill(n, val);
            } else if (c->spr_dma) {
                dma_cycle(n);
                val = dma_read(n, (uint16_t)(c->spr_page << 8 | sa), &prev, internal, 0);
                TRACE('o', c->spr_page << 8 | sa, val);
                end_cycle(n, 1, 0);
                sa++; cnt++;
            } else {
                dma_cycle(n); TRACE('d', ra, 0); dma_idle_read(n, ra, pad); end_cycle(n, 1, 0);
            }
        } else {                             /* "put" cycle */
            if (c->spr_dma && (cnt & 1)) {
                dma_cycle(n); TRACE('p', 0x2004, val); cpu_bus_write(n, 0x2004, val); end_cycle(n, 1, 0);
                if (++cnt == 0x200) c->spr_dma = 0;
            } else {
                dma_cycle(n); TRACE('d', ra, 0); dma_idle_read(n, ra, pad); end_cycle(n, 1, 0);
            }
        }
    }
}

void cpu_start_dmc_dma(Nes *n) { Cpu *c = &n->s.cpu; c->dmc_running = 1; c->need_dummy = 1; c->need_halt = 1; }
void cpu_stop_dmc_dma(Nes *n) {
    Cpu *c = &n->s.cpu;
    if (!c->dmc_running) return;
    if (c->need_halt) {                      /* halt not reached yet: the DMA never happens */
        c->dmc_running = 0; c->need_dummy = 0;
        if (!c->spr_dma) c->need_halt = 0;   /* ...but a pending OAM DMA still needs its halt */
    } else c->abort_dmc = 1;                 /* already halted: stop after this cycle */
}

/* ---- bus access helpers ---- */
static uint8_t rd(Nes *n, uint16_t a) {
    if (n->s.cpu.need_halt) process_dma(n, a);
    start_cycle(n, 1);
    uint8_t v = cpu_bus_read(n, a);
    TRACE('R', a, v);
    end_cycle(n, 1, 1);
    return v;
}
static void wr(Nes *n, uint16_t a, uint8_t v) {
    start_cycle(n, 0);
    TRACE('W', a, v);
    cpu_bus_write(n, a, v);
    end_cycle(n, 0, 1);
}
#define CPU Cpu *c = &n->s.cpu
static uint8_t fetch(Nes *n) { return rd(n, n->s.cpu.pc++); }
static void push(Nes *n, uint8_t v) { CPU; wr(n, 0x100 | c->s, v); c->s--; }
static uint8_t pop(Nes *n) { CPU; c->s++; return rd(n, 0x100 | c->s); }
static void zn(Cpu *c, uint8_t v) { c->p = (uint8_t)((c->p & ~(FZ | FN)) | (v ? 0 : FZ) | (v & 0x80)); }

/* ---- addressing (all dummy accesses included) ---- */
static uint16_t a_zp(Nes *n) { return fetch(n); }
static uint16_t a_zpi(Nes *n, uint8_t i) { uint8_t b = fetch(n); rd(n, b); return (uint8_t)(b + i); }
static uint16_t a_abs(Nes *n) { uint16_t lo = fetch(n); return lo | (uint16_t)fetch(n) << 8; }
static uint16_t a_abi(Nes *n, uint8_t i, int always) {
    uint16_t b = a_abs(n), a = (uint16_t)(b + i);
    if (always || ((a ^ b) & 0xFF00)) rd(n, (b & 0xFF00) | (a & 0xFF));
    return a;
}
static uint16_t a_izx(Nes *n) {
    CPU; uint8_t b = fetch(n); rd(n, b); b += c->x;
    uint16_t lo = rd(n, b); return lo | (uint16_t)rd(n, (uint8_t)(b + 1)) << 8;
}
static uint16_t ptr_zp(Nes *n, uint8_t b) { uint16_t lo = rd(n, b); return lo | (uint16_t)rd(n, (uint8_t)(b + 1)) << 8; }
static uint16_t a_izy(Nes *n, int always) {
    CPU; uint16_t b = ptr_zp(n, fetch(n)), a = (uint16_t)(b + c->y);
    if (always || ((a ^ b) & 0xFF00)) rd(n, (b & 0xFF00) | (a & 0xFF));
    return a;
}

/* ---- ALU ---- */
static void adc(Cpu *c, uint8_t v) {
    unsigned r = c->a + v + (c->p & FC);
    c->p &= (uint8_t)~(FC | FV);
    if (r > 0xFF) c->p |= FC;
    if (~(c->a ^ v) & (c->a ^ r) & 0x80) c->p |= FV;
    c->a = (uint8_t)r; zn(c, c->a);
}
static void cmp(Cpu *c, uint8_t r, uint8_t v) { c->p = (uint8_t)((c->p & ~FC) | (r >= v ? FC : 0)); zn(c, (uint8_t)(r - v)); }
static uint8_t asl(Cpu *c, uint8_t v) { c->p = (uint8_t)((c->p & ~FC) | (v >> 7)); v <<= 1; zn(c, v); return v; }
static uint8_t lsr(Cpu *c, uint8_t v) { c->p = (uint8_t)((c->p & ~FC) | (v & 1)); v >>= 1; zn(c, v); return v; }
static uint8_t rol(Cpu *c, uint8_t v) { uint8_t r = (uint8_t)(v << 1 | (c->p & FC)); c->p = (uint8_t)((c->p & ~FC) | (v >> 7)); zn(c, r); return r; }
static uint8_t ror(Cpu *c, uint8_t v) { uint8_t r = (uint8_t)(v >> 1 | (c->p & FC) << 7); c->p = (uint8_t)((c->p & ~FC) | (v & 1)); zn(c, r); return r; }
static void bit(Cpu *c, uint8_t v) { c->p = (uint8_t)((c->p & ~(FZ | FV | FN)) | ((c->a & v) ? 0 : FZ) | (v & (FV | FN))); }

static void branch(Nes *n, int cond) {
    CPU; int8_t off = (int8_t)fetch(n);
    if (!cond) return;
    if (c->run_irq && !c->prev_run_irq) c->run_irq = 0;  /* taken branch delays a fresh IRQ */
    rd(n, c->pc);
    uint16_t t = (uint16_t)(c->pc + off);
    if ((t ^ c->pc) & 0xFF00) rd(n, (c->pc & 0xFF00) | (t & 0xFF));
    c->pc = t;
}

/* SHA/SHX/SHY/TAS store val & (H+1); on a page cross the high address byte is
 * replaced by the stored value itself. If DMA halts the CPU (RDY low) on one of
 * the two cycles before the write, the "& (H+1)" term drops out of both. */
static void shx(Nes *n, uint16_t base, uint8_t idx, uint8_t val) {
    CPU;
    uint16_t a = (uint16_t)(base + idx);
    uint8_t out = c->dma_hit ? val : (uint8_t)(val & ((base >> 8) + 1));
    uint8_t hi = a >> 8;
    if ((a ^ base) & 0xFF00) hi = out;
    wr(n, (uint16_t)(hi << 8 | (a & 0xFF)), out);
}

static void interrupt(Nes *n) {
    CPU;
    rd(n, c->pc); rd(n, c->pc);
    push(n, c->pc >> 8); push(n, c->pc & 0xFF);
    uint16_t vec = 0xFFFE;
    if (c->need_nmi) { c->need_nmi = 0; vec = 0xFFFA; }
    push(n, (uint8_t)((c->p & ~FB) | FU));
    c->p |= FI;
    uint16_t lo = rd(n, vec);
    c->pc = lo | (uint16_t)rd(n, vec + 1) << 8;
}

/* a cycle with the CPU held in reset: time and M2 run on, nothing executes */
void cpu_idle_cycle(Nes *n) { start_cycle(n, 1); end_cycle(n, 1, 0); }

void cpu_power(Nes *n, int soft) {
    CPU;
    if (!soft) { c->a = c->x = c->y = 0; c->s = 0; c->p = FI | FU; }
    c->p |= FI;
    c->jammed = 0; c->irq_src = 0; c->nmi_line = c->prev_nmi_line = 0;
    c->need_nmi = c->prev_need_nmi = c->run_irq = c->prev_run_irq = 0;
    c->need_halt = c->need_dummy = c->dmc_running = c->abort_dmc = c->spr_dma = 0;
    /* 8-cycle reset sequence on the real bus: dummy reads, three suppressed
     * pushes (why S ends up at $FD after power-on), then the vector fetch.  */
    rd(n, c->pc); rd(n, c->pc); rd(n, c->pc);
    for (int i = 0; i < 3; i++) { rd(n, (uint16_t)(0x100 | c->s)); c->s--; }
    uint16_t lo = rd(n, 0xFFFC);
    c->pc = (uint16_t)(lo | rd(n, 0xFFFD) << 8);
}

#define RD(op, ea, body)  case op: { uint16_t e = ea; uint8_t v = rd(n, e); body; } break;
#define IM(op, body)      case op: { uint8_t v = fetch(n); body; } break;
#define WR(op, ea, val)   case op: { uint16_t e = ea; wr(n, e, val); } break;
#define RMW(op, ea, body) case op: { uint16_t e = ea; uint8_t v = rd(n, e); wr(n, e, v); body; wr(n, e, v); } break;
#define IMP(op, body)     case op: { rd(n, c->pc); body; } break;
/* the standard 8 addressing variants of a read op */
#define RD8(i_, z, zx, ab, ax, ay, ix, iy, body) IM(i_, body) RD(z, a_zp(n), body) RD(zx, a_zpi(n, c->x), body) \
    RD(ab, a_abs(n), body) RD(ax, a_abi(n, c->x, 0), body) RD(ay, a_abi(n, c->y, 0), body) \
    RD(ix, a_izx(n), body) RD(iy, a_izy(n, 0), body)
/* the 7 addressing variants of an unofficial RMW combo */
#define RMW7(z, zx, ab, ax, ay, ix, iy, body) RMW(z, a_zp(n), body) RMW(zx, a_zpi(n, c->x), body) \
    RMW(ab, a_abs(n), body) RMW(ax, a_abi(n, c->x, 1), body) RMW(ay, a_abi(n, c->y, 1), body) \
    RMW(ix, a_izx(n), body) RMW(iy, a_izy(n, 1), body)

void cpu_step(Nes *n) {
    CPU;
    if (c->jammed) { start_cycle(n, 1); end_cycle(n, 1, 1); return; }
    uint8_t op = fetch(n);
    switch (op) {
    RD8(0x69, 0x65, 0x75, 0x6D, 0x7D, 0x79, 0x61, 0x71, adc(c, v))
    RD8(0x29, 0x25, 0x35, 0x2D, 0x3D, 0x39, 0x21, 0x31, c->a &= v; zn(c, c->a))
    RD8(0x09, 0x05, 0x15, 0x0D, 0x1D, 0x19, 0x01, 0x11, c->a |= v; zn(c, c->a))
    RD8(0x49, 0x45, 0x55, 0x4D, 0x5D, 0x59, 0x41, 0x51, c->a ^= v; zn(c, c->a))
    RD8(0xC9, 0xC5, 0xD5, 0xCD, 0xDD, 0xD9, 0xC1, 0xD1, cmp(c, c->a, v))
    RD8(0xE9, 0xE5, 0xF5, 0xED, 0xFD, 0xF9, 0xE1, 0xF1, adc(c, (uint8_t)~v))
    RD8(0xA9, 0xA5, 0xB5, 0xAD, 0xBD, 0xB9, 0xA1, 0xB1, c->a = v; zn(c, v))
    IM(0xEB, adc(c, (uint8_t)~v))
    IM(0xA2, c->x = v; zn(c, v)) RD(0xA6, a_zp(n), c->x = v; zn(c, v)) RD(0xB6, a_zpi(n, c->y), c->x = v; zn(c, v))
    RD(0xAE, a_abs(n), c->x = v; zn(c, v)) RD(0xBE, a_abi(n, c->y, 0), c->x = v; zn(c, v))
    IM(0xA0, c->y = v; zn(c, v)) RD(0xA4, a_zp(n), c->y = v; zn(c, v)) RD(0xB4, a_zpi(n, c->x), c->y = v; zn(c, v))
    RD(0xAC, a_abs(n), c->y = v; zn(c, v)) RD(0xBC, a_abi(n, c->x, 0), c->y = v; zn(c, v))
    IM(0xE0, cmp(c, c->x, v)) RD(0xE4, a_zp(n), cmp(c, c->x, v)) RD(0xEC, a_abs(n), cmp(c, c->x, v))
    IM(0xC0, cmp(c, c->y, v)) RD(0xC4, a_zp(n), cmp(c, c->y, v)) RD(0xCC, a_abs(n), cmp(c, c->y, v))
    RD(0x24, a_zp(n), bit(c, v)) RD(0x2C, a_abs(n), bit(c, v))
    /* stores */
    WR(0x85, a_zp(n), c->a) WR(0x95, a_zpi(n, c->x), c->a) WR(0x8D, a_abs(n), c->a) WR(0x9D, a_abi(n, c->x, 1), c->a)
    WR(0x99, a_abi(n, c->y, 1), c->a) WR(0x81, a_izx(n), c->a) WR(0x91, a_izy(n, 1), c->a)
    WR(0x86, a_zp(n), c->x) WR(0x96, a_zpi(n, c->y), c->x) WR(0x8E, a_abs(n), c->x)
    WR(0x84, a_zp(n), c->y) WR(0x94, a_zpi(n, c->x), c->y) WR(0x8C, a_abs(n), c->y)
    /* read-modify-write */
    IMP(0x0A, c->a = asl(c, c->a)) IMP(0x4A, c->a = lsr(c, c->a)) IMP(0x2A, c->a = rol(c, c->a)) IMP(0x6A, c->a = ror(c, c->a))
    RMW(0x06, a_zp(n), v = asl(c, v)) RMW(0x16, a_zpi(n, c->x), v = asl(c, v)) RMW(0x0E, a_abs(n), v = asl(c, v)) RMW(0x1E, a_abi(n, c->x, 1), v = asl(c, v))
    RMW(0x46, a_zp(n), v = lsr(c, v)) RMW(0x56, a_zpi(n, c->x), v = lsr(c, v)) RMW(0x4E, a_abs(n), v = lsr(c, v)) RMW(0x5E, a_abi(n, c->x, 1), v = lsr(c, v))
    RMW(0x26, a_zp(n), v = rol(c, v)) RMW(0x36, a_zpi(n, c->x), v = rol(c, v)) RMW(0x2E, a_abs(n), v = rol(c, v)) RMW(0x3E, a_abi(n, c->x, 1), v = rol(c, v))
    RMW(0x66, a_zp(n), v = ror(c, v)) RMW(0x76, a_zpi(n, c->x), v = ror(c, v)) RMW(0x6E, a_abs(n), v = ror(c, v)) RMW(0x7E, a_abi(n, c->x, 1), v = ror(c, v))
    RMW(0xE6, a_zp(n), v++; zn(c, v)) RMW(0xF6, a_zpi(n, c->x), v++; zn(c, v)) RMW(0xEE, a_abs(n), v++; zn(c, v)) RMW(0xFE, a_abi(n, c->x, 1), v++; zn(c, v))
    RMW(0xC6, a_zp(n), v--; zn(c, v)) RMW(0xD6, a_zpi(n, c->x), v--; zn(c, v)) RMW(0xCE, a_abs(n), v--; zn(c, v)) RMW(0xDE, a_abi(n, c->x, 1), v--; zn(c, v))
    /* implied */
    IMP(0xAA, c->x = c->a; zn(c, c->x)) IMP(0xA8, c->y = c->a; zn(c, c->y)) IMP(0xBA, c->x = c->s; zn(c, c->x))
    IMP(0x8A, c->a = c->x; zn(c, c->a)) IMP(0x9A, c->s = c->x) IMP(0x98, c->a = c->y; zn(c, c->a))
    IMP(0xE8, c->x++; zn(c, c->x)) IMP(0xC8, c->y++; zn(c, c->y)) IMP(0xCA, c->x--; zn(c, c->x)) IMP(0x88, c->y--; zn(c, c->y))
    IMP(0x18, c->p &= ~FC) IMP(0x38, c->p |= FC) IMP(0x58, c->p &= ~FI) IMP(0x78, c->p |= FI)
    IMP(0xB8, c->p &= ~FV) IMP(0xD8, c->p &= ~FD) IMP(0xF8, c->p |= FD)
    IMP(0xEA, (void)0) IMP(0x1A, (void)0) IMP(0x3A, (void)0) IMP(0x5A, (void)0) IMP(0x7A, (void)0) IMP(0xDA, (void)0) IMP(0xFA, (void)0)
    /* stack / flow */
    IMP(0x48, push(n, c->a)) IMP(0x08, push(n, c->p | FB | FU))
    IMP(0x68, rd(n, 0x100 | c->s); c->a = pop(n); zn(c, c->a))
    IMP(0x28, rd(n, 0x100 | c->s); c->p = (uint8_t)((pop(n) & ~FB) | FU))
    case 0x20: { uint8_t lo = fetch(n); rd(n, 0x100 | c->s); push(n, c->pc >> 8); push(n, c->pc & 0xFF);
                 c->pc = (uint16_t)(lo | rd(n, c->pc) << 8); } break;
    IMP(0x60, rd(n, 0x100 | c->s); { uint16_t lo = pop(n); c->pc = lo | (uint16_t)pop(n) << 8; } rd(n, c->pc); c->pc++)
    IMP(0x40, rd(n, 0x100 | c->s); c->p = (uint8_t)((pop(n) & ~FB) | FU); { uint16_t lo = pop(n); c->pc = lo | (uint16_t)pop(n) << 8; })
    case 0x00: {
        fetch(n);
        push(n, c->pc >> 8); push(n, c->pc & 0xFF);
        uint16_t vec = 0xFFFE;
        if (c->need_nmi) { c->need_nmi = 0; vec = 0xFFFA; }   /* NMI hijacks BRK */
        push(n, c->p | FB | FU); c->p |= FI;
        uint16_t lo = rd(n, vec); c->pc = lo | (uint16_t)rd(n, vec + 1) << 8;
        c->prev_need_nmi = 0;
    } break;
    case 0x4C: c->pc = a_abs(n); break;
    case 0x6C: { uint16_t p = a_abs(n); uint16_t lo = rd(n, p);
                 c->pc = lo | (uint16_t)rd(n, (p & 0xFF00) | ((p + 1) & 0xFF)) << 8; } break;
    case 0x10: branch(n, !(c->p & FN)); break; case 0x30: branch(n, c->p & FN); break;
    case 0x50: branch(n, !(c->p & FV)); break; case 0x70: branch(n, c->p & FV); break;
    case 0x90: branch(n, !(c->p & FC)); break; case 0xB0: branch(n, c->p & FC); break;
    case 0xD0: branch(n, !(c->p & FZ)); break; case 0xF0: branch(n, c->p & FZ); break;
    /* ---- unofficial ---- */
    RMW7(0x07, 0x17, 0x0F, 0x1F, 0x1B, 0x03, 0x13, v = asl(c, v); c->a |= v; zn(c, c->a))          /* SLO */
    RMW7(0x27, 0x37, 0x2F, 0x3F, 0x3B, 0x23, 0x33, v = rol(c, v); c->a &= v; zn(c, c->a))          /* RLA */
    RMW7(0x47, 0x57, 0x4F, 0x5F, 0x5B, 0x43, 0x53, v = lsr(c, v); c->a ^= v; zn(c, c->a))          /* SRE */
    RMW7(0x67, 0x77, 0x6F, 0x7F, 0x7B, 0x63, 0x73, v = ror(c, v); adc(c, v))                       /* RRA */
    RMW7(0xC7, 0xD7, 0xCF, 0xDF, 0xDB, 0xC3, 0xD3, v--; cmp(c, c->a, v))                           /* DCP */
    RMW7(0xE7, 0xF7, 0xEF, 0xFF, 0xFB, 0xE3, 0xF3, v++; adc(c, (uint8_t)~v))                       /* ISC */
    WR(0x87, a_zp(n), c->a & c->x) WR(0x97, a_zpi(n, c->y), c->a & c->x)                            /* SAX */
    WR(0x8F, a_abs(n), c->a & c->x) WR(0x83, a_izx(n), c->a & c->x)
    RD(0xA7, a_zp(n), c->a = c->x = v; zn(c, v)) RD(0xB7, a_zpi(n, c->y), c->a = c->x = v; zn(c, v))  /* LAX */
    RD(0xAF, a_abs(n), c->a = c->x = v; zn(c, v)) RD(0xBF, a_abi(n, c->y, 0), c->a = c->x = v; zn(c, v))
    RD(0xA3, a_izx(n), c->a = c->x = v; zn(c, v)) RD(0xB3, a_izy(n, 0), c->a = c->x = v; zn(c, v))
    IM(0xAB, c->a = c->x = (uint8_t)((c->a | 0xFF) & v); zn(c, c->a))                              /* LXA */
    IM(0x0B, c->a &= v; zn(c, c->a); c->p = (uint8_t)((c->p & ~FC) | (c->a >> 7)))                 /* ANC */
    IM(0x2B, c->a &= v; zn(c, c->a); c->p = (uint8_t)((c->p & ~FC) | (c->a >> 7)))
    IM(0x4B, c->a = lsr(c, c->a & v))                                                              /* ALR */
    IM(0x6B, c->a = (uint8_t)(((c->a & v) >> 1) | (c->p & FC) << 7); zn(c, c->a);                  /* ARR */
           c->p = (uint8_t)((c->p & ~(FC | FV)) | ((c->a >> 6) & 1) | ((((c->a >> 6) ^ (c->a >> 5)) & 1) << 6)))
    IM(0x8B, c->a = (uint8_t)((c->a | 0xFF) & c->x & v); zn(c, c->a))                              /* ANE */
    IM(0xCB, { uint8_t t = c->a & c->x; c->p = (uint8_t)((c->p & ~FC) | (t >= v ? FC : 0)); c->x = (uint8_t)(t - v); zn(c, c->x); }) /* SBX */
    RD(0xBB, a_abi(n, c->y, 0), c->a = c->x = c->s = v & c->s; zn(c, c->a))                       /* LAS */
    case 0x9C: { uint16_t lo = fetch(n); c->dma_hit = 0; uint16_t b = (uint16_t)(lo | fetch(n) << 8);
                 rd(n, (b & 0xFF00) | ((b + c->x) & 0xFF)); shx(n, b, c->x, c->y); } break;
    case 0x9E: { uint16_t lo = fetch(n); c->dma_hit = 0; uint16_t b = (uint16_t)(lo | fetch(n) << 8);
                 rd(n, (b & 0xFF00) | ((b + c->y) & 0xFF)); shx(n, b, c->y, c->x); } break;
    case 0x9F: { uint16_t lo = fetch(n); c->dma_hit = 0; uint16_t b = (uint16_t)(lo | fetch(n) << 8);
                 rd(n, (b & 0xFF00) | ((b + c->y) & 0xFF)); shx(n, b, c->y, c->a & c->x); } break;
    case 0x9B: { uint16_t lo = fetch(n); c->dma_hit = 0; uint16_t b = (uint16_t)(lo | fetch(n) << 8);
                 rd(n, (b & 0xFF00) | ((b + c->y) & 0xFF)); c->s = c->a & c->x; shx(n, b, c->y, c->s); } break;
    case 0x93: { uint8_t z = fetch(n); uint16_t lo = rd(n, z); c->dma_hit = 0; uint16_t b = (uint16_t)(lo | rd(n, (uint8_t)(z + 1)) << 8);
                 rd(n, (b & 0xFF00) | ((b + c->y) & 0xFF)); shx(n, b, c->y, c->a & c->x); } break;
    /* unofficial NOPs (they still perform their reads) */
    IM(0x80, (void)v) IM(0x82, (void)v) IM(0x89, (void)v) IM(0xC2, (void)v) IM(0xE2, (void)v)
    RD(0x04, a_zp(n), (void)v) RD(0x44, a_zp(n), (void)v) RD(0x64, a_zp(n), (void)v) RD(0x0C, a_abs(n), (void)v)
    RD(0x14, a_zpi(n, c->x), (void)v) RD(0x34, a_zpi(n, c->x), (void)v) RD(0x54, a_zpi(n, c->x), (void)v)
    RD(0x74, a_zpi(n, c->x), (void)v) RD(0xD4, a_zpi(n, c->x), (void)v) RD(0xF4, a_zpi(n, c->x), (void)v)
    RD(0x1C, a_abi(n, c->x, 0), (void)v) RD(0x3C, a_abi(n, c->x, 0), (void)v) RD(0x5C, a_abi(n, c->x, 0), (void)v)
    RD(0x7C, a_abi(n, c->x, 0), (void)v) RD(0xDC, a_abi(n, c->x, 0), (void)v) RD(0xFC, a_abi(n, c->x, 0), (void)v)
    default: /* $02,$12,...: JAM - the CPU locks up until reset */
        rd(n, c->pc); c->jammed = 1; return;
    }
    if (c->prev_run_irq || c->prev_need_nmi) interrupt(n);
}
