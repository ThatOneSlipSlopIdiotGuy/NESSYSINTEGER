#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <math.h>
#include "nes.h"
/* Worst-case misread odds (of 65536) at the bottom of the power-off ramp. Calibrated so a game
 * that leaves its battery RAM writable loses a byte in roughly one unprotected power-off in 20. */
#define BROWN_MAX 40u

/* ------------------------------------------------------------ bus */
static uint8_t pad_bit(Nes *n, int port) {
    NesState *s = &n->s;
    uint8_t b = n->pad[port];
    if (!n->allow_opposite) { if ((b & 0x30) == 0x30) b &= ~0x30; if ((b & 0xC0) == 0xC0) b &= ~0xC0; }
    if (s->strobe) return b & 1;
    uint8_t r = s->pad_shift[port] & 1;
    s->pad_shift[port] = (uint8_t)(s->pad_shift[port] >> 1 | 0x80);   /* official pads return 1 after 8 reads */
    return r;
}

uint8_t cpu_bus_read(Nes *n, uint16_t a) {
    Cpu *c = &n->s.cpu;
    if (n->flat_mem) return c->bus = c->ibus = n->flat_mem[a];
    uint8_t val = 0xFF, drv = 0, cv = 0, cd;
    if (a < 0x2000) { val = n->s.ram[a & 0x7FF]; drv = 0xFF; }
    else if (a < 0x4000) { val = ppu_reg_read(n, a); drv = 0xFF; }
    else if (a < 0x4020 && a != 0x4016 && a != 0x4017) {
        /* 2A03 registers. $4015 is read onto the internal data bus only (bit 5 floats there);
         * the write-only ones drive nothing, so the read sees the external bus. The internal
         * bus follows every access; the external one every access except $4015 reads. */
        if (!n->conn.perfect) cart_cpu_read(n, a, &cv);
        if (a == 0x4015) return c->ibus = (uint8_t)((apu_status(n, 1) & 0xDF) | (c->ibus & 0x20));
        return c->ibus = c->bus;
    } else if (a == 0x4016 || a == 0x4017) {
        val = (uint8_t)(pad_bit(n, a & 1) | 0xE0); drv = 0x1F;
    }
    if (a >= 0x4020 || !n->conn.perfect) {
        cd = cart_cpu_read(n, a, &cv);
        if (cd) { val = (uint8_t)((val | (uint8_t)~drv) & (cv | (uint8_t)~cd)); drv |= cd; }  /* conflicts AND */
    }
    uint8_t r = (uint8_t)((val & drv) | (c->bus & ~drv));
    if (n->ngenie && (a & 0x8000))                        /* Game Genie sits between console and cart */
        for (int i = 0; i < n->ngenie; i++)
            if (n->genie[i].addr == a && (!n->genie[i].has_cmp || n->genie[i].cmp == r)) { r = n->genie[i].val; break; }
    if (n->brownout) {                                    /* power-off: marginal logic levels misread bits */
        uint32_t z = n->brown_rng; z ^= z << 13; z ^= z >> 17; z ^= z << 5; n->brown_rng = z;
        if ((z & 0xFFFF) < n->brown_p) r ^= (uint8_t)(1u << ((z >> 16) & 7));
    }
    c->bus = c->ibus = r;
    if (n->read_hook) n->read_hook(n, a, r);
    if (!n->conn.perfect) cart_cpu_read_post(n, a, r);
    return r;
}

void cpu_bus_write(Nes *n, uint16_t a, uint8_t v) {
    Cpu *c = &n->s.cpu;
    c->bus = c->ibus = v;
    if (n->flat_mem) { n->flat_mem[a] = v; return; }
    if (a < 0x2000) n->s.ram[a & 0x7FF] = v;
    else if (a < 0x4000) ppu_reg_write(n, a, v);
    else if (a < 0x4020) {
        if (a == 0x4014) { c->spr_dma = 1; c->spr_page = v; c->need_halt = 1; }
        else if (a == 0x4016) n->s.out_latch = v;     /* OUT0-2 reach the ports on the next get->put edge */
        else if (a <= 0x4017) apu_write(n, a, v);
    }
    if (a >= 0x2000 || !n->conn.perfect) cart_cpu_write(n, a, v);
}

