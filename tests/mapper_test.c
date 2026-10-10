/* mapper_test - drives each board through its registers via the cartridge connector
 * and checks PRG/CHR/nametable mappings, IRQ timing, save memory and sound pitch.
 * Every 8 KB PRG bank and 1 KB CHR bank of the synthetic cartridges holds its own
 * bank number, so a read tells which bank is mapped.
 *   gcc -O2 -o mapper_test tests/mapper_test.c src/cpu.c src/ppu.c src/apu.c src/cart.c src/nes.c src/boards*.c -lm */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../src/nes.h"

static int checks, fails;
static char ctx[96];
static void ok(int cond, const char *what, long got, long want) {
    checks++;
    if (!cond) { fails++; printf("  FAIL %-34s %-36s got %ld want %ld\n", ctx, what, got, want); }
}
static void eq_(const char *what, long got, long want) { ok(got == want, what, got, want); }
#define EQ(what, got, want) eq_(what, (long)(got), (long)(want))      /* evaluates each argument once */

static Nes *cart(int mapper, int sub, int prg_kb, int chr_kb, int flags6) {
    size_t prg = (size_t)prg_kb * 1024, chr = (size_t)chr_kb * 1024, size = 16 + prg + chr;
    uint8_t *d = calloc(1, size);
    memcpy(d, "NES\x1a", 4);
    d[4] = (uint8_t)(prg / 0x4000); d[5] = (uint8_t)(chr / 0x2000);
    d[6] = (uint8_t)((mapper & 15) << 4 | flags6); d[7] = (uint8_t)((mapper & 0xF0) | 0x08);
    d[8] = (uint8_t)((mapper >> 8) | sub << 4);
    d[10] = 0x07;                                     /* 8 KB PRG-RAM */
    d[11] = chr ? 0 : 0x07;                           /* 8 KB CHR-RAM when no CHR-ROM */
    for (size_t i = 0; i < prg; i++) d[16 + i] = (uint8_t)(i / 0x2000);
    for (size_t i = 0; i < chr; i++) d[16 + prg + i] = (uint8_t)(i / 0x400);
    Nes *n = nes_create(); char err[128];
    if (!nes_load_rom(n, d, size, 0, err, sizeof err)) { printf("  load failed (%s): %s\n", ctx, err); exit(1); }
    free(d);
    return n;
}
static uint8_t R(Nes *n, uint16_t a) { uint8_t v = 0; cart_cpu_read(n, a, &v); return v; }
static void W(Nes *n, uint16_t a, uint8_t v) { cart_cpu_write(n, a, v); }
static uint8_t P(Nes *n, uint16_t a) { return cart_ppu_read(n, a); }
static void PW(Nes *n, uint16_t a, uint8_t v) { cart_ppu_write(n, a, v); }
static int ntpage(Nes *n, int k) {                    /* 0/1 = CIRAM page, else 100 + ROM byte */
    n->s.ciram[0x155] = 0xA0; n->s.ciram[0x555] = 0xB1;
    uint8_t v = P(n, (uint16_t)(0x2000 + k * 0x400 + 0x155));
    return v == 0xA0 ? 0 : v == 0xB1 ? 1 : 100 + v;
}
static void mir(Nes *n, int p0, int p1, int p2, int p3) {
    char w[24]; int e[4] = { p0, p1, p2, p3 };
    for (int k = 0; k < 4; k++) { snprintf(w, sizeof w, "nametable %d", k); EQ(w, ntpage(n, k), e[k]); }
}
static void banks(Nes *n, int b8000, int bA000, int bC000, int bE000) {
    EQ("PRG $8000", R(n, 0x8000), b8000); EQ("PRG $A000", R(n, 0xA000), bA000);
    EQ("PRG $C000", R(n, 0xC000), bC000); EQ("PRG $E000", R(n, 0xE000), bE000);
}
static void chrs(Nes *n, const int *e) { char w[24]; for (int i = 0; i < 8; i++) { snprintf(w, sizeof w, "CHR $%04X", i * 0x400); EQ(w, P(n, (uint16_t)(i * 0x400)), e[i]); } }
static int irq_after(Nes *n, int max) { for (int c = 1; c <= max; c++) { cart_m2(n); if (cart_irq(n)) return c; } return -1; }
/* pitch of the cartridge audio: autocorrelation over `secs`, sampling every `step` CPU cycles */
static double pitch(Nes *n, int step, double secs, double fmin, double fmax) {
    int ns = (int)(1789773.0 * secs / step); float *x = malloc(sizeof(float) * ns); double mean = 0;
    for (int i = 0; i < ns; i++) { float acc = 0; for (int k = 0; k < step; k++) { cart_m2(n); acc += cart_audio(n); } x[i] = acc / step; mean += x[i]; }
    mean /= ns; double best = 0; int lag_best = 0, lo = (int)(1789773.0 / step / fmax), hi = (int)(1789773.0 / step / fmin);
    double e0 = 0; for (int i = 0; i < ns; i++) e0 += (x[i] - mean) * (x[i] - mean);
    if (e0 < 1e-9) { free(x); return 0; }
    for (int lag = lo; lag <= hi && lag < ns / 2; lag++) {
        double s = 0; for (int i = 0; i + lag < ns; i++) s += (x[i] - mean) * (x[i + lag] - mean);
        s /= (ns - lag);
        if (s > best * 1.02) { best = s; lag_best = lag; }
    }
    free(x);
    return lag_best ? 1789773.0 / step / lag_best : 0;
}
static void near(const char *what, double got, double want, double tol) { checks++; if (fabs(got - want) > tol) { fails++; printf("  FAIL %-34s %-36s got %.1f want %.1f\n", ctx, what, got, want); } }

