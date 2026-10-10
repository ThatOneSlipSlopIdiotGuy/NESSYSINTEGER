/* Cartridge = connector + board. Every console<->cart signal goes through the connector
 * model: the NES-001's 72-pin tray connector, the NES-101's vertical 72-pin slot, or the
 * Famicom's 60-pin slot. Each contact's quality comes from the cart's pose, the console
 * connector's pin springs, oxidation on both sides, loose dust and breath moisture. A pin
 * that loses contact makes the cart see a floating input, and makes the console see an
 * undriven line (CPU: open bus, PPU: the address low byte left on AD0-7, /IRQ: the
 * console's pull-up). Pinouts: nesdev wiki "Cartridge connector". */
#include <string.h>
#include <math.h>
#include "nes.h"
#include "board.h"

typedef struct { const char *name; int sig; } PinInfo;
#define CA(i) (S_CA0 + (i))
#define CD(i) (S_CD0 + (i))
#define PA(i) (S_PA0 + (i))
#define PD(i) (S_PD0 + (i))
/* NES 72-pin: pins 1-36 on the label side (pin 36 at the label's left end), pin n faces n+36 */
static const PinInfo PIN72[PINS + 1] = {
    {"", S_NONE},
    {"GND", S_GND}, {"CPU A11", CA(11)}, {"CPU A10", CA(10)}, {"CPU A9", CA(9)}, {"CPU A8", CA(8)}, {"CPU A7", CA(7)},
    {"CPU A6", CA(6)}, {"CPU A5", CA(5)}, {"CPU A4", CA(4)}, {"CPU A3", CA(3)}, {"CPU A2", CA(2)}, {"CPU A1", CA(1)},
    {"CPU A0", CA(0)}, {"CPU R/W", S_RW}, {"/IRQ", S_IRQ}, {"EXP 0", S_EXP0}, {"EXP 1", S_EXP0 + 1}, {"EXP 2", S_EXP0 + 2},
    {"EXP 3", S_EXP0 + 3}, {"EXP 4", S_EXP0 + 4}, {"PPU /RD", S_RD}, {"CIRAM A10", S_CIA10}, {"PPU A6", PA(6)},
    {"PPU A5", PA(5)}, {"PPU A4", PA(4)}, {"PPU A3", PA(3)}, {"PPU A2", PA(2)}, {"PPU A1", PA(1)}, {"PPU A0", PA(0)},
    {"PPU D0", PD(0)}, {"PPU D1", PD(1)}, {"PPU D2", PD(2)}, {"PPU D3", PD(3)}, {"CIC toPak", S_CIC0}, {"CIC toMB", S_CIC0 + 1},
    {"+5V", S_VCC},
    {"SYS CLK", S_CLK}, {"M2", S_M2}, {"CPU A12", CA(12)}, {"CPU A13", CA(13)}, {"CPU A14", CA(14)}, {"CPU D7", CD(7)},
    {"CPU D6", CD(6)}, {"CPU D5", CD(5)}, {"CPU D4", CD(4)}, {"CPU D3", CD(3)}, {"CPU D2", CD(2)}, {"CPU D1", CD(1)},
    {"CPU D0", CD(0)}, {"/ROMSEL", S_ROMSEL}, {"EXP 9", S_EXP0 + 9}, {"EXP 8", S_EXP0 + 8}, {"EXP 7", S_EXP0 + 7},
    {"EXP 6", S_EXP0 + 6}, {"EXP 5", S_EXP0 + 5}, {"PPU /WR", S_WR}, {"CIRAM /CE", S_CICE}, {"PPU /A13", S_NA13},
    {"PPU A7", PA(7)}, {"PPU A8", PA(8)}, {"PPU A9", PA(9)}, {"PPU A11", PA(11)}, {"PPU A10", PA(10)}, {"PPU A12", PA(12)},
    {"PPU A13", PA(13)}, {"PPU D7", PD(7)}, {"PPU D6", PD(6)}, {"PPU D5", PD(5)}, {"PPU D4", PD(4)}, {"CIC +RST", S_CIC0 + 2},
    {"CIC CLK", S_CIC0 + 3}, {"GND", S_GND},
};
/* Famicom 60-pin: pins 1-30 on the label side, left to right; pin n faces n+30. Pins 45/46
 * carry the console's own audio through the cartridge (plain carts bridge them). */