/* the bit a controller port is presenting right now, without clocking it */
uint8_t nes_pad_peek(Nes *n, int port) {
    return (uint8_t)((n->s.strobe ? n->pad[port] : n->s.pad_shift[port]) & 1);
}

uint8_t nes_peek(Nes *n, uint16_t a) {
    CartState *cs = &n->s.cart;
    if (a < 0x2000) return n->s.ram[a & 0x7FF];
    if (a < 0x4000) return ppu_reg_peek(n, a);
    if (!n->has_rom) return 0;
    if (a >= 0x8000) return n->prg[(cs->prg_off[(a >> 13) & 3] + (a & 0x1FFF)) % n->prg_size];
    if (a >= 0x6000 && n->wram_size) return cs->wram[(cs->wram_off + (a & 0x1FFF)) % n->wram_size];
    return n->s.cpu.bus;
}

void nes_cpu_clock(Nes *n) {
    if (!(n->s.cpu.cycles & 1)) {             /* get -> put transition: the strobe line follows OUT0 */
        n->s.strobe = n->s.out_latch & 1;
        if (n->s.strobe) {
            n->s.pad_shift[0] = n->pad[0]; n->s.pad_shift[1] = n->pad[1];
            if (!n->allow_opposite)
                for (int i = 0; i < 2; i++) {
                    if ((n->s.pad_shift[i] & 0x30) == 0x30) n->s.pad_shift[i] &= (uint8_t)~0x30;
                    if ((n->s.pad_shift[i] & 0xC0) == 0xC0) n->s.pad_shift[i] &= (uint8_t)~0xC0;
                }
        }
    }
    cart_m2(n); apu_cycle(n);
}

/* debugger / cheat helpers: no side effects on the emulated bus */
void nes_poke(Nes *n, uint16_t a, uint8_t v) {
    CartState *cs = &n->s.cart;
    if (a < 0x2000) n->s.ram[a & 0x7FF] = v;
    else if (!n->has_rom) return;
    else if (a >= 0x8000) n->prg[(cs->prg_off[(a >> 13) & 3] + (a & 0x1FFF)) % n->prg_size] = v;   /* patches the ROM image */
    else if (a >= 0x6000 && n->wram_size) cs->wram[(cs->wram_off + (a & 0x1FFF)) % n->wram_size] = v;
}
uint8_t nes_ppu_peek(Nes *n, uint16_t a) {
    a &= 0x3FFF;
    if (a >= 0x3F00) { a &= 0x1F; if ((a & 0x13) == 0x10) a &= 0x0F; return n->s.ppu.pal[a]; }
    uint8_t *p = cart_vram_ptr(n, a);
    return p ? *p : 0;
}
void nes_ppu_poke(Nes *n, uint16_t a, uint8_t v) {
    a &= 0x3FFF;
    if (a >= 0x3F00) { a &= 0x1F; if ((a & 0x13) == 0x10) a &= 0x0F; n->s.ppu.pal[a] = v & 0x3F; return; }
    uint8_t *p = cart_vram_ptr(n, a);
    if (p) *p = v;                                         /* CHR-ROM included: patches the ROM image */
}
int nes_genie_decode(const char *code, uint16_t *addr, uint8_t *val, uint8_t *cmp, int *has_cmp) {
    static const char L[] = "APZLGITYEOXUKSVN";
    int d[8], len = 0;
    for (; *code && len < 9; code++) {
        char ch = *code; if (ch == '-' || ch == ' ') continue;
        if (ch >= 'a' && ch <= 'z') ch = (char)(ch - 32);
        const char *q = strchr(L, ch);
        if (!q || !ch) return 0;
        if (len == 8) return 0;
        d[len++] = (int)(q - L);
    }
    if (len != 6 && len != 8) return 0;
    *addr = (uint16_t)(0x8000 | ((d[3] & 7) << 12) | ((d[5] & 7) << 8) | ((d[4] & 8) << 8) |
                       ((d[2] & 7) << 4) | ((d[1] & 8) << 4) | (d[4] & 7) | (d[3] & 8));
    *val = (uint8_t)(((d[1] & 7) << 4) | ((d[0] & 8) << 4) | (d[0] & 7) | (d[len - 1] & 8));
    *has_cmp = len == 8;
    *cmp = len == 8 ? (uint8_t)(((d[7] & 7) << 4) | ((d[6] & 8) << 4) | (d[6] & 7) | (d[5] & 8)) : 0;
    return 1;
}
const char *nes_mapper_name(int m) {
    switch (m) {
    case 0: return "NROM"; case 1: return "MMC1 (SxROM)"; case 2: return "UxROM"; case 3: return "CNROM";
    case 4: return "MMC3 (TxROM)"; case 7: return "AxROM"; case 9: return "MMC2 (PxROM)"; case 10: return "MMC4 (FxROM)";
    case 11: return "Color Dreams"; case 34: return "BNROM/NINA-001"; case 66: return "GxROM"; case 71: return "Camerica";
    case 79: return "NINA-03/06"; case 87: return "Jaleco J87"; case 94: return "UN1ROM"; case 140: return "Jaleco J11";
    case 180: return "UNROM (AND)"; case 206: return "DxROM/Namco 108";
    case 5: return "MMC5 (ExROM)"; case 13: return "CPROM"; case 16: return "Bandai FCG"; case 18: return "Jaleco SS8806";
    case 19: return "Namco 163"; case 21: return "VRC4a/c"; case 22: return "VRC2a"; case 23: return "VRC2b/VRC4e/f";
    case 24: return "VRC6a"; case 25: return "VRC2c/VRC4b/d"; case 26: return "VRC6b"; case 32: return "Irem G-101";
    case 33: return "Taito TC0190"; case 40: return "NTDEC 2722"; case 142: return "Kaiser KS7032"; case 64: return "Tengen RAMBO-1"; case 65: return "Irem H-3001";
    case 67: return "Sunsoft-3"; case 68: return "Sunsoft-4"; case 69: return "Sunsoft FME-7/5B"; case 73: return "VRC3";
    case 75: return "VRC1"; case 76: return "Namco 3446"; case 78: return "Irem 74HC161"; case 80: return "Taito X1-005";
    case 82: return "Taito X1-017"; case 85: return "VRC7"; case 118: return "TxSROM"; case 119: return "TQROM";
    case 159: return "Bandai LZ93D50+24C01"; case 185: return "CNROM (protected)"; case 210: return "Namco 175/340";
    default: return "?";
    }
}
int nes_irq_line(Nes *n) {
    uint8_t s = n->s.cpu.irq_src;
    if (n->s.apu.fc.inhibit) s &= (uint8_t)~IRQ_FRAME;    /* the flag can be raised while inhibited, but never IRQs */
    return s || cart_irq(n);
}