/* ---------------- VRC2 / VRC4 ---------------- */
static uint16_t vaddr(int m, int s, uint16_t base, int sel) {
    int b0 = sel & 1, b1 = sel >> 1;
    switch (m) {
    case 21: return s == 2 ? (uint16_t)(base | b0 << 6 | b1 << 7) : (uint16_t)(base | b0 << 1 | b1 << 2);
    case 22: return (uint16_t)(base | b0 << 1 | b1);
    case 23: return s == 2 ? (uint16_t)(base | b0 << 2 | b1 << 3) : (uint16_t)(base | b0 | b1 << 1);
    default: return s == 2 ? (uint16_t)(base | b0 << 3 | b1 << 2) : (uint16_t)(base | b0 << 1 | b1);
    }
}
static void test_vrc24(int m, int s) {
    snprintf(ctx, sizeof ctx, "VRC2/4 mapper %d sub %d", m, s);
    int vrc2 = m == 22 || ((m == 23 || m == 25) && s == 3);
    Nes *n = cart(m, s, 256, 256, 0);
    W(n, vaddr(m, s, 0x8000, 0), 5); W(n, vaddr(m, s, 0xA000, 0), 6);
    banks(n, 5, 6, 30, 31);
    if (!vrc2) { W(n, vaddr(m, s, 0x9000, 2), 2); banks(n, 30, 6, 5, 31); W(n, vaddr(m, s, 0x9000, 2), 0); }
    int want[8];
    for (int k = 0; k < 8; k++) {
        int val = 0x10 + k * 0x11;                              /* 9-bit values on VRC4 */
        uint16_t base = (uint16_t)(0xB000 + (k >> 1) * 0x1000);
        W(n, vaddr(m, s, base, (k & 1) * 2 + 0), (uint8_t)(val & 15));
        W(n, vaddr(m, s, base, (k & 1) * 2 + 1), (uint8_t)(val >> 4));
        int bank = vrc2 ? (val & 0xFF) : val;
        if (m == 22) bank >>= 1;
        want[k] = bank & 0xFF;
    }
    chrs(n, want);
    W(n, vaddr(m, s, 0x9000, 0), 1); mir(n, 0, 0, 1, 1);
    W(n, vaddr(m, s, 0x9000, 0), 0); mir(n, 0, 1, 0, 1);
    if (!vrc2) {
        W(n, vaddr(m, s, 0x9000, 0), 2); mir(n, 0, 0, 0, 0);
        W(n, vaddr(m, s, 0xF000, 0), 0xE); W(n, vaddr(m, s, 0xF000, 1), 0xF);      /* latch $FE */
        W(n, vaddr(m, s, 0xF000, 2), 6);                                            /* enable, cycle mode */
        EQ("IRQ, cycle mode, latch $FE", irq_after(n, 10), 2);
        W(n, vaddr(m, s, 0xF000, 3), 0); EQ("IRQ acknowledged", cart_irq(n), 0);
        W(n, vaddr(m, s, 0xF000, 2), 2);                                            /* scanline mode */
        int c = irq_after(n, 400); ok(c >= 226 && c <= 229, "IRQ, scanline mode (2 lines)", c, 227);
    }
    nes_destroy(n);
}
static void test_vrc1(void) {
    strcpy(ctx, "VRC1 (75)"); Nes *n = cart(75, 0, 128, 128, 0);
    W(n, 0x8000, 3); W(n, 0xA000, 4); W(n, 0xC000, 5); banks(n, 3, 4, 5, 15);
    W(n, 0x9000, 0x07); W(n, 0xE000, 0x02); W(n, 0xF000, 0x05);
    int e[8] = { 72, 73, 74, 75, 84, 85, 86, 87 }; chrs(n, e);                     /* (2|16)*4 and (5|16)*4 */
    mir(n, 0, 0, 1, 1); nes_destroy(n);
}
static void test_vrc3(void) {
    strcpy(ctx, "VRC3 (73)"); Nes *n = cart(73, 0, 128, 0, 0);
    W(n, 0xF000, 2); banks(n, 4, 5, 14, 15);
    W(n, 0x8000, 0xE); W(n, 0x9000, 0xF); W(n, 0xA000, 0xF); W(n, 0xB000, 0xF);   /* latch $FFFE */
    W(n, 0xC000, 2); EQ("16-bit IRQ after 2 cycles", irq_after(n, 10), 2);
    W(n, 0xD000, 0); EQ("acknowledged", cart_irq(n), 0);
    W(n, 0x8000, 0xD); W(n, 0x9000, 0xF); W(n, 0xC000, 6); EQ("8-bit IRQ after 3 cycles", irq_after(n, 10), 3);
    nes_destroy(n);
}
static void test_vrc6(int m) {
    snprintf(ctx, sizeof ctx, "VRC6 (%d)", m); Nes *n = cart(m, 0, 256, 256, 0);
    uint16_t x1 = m == 26 ? 2 : 1, x2 = m == 26 ? 1 : 2, x3 = 3;
    W(n, 0x8000, 3); W(n, 0xC000, 9); banks(n, 6, 7, 9, 31);
    for (int k = 0; k < 4; k++) { W(n, (uint16_t)(0xD000 | (k == 1 ? x1 : k == 2 ? x2 : k)), (uint8_t)(40 + k)); W(n, (uint16_t)(0xE000 | (k == 1 ? x1 : k == 2 ? x2 : k)), (uint8_t)(50 + k)); }
    W(n, (uint16_t)(0xB000 | x3), 0x84);                                            /* mode 0, H mirroring, RAM on */
    int e[8] = { 40, 41, 42, 43, 50, 51, 52, 53 }; chrs(n, e); mir(n, 0, 0, 1, 1);
    W(n, 0x6000, 0x5A); EQ("PRG-RAM", R(n, 0x6000), 0x5A);
    W(n, (uint16_t)(0xF000 | x1), 6); W(n, 0xF000, 0xFD); W(n, (uint16_t)(0xF000 | x1), 6); EQ("IRQ (cycle mode)", irq_after(n, 10), 3);
    W(n, 0x9000, 0x7F); W(n, (uint16_t)(0x9000 | x1), 0xFF); W(n, (uint16_t)(0x9000 | x2), 0x81);   /* pulse 1: 50% duty, period $1FF */
    near("pulse 1 pitch (Hz)", pitch(n, 16, 0.25, 100, 500), 1789773.0 / (16 * 512), 3);
    W(n, (uint16_t)(0x9000 | x2), 0x01);
    W(n, 0xB000, 0x10); W(n, (uint16_t)(0xB000 | x1), 0xFF); W(n, (uint16_t)(0xB000 | x2), 0x80);   /* saw, period $FF */
    near("saw pitch (Hz)", pitch(n, 16, 0.25, 200, 1200), 1789773.0 / (14 * 256), 6);
    nes_destroy(n);
}
static void test_vrc7(int s) {
    snprintf(ctx, sizeof ctx, "VRC7 (85) sub %d", s); Nes *n = cart(85, s, 256, 256, 0);
    uint16_t hi = s == 1 ? 0x08 : 0x10;
    W(n, 0x8000, 7); W(n, (uint16_t)(0x8000 | hi), 8); W(n, 0x9000, 9); banks(n, 7, 8, 9, 31);
    for (int k = 0; k < 8; k++) W(n, (uint16_t)(0xA000 + (k >> 1) * 0x1000 + ((k & 1) ? hi : 0)), (uint8_t)(100 + k));
    int e[8] = { 100, 101, 102, 103, 104, 105, 106, 107 }; chrs(n, e);
    W(n, 0xE000, 0x01); W(n, 0x6000, 0x33); EQ("WRAM protected while $E000 bit 7 is clear", R(n, 0x6000) == 0x33, 0);
    W(n, 0xE000, 0x81); mir(n, 0, 0, 1, 1);
    W(n, 0x6000, 0x33); EQ("WRAM enabled by $E000 bit 7", R(n, 0x6000), 0x33);
    W(n, (uint16_t)(0xE000 | hi), 0xFE); W(n, 0xF000, 6); EQ("IRQ (cycle mode)", irq_after(n, 10), 2);
    if (s == 2) {                                                                    /* audio is on VRC7a */
        W(n, 0x9010, 0x30); W(n, 0x9030, 0x30);                                     /* ch 0: instrument 3, full volume */
        W(n, 0x9010, 0x10); W(n, 0x9030, 0x22);                                     /* fnum $122 = 290 */
        W(n, 0x9010, 0x20); W(n, 0x9030, 0x19);                                     /* key on, block 4, fnum bit 8 */
        for (int i = 0; i < 1789773 / 20; i++) cart_m2(n);
        near("FM channel pitch (Hz, A4)", pitch(n, 36, 0.3, 200, 900), 49716.0 * 290 * 8 / 262144.0, 4);
        W(n, 0xE000, 0xC1); float q = 0; for (int i = 0; i < 4000; i++) { cart_m2(n); q += fabsf(cart_audio(n)); }
        EQ("$E000 bit 6 silences the FM chip", q == 0.0f, 1);
        W(n, 0x9010, 0x10); W(n, 0x9030, 0x55); W(n, 0xE000, 0x81); EQ("writes ignored while silenced", n->s.cart.vrc7.reg[0x10], 0);
    }
    nes_destroy(n);
}
/* ---------------- Sunsoft ---------------- */
static void test_sunsoft(void) {
    strcpy(ctx, "Sunsoft-3 (67)"); Nes *n = cart(67, 0, 128, 128, 0);
    W(n, 0xF800, 3); banks(n, 6, 7, 14, 15);
    W(n, 0x8800, 10); W(n, 0x9800, 11); W(n, 0xA800, 12); W(n, 0xB800, 13);
    int e[8] = { 20, 21, 22, 23, 24, 25, 26, 27 }; chrs(n, e);
    W(n, 0xE800, 1); mir(n, 0, 0, 1, 1);
    W(n, 0xC800, 0x00); W(n, 0xC800, 0x03); W(n, 0xD800, 0x10); EQ("IRQ on underflow", irq_after(n, 10), 4);
    nes_destroy(n);
    strcpy(ctx, "Sunsoft-4 (68)"); n = cart(68, 0, 128, 256, 0);
    W(n, 0xF000, 0x12); banks(n, 4, 5, 14, 15);
    W(n, 0x8000, 7); { int e2[8] = { 14, 15, 2, 3, 4, 5, 6, 7 }; e2[2] = 0; e2[3] = 1; e2[4] = 0; e2[5] = 1; e2[6] = 0; e2[7] = 1; (void)e2; }
    EQ("CHR $0000", P(n, 0x0000), 14); EQ("CHR $0400", P(n, 0x0400), 15);
    W(n, 0xC000, 0x05); W(n, 0xD000, 0x06); W(n, 0xE000, 0x10);                     /* CHR-ROM nametables, V */
    EQ("ROM nametable 0", ntpage(n, 0), 100 + 0x85); EQ("ROM nametable 1", ntpage(n, 1), 100 + 0x86);
    W(n, 0xE000, 0x01); mir(n, 0, 0, 1, 1);
    W(n, 0x6000, 0x77); EQ("PRG-RAM", R(n, 0x6000), 0x77);
    nes_destroy(n);
    strcpy(ctx, "FME-7/5B (69)"); n = cart(69, 0, 256, 256, 0);
    W(n, 0x8000, 9); W(n, 0xA000, 3); W(n, 0x8000, 10); W(n, 0xA000, 4); W(n, 0x8000, 11); W(n, 0xA000, 5); banks(n, 3, 4, 5, 31);
    for (int k = 0; k < 8; k++) { W(n, 0x8000, (uint8_t)k); W(n, 0xA000, (uint8_t)(200 + k)); }
    int e3[8] = { 200, 201, 202, 203, 204, 205, 206, 207 }; chrs(n, e3);
    W(n, 0x8000, 12); W(n, 0xA000, 1); mir(n, 0, 0, 1, 1);
    W(n, 0x8000, 8); W(n, 0xA000, 0x02); EQ("ROM bank at $6000", R(n, 0x6000), 2);
    W(n, 0x8000, 8); W(n, 0xA000, 0xC0); W(n, 0x6000, 0x44); EQ("RAM at $6000", R(n, 0x6000), 0x44);
    W(n, 0x8000, 14); W(n, 0xA000, 2); W(n, 0x8000, 15); W(n, 0xA000, 0); W(n, 0x8000, 13); W(n, 0xA000, 0x81);
    EQ("IRQ on counter wrap", irq_after(n, 10), 3);
    W(n, 0xC000, 0); W(n, 0xE000, 254); W(n, 0xC000, 1); W(n, 0xE000, 0); W(n, 0xC000, 7); W(n, 0xE000, 0x3E); W(n, 0xC000, 8); W(n, 0xE000, 0x0F);
    near("5B tone A pitch (Hz)", pitch(n, 16, 0.25, 100, 500), 1789773.0 / (32 * 254), 3);
    nes_destroy(n);
}
/* ---------------- Namco ---------------- */
static void test_namco(void) {
    strcpy(ctx, "Namco 163 (19)"); Nes *n = cart(19, 0, 256, 256, 0);
    W(n, 0xE000, 0x03); W(n, 0xE800, 0x04); W(n, 0xF000, 0x05); banks(n, 3, 4, 5, 31);
    for (int k = 0; k < 8; k++) W(n, (uint16_t)(0x8000 + k * 0x800), (uint8_t)(60 + k));
    int e[8] = { 60, 61, 62, 63, 64, 65, 66, 67 }; chrs(n, e);
    W(n, 0xC000, 0xE0); W(n, 0xC800, 0xE1); W(n, 0xD000, 0x09); W(n, 0xD800, 0xE1);
    EQ("NT 0 -> CIRAM A", ntpage(n, 0), 0); EQ("NT 1 -> CIRAM B", ntpage(n, 1), 1); EQ("NT 2 -> CHR-ROM page 9", ntpage(n, 2), 109); EQ("NT 3 -> CIRAM B", ntpage(n, 3), 1);
    W(n, 0x8000, 0xE0); EQ("CHR window from CIRAM", P(n, 0x0155), 0xA0);
    W(n, 0xF800, 0x80); W(n, 0x4800, 0x11); W(n, 0x4800, 0x22); W(n, 0xF800, 0x80); EQ("sound RAM + auto-increment", R(n, 0x4800), 0x11); EQ("next", R(n, 0x4800), 0x22);
    W(n, 0x5000, 0xFD); W(n, 0x5800, 0xFF); EQ("IRQ at $7FFF", irq_after(n, 10), 2);
    /* one wave channel: 32 samples of a square, freq 7733 -> ~440 Hz */
    W(n, 0xF800, 0x80); for (int i = 0; i < 16; i++) W(n, 0x4800, i < 8 ? 0xFF : 0x00);
    W(n, 0xF800, 0xF8); W(n, 0x4800, 7733 & 0xFF); W(n, 0x4800, 0); W(n, 0x4800, (7733 >> 8) & 0xFF); W(n, 0x4800, 0);
    W(n, 0x4800, 0xE0); W(n, 0x4800, 0); W(n, 0x4800, 0x00); W(n, 0x4800, 0x0F);   /* length 32, wave at 0, vol 15, 1 channel */
    near("wavetable pitch (Hz)", pitch(n, 15, 0.25, 200, 900), 7733.0 * 1789773.0 / (15.0 * 65536 * 32), 4);
    nes_destroy(n);
    strcpy(ctx, "Namco 340 (210 sub 2)"); n = cart(210, 2, 128, 128, 0);
    W(n, 0xE000, 0x42); W(n, 0xE800, 3); W(n, 0xF000, 4); banks(n, 2, 3, 4, 15); mir(n, 0, 1, 0, 1);
    nes_destroy(n);
    strcpy(ctx, "Namco 3446 (76)"); n = cart(76, 0, 128, 128, 0);
    for (int k = 2; k < 8; k++) { W(n, 0x8000, (uint8_t)k); W(n, 0x8001, (uint8_t)(k + 8)); }
    banks(n, 14, 15, 14, 15);
    int e4[8] = { 20, 21, 22, 23, 24, 25, 26, 27 }; chrs(n, e4);
    nes_destroy(n);
}
/* ---------------- Bandai FCG + EEPROM ---------------- */
static void i2c(Nes *n, uint16_t r, int scl, int sda) { W(n, r, (uint8_t)(scl << 5 | sda << 6)); }
static void i2c_start(Nes *n, uint16_t r) { i2c(n, r, 1, 1); i2c(n, r, 1, 0); i2c(n, r, 0, 0); }
static void i2c_stop(Nes *n, uint16_t r) { i2c(n, r, 0, 0); i2c(n, r, 1, 0); i2c(n, r, 1, 1); }
static int i2c_byte(Nes *n, uint16_t r, uint8_t b) {
    for (int i = 7; i >= 0; i--) { int bit = (b >> i) & 1; i2c(n, r, 0, bit); i2c(n, r, 1, bit); i2c(n, r, 0, bit); }
    i2c(n, r, 0, 1); i2c(n, r, 1, 1); int ack = !((R(n, 0x6000) >> 4) & 1); i2c(n, r, 0, 1);
    return ack;
}
static uint8_t i2c_read(Nes *n, uint16_t r, int last) {
    uint8_t b = 0;
    for (int i = 0; i < 8; i++) { i2c(n, r, 0, 1); i2c(n, r, 1, 1); b = (uint8_t)(b << 1 | ((R(n, 0x6000) >> 4) & 1)); }
    i2c(n, r, 0, last); i2c(n, r, 1, last); i2c(n, r, 0, 1);
    return b;
}
static void test_bandai(void) {
    strcpy(ctx, "Bandai FCG-1/2 (16 sub 4)"); Nes *n = cart(16, 4, 256, 256, 0);
    W(n, 0x6008, 3); banks(n, 6, 7, 30, 31);
    for (int k = 0; k < 8; k++) W(n, (uint16_t)(0x6000 + k), (uint8_t)(30 + k));
    int e[8] = { 30, 31, 32, 33, 34, 35, 36, 37 }; chrs(n, e);
    W(n, 0x6009, 1); mir(n, 0, 0, 1, 1);
    W(n, 0x600B, 3); W(n, 0x600C, 0); W(n, 0x600A, 1); EQ("IRQ (counter 3)", irq_after(n, 10), 4);
    nes_destroy(n);
    strcpy(ctx, "Bandai LZ93D50+24C02 (16 sub 5)"); n = cart(16, 5, 256, 256, 2);
    W(n, 0x8008, 2); banks(n, 4, 5, 30, 31);
    W(n, 0x800B, 5); W(n, 0x800C, 0); W(n, 0x800A, 1); EQ("IRQ from latch 5", irq_after(n, 10), 6);
    uint16_t r = 0x800D;
    i2c_start(n, r); int a1 = i2c_byte(n, r, 0xA0), a2 = i2c_byte(n, r, 0x42), a3 = i2c_byte(n, r, 0x5C); i2c_stop(n, r);
    EQ("24C02 write ACKs", a1 + a2 + a3, 3);
    i2c_start(n, r); i2c_byte(n, r, 0xA0); i2c_byte(n, r, 0x42); i2c_start(n, r); i2c_byte(n, r, 0xA1);
    EQ("24C02 random read", i2c_read(n, r, 1), 0x5C); i2c_stop(n, r);
    EQ("stored in the battery save", n->s.cart.wram[0x42], 0x5C);
    nes_destroy(n);
    strcpy(ctx, "Bandai LZ93D50+24C01 (159)"); n = cart(159, 0, 256, 256, 2);
    i2c_start(n, r); i2c_byte(n, r, (uint8_t)(0x21 << 1)); i2c_byte(n, r, 0x9A); i2c_stop(n, r);
    i2c_start(n, r); i2c_byte(n, r, (uint8_t)(0x21 << 1 | 1)); EQ("24C01 read", i2c_read(n, r, 1), 0x9A); i2c_stop(n, r);
    nes_destroy(n);
}
/* ---------------- Jaleco, Irem, Taito, discrete ---------------- */
static void test_misc(void) {
    strcpy(ctx, "Jaleco SS8806 (18)"); Nes *n = cart(18, 0, 256, 256, 0);
    W(n, 0x8000, 0x5); W(n, 0x8001, 0x1); W(n, 0x8002, 0x6); W(n, 0x9000, 0x7); banks(n, 21, 6, 7, 31);
    W(n, 0xA000, 0x9); W(n, 0xA001, 0x2); W(n, 0xD003, 0x3); W(n, 0xD002, 0x4);
    EQ("CHR 0", P(n, 0x0000), 0x29); EQ("CHR 7", P(n, 0x1C00), 0x34);
    W(n, 0xF002, 1); mir(n, 0, 1, 0, 1);
    W(n, 0xE000, 0x3); W(n, 0xE001, 0); W(n, 0xE002, 0); W(n, 0xE003, 0); W(n, 0xF000, 0); W(n, 0xF001, 0x09);
    EQ("4-bit IRQ (count 3)", irq_after(n, 10), 4);
    nes_destroy(n);
    strcpy(ctx, "Irem G-101 (32)"); n = cart(32, 0, 256, 256, 0);
    W(n, 0x8000, 4); W(n, 0xA000, 5); banks(n, 4, 5, 30, 31); W(n, 0x9000, 3); banks(n, 30, 5, 4, 31); mir(n, 0, 0, 1, 1);
    W(n, 0xB003, 77); EQ("CHR 3", P(n, 0x0C00), 77);
    nes_destroy(n);
    strcpy(ctx, "Irem G-101 Major League (32 sub 1)"); n = cart(32, 1, 256, 256, 0); W(n, 0x9000, 3); mir(n, 0, 0, 0, 0); nes_destroy(n);
    strcpy(ctx, "Irem H-3001 (65)"); n = cart(65, 0, 256, 256, 0);
    W(n, 0x8000, 4); W(n, 0xA000, 5); W(n, 0xC000, 6); banks(n, 4, 5, 6, 31); W(n, 0xB005, 99); EQ("CHR 5", P(n, 0x1400), 99);
    W(n, 0x9001, 0x80); mir(n, 0, 0, 1, 1);
    W(n, 0x9005, 0); W(n, 0x9006, 3); W(n, 0x9004, 0); W(n, 0x9003, 0x80); EQ("IRQ after 3", irq_after(n, 10), 3);
    nes_destroy(n);
    strcpy(ctx, "Irem 78 Holy Diver (sub 3)"); n = cart(78, 3, 128, 128, 0);
    n->prg[0x2000 * 15 + 0x100] = 0xFF; W(n, 0xE100, 0x5B); banks(n, 6, 7, 14, 15); EQ("CHR 8K bank 5", P(n, 0), 40); mir(n, 0, 1, 0, 1);
    nes_destroy(n);
    strcpy(ctx, "Irem 78 Cosmo Carrier (sub 1)"); n = cart(78, 1, 128, 128, 0);
    n->prg[0x2000 * 15 + 0x100] = 0xFF; W(n, 0xE100, 0x08); mir(n, 1, 1, 1, 1); W(n, 0xE100, 0x00); mir(n, 0, 0, 0, 0);
    nes_destroy(n);
    strcpy(ctx, "Taito TC0190 (33)"); n = cart(33, 0, 256, 256, 0);
    W(n, 0x8000, 0x45); W(n, 0x8001, 6); banks(n, 5, 6, 30, 31); mir(n, 0, 0, 1, 1);
    W(n, 0x8002, 10); W(n, 0x8003, 11); W(n, 0xA000, 50); W(n, 0xA003, 53);
    int e[8] = { 20, 21, 22, 23, 50, 1, 2, 53 }; e[5] = P(n, 0x1400); e[6] = P(n, 0x1800); chrs(n, e);
    nes_destroy(n);
    strcpy(ctx, "Taito X1-005 (80)"); n = cart(80, 0, 256, 256, 0);
    W(n, 0x7EFA, 3); W(n, 0x7EFC, 4); W(n, 0x7EFE, 5); banks(n, 3, 4, 5, 31);
    W(n, 0x7EF0, 20); W(n, 0x7EF5, 99); EQ("CHR 0 (2K)", P(n, 0), 20); EQ("CHR 7", P(n, 0x1C00), 99);
    W(n, 0x7EF6, 1); mir(n, 0, 1, 0, 1);
    W(n, 0x7F10, 0x12); EQ("RAM locked", R(n, 0x7F10) == 0x12, 0);
    W(n, 0x7EF8, 0xA3); W(n, 0x7F10, 0x12); EQ("internal RAM", R(n, 0x7F10), 0x12);
    nes_destroy(n);
    strcpy(ctx, "Taito X1-017 (82)"); n = cart(82, 0, 256, 256, 0);
    W(n, 0x7EFA, 3 << 2); W(n, 0x7EFB, 4 << 2); W(n, 0x7EFC, 5 << 2); banks(n, 3, 4, 5, 31);
    W(n, 0x7EF7, 0xCA); W(n, 0x6010, 0x66); EQ("RAM bank 1 enabled", R(n, 0x6010), 0x66);
    W(n, 0x7EF6, 2); W(n, 0x7EF0, 20); EQ("CHR mode swap: 2K bank at $1000", P(n, 0x1000), 20);
    nes_destroy(n);
    strcpy(ctx, "CPROM (13)"); n = cart(13, 0, 32, 0, 0);
    n->prg[0x100] = 0xFF;
    W(n, 0x8100, 2); PW(n, 0x1010, 0x2A); W(n, 0x8100, 1); PW(n, 0x1010, 0x1A); W(n, 0x8100, 2); EQ("CHR-RAM bank 2", P(n, 0x1010), 0x2A);
    W(n, 0x8100, 1); EQ("CHR-RAM bank 1", P(n, 0x1010), 0x1A);
    nes_destroy(n);
    strcpy(ctx, "NTDEC 2722 (40)"); n = cart(40, 0, 64, 8, 0);
    banks(n, 4, 5, 0, 7); EQ("bank 6 at $6000", R(n, 0x6000), 6); W(n, 0xE000, 3); EQ("$C000 switch", R(n, 0xC000), 3);
    W(n, 0xA000, 0); EQ("IRQ after 4096 cycles", irq_after(n, 5000), 4096);
    { int c = 0; while (cart_irq(n) && c < 5000) { cart_m2(n); c++; } EQ("self-acknowledges 4096 cycles later", c, 4096); }
    EQ("and fires again 4096 cycles after that", irq_after(n, 5000), 4096);
    W(n, 0x8000, 0); EQ("$8000 acknowledges", cart_irq(n), 0); EQ("stays off while disabled", irq_after(n, 9000), -1);
    nes_destroy(n);
    strcpy(ctx, "CNROM protection (185 sub 4)"); n = cart(185, 4, 32, 8, 0);
    memset(n->prg, 0xFF, 0x8000); W(n, 0x8000, 0x00); EQ("CHR enabled", P(n, 0x0055), 0); W(n, 0x8000, 0x01); EQ("CHR disabled: open bus", P(n, 0x0055), 0x55);
    nes_destroy(n);
    strcpy(ctx, "RAMBO-1 (64)"); n = cart(64, 0, 256, 256, 0);
    W(n, 0x8000, 6); W(n, 0x8001, 3); W(n, 0x8000, 7); W(n, 0x8001, 4); W(n, 0x8000, 15); W(n, 0x8001, 5); banks(n, 3, 4, 5, 31);
    W(n, 0x8000, 0x46); banks(n, 5, 3, 4, 31);
    W(n, 0x8000, 0x20); W(n, 0x8001, 10); W(n, 0x8000, 0x28); W(n, 0x8001, 11); EQ("1K mode R0", P(n, 0), 10); EQ("1K mode R8", P(n, 0x400), 11);
    W(n, 0xC000, 2); W(n, 0xC001, 1); W(n, 0xE001, 0); int c = irq_after(n, 100); ok(c > 0 && c <= 20, "cycle-mode IRQ", c, 16);
    nes_destroy(n);
    strcpy(ctx, "MMC6 (4 sub 1)"); n = cart(4, 1, 256, 256, 0);
    W(n, 0x8000, 0x20); W(n, 0xA001, 0xF0); W(n, 0x7010, 0x5A); EQ("RAM write/read", R(n, 0x7010), 0x5A); EQ("mirrored every 1 KB", R(n, 0x7410), 0x5A);
    W(n, 0xA001, 0xA0); W(n, 0x7010, 0x11); EQ("write-protected", R(n, 0x7010), 0x5A);
    W(n, 0xA001, 0x80); EQ("disabled half reads $00", R(n, 0x7010), 0x00);
    W(n, 0x8000, 0x00); { uint8_t q = 0x77; int drv = cart_cpu_read(n, 0x7010, &q); EQ("master disable: open bus", drv, 0); }
    nes_destroy(n);
    strcpy(ctx, "TxSROM (118)"); n = cart(118, 0, 256, 256, 0);
    W(n, 0x8000, 0); W(n, 0x8001, 0x80); W(n, 0x8000, 1); W(n, 0x8001, 0x00); mir(n, 1, 1, 0, 0);
    nes_destroy(n);
    strcpy(ctx, "TQROM (119)"); n = cart(119, 0, 128, 64, 0);
    W(n, 0x8000, 2); W(n, 0x8001, 0x43); PW(n, 0x1000, 0x77); EQ("CHR-RAM page", P(n, 0x1000), 0x77);
    W(n, 0x8000, 2); W(n, 0x8001, 0x05); EQ("CHR-ROM page", P(n, 0x1000), 5);
    W(n, 0x8000, 2); W(n, 0x8001, 0x43); EQ("RAM kept its data", P(n, 0x1000), 0x77);
    nes_destroy(n);
}
/* ---------------- MMC5 ---------------- */
static void test_mmc5(void) {
    strcpy(ctx, "MMC5 (5)"); Nes *n = cart(5, 0, 512, 512, 0);
    W(n, 0x5100, 3); W(n, 0x5114, 0x80 | 3); W(n, 0x5115, 0x80 | 4); W(n, 0x5116, 0x80 | 5); W(n, 0x5117, 9); banks(n, 3, 4, 5, 9);
    W(n, 0x5100, 0); W(n, 0x5117, 0x0B); banks(n, 8, 9, 10, 11);
    W(n, 0x5100, 1); W(n, 0x5115, 0x80 | 6); W(n, 0x5117, 0x0F); banks(n, 6, 7, 14, 15);
    W(n, 0x5100, 3); W(n, 0x5114, 0x01); W(n, 0x8010, 0x99); EQ("RAM window locked", R(n, 0x8010) == 0x99, 0);
    W(n, 0x5102, 2); W(n, 0x5103, 1); W(n, 0x8010, 0x99); EQ("RAM window at $8000 (bank 1)", R(n, 0x8010), 0x99);
    W(n, 0x5113, 1); EQ("same RAM at $6000 bank 1", R(n, 0x6010), 0x99);
    W(n, 0x5101, 3); for (int k = 0; k < 8; k++) W(n, (uint16_t)(0x5120 + k), (uint8_t)(70 + k));
    int e[8] = { 70, 71, 72, 73, 74, 75, 76, 77 }; chrs(n, e);
    W(n, 0x5130, 1); W(n, 0x5121, 2); EQ("upper CHR bits ($5130)", P(n, 0x0400), (256 + 2) & 0xFF);
    W(n, 0x5105, 0xE4); EQ("NT 0 CIRAM A", ntpage(n, 0), 0); EQ("NT 1 CIRAM B", ntpage(n, 1), 1);
    W(n, 0x5104, 0); n->s.cart.exram[0x155] = 0x3C; EQ("NT 2 = ExRAM", ntpage(n, 2), 100 + 0x3C);
    W(n, 0x5106, 0x21); W(n, 0x5107, 2); EQ("NT 3 = fill tile", ntpage(n, 3), 100 + 0x21); EQ("fill attribute", P(n, 0x2FC0), 0xAA);
    W(n, 0x5205, 200); W(n, 0x5206, 123); EQ("multiplier low", R(n, 0x5205), (200 * 123) & 0xFF); EQ("multiplier high", R(n, 0x5206), (200 * 123) >> 8);
    W(n, 0x5015, 1); W(n, 0x5000, 0xBF); W(n, 0x5002, 0xFD); W(n, 0x5003, 0x08);   /* 50% duty, period $FD */
    near("pulse 1 pitch (Hz)", pitch(n, 16, 0.2, 200, 800), 1789773.0 / (16 * 254), 4);
    nes_destroy(n);
    /* scanline IRQ and extended attributes with the real PPU fetch pattern */
    strcpy(ctx, "MMC5 + PPU"); n = cart(5, 0, 512, 512, 0);
    Ppu *p = &n->s.ppu; p->ignore_writes = 0;
    W(n, 0x5101, 1); W(n, 0x5123, 0); W(n, 0x5127, 0); W(n, 0x5105, 0x00); W(n, 0x5203, 100); W(n, 0x5204, 0x80);
    cpu_bus_write(n, 0x2000, 0x00); cpu_bus_write(n, 0x2001, 0x0A);
    p->scanline = 261; p->dot = 0;
    int irq_line = -1, inframe_seen = 0;
    int dots = 0;   /* M2 runs once per CPU cycle = every 3 PPU dots */
    while (!(p->scanline == 200)) { ppu_run_to(n, p->mclk + 4); if (++dots % 3 == 0) cart_m2(n); if (cart_irq(n) && irq_line < 0) irq_line = p->scanline; if (n->s.cart.mreg[67]) inframe_seen = 1; }
    EQ("IRQ on the programmed scanline", irq_line, 100); EQ("in-frame flag while rendering", inframe_seen, 1);
    EQ("$5204: IRQ pending + in frame", R(n, 0x5204), 0xC0); EQ("read acknowledges", cart_irq(n), 0);
    while (!(p->scanline == 245)) { ppu_run_to(n, p->mclk + 4); if (++dots % 3 == 0) cart_m2(n); }
    EQ("out of frame in vblank", R(n, 0x5204) & 0x40, 0);
    W(n, 0x5104, 1);                                            /* extended attributes */
    for (int i = 0; i < 0x3C0; i++) n->s.cart.exram[i] = (uint8_t)(0x80 | 63);   /* palette 2, 4 KB bank 63 */
    p->pal[0] = 0x0F; p->pal[11] = 0x2A;
    while (!(p->scanline == 20 && p->dot == 300)) { ppu_run_to(n, p->mclk + 4); if (++dots % 3 == 0) cart_m2(n); }
    EQ("ext-attr tile: palette 2, color 3", n->fb[20 * 256 + 64] & 0x3F, 0x2A);
    nes_destroy(n);
}