static const PinInfo PIN60[61] = {
    {"", S_NONE},
    {"GND", S_GND}, {"CPU A11", CA(11)}, {"CPU A10", CA(10)}, {"CPU A9", CA(9)}, {"CPU A8", CA(8)}, {"CPU A7", CA(7)},
    {"CPU A6", CA(6)}, {"CPU A5", CA(5)}, {"CPU A4", CA(4)}, {"CPU A3", CA(3)}, {"CPU A2", CA(2)}, {"CPU A1", CA(1)},
    {"CPU A0", CA(0)}, {"CPU R/W", S_RW}, {"/IRQ", S_IRQ}, {"GND", S_GND}, {"PPU /RD", S_RD}, {"CIRAM A10", S_CIA10},
    {"PPU A6", PA(6)}, {"PPU A5", PA(5)}, {"PPU A4", PA(4)}, {"PPU A3", PA(3)}, {"PPU A2", PA(2)}, {"PPU A1", PA(1)},
    {"PPU A0", PA(0)}, {"PPU D0", PD(0)}, {"PPU D1", PD(1)}, {"PPU D2", PD(2)}, {"PPU D3", PD(3)}, {"+5V", S_VCC},
    {"+5V / cart present", S_VCC}, {"M2", S_M2}, {"CPU A12", CA(12)}, {"CPU A13", CA(13)}, {"CPU A14", CA(14)},
    {"CPU D7", CD(7)}, {"CPU D6", CD(6)}, {"CPU D5", CD(5)}, {"CPU D4", CD(4)}, {"CPU D3", CD(3)}, {"CPU D2", CD(2)},
    {"CPU D1", CD(1)}, {"CPU D0", CD(0)}, {"/ROMSEL", S_ROMSEL}, {"Audio from 2A03", S_AIN}, {"Audio to RF", S_AOUT},
    {"PPU /WR", S_WR}, {"CIRAM /CE", S_CICE}, {"PPU /A13", S_NA13}, {"PPU A7", PA(7)}, {"PPU A8", PA(8)}, {"PPU A9", PA(9)},
    {"PPU A10", PA(10)}, {"PPU A11", PA(11)}, {"PPU A12", PA(12)}, {"PPU A13", PA(13)}, {"PPU D7", PD(7)}, {"PPU D6", PD(6)},
    {"PPU D5", PD(5)}, {"PPU D4", PD(4)},
};
static const PinInfo *pin_info(const Connector *c, int p) {
    static const PinInfo none = { "", S_NONE };
    static const PinInfo top[4] = { { "(no CIC - floats)", S_NONE }, { "(no CIC - floats)", S_NONE },
                                    { "(+5V to the cart's CIC)", S_NONE }, { "(PPU D4 to the cart's CIC)", S_NONE } };
    if (p < 1 || p > c->npins) return &none;
    if (c->slot == SLOT_FC) return &PIN60[p];
    if (c->slot == SLOT_NES_TOP && (p == 34 || p == 35 || p == 70 || p == 71))   /* NES-101 has no lockout chip */
        return &top[p == 34 ? 0 : p == 35 ? 1 : p == 70 ? 2 : 3];
    return &PIN72[p];
}
const char *conn_pin_name(const Connector *c, int pin) { return pin_info(c, pin)->name; }
int conn_pin_signal(const Connector *c, int pin) { return pin_info(c, pin)->sig; }
int conn_slot_for(int console) { return console == CON_FAMICOM ? SLOT_FC : console == CON_NES101 ? SLOT_NES_TOP : SLOT_NES; }
/* where a contact sits as you face the label: row 0 = label side; x = 0 at the left end */
void conn_pin_geom(const Connector *c, int p, int *row, float *x) {
    if (c->slot == SLOT_FC) { *row = p > 30; *x = (float)(*row ? p - 31 : p - 1) / 29.0f; }
    else { *row = p > 36; *x = (float)(*row ? 72 - p : 36 - p) / 35.0f; }
}

static uint32_t rnd(Connector *c) { c->rng ^= c->rng << 13; c->rng ^= c->rng >> 17; c->rng ^= c->rng << 5; return c->rng; }

void conn_reseat(Connector *c) {
    c->seat = 1; c->roll = 0; c->pitch = 0; c->dirt = 0; c->tray = 1; c->press = 0;
    memset(c->mode, PIN_AUTO, sizeof c->mode);
}
void conn_init(Connector *c) {
    memset(c, 0, sizeof *c);
    c->rng = 0x9E3779B9u;
    for (int i = 1; i <= PINS; i++) { c->tol[i] = ((float)(rnd(c) % 1000) / 1000.0f - 0.5f) * 0.16f; c->tension[i] = 1.0f; }
    conn_reseat(c);
    c->float_mode = FLOAT_REAL;
    c->slot = SLOT_NES; c->npins = 72;
}
/* Connector condition presets (pin springs + oxidation on the console side). The NES-001
 * tray presses the cart into the label-side row, so that row's springs wear first. */
void conn_set_condition(Connector *c, int cond, uint32_t seed) {
    uint32_t r = seed | 1u;
    for (int p = 1; p <= PINS; p++) {
        r ^= r << 13; r ^= r >> 17; r ^= r << 5;
        float u = (float)(r % 1000) / 1000.0f;
        int top = c->slot == SLOT_FC ? p <= 30 : p <= 36;
        switch (cond) {
        case COND_NEW:         c->tension[p] = 1.00f + 0.04f * u; c->cgrime[p] = 0.0f; break;
        case COND_REFURB:      c->tension[p] = 0.95f + 0.08f * u; c->cgrime[p] = 0.02f * u; break;
        case COND_AFTERMARKET: c->tension[p] = 1.80f + 0.15f * u; c->cgrime[p] = 0.0f; break;
        case COND_USED:        c->tension[p] = (top ? 0.80f : 0.88f) + 0.14f * u; c->cgrime[p] = 0.04f + 0.12f * u; break;
        default:               c->tension[p] = (top ? 0.50f : 0.62f) + 0.18f * u; c->cgrime[p] = 0.10f + 0.25f * u; break;
        }
    }
}
void conn_wear_pushdown(Connector *c) {      /* every push-down bends the springs a little, the label-side row most */
    for (int p = 1; p <= PINS; p++) {
        float w = (p <= 36 ? 0.0015f : 0.0005f) * (0.5f + (float)(rnd(c) % 1000) / 1000.0f);
        if (c->tension[p] > 0.25f) c->tension[p] -= w;
    }
}

static void build_masks(Connector *c) {
    c->ca_mask = c->pa_mask = 0; c->cd_mask = c->pd_mask = 0;
    for (int i = 0; i < 15; i++) if (c->sig_ok[CA(i)]) c->ca_mask |= (uint16_t)(1 << i);
    for (int i = 0; i < 14; i++) if (c->sig_ok[PA(i)]) c->pa_mask |= (uint16_t)(1 << i);
    for (int i = 0; i < 8; i++) { if (c->sig_ok[CD(i)]) c->cd_mask |= (uint8_t)(1 << i); if (c->sig_ok[PD(i)]) c->pd_mask |= (uint8_t)(1 << i); }
}

/* Contact quality: spring force pressing the finger onto the pin, minus how far the pose
 * lifts it away, minus the insulating film (oxidation, grime, dust; breath moisture
 * conducts through it for a while). Solid above Q_SOLID, open below Q_OPEN, and in
 * between an intermittent contact whose chance of passing an access rises with quality. */