/* ------------------------------------------------------------ lifecycle */
Nes *nes_create(void) {
    Nes *n = (Nes *)calloc(1, sizeof(Nes));
    conn_init(&n->conn);
    conn_update(n);
    n->ppu_offset = 1; n->exp_audio = 1; n->cic_enabled = 1; n->cic_key = KEY_LICENSED; n->led = 1; n->brown_max = BROWN_MAX; n->ram_init = 4; n->seed = 0x2545F491u; n->console_id = 0x4E455331u;
    n->was_powered = 1;
    return n;
}
void nes_destroy(Nes *n) { if (!n) return; free(n->prg); free(n->chr); free(n); }

static uint32_t crc32(const uint8_t *d, size_t len) {
    uint32_t c = 0xFFFFFFFFu;
    for (size_t i = 0; i < len; i++) { c ^= d[i]; for (int k = 0; k < 8; k++) c = (c >> 1) ^ (0xEDB88320u & (0u - (c & 1))); }
    return ~c;
}

static uint32_t mix32(uint32_t x) { x ^= x >> 16; x *= 0x7FEB352Du; x ^= x >> 15; x *= 0x846CA68Bu; x ^= x >> 16; return x; }

/* "Like a real console". An SRAM cell powers up to whichever value its transistor
 * mismatch favours, so a given chip shows much the same picture at every power-on,
 * typically runs of $00 and $FF with some cells settling the other way, while a few
 * cells are too balanced to call and come up differently each time. Every chip gets a
 * fingerprint (console RAM from the console's id, cartridge RAM from the cartridge);
 * every power-on re-rolls only the unstable cells. `ref`, if given, replaces the
 * $00/$FF runs as the base picture (used for the palette's real dump). */