/* console models: PPU warm-up after power-on and what the reset button reaches */
static void test_console(void) {
    static const char *NAME[4] = { "", "NES front-loader", "NES top-loader", "Famicom" };
    for (int model = 1; model <= 3; model++) {
        snprintf(ctx, sizeof ctx, "console: %s", NAME[model]);
        Nes *n = cart(0, 0, 32, 0, 0); Ppu *p = &n->s.ppu;
        n->console_setting = model; nes_power(n);
        n->s.ciram[0x100] = 0;
        cpu_bus_write(n, 0x2006, 0x21); cpu_bus_write(n, 0x2006, 0x00); ppu_run_to(n, p->mclk + 40); cpu_bus_write(n, 0x2007, 0x5A);
        EQ("PPU writes right after power-on", n->s.ciram[0x100] == 0x5A, model != CON_NES);
        p->ignore_writes = 0; cpu_bus_write(n, 0x2000, 0x80);
        nes_reset(n);
        EQ("PPUCTRL survives the reset button", p->ctrl == 0x80, model != CON_NES);
        nes_destroy(n);
    }
    strcpy(ctx, "console: auto");
    Nes *n = cart(85, 2, 64, 0, 0); EQ("VRC7 board -> Famicom", n->console, CON_FAMICOM); nes_destroy(n);
    n = cart(4, 0, 64, 64, 0); EQ("MMC3 board -> NES", n->console, CON_NES); nes_destroy(n);
}