#define Q_OPEN 0.36f
#define Q_SOLID 0.58f
void conn_update(Nes *n) {
    Connector *c = &n->conn;
    c->slot = (uint8_t)conn_slot_for(nes_console_resolve(n));
    c->npins = (uint8_t)(c->slot == SLOT_FC ? 60 : 72);
    int gnd = 0, vcc = 0;
    float gp = 0, vp = 0;
    memset(c->sig_ok, 0, sizeof c->sig_ok);
    memset(c->sig_p, 0, sizeof c->sig_p);
    memset(c->nb, 0, sizeof c->nb);
    c->nflaky = 0;
    float hold = c->slot == SLOT_NES ? (c->tray > 0.5f ? 0.45f : 1.0f) : 0.8f;     /* the latched tray clamps tilting */
    float engage = c->slot == SLOT_NES ? 0.40f + 0.60f * c->tray : 1.0f;            /* pushing the tray down engages the pins */
    float insert = (1.0f - c->seat) * 2.2f;
    for (int p = 1; p <= PINS; p++) { c->conn[p] = c->flaky[p] = 0; c->flaky_p[p] = c->quality[p] = 0; }
    for (int p = 1; p <= c->npins; p++) {
        int row; float x; conn_pin_geom(c, p, &row, &x);
        float lift = insert + hold * (c->roll > 0 ? c->roll * x : -c->roll * (1.0f - x)) +
                     hold * 0.6f * (row == 0 ? (c->pitch > 0 ? c->pitch : 0) : (c->pitch < 0 ? -c->pitch : 0));
        float force = (engage + 0.3f * c->press) * c->tension[p] - lift;
        float film = c->cgrime[p] + c->fgrime[p] + 0.9f * c->dirt - 0.6f * c->moist;
        if (film < 0) film = 0;
        float q = force - 0.5f * film + c->tol[p];
        int on = q >= Q_OPEN, fl = on && q < Q_SOLID;
        float fp = fl ? 0.08f + 0.92f * powf((q - Q_OPEN) / (Q_SOLID - Q_OPEN), 1.5f) : (on ? 1.0f : 0.0f);
        switch (c->mode[p]) {
        case PIN_OFF: on = 0; fl = 0; fp = 0; break;
        case PIN_ON: on = 1; fl = 0; fp = 1; break;
        case PIN_FLAKY: on = 1; fl = 1; fp = 0.5f; break;
        }
        c->quality[p] = q;
        c->conn[p] = (uint8_t)on; c->flaky[p] = (uint8_t)fl; c->flaky_p[p] = fp;
        int sig = pin_info(c, p)->sig;
        if (sig == S_GND) { if (on) gnd = 1; if (fp > gp) gp = fp; }
        else if (sig == S_VCC) { if (on) vcc = 1; if (fp > vp) vp = fp; }
        else if (sig != S_NONE && on) {
            if (fl) c->flaky_list[c->nflaky++] = (uint8_t)p;
            c->sig_ok[sig] = 1;
            if (fp > c->sig_p[sig]) c->sig_p[sig] = fp;
        }
        /* neighbours on the same side of the edge, for capacitive coupling of floating lines */
        for (int d = -1; d <= 1; d += 2) {
            int q2 = p + d, r2; float x2;
            if (sig <= S_VCC || q2 < 1 || q2 > c->npins) continue;
            conn_pin_geom(c, q2, &r2, &x2);
            if (r2 == row) c->nb[sig][d > 0] = (uint8_t)pin_info(c, q2)->sig;
        }
    }
    c->powered = (uint8_t)(gnd && vcc);
    c->sig_p[S_GND] = gp; c->sig_p[S_VCC] = vp;
    c->vcc_flaky = (uint8_t)(c->powered && (gp < 1.0f || vp < 1.0f));
    c->cic_ok = (uint8_t)(c->sig_ok[S_CIC0] && c->sig_ok[S_CIC0 + 1] && c->sig_ok[S_CIC0 + 2] && c->sig_ok[S_CIC0 + 3]);
    build_masks(c);
    int all = c->powered && !c->vcc_flaky && c->nflaky == 0 && c->ca_mask == 0x7FFF && c->pa_mask == 0x3FFF && c->cd_mask == 0xFF &&
              c->pd_mask == 0xFF && c->sig_ok[S_RW] && c->sig_ok[S_ROMSEL] && c->sig_ok[S_M2] && c->sig_ok[S_IRQ] &&
              c->sig_ok[S_RD] && c->sig_ok[S_WR] && c->sig_ok[S_NA13] && c->sig_ok[S_CICE] && c->sig_ok[S_CIA10];
    c->perfect = (uint8_t)all;
}

static void sample(Connector *c) {   /* intermittent contacts flicker on every access */
    if (!c->nflaky) return;
    for (int i = 0; i < c->nflaky; i++) {
        int p = c->flaky_list[i];
        c->sig_ok[pin_info(c, p)->sig] = (rnd(c) % 1000) < (uint32_t)(c->flaky_p[p] * 1000);
    }
    build_masks(c);
}
/* "Realistic" floating input: a CMOS input keeps the charge it last saw, which slowly
 * leaks, and a strongly driven neighbouring contact couples into it. M2's neighbour on
 * the NES connector is the 21 MHz system clock, so a floating M2 picks up clock noise. */