void nes_fill_power_on(Nes *n, uint8_t *m, size_t len, uint32_t chip, const uint8_t *ref) {
    uint32_t owner = (chip & 0x100) ? n->rom_crc : n->console_id;
    uint32_t fp = mix32(owner ^ mix32(chip));
    uint32_t boot = mix32(n->seed += 0x9E3779B9u);
    size_t run = (size_t)4 << (fp & 3);                       /* $00/$FF run length: 4-32 bytes */
    int style = (int)((fp >> 2) & 3);                         /* stripes (x2), mostly $00, mostly $FF */
    uint8_t pol = ((fp >> 4) & 1) ? 0xFF : 0x00;
    for (size_t i = 0; i < len; i++) {
        uint8_t base = ref ? ref[i] : style < 2 ? (uint8_t)((((i / run) & 1) ? 0xFF : 0x00) ^ pol)
                                                 : style == 2 ? 0x00 : 0xFF;
        uint32_t a = mix32((uint32_t)i ^ fp), b = mix32((uint32_t)i * 0x9E3779B1u ^ fp ^ 0xA5A5A5A5u);
        uint8_t odd  = (uint8_t)(a & a >> 8 & a >> 16);                     /* 1 in 8 cells settle against it */
        uint8_t weak = (uint8_t)(b & b >> 8 & b >> 16 & b >> 24 & a >> 24); /* 1 in 32 are unstable */
        uint8_t coin = (uint8_t)mix32((uint32_t)i ^ boot);
        m[i] = (uint8_t)(((base ^ odd) & ~weak) | (coin & weak));
    }
}

static void fill_ram(Nes *n, uint8_t *m, size_t len, uint32_t chip) {
    if (n->ram_init == 4) { nes_fill_power_on(n, m, len, chip, NULL); return; }
    uint32_t r = (n->seed += 0x9E3779B9u) | 1;
    for (size_t i = 0; i < len; i++) {
        switch (n->ram_init) {
        case 1: m[i] = 0xFF; break;
        case 2: r ^= r << 13; r ^= r >> 17; r ^= r << 5; m[i] = (uint8_t)r; break;
        case 3: m[i] = (i & 4) ? 0xFF : 0x00; break;
        default: m[i] = 0x00; break;
        }
    }
}

int nes_load_rom(Nes *n, const uint8_t *d, size_t size, int hotswap, char *err, size_t errlen) {
    if (size < 16 || memcmp(d, "NES\x1A", 4)) { snprintf(err, errlen, "Not an iNES (.nes) file."); return 0; }
    int nes2 = (d[7] & 0x0C) == 0x08;
    int mapper = (d[6] >> 4) | (d[7] & 0xF0), sub = 0;
    uint32_t prg = d[4], chr = d[5];
    uint32_t wram = 0x2000, chrram = 0x2000;
    if (nes2) {
        mapper |= (d[8] & 0x0F) << 8; sub = d[8] >> 4;
        prg |= (uint32_t)(d[9] & 0x0F) << 8; chr |= (uint32_t)(d[9] & 0xF0) << 4;
        wram = 0;
        if (d[10] & 0x0F) wram += 64u << (d[10] & 0x0F);
        if (d[10] >> 4) wram += 64u << (d[10] >> 4);
        chrram = (d[11] & 0x0F) ? (64u << (d[11] & 0x0F)) : 0;
    } else if (!memcmp(d + 7, "DiskDude!", 9)) mapper &= 0x0F;
    prg *= 0x4000; chr *= 0x2000;
    size_t off = 16 + ((d[6] & 4) ? 512 : 0);
    if (!prg || off + prg + chr > size) { snprintf(err, errlen, "ROM file is truncated or has a bad header."); return 0; }
    int oldmapper = n->mapper; n->mapper = mapper;
    if (!cart_setup(n)) { n->mapper = oldmapper; snprintf(err, errlen, "Mapper %d is not supported yet.", mapper); return 0; }
    free(n->prg); free(n->chr);
    n->prg = (uint8_t *)malloc(prg); memcpy(n->prg, d + off, prg);
    n->chr = NULL; n->chr_size = chr;
    if (chr) { n->chr = (uint8_t *)malloc(chr); memcpy(n->chr, d + off + prg, chr); }
    n->prg_size = prg; n->submapper = sub;
    n->chr_is_ram = chr == 0;
    n->chr_ram_size = chr ? 0 : (chrram ? chrram : 0x2000);
    if (n->chr_ram_size > 0x8000) n->chr_ram_size = 0x8000;
    n->wram_size = wram > 0x10000 ? 0x10000 : wram;
    n->mirror_v = d[6] & 1; n->battery = (d[6] >> 1) & 1; n->four_screen = (d[6] >> 3) & 1;
    n->bus_conflicts = nes2 && sub == 2;
    n->rom_crc = crc32(d + 16, size - 16);
    n->has_rom = 1;
    CartState *cs = &n->s.cart;
    /* Battery RAM has been kept powered since the cartridge was made: a save that was never
     * written is blank (and the front end loads the .sav over it). Plain work RAM loses power
     * with the console, so it comes up with this chip's real power-on pattern. */
    if (n->battery) memset(cs->wram, 0, sizeof cs->wram);
    else fill_ram(n, cs->wram, sizeof cs->wram, 0x103);
    fill_ram(n, cs->chr_ram, sizeof cs->chr_ram, 0x104);
    if (d[6] & 4) memcpy(cs->wram + 0x1000, d + 16, 512);
    cart_power(n);
    if (!hotswap) nes_power(n);
    return 1;
}