/* a cartridge with a real program: main loop + NMI handler, 8 KB battery RAM the game never writes */
static const uint8_t CODE[] = { 0x78,0xD8,0xA2,0xFF,0x9A,0xA9,0x80,0x8D,0x00,0x20, 0xE6,0x10,0x4C,0x0A,0x80,
                                0x48,0xA5,0x10,0x8D,0x00,0x03,0x68,0x40 };
static Nes *code_cart(int console) {
    size_t prg = 32768, chr = 8192, size = 16 + prg + chr;
    uint8_t *d = calloc(1, size);
    memcpy(d, "NES\x1a", 4); d[4] = 2; d[5] = 1; d[6] = 0x02; d[7] = 0x08; d[10] = 0x70;
    memcpy(d + 16, CODE, sizeof CODE);
    uint8_t *v = d + 16 + 0x7FFA; v[0] = 0x0F; v[1] = 0x80; v[2] = 0x00; v[3] = 0x80; v[4] = 0x0A; v[5] = 0x80;
    Nes *n = nes_create(); char err[128];
    n->console_setting = console;
    if (!nes_load_rom(n, d, size, 0, err, sizeof err)) { printf("  load failed: %s\n", err); exit(1); }
    free(d); return n;
}
static int pin_of(Nes *n, int sig) { for (int p = 1; p <= n->conn.npins; p++) if (conn_pin_signal(&n->conn, p) == sig) return p; return 0; }
static int open_pins(Nes *n) { int k = 0; for (int p = 1; p <= n->conn.npins; p++) k += !n->conn.conn[p]; return k; }