static int known_value(Connector *c, int ns) {
    if (ns == S_CLK) return (int)(rnd(c) & 1);
    if (ns >= S_CA0 && ns < S_CA0 + 15) return c->sig_ok[ns] ? (c->ctx_ca >> (ns - S_CA0)) & 1 : -1;
    if (ns >= S_PA0 && ns < S_PA0 + 14) return c->sig_ok[ns] ? (c->ctx_pa >> (ns - S_PA0)) & 1 : -1;
    if (ns == S_M2) return c->sig_ok[ns] ? 1 : -1;
    return -1;
}
static int real_bit(Connector *c, int sig, int held) {
    uint32_t r = rnd(c);
    if ((r & 0xFF) == 0) return held ^ 1;           /* the stored charge leaks away */
    if (r & 0x100) return held;
    int v = known_value(c, c->nb[sig][(r >> 9) & 1]);
    return v < 0 ? held : v;
}
static uint32_t flt(Connector *c, uint32_t actual, uint32_t mask, uint32_t width_mask, uint32_t *hold, int base) {
    uint32_t f = 0;
    switch (c->float_mode) {
    case FLOAT_LOW: f = 0; break;
    case FLOAT_NOISE: f = rnd(c); break;
    case FLOAT_HOLD: f = *hold; break;
    case FLOAT_REAL:
        for (int i = 0; (width_mask >> i) != 0; i++)
            if (!((mask >> i) & 1)) f |= (uint32_t)real_bit(c, base + i, (int)((*hold >> i) & 1)) << i;
        break;
    default: f = 0xFFFFFFFFu; break;
    }
    uint32_t r = ((actual & mask) | (f & ~mask)) & width_mask;
    *hold = r;
    return r;
}
static int line(Connector *c, int sig, int actual) {
    if (c->sig_ok[sig]) { c->hold_ctl[sig] = (uint8_t)actual; return actual; }
    uint32_t h = c->hold_ctl[sig];
    int r = c->float_mode == FLOAT_REAL ? real_bit(c, sig, (int)(h & 1)) : (int)flt(c, (uint32_t)actual, 0, 1, &h, sig);
    c->hold_ctl[sig] = (uint8_t)r;
    return r;
}
static uint16_t cpu_addr_seen(Connector *c, uint16_t a) { c->ctx_ca = a; uint32_t h = c->hold_ca; uint16_t r = (uint16_t)flt(c, a, c->ca_mask, 0x7FFF, &h, S_CA0); c->hold_ca = (uint16_t)h; return r; }
static uint16_t ppu_addr_seen(Connector *c, uint16_t a) { c->ctx_pa = a; uint32_t h = c->hold_pa; uint16_t r = (uint16_t)flt(c, a, c->pa_mask, 0x3FFF, &h, S_PA0); c->hold_pa = (uint16_t)h; return r; }

/* ======================= boards / mappers ======================= */
void prg8(Nes *n, int slot, int bank) { CS; uint32_t nb = n->prg_size / 0x2000; cs->prg_off[slot] = (uint32_t)(((unsigned)bank % nb) * 0x2000); }
void prg16(Nes *n, int slot, int bank) { prg8(n, slot, bank * 2); prg8(n, slot + 1, bank * 2 + 1); }
void prg32(Nes *n, int bank) { prg16(n, 0, bank * 2); prg16(n, 2, bank * 2 + 1); }
uint32_t chr_total(Nes *n) { return n->chr_is_ram ? n->chr_ram_size : n->chr_size; }
void chr1(Nes *n, int slot, int bank) { CS; uint32_t nb = chr_total(n) / 0x400; if (!nb) nb = 8; cs->chr_off[slot] = (uint32_t)(((unsigned)bank % nb) * 0x400); cs->chr_src &= (uint8_t)~(1u << slot); }
void chr1_ram(Nes *n, int slot, int bank) { CS; uint32_t nb = (n->chr_ram_size ? n->chr_ram_size : 0x2000) / 0x400; cs->chr_off[slot] = (uint32_t)(((unsigned)bank % nb) * 0x400); cs->chr_src |= (uint8_t)(1u << slot); }
int nprg8(Nes *n) { return (int)(n->prg_size / 0x2000); }
uint8_t prg_read(Nes *n, uint16_t a) { CS; return n->prg[(cs->prg_off[(a >> 13) & 3] + (a & 0x1FFF)) % n->prg_size]; }
uint8_t prg_abs(Nes *n, uint32_t off) { return n->prg[off % n->prg_size]; }
uint8_t chr_abs(Nes *n, uint32_t off) { return n->chr_size ? n->chr[off % n->chr_size] : n->s.cart.chr_ram[off % (n->chr_ram_size ? n->chr_ram_size : 0x2000)]; }
void chr2(Nes *n, int slot, int bank) { chr1(n, slot, bank * 2); chr1(n, slot + 1, bank * 2 + 1); }
void chr4(Nes *n, int slot, int bank) { for (int i = 0; i < 4; i++) chr1(n, slot + i, bank * 4 + i); }
void chr8(Nes *n, int bank) { chr4(n, 0, bank * 2); chr4(n, 4, bank * 2 + 1); }
void mirror(Nes *n, int m) {
    static const uint8_t T[5][4] = { {0,0,1,1}, {0,1,0,1}, {0,0,0,0}, {1,1,1,1}, {2,3,4,5} };
    if (n->four_screen) m = MIR_4;
    memcpy(n->s.cart.nt_map, T[m], 4);
}