/* Console model. NES front-loader (NES-001): the reset line also resets the PPU, so
 * $2000/$2001/$2005/$2006 writes are ignored until the first pre-render line after power
 * or reset. Famicom (HVC-001) and NES top-loader (NES-101): PPU /RST is tied to 5V, so
 * the PPU starts about a frame before the CPU at power-on (its warm-up is over when the
 * game starts) and the reset button resets only the CPU. Auto picks Famicom for board
 * families that were only ever made as Famicom cartridges. */
int nes_console_resolve(const Nes *n) {
    if (n->console_setting) return n->console_setting;
    switch (n->mapper) {
    case 16: case 19: case 20: case 21: case 22: case 23: case 24: case 25: case 26:
    case 40: case 73: case 75: case 80: case 82: case 85: case 159: case 210: return CON_FAMICOM;
    default: return CON_NES;
    }
}
const char *nes_console_name(int c) { return c == CON_FAMICOM ? "Famicom" : c == CON_NES101 ? "NES top-loader" : c == CON_NES ? "NES" : "Auto"; }

void nes_power(Nes *n) {
    memset(&n->s.cpu, 0, sizeof n->s.cpu);
    fill_ram(n, n->s.ram, sizeof n->s.ram, 1);
    fill_ram(n, n->s.ciram, sizeof n->s.ciram, 2);
    n->s.strobe = n->s.out_latch = 0; n->s.pad_shift[0] = n->s.pad_shift[1] = 0;
    if (n->has_rom) cart_power(n);
    n->console = nes_console_resolve(n);
    conn_update(n);                                /* the slot depends on the console model */
    ppu_power(n, 0);
    if (n->console != CON_NES) n->s.ppu.ignore_writes = 0;   /* PPU warmed up before the CPU started */
    apu_power(n, 0);
    /* master clocks: CPU starts one CPU-cycle in, plus the selected CPU/PPU phase alignment */
    n->s.cpu.cycles = (uint64_t)-1;
    n->s.cpu.mclk = 12 + (n->cpu_align & 3);
    n->s.ppu.mclk = 0;
    if (n->has_rom) cpu_power(n, 0);
    n->cic_timer = 0;
    n->lock_dead = n->lock_frames = n->lock_low = 0; n->in_reset = 0; n->reset_pulse = 0; n->led = 1;
    n->stun_ok = (int)((n->seed ^ n->console_id) % 100) < 85;
}

void nes_reset(Nes *n) {
    n->lock_dead = n->lock_frames = n->lock_low = 0;     /* the reset button also resets the 10NES lock */
    if (n->console == CON_NES) ppu_power(n, 1);   /* only the front-loader's reset line reaches the PPU */
    apu_power(n, 1);
    if (n->has_rom) cpu_power(n, 1);
}

static uint32_t xr(uint32_t *s) { uint32_t z = *s; z ^= z << 13; z ^= z >> 17; z ^= z << 5; return *s = z; }
static float frand(uint32_t *s) { return (float)(xr(s) % 100000u) / 100000.0f; }