/* cartridge audio: NES = EXP6 only; Famicom = the console's own audio loops through pins 45/46 */
static void test_connector(void) {
    strcpy(ctx, "audio path: NES-001"); Nes *n = cart(24, 0, 128, 128, 0);
    n->console_setting = CON_NES; nes_power(n);
    W(n, 0x9000, 0x7F); W(n, 0x9001, 0x40); W(n, 0x9002, 0x80);
    float on = 0; for (int i = 0; i < 2000; i++) { cart_m2(n); on += fabsf(cart_audio(n)); }
    n->conn.mode[pin_of(n, S_EXP0 + 6)] = PIN_OFF; conn_update(n);
    float off = 0; for (int i = 0; i < 2000; i++) { cart_m2(n); off += fabsf(cart_audio(n)); }
    ok(on > 1 && off == 0, "VRC6 audio present, then cut at EXP6", (long)(off * 1000), 0);
    EQ("console audio doesn't pass the cart", cart_audio_path(n, 0.25f) == 0.25f, 1);
    nes_destroy(n);
    strcpy(ctx, "audio path: Famicom"); n = cart(24, 0, 128, 128, 0);
    EQ("VRC6 board runs on a Famicom (auto)", n->conn.slot, SLOT_FC);
    EQ("60 contacts", n->conn.npins, 60);
    W(n, 0x9000, 0x7F); W(n, 0x9001, 0x40); W(n, 0x9002, 0x80);
    on = 0; for (int i = 0; i < 2000; i++) { cart_m2(n); on += fabsf(cart_audio_path(n, 0.25f) - 0.25f); }
    ok(on > 1, "VRC6 audio mixed into the loop", (long)on, 1);
    EQ("pin 45 is audio from the 2A03", pin_of(n, S_AIN), 45);
    EQ("pin 46 is audio to RF", pin_of(n, S_AOUT), 46);
    n->conn.mode[46] = PIN_OFF; conn_update(n);
    EQ("open pin 46 silences everything", cart_audio_path(n, 0.25f) == 0.0f, 1);
    nes_destroy(n);
}