static void update(Nes *n) {
    CS; uint8_t *r = cs->regs;
    int last16 = (int)(n->prg_size / 0x4000) - 1;
    switch (n->mapper) {
    case 1: {
        int ctrl = r[0], prgsel = r[3] & 15, hi = (n->prg_size > 0x40000) ? (r[1] & 0x10) : 0;
        static const int M[4] = { MIR_A, MIR_B, MIR_V, MIR_H };
        mirror(n, M[ctrl & 3]);
        int base = hi ? 16 : 0, lastb = hi ? 31 : (last16 > 15 ? 15 : last16);
        switch ((ctrl >> 2) & 3) {
        case 0: case 1: prg16(n, 0, base + (prgsel & 0xE)); prg16(n, 2, base + (prgsel | 1)); break;
        case 2: prg16(n, 0, base); prg16(n, 2, base + prgsel); break;
        case 3: prg16(n, 0, base + prgsel); prg16(n, 2, lastb); break;
        }
        if (ctrl & 0x10) { chr4(n, 0, r[1]); chr4(n, 4, r[2]); } else chr8(n, r[1] >> 1);
        cs->wram_on = !(r[3] & 0x10);
        cs->wram_off = (n->wram_size > 0x2000) ? (uint32_t)(((r[1] >> 2) & 3) * 0x2000) % n->wram_size : 0;
    } break;
    case 2: prg16(n, 0, r[0]); prg16(n, 2, last16); chr8(n, 0); break;
    case 94: prg16(n, 0, r[0]); prg16(n, 2, last16); chr8(n, 0); break;
    case 180: prg16(n, 0, 0); prg16(n, 2, r[0]); chr8(n, 0); break;
    case 71: prg16(n, 0, r[0]); prg16(n, 2, last16); chr8(n, 0); if (n->submapper == 1) mirror(n, (r[1] & 0x10) ? MIR_B : MIR_A); break;
    case 3: case 87: prg32(n, 0); chr8(n, r[0]); break;
    case 7: prg32(n, r[0] & 15); chr8(n, 0); mirror(n, (r[0] & 0x10) ? MIR_B : MIR_A); break;
    case 11: prg32(n, r[0] & 3); chr8(n, r[0] >> 4); break;
    case 66: prg32(n, (r[0] >> 4) & 3); chr8(n, r[0] & 3); break;
    case 140: prg32(n, (r[0] >> 4) & 3); chr8(n, r[0] & 15); break;
    case 79: prg32(n, (r[0] >> 3) & 1); chr8(n, r[0] & 7); break;
    case 34:
        prg32(n, r[0]);
        if (chr_total(n) > 0x2000) { chr4(n, 0, r[1]); chr4(n, 4, r[2]); } else chr8(n, 0);
        break;
    case 76: {   /* Namco 3446: Namco 108 with 2 KB CHR banks from R2-R5 */
        int nb8 = (int)(n->prg_size / 0x2000);
        chr2(n, 0, r[3]); chr2(n, 2, r[4]); chr2(n, 4, r[5]); chr2(n, 6, r[6]);
        prg8(n, 0, r[7]); prg8(n, 1, r[8]); prg8(n, 2, nb8 - 2); prg8(n, 3, nb8 - 1);
    } break;
    case 4: case 206: case 118: case 119: {
        int inv = (r[0] & 0x80) ? 4 : 0, nb8 = (int)(n->prg_size / 0x2000);
        int bk[8] = { r[1] & 0xFE, r[1] | 1, r[2] & 0xFE, r[2] | 1, r[3], r[4], r[5], r[6] };   /* 1 KB banks, slot order before inversion */
        for (int i = 0; i < 8; i++) {
            if (n->mapper == 119 && (bk[i] & 0x40)) chr1_ram(n, i ^ inv, bk[i] & 7);   /* TQROM: bit 6 selects the 8 KB CHR-RAM */
            else chr1(n, i ^ inv, n->mapper == 119 ? (bk[i] & 0x3F) : bk[i]);
        }
        if (n->mapper == 118) for (int k = 0; k < 4; k++) cs->nt_map[k] = (uint8_t)((bk[k] >> 7) & 1);   /* TxSROM: CHR A17 drives CIRAM A10 */
        if (n->mapper == 206) r[0] &= 0x07;
        if (r[0] & 0x40) { prg8(n, 0, nb8 - 2); prg8(n, 2, r[7]); } else { prg8(n, 0, r[7]); prg8(n, 2, nb8 - 2); }
        prg8(n, 1, r[8]); prg8(n, 3, nb8 - 1);
        if (n->mapper == 4 || n->mapper == 119) mirror(n, (r[9] & 1) ? MIR_H : MIR_V);
        if (n->mapper != 206) { cs->wram_on = (r[10] & 0x80) != 0; cs->wram_wp = (r[10] & 0x40) != 0; }
    } break;
    case 9: case 10:
        if (n->mapper == 9) { prg8(n, 0, r[0]); prg8(n, 1, (int)(n->prg_size / 0x2000) - 3); prg16(n, 2, last16); }
        else { prg16(n, 0, r[0]); prg16(n, 2, last16); }
        chr4(n, 0, cs->latch0 ? r[2] : r[1]); chr4(n, 4, cs->latch1 ? r[4] : r[3]);
        mirror(n, (r[5] & 1) ? MIR_H : MIR_V);
        break;
    default: /* 0 */
        prg16(n, 0, 0); prg16(n, 2, last16 > 0 ? 1 : 0); chr8(n, 0); break;
    }
}

int cart_setup(Nes *n) {
    static const int OK[] = { 0, 1, 2, 3, 4, 7, 9, 10, 11, 34, 66, 71, 76, 79, 87, 94, 118, 119, 140, 180, 206, -1 };
    n->board = board_find(n->mapper);
    if (n->board) return 1;
    for (int i = 0; OK[i] >= 0; i++) if (OK[i] == n->mapper) return 1;
    return 0;
}

void cart_power(Nes *n) {
    CS;
    memset(cs->regs, 0, sizeof cs->regs);
    cs->irq_latch = cs->irq_ctr = cs->irq_reload = cs->irq_en = cs->irq_line = 0;
    cs->latch0 = cs->latch1 = 1; cs->shift = 0; cs->shift_n = 0; cs->last_write = 0;
    cs->wram_on = 1; cs->wram_wp = 0; cs->wram_off = 0; cs->a12 = 0; cs->a12_low = 0;
    if (n->mapper == 1) cs->regs[0] = 0x0C;
    if (n->mapper == 4 || n->mapper == 118 || n->mapper == 119) { cs->regs[7] = 0; cs->regs[8] = 1; }
    cs->chr_src = 0;
    memset(cs->mreg, 0, sizeof cs->mreg); memset(cs->mw, 0, sizeof cs->mw); memset(cs->mc, 0, sizeof cs->mc);
    memset(&cs->vrc6, 0, sizeof cs->vrc6); memset(&cs->vrc7, 0, sizeof cs->vrc7); memset(&cs->s5b, 0, sizeof cs->s5b);
    memset(&cs->n163, 0, sizeof cs->n163); memset(&cs->mmc5snd, 0, sizeof cs->mmc5snd); memset(&cs->fdssnd, 0, sizeof cs->fdssnd);
    mirror(n, n->mirror_v ? MIR_V : MIR_H);
    if (n->mapper == 119 && !n->chr_ram_size) n->chr_ram_size = 0x2000;
    if (n->mapper == 4 && n->submapper == 1) n->wram_size = 0x400;     /* MMC6: 1 KB inside the chip */
    if (n->board) { if (n->chr_is_ram) for (int i = 0; i < 8; i++) chr1(n, i, i); prg32(n, 0); if (n->board->power) n->board->power(n); return; }
    update(n);
}