/* One frame of the 10NES handshake. The lock and key exchange stream bits continuously;
 * a missing CIC contact, a key for another region, or no key at all fails at once, and an
 * intermittent contact garbles one of the ~16 bits a frame with the odds of its contact. */
static int cic_exchange_ok(Nes *n) {
    Connector *c = &n->conn;
    if (!c->powered) return 0;
    if (n->cic_key == KEY_OTHER_REGION || n->cic_key == KEY_NONE) return 0;
    if (n->cic_key == KEY_STUN) return n->stun_ok;     /* unlicensed carts zap the lock instead of answering it */
    float p = 1.0f;
    for (int i = 0; i < 4; i++) { if (!c->sig_ok[S_CIC0 + i]) return 0; p *= c->sig_p[S_CIC0 + i]; }
    return p >= 1.0f || frand(&c->rng) < powf(p, 16.0f);
}

void nes_run_frame(Nes *n) {
    n->audio_len = 0;
    if (!n->has_rom) return;
    Connector *c = &n->conn;
    if (c->powered && c->vcc_flaky) {              /* a power contact that only touches now and then */
        float p = c->sig_p[S_VCC] * c->sig_p[S_GND];
        if (frand(&c->rng) > powf(p, 4.0f)) cart_power(n);   /* the dip wipes the mapper's registers */
    }
    if (c->powered != n->was_powered) {            /* cart lost/regained power */
        if (c->powered) {
            cart_power(n);
            fill_ram(n, n->s.cart.chr_ram, sizeof n->s.cart.chr_ram, 0x104);
            if (!n->battery) fill_ram(n, n->s.cart.wram, sizeof n->s.cart.wram, 0x103);
        }
        n->was_powered = c->powered;
    }
    /* The reset button feeds the lock's reset input: releasing it restarts the handshake. */
    int btn = n->reset_button || n->reset_pulse > 0;
    if (n->reset_pulse > 0) n->reset_pulse--;
    if (n->btn_prev && !btn) { n->lock_dead = n->lock_frames = 0; n->stun_ok = (int)(xr(&c->rng) % 100) < 85; }
    n->btn_prev = btn;
    /* 10NES lock (NES-001 only). Once the handshake fails, the lock's die loop drives the
     * CPU/PPU reset line with a 1 Hz square wave until it is reset itself. */
    int lock = n->console == CON_NES && n->cic_enabled;
    if (lock && !btn && !n->lock_dead && !cic_exchange_ok(n)) { n->lock_dead = 1; n->lock_frames = 0; }
    n->lock_low = lock && n->lock_dead && ((n->lock_frames++ / 30) & 1) == 0;
    n->led = !n->lock_low;                          /* the power LED hangs off the lock's reset output */
    /* The reset line. On the NES-001 it also holds the PPU, which disables the picture;
     * on the Famicom and NES-101 only the CPU (and the 2A03's APU) is held. */
    int hold = btn || n->lock_low;
    if (hold && !n->in_reset) {
        n->in_reset = 1;
        apu_power(n, 1);
        if (n->console == CON_NES) {
            ppu_power(n, 1); n->s.ppu.held = 1;
            for (int i = 0; i < NES_W * NES_H; i++) n->fb[i] = 0x0F;
        }
    } else if (!hold && n->in_reset) {
        n->in_reset = 0;
        if (n->console == CON_NES) { n->s.ppu.held = 0; ppu_power(n, 1); }
        cpu_power(n, 1);                            /* the 6502 runs its reset sequence as the line goes high */
    }
    for (int i = 0; i < n->nfreeze; i++)                  /* RAM freeze ("Action Replay" style) */
        if (n->freeze[i].addr < 0x2000) n->s.ram[n->freeze[i].addr & 0x7FF] = n->freeze[i].val;
        else if (n->freeze[i].addr >= 0x6000 && n->freeze[i].addr < 0x8000) nes_poke(n, n->freeze[i].addr, n->freeze[i].val);
    n->s.ppu.frame_done = 0;
    uint64_t limit = n->s.cpu.cycles + 60000;
    if (n->in_reset && n->s.ppu.held) { uint64_t end = n->s.cpu.cycles + 29781; while (n->s.cpu.cycles < end) cpu_idle_cycle(n); }
    else if (n->in_reset) while (!n->s.ppu.frame_done && n->s.cpu.cycles < limit) cpu_idle_cycle(n);
    else while (!n->s.ppu.frame_done && n->s.cpu.cycles < limit) cpu_step(n);
    n->s.ppu.frame_done = 0;
}