/* contact physics: tray, springs, oxidation, tilting */
static void test_contacts(void) {
    strcpy(ctx, "contacts"); Nes *n = cart(0, 0, 32, 8, 0);
    n->console_setting = CON_NES; nes_power(n);
    EQ("pristine connector: perfect", n->conn.perfect, 1);
    EQ("pin 36 is +5V, 72 GND", conn_pin_signal(&n->conn, 36) == S_VCC && conn_pin_signal(&n->conn, 72) == S_GND, 1);
    n->conn.tray = 0; conn_update(n);
    EQ("tray up, factory springs: contact lost", n->conn.perfect, 0);
    conn_set_condition(&n->conn, COND_AFTERMARKET, 7); conn_update(n);
    EQ("tray up, aftermarket connector: works anyway", n->conn.perfect, 1);
    n->conn.tray = 1; conn_set_condition(&n->conn, COND_WORN, 7); conn_update(n);
    int worn_bad = open_pins(n) + n->conn.nflaky;
    ok(!n->conn.perfect && worn_bad > 0, "worn connector: bad contacts even pushed down", worn_bad, 1);
    n->conn.press = 1; conn_update(n);
    ok(open_pins(n) + n->conn.nflaky < worn_bad, "pressing on the cart helps", open_pins(n) + n->conn.nflaky, worn_bad);
    n->conn.press = 0; n->conn.moist = 1; conn_update(n);
    ok(open_pins(n) + n->conn.nflaky < worn_bad, "breath moisture conducts for a while", open_pins(n) + n->conn.nflaky, worn_bad);
    nes_destroy(n);
    strcpy(ctx, "contacts: Famicom tilt"); n = cart(0, 0, 32, 8, 0);
    n->console_setting = CON_FAMICOM; nes_power(n);
    EQ("Famicom slot", n->conn.slot, SLOT_FC);
    n->conn.roll = 1.0f; conn_update(n);
    int row; float x; conn_pin_geom(&n->conn, 30, &row, &x);
    ok(!n->conn.conn[30] && n->conn.conn[1], "lifting the right end opens pin 30, pin 1 still touches", n->conn.conn[30] * 10 + n->conn.conn[1], 1);
    nes_destroy(n);
    strcpy(ctx, "contacts: NES-101"); n = cart(0, 0, 32, 8, 0);
    n->console_setting = CON_NES101; nes_power(n);
    EQ("top-loader: CIC pins carry nothing", conn_pin_signal(&n->conn, 34) == S_NONE && conn_pin_signal(&n->conn, 71) == S_NONE, 1);
    nes_destroy(n);
}