static uint8_t *chr_ptr(Nes *n, uint16_t a) {
    CS; int slot = (a >> 10) & 7; uint32_t off = cs->chr_off[slot] + (a & 0x3FF);
    if (n->chr_is_ram || ((cs->chr_src >> slot) & 1)) return &cs->chr_ram[off % (n->chr_ram_size ? n->chr_ram_size : 0x2000)];
    return &n->chr[off % n->chr_size];
}
static int chr_writable(Nes *n, uint16_t a) { return n->chr_is_ram || ((n->s.cart.chr_src >> ((a >> 10) & 7)) & 1); }
uint8_t cart_chr_peek(Nes *n, uint16_t a) { return n->has_rom ? *chr_ptr(n, a & 0x1FFF) : 0; }
/* the same, through a recorded set of 1 KB windows (the PPU viewer's per-scanline banks) */
uint8_t cart_chr_peek_banks(Nes *n, uint16_t a, const uint32_t off[8], uint8_t src) {
    if (!n->has_rom) return 0;
    int slot = (a >> 10) & 7; uint32_t o = off[slot] + (a & 0x3FF);
    if (n->chr_is_ram || ((src >> slot) & 1)) return n->s.cart.chr_ram[o % (n->chr_ram_size ? n->chr_ram_size : 0x2000)];
    return n->chr_size ? n->chr[o % n->chr_size] : 0;
}
static uint8_t *nt_ptr(Nes *n, int page, uint16_t a);
/* direct pointer into whatever memory backs a PPU address (debugger use) */
uint8_t *cart_vram_ptr(Nes *n, uint16_t a) {
    a &= 0x3FFF;
    if (!n->has_rom) return NULL;
    if (a < 0x2000) return chr_ptr(n, a);
    return nt_ptr(n, n->s.cart.nt_map[(a >> 10) & 3], a);
}

static void mmc2_latch(Nes *n, uint16_t a) {
    CS;
    if (n->mapper != 9 && n->mapper != 10) return;
    int ch = 0;
    if ((n->mapper == 9 ? a == 0x0FD8 : (a & 0xFFF8) == 0x0FD8)) { cs->latch0 = 0; ch = 1; }
    else if ((n->mapper == 9 ? a == 0x0FE8 : (a & 0xFFF8) == 0x0FE8)) { cs->latch0 = 1; ch = 1; }
    else if ((a & 0xFFF8) == 0x1FD8) { cs->latch1 = 0; ch = 1; }
    else if ((a & 0xFFF8) == 0x1FE8) { cs->latch1 = 1; ch = 1; }
    if (ch) update(n);
}

static int mmc3_irq_board(Nes *n) { return n->mapper == 4 || n->mapper == 118 || n->mapper == 119; }
static void mmc3_clock(Nes *n) {
    CS;
    if (cs->irq_ctr == 0 || cs->irq_reload) { cs->irq_ctr = cs->irq_latch; cs->irq_reload = 0; } else cs->irq_ctr--;
    if (cs->irq_ctr == 0 && cs->irq_en) cs->irq_line = 1;
}

/* ---- board-level CPU side; sa is the 15-bit address as the cart sees it ---- */
static uint8_t board_cpu_read(Nes *n, uint16_t sa, int romsel_low, int m2, uint8_t *v) {
    CS;
    if (n->board) return (uint8_t)n->board->cpu_read(n, (uint16_t)(romsel_low ? (0x8000 | sa) : sa), m2, v);
    if (n->mapper == 4 && n->submapper == 1 && !romsel_low && m2 && (sa & 0x6000) == 0x6000) {   /* MMC6 */
        uint8_t ctl = cs->regs[10];
        if (sa < 0x7000 || !(cs->regs[0] & 0x20) || !(ctl & 0xA0)) return 0;
        int hi = (sa >> 9) & 1;
        *v = (hi ? (ctl & 0x80) : (ctl & 0x20)) ? cs->wram[sa & 0x3FF] : 0;   /* the other half reads $00 */
        return 0xFF;
    }
    if (romsel_low) { *v = n->prg[(cs->prg_off[(sa >> 13) & 3] + (sa & 0x1FFF)) % n->prg_size]; return 0xFF; }
    if (m2 && (sa & 0x6000) == 0x6000 && n->wram_size && cs->wram_on) {
        *v = cs->wram[(cs->wram_off + (sa & 0x1FFF)) % n->wram_size]; return 0xFF;
    }
    return 0;
}