/* Switching the power off. The 5 V rail takes tens of milliseconds to collapse: below about
 * 4.5 V the 2A03 misreads bits and runs off into the weeds, while battery-backed cartridge RAM
 * - on the falling rail, then on its battery - still accepts the writes that produces. Holding
 * RESET keeps the CPU parked, which is why the game manuals say to hold it while switching off. */
void nes_power_off(Nes *n) {
    if (!n->has_rom || n->in_reset || n->reset_button || n->reset_pulse > 0) return;
    n->brownout = 1;
    n->brown_rng = (n->seed ^ n->console_id ^ 0xB10C0FF5u) | 1u;
    uint64_t start = n->s.cpu.cycles, len = 36000;           /* ~20 ms of the falling edge */
    while (n->s.cpu.cycles - start < len) {
        double t = (double)(n->s.cpu.cycles - start) / (double)len;
        n->brown_p = (uint32_t)(t * t * n->brown_max);
        cpu_step(n);
    }
    n->brownout = 0;
}

/* ------------------------------------------------------------ save states */
#define STATE_MAGIC 0x31495353u  /* "SSI1" */
size_t nes_state_size(void) { return 12 + sizeof(NesState); }
void nes_save_state(Nes *n, uint8_t *buf) {
    uint32_t h[3] = { STATE_MAGIC, n->rom_crc, (uint32_t)sizeof(NesState) };
    memcpy(buf, h, 12);
    memcpy(buf + 12, &n->s, sizeof(NesState));
}
int nes_load_state(Nes *n, const uint8_t *buf, size_t len) {
    uint32_t h[3];
    if (len != nes_state_size()) return 0;
    memcpy(h, buf, 12);
    if (h[0] != STATE_MAGIC || h[2] != sizeof(NesState) || h[1] != n->rom_crc) return 0;
    memcpy(&n->s, buf + 12, sizeof(NesState));
    return 1;
}

/* ------------------------------------------------------------ NTSC palette
 * Generated by modelling the 2C02 composite signal (square wave between two
 * voltage levels, 12 phases per colour clock, emphasis attenuation) and
 * decoding it as an ideal NTSC TV would. Indices: colour | emphasis << 6. */
void nes_palette(uint32_t out[512]) {
    static const double lv[8] = { 0.350, 0.518, 0.962, 1.550, 1.094, 1.506, 1.962, 1.962 };
    const double black = 0.518, white = 1.962, att = 0.746, PI = 3.14159265358979;
    for (int px = 0; px < 512; px++) {
        int color = px & 15, level = color < 14 ? (px >> 4) & 3 : 1, emph = px >> 6;
        double lo = lv[level + (color == 0 ? 4 : 0)], hi = lv[level + (color < 13 ? 4 : 0)];
        double y = 0, i = 0, q = 0;
        for (int p = 0; p < 12; p++) {
            #define INPHASE(c) (((c) + p) % 12 < 6)
            double s = INPHASE(color) ? hi : lo;
            if (color == 0) s = hi; else if (color >= 13) s = lo;
            if (((emph & 1) && INPHASE(0)) || ((emph & 2) && INPHASE(4)) || ((emph & 4) && INPHASE(8))) s *= att;
            double v = (s - black) / (white - black) / 12.0;
            double ph = PI * (p + 3.9) / 6.0;
            y += v; i += v * cos(ph); q += v * sin(ph);
        }
        double rgb[3] = { y + 0.946882 * i + 0.623557 * q, y - 0.274788 * i - 0.635691 * q, y - 1.108545 * i + 1.709007 * q };
        uint32_t c = 0;
        for (int k = 0; k < 3; k++) {
            double g = rgb[k] <= 0 ? 0 : pow(rgb[k], 2.2 / 1.8);
            int b = (int)(g * 255.0 + 0.5); if (b > 255) b = 255; if (b < 0) b = 0;
            c = c << 8 | (uint32_t)b;
        }
        out[px] = c;
    }
}