/* 10NES lock: die loop = reset line low 0.5 s / high 0.5 s, LED with it; reset button restarts it */
static void test_cic(void) {
    strcpy(ctx, "CIC lock"); Nes *n = code_cart(CON_NES);
    for (int f = 0; f < 5; f++) nes_run_frame(n);
    EQ("good key: running", n->in_reset, 0);
    n->conn.mode[35] = PIN_OFF; conn_update(n);
    nes_run_frame(n);
    EQ("CIC toMB open: lock asserts reset", n->in_reset, 1);
    EQ("power LED off", n->led, 0);
    EQ("NES-001 picture disabled", n->fb[1000], 0x0F);
    int low = 1, flips = 0, len = 1;
    for (int f = 0; f < 120; f++) { nes_run_frame(n); if (n->in_reset != low) { flips++; low = n->in_reset; ok(len == 30, "half period = 30 frames", len, 30); len = 0; } len++; }
    EQ("1 Hz square wave (4 edges in 2 s)", flips, 4);
    n->conn.mode[35] = PIN_AUTO; conn_update(n);
    for (int f = 0; f < 100; f++) nes_run_frame(n);
    EQ("contact back: still blinking until reset", n->lock_dead, 1);
    n->reset_pulse = 3;
    for (int f = 0; f < 10; f++) nes_run_frame(n);
    EQ("reset button: new handshake succeeds", n->lock_dead + n->in_reset, 0);
    n->conn.mode[70] = PIN_FLAKY; conn_update(n);
    for (int f = 0; f < 3; f++) nes_run_frame(n);
    EQ("intermittent CIC contact fails within frames", n->lock_dead, 1);
    n->conn.mode[70] = PIN_AUTO; conn_update(n); n->cic_key = KEY_OTHER_REGION; n->reset_pulse = 3;
    for (int f = 0; f < 6; f++) nes_run_frame(n);
    EQ("cartridge for another region: locked out", n->lock_dead, 1);
    nes_destroy(n);
    strcpy(ctx, "CIC lock: Famicom"); n = code_cart(CON_FAMICOM); n->cic_key = KEY_NONE;
    for (int f = 0; f < 60; f++) nes_run_frame(n);
    EQ("no lockout on a Famicom", n->in_reset + n->lock_dead, 0);
    nes_destroy(n);
}