static void board_cpu_write(Nes *n, uint16_t sa, int romsel_low, int m2, uint8_t v) {
    CS; uint8_t *r = cs->regs;
    if (n->board) { if (n->board->cpu_write) n->board->cpu_write(n, (uint16_t)(romsel_low ? (0x8000 | sa) : sa), m2, v); return; }
    if (n->mapper == 4 && n->submapper == 1 && !romsel_low) {                                       /* MMC6 */
        uint8_t ctl = r[10]; int hi = (sa >> 9) & 1;
        if (m2 && sa >= 0x7000 && (r[0] & 0x20) && (hi ? (ctl & 0xC0) == 0xC0 : (ctl & 0x30) == 0x30)) cs->wram[sa & 0x3FF] = v;
        if ((sa & 0x6000) == 0x6000) return;
    }
    if (!romsel_low) {
        if (!m2) return;
        if ((sa & 0x6000) == 0x6000) {
            if (n->mapper == 34 && chr_total(n) > 0x2000 && sa >= 0x7FFD) { r[sa - 0x7FFD] = v; update(n); }
            if (n->mapper == 87) { r[0] = (uint8_t)(((v & 1) << 1) | ((v >> 1) & 1)); update(n); return; }
            if (n->mapper == 140) { r[0] = v; update(n); return; }
            if (n->wram_size && cs->wram_on && !cs->wram_wp) cs->wram[(cs->wram_off + (sa & 0x1FFF)) % n->wram_size] = v;
        } else if (n->mapper == 79 && (sa & 0x6100) == 0x4100) { r[0] = v; update(n); }
        return;
    }
    /* bus conflict: ROM drives the bus at the same time on discrete-logic boards */
    if (n->bus_conflicts && (n->mapper == 2 || n->mapper == 3 || n->mapper == 7 || n->mapper == 11 || n->mapper == 66 || n->mapper == 94 || n->mapper == 180))
        v &= n->prg[(cs->prg_off[(sa >> 13) & 3] + (sa & 0x1FFF)) % n->prg_size];
    switch (n->mapper) {
    case 1: {
        uint64_t cyc = n->s.cpu.cycles;
        int consecutive = cyc - cs->last_write == 1;
        cs->last_write = cyc;
        if (consecutive) return;                      /* MMC1 ignores back-to-back writes (RMW) */
        if (v & 0x80) { cs->shift = 0; cs->shift_n = 0; r[0] |= 0x0C; update(n); return; }
        cs->shift = (uint8_t)((cs->shift >> 1) | (v & 1) << 4);
        if (++cs->shift_n == 5) { r[(sa >> 13) & 3] = cs->shift; cs->shift = 0; cs->shift_n = 0; update(n); }
    } break;
    case 4: case 206: case 118: case 119: case 76:
        switch (sa & 0x6001) {
        case 0x0000: r[0] = v; break;
        case 0x0001: r[1 + (r[0] & 7)] = v; break;
        case 0x2000: r[9] = v; break;
        case 0x2001: r[10] = v; break;
        case 0x4000: if (mmc3_irq_board(n)) cs->irq_latch = v; break;
        case 0x4001: if (mmc3_irq_board(n)) { cs->irq_ctr = 0; cs->irq_reload = 1; } break;
        case 0x6000: if (mmc3_irq_board(n)) { cs->irq_en = 0; cs->irq_line = 0; } break;
        case 0x6001: if (mmc3_irq_board(n)) cs->irq_en = 1; break;
        }
        update(n); break;
    case 9: case 10:
        if (sa >= 0x2000) { r[((sa >> 12) & 7) - 2] = v; update(n); }
        break;
    case 34: if (chr_total(n) <= 0x2000) { r[0] = v; update(n); } break;
    case 71: if (sa >= 0x4000) r[0] = v; else if (sa >= 0x1000) r[1] = v; update(n); break;
    case 94: r[0] = (v >> 2) & 7; update(n); break;
    case 0: break;
    default: r[0] = v; update(n); break;
    }
}

uint8_t cart_cpu_read(Nes *n, uint16_t a, uint8_t *val) {
    if (!n->has_rom) return 0;
    Connector *c = &n->conn;
    if (c->perfect) return board_cpu_read(n, a & 0x7FFF, a >= 0x8000, 1, val);
    if (!c->powered) return 0;
    sample(c);
    uint16_t sa = cpu_addr_seen(c, a & 0x7FFF);
    int romsel = !line(c, S_ROMSEL, a >= 0x8000 ? 0 : 1);
    int m2 = line(c, S_M2, 1);
    line(c, S_RW, 1);
    uint8_t drv = board_cpu_read(n, sa, romsel, m2, val);
    return drv & c->cd_mask;
}

/* R/W floating low: the cart thinks every read is a write of what's on the bus */
void cart_cpu_read_post(Nes *n, uint16_t a, uint8_t busval) {
    Connector *c = &n->conn;
    if (!n->has_rom || c->perfect || !c->powered || c->sig_ok[S_RW]) return;
    if (line(c, S_RW, 1)) return;
    uint16_t sa = cpu_addr_seen(c, a & 0x7FFF);
    board_cpu_write(n, sa, !line(c, S_ROMSEL, a >= 0x8000 ? 0 : 1), line(c, S_M2, 1), busval);
}

void cart_cpu_write(Nes *n, uint16_t a, uint8_t v) {
    if (!n->has_rom) return;
    Connector *c = &n->conn;
    if (c->perfect) { board_cpu_write(n, a & 0x7FFF, a >= 0x8000, 1, v); return; }
    if (!c->powered) return;
    sample(c);
    uint16_t sa = cpu_addr_seen(c, a & 0x7FFF);
    int romsel = !line(c, S_ROMSEL, a >= 0x8000 ? 0 : 1);
    int m2 = line(c, S_M2, 1);
    int rw = line(c, S_RW, 0);
    uint32_t h = c->hold_cd;
    uint8_t sv = (uint8_t)flt(c, v, c->cd_mask, 0xFF, &h, S_CD0); c->hold_cd = (uint8_t)h;
    if (!rw) board_cpu_write(n, sa, romsel, m2, sv);
}

/* ---- PPU side ---- */
static uint8_t *nt_ptr(Nes *n, int page, uint16_t a) {
    CS;
    if (page >= 2) return &cs->vram[((page - 2) * 0x400 + (a & 0x3FF)) & 0xFFF];
    return &n->s.ciram[page * 0x400 + (a & 0x3FF)];
}

uint8_t cart_ppu_read(Nes *n, uint16_t a) {
    CS;
    Connector *c = &n->conn;
    if (!n->has_rom) return (uint8_t)a;
    if (c->perfect) {
        uint8_t v;
        if (n->board && n->board->ppu_read) {
            int k = n->board->ppu_read(n, a, &v);
            if (k == PPU_DRIVE) return v;
            if (k == PPU_CIRAM0 || k == PPU_CIRAM1) return n->s.ciram[(k - PPU_CIRAM0) * 0x400 + (a & 0x3FF)];
            if (k == PPU_OPEN) return (uint8_t)a;
        }
        if (a < 0x2000) { v = *chr_ptr(n, a); mmc2_latch(n, a); }
        else v = *nt_ptr(n, cs->nt_map[(a >> 10) & 3], a);
        return v;
    }
    uint8_t val = 0xFF, drv = 0;
    int ce = 1, a10 = 0;
    if (c->powered) {
        sample(c);
        uint16_t sa = ppu_addr_seen(c, a);
        int rd = !line(c, S_RD, 0);
        int na13 = line(c, S_NA13, (a & 0x2000) ? 0 : 1);
        int page = cs->nt_map[(sa >> 10) & 3];
        int k = PPU_DEFAULT; uint8_t bv = 0;
        if (n->board && n->board->ppu_read) k = n->board->ppu_read(n, sa, &bv);
        if (k == PPU_DRIVE) { if (rd) { val = bv; drv = 0xFF; } page = 2; }
        else if (k == PPU_OPEN) page = 2;
        else if (k == PPU_CIRAM0 || k == PPU_CIRAM1) page = k - PPU_CIRAM0;
        else if (!(sa & 0x2000)) { if (rd) { val = *chr_ptr(n, sa); drv = 0xFF; } mmc2_latch(n, sa); }
        else if (page >= 2 && rd) { val = *nt_ptr(n, page, sa); drv = 0xFF; }
        drv &= c->pd_mask;
        ce = (page >= 2 || !(sa & 0x2000)) ? 1 : na13;
        a10 = page & 1;
    }
    ce = line(c, S_CICE, ce);
    a10 = line(c, S_CIA10, a10);
    if (!ce) { val = (uint8_t)((val | (uint8_t)~drv) & n->s.ciram[a10 * 0x400 + (a & 0x3FF)]); drv = 0xFF; }  /* bus conflict = AND */
    return (uint8_t)((val & drv) | ((uint8_t)a & ~drv));    /* undriven bits: address low byte left on AD0-7 */
}

void cart_ppu_write(Nes *n, uint16_t a, uint8_t v) {
    CS;
    Connector *c = &n->conn;
    if (!n->has_rom) return;
    if (c->perfect) {
        if (n->board && n->board->ppu_write) {
            int k = n->board->ppu_write(n, a, v);
            if (k == PPU_DRIVE || k == PPU_OPEN) return;
            if (k == PPU_CIRAM0 || k == PPU_CIRAM1) { n->s.ciram[(k - PPU_CIRAM0) * 0x400 + (a & 0x3FF)] = v; return; }
        }
        if (a < 0x2000) { if (chr_writable(n, a)) *chr_ptr(n, a) = v; }
        else *nt_ptr(n, cs->nt_map[(a >> 10) & 3], a) = v;
        return;
    }
    int ce = 1, a10 = 0;
    if (c->powered) {
        sample(c);
        uint16_t sa = ppu_addr_seen(c, a);
        int wrl = !line(c, S_WR, 0);
        int na13 = line(c, S_NA13, (a & 0x2000) ? 0 : 1);
        uint32_t h = c->hold_pd;
        uint8_t sv = (uint8_t)flt(c, v, c->pd_mask, 0xFF, &h, S_PD0); c->hold_pd = (uint8_t)h;
        int page = cs->nt_map[(sa >> 10) & 3];
        int k = PPU_DEFAULT;
        if (wrl && n->board && n->board->ppu_write) k = n->board->ppu_write(n, sa, sv);
        if (k == PPU_DRIVE || k == PPU_OPEN) page = 2;
        else if (k == PPU_CIRAM0 || k == PPU_CIRAM1) page = k - PPU_CIRAM0;
        else if (wrl) {
            if (!(sa & 0x2000)) { if (chr_writable(n, sa)) *chr_ptr(n, sa) = sv; }
            else if (page >= 2) *nt_ptr(n, page, sa) = sv;
        }
        ce = (page >= 2 || !(sa & 0x2000)) ? 1 : na13;
        a10 = page & 1;
    }
    ce = line(c, S_CICE, ce);
    a10 = line(c, S_CIA10, a10);
    if (!ce) n->s.ciram[a10 * 0x400 + (a & 0x3FF)] = v;
}

void cart_ppu_addr(Nes *n, uint16_t a) {
    CS;
    if (!n->has_rom) return;
    if (n->board) {
        if (!n->board->ppu_addr) return;
        if (!n->conn.perfect) { if (!n->conn.powered) return; a = ppu_addr_seen(&n->conn, a); }
        n->board->ppu_addr(n, a); return;
    }
    if (!mmc3_irq_board(n)) return;
    Connector *c = &n->conn;
    int a12 = (a >> 12) & 1;
    if (!c->perfect) { if (!c->powered) return; uint16_t sa = ppu_addr_seen(c, a); a12 = (sa >> 12) & 1; }
    if (a12 && !cs->a12) { if (cs->a12_low >= 3) mmc3_clock(n); }
    if (!a12 && cs->a12) cs->a12_low = 0;
    cs->a12 = (uint8_t)a12;
}

void cart_m2(Nes *n) {
    CS;
    if (!cs->a12 && cs->a12_low < 255) cs->a12_low++;
    if (n->board && n->board->m2 && n->has_rom && (n->conn.perfect || n->conn.powered)) n->board->m2(n);
}
/* Cartridge audio. NES: rides on EXP6 (the common expansion-audio mod). Famicom: the 2A03's
 * audio leaves on pin 45, goes through the cartridge (plain carts just bridge 45 to 46, sound
 * carts mix their chip in) and comes back on pin 46 - so a dirty pin 45/46 silences everything. */
float cart_audio(Nes *n) {
    if (!n->board || !n->board->audio || !n->exp_audio || !n->has_rom) return 0.0f;
    Connector *c = &n->conn;
    if (!c->powered) return 0.0f;
    if (c->slot != SLOT_FC && !c->sig_ok[S_EXP0 + 6]) return 0.0f;
    return n->board->audio(n);
}
float cart_audio_path(Nes *n, float s) {
    Connector *c = &n->conn;
    if (c->slot != SLOT_FC) return s + cart_audio(n);
    if (!n->has_rom || !c->sig_ok[S_AIN] || !c->sig_ok[S_AOUT]) return 0.0f;
    return s + cart_audio(n);
}
int cart_irq(Nes *n) { return n->s.cart.irq_line && (n->conn.perfect || n->conn.sig_ok[S_IRQ]); }