/* holding RESET: the CPU stays parked; on release it runs the reset vector */
static void test_reset_hold(void) {
    for (int model = CON_NES; model <= CON_FAMICOM; model += 2) {
        snprintf(ctx, sizeof ctx, "reset button: %s", model == CON_NES ? "NES-001" : "Famicom");
        Nes *n = code_cart(model);
        for (int f = 0; f < 5; f++) nes_run_frame(n);
        n->reset_button = 1; nes_run_frame(n);
        uint16_t pc = n->s.cpu.pc; uint8_t z = n->s.ram[0x10];
        for (int f = 0; f < 10; f++) nes_run_frame(n);
        EQ("CPU parked while held", n->s.cpu.pc == pc && n->s.ram[0x10] == z, 1);
        EQ("PPU held only on the NES-001", n->s.ppu.held, model == CON_NES);
        n->reset_button = 0; nes_run_frame(n);
        ok(n->s.cpu.pc >= 0x800A && n->s.cpu.pc <= 0x8016, "released: back in the game's loop", n->s.cpu.pc, 0x800A);
        nes_destroy(n);
    }
}

/* switching off: holding RESET protects battery RAM; otherwise it is occasionally hit */
static void test_power_off(void) {
    strcpy(ctx, "power-off");
    int hit = 0, held_hit = 0;
    for (int t = 0; t < 200; t++) {
        for (int hold = 0; hold < 2; hold++) {
            Nes *n = code_cart(CON_NES); n->seed = 0x9E3779B1u * (uint32_t)(t + 1); nes_power(n);
            for (int f = 0; f < 5 + t % 7; f++) nes_run_frame(n);
            memset(n->s.cart.wram, 0x5A, n->wram_size);
            n->reset_button = hold; nes_power_off(n);
            int c = 0; for (uint32_t i = 0; i < n->wram_size; i++) c += n->s.cart.wram[i] != 0x5A;
            if (c) { if (hold) held_hit++; else hit++; }
            nes_destroy(n);
        }
    }
    EQ("holding RESET: never corrupted", held_hit, 0);
    ok(hit >= 2 && hit <= 30, "not holding it: corrupted now and then (of 200)", hit, 10);
}

int main(void) {
    int vs[][2] = { {21,1},{21,2},{21,0},{22,0},{23,1},{23,2},{23,3},{23,0},{25,1},{25,2},{25,3},{25,0} };
    for (unsigned i = 0; i < sizeof vs / sizeof vs[0]; i++) test_vrc24(vs[i][0], vs[i][1]);
    test_vrc1(); test_vrc3(); test_vrc6(24); test_vrc6(26); test_vrc7(1); test_vrc7(2);
    test_sunsoft(); test_namco(); test_bandai(); test_misc(); test_mmc5(); test_console(); test_connector(); test_contacts(); test_cic(); test_reset_hold(); test_power_off();
    printf("%d checks, %d failures\n", checks, fails);
    return fails != 0;
}
