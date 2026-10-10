/* dma_trace - checks NESSYSINTEGER's DMA / APU cadence against the worked examples
 * on the NESdev wiki "DMA" page and AccuracyCoin's documented frame-IRQ and
 * controller-strobe rules.
 *   gcc -O2 -o dma_trace tests/dma_trace.c src/cpu.c src/ppu.c src/apu.c src/cart.c src/nes.c -lm */
#include <stdio.h>
#include <string.h>
#include "../src/nes.h"

#define MAXT 700000
static char K[MAXT]; static uint64_t C[MAXT]; static uint16_t AD[MAXT]; static int nt;
static uint8_t rom[16 + 0x8000], P[4096];
static int pl, fails, checks;

static void tr(Nes *n, char k, uint16_t a, uint8_t v) { (void)v; if (nt < MAXT) { K[nt] = k; C[nt] = n->s.cpu.cycles; AD[nt] = a; nt++; } }
static void b(int x) { P[pl++] = (uint8_t)x; }
static void setup(int ctrl, int len) {          /* SEI; $4010=ctrl; $4012=$00 (sample at $C000); $4013=len */
    pl = 0; b(0x78);
    b(0xA9); b(ctrl); b(0x8D); b(0x10); b(0x40);
    b(0xA9); b(0x00); b(0x8D); b(0x12); b(0x40);
    b(0xA9); b(len);  b(0x8D); b(0x13); b(0x40);
}
static void nops(int k) { while (k-- > 0) b(0xEA); }
static void flip(void) { b(0xA5); b(0x00); }     /* LDA zp: 3 cycles, flips the parity of what follows */
static void sta(int v, int addr) { b(0xA9); b(v); b(0x8D); b(addr & 0xFF); b(addr >> 8); }
static Nes *run(int steps) {
    memset(rom, 0xEA, sizeof rom);
    memcpy(rom, "NES\x1a\x02", 5); memset(rom + 5, 0, 11);
    memcpy(rom + 16, P, (size_t)pl);
    rom[16 + 0x1000] = 0x40;                     /* $9000: RTI */
    rom[16 + 0x7FFA] = 0x00; rom[16 + 0x7FFB] = 0x90; rom[16 + 0x7FFC] = 0x00; rom[16 + 0x7FFD] = 0x80;
    rom[16 + 0x7FFE] = 0x00; rom[16 + 0x7FFF] = 0x90;
    Nes *n = nes_create(); char err[64];
    nes_load_rom(n, rom, sizeof rom, 0, err, sizeof err);
    nt = 0; n->trace = tr;
    for (int i = 0; i < steps; i++) cpu_step(n);
    return n;
}
static int bus(char k) { return k && strchr("RWHdGop", k) != NULL; }
static int nth_write(uint16_t a, int which) { for (int i = 0; i < nt; i++) if (K[i] == 'W' && AD[i] == a && !which--) return i; return -1; }
static void cyc_str(int i, int count, char *out) {
    out[0] = 0;
    for (; i < nt && count; i++) if (bus(K[i])) { char t[4] = { K[i], (C[i] & 1) ? 'g' : 'p', ' ', 0 }; strcat(out, t); count--; }
    if (*out) out[strlen(out) - 1] = 0;
}
static void result(const char *name, int ok, const char *extra) {
    checks++; if (!ok) fails++;
    printf("%-50s %s%s\n", name, ok ? "ok" : "MISMATCH", extra ? extra : "");
}
static void expect(const char *name, const char *got, const char *want) {
    result(name, !strcmp(got, want), NULL);
    if (strcmp(got, want)) printf("    got:  %s\n    want: %s\n", got, want);
}

/* every DMC burst: halt on the first non-write cycle at/after the scheduled attempt,
 * one dummy cycle, an alignment cycle if needed, then the get; doomed ones only halt */
static void validate(const char *name) {
    int bursts = 0, bad = 0;
    for (int i = 0; i < nt; i++) {
        if (K[i] != 'H') continue;
        bursts++;
        int e = i; while (e >= 0 && !strchr("LSA", K[e])) e--;
        if (e < 0) { bad++; continue; }
        uint64_t attempt = C[e] + 1;
        int ok = C[i] >= attempt;
        for (int j = e + 1; j < i; j++) if (bus(K[j]) && C[j] >= attempt && K[j] != 'W') ok = 0;
        char seq[16]; int m = 0;
        for (int j = i; j < nt && m < 6; j++) if (bus(K[j])) { seq[m++] = K[j]; if (K[j] == 'R' || K[j] == 'W') break; }
        seq[m] = 0;
        const char *want = K[e] == 'A' ? "HR" : (C[i] & 1) ? "HdGR" : "HddGR";
        if (strcmp(seq, want)) ok = 0;
        if (!ok && ++bad <= 3) printf("    burst @%llu (%c, sched %c@%llu): %s want %s\n", (unsigned long long)C[i],
                                      (C[i] & 1) ? 'g' : 'p', K[e], (unsigned long long)C[e], seq, want);
    }
    char ex[48]; snprintf(ex, sizeof ex, "  (%d bursts)", bursts);
    result(name, !bad && bursts > 3, ex);
}

static int fc_flag(int inhibit, int read_rel, int lo, int hi) {   /* bit (rel-lo) = frame IRQ flag at S+rel */
    Nes *n = nes_create();
    nes_power(n);
    n->s.cpu.cycles = 1001;                                          /* write on a get cycle: reset 4 cycles later */
    apu_write(n, 0x4017, inhibit ? 0x40 : 0x00);
    uint64_t S = n->s.cpu.cycles + 4 + 29828;
    int mask = 0;
    while (n->s.cpu.cycles < S + (uint64_t)hi + 1) {
        n->s.cpu.cycles++;
        apu_cycle(n);
        long rel = (long)(n->s.cpu.cycles - S);
        if (rel == read_rel) apu_status(n, 1);
        if (rel >= lo && rel <= hi && (n->s.cpu.irq_src & IRQ_FRAME)) mask |= 1 << (rel - lo);
    }
    nes_destroy(n);
    return mask;
}


static uint8_t TV[MAXT];
static void trv(Nes *n, char k, uint16_t a, uint8_t v) { if (nt < MAXT) { K[nt] = k; C[nt] = n->s.cpu.cycles; AD[nt] = a; TV[nt] = v; nt++; } }
/* DMC fetches landing on a halted read of `reg` (AccuracyCoin "DMC DMA Bus Conflicts" and
 * "Internal Data Bus"): the 6502's address bits 15-5 plus the DMA's bits 4-0 activate 2A03
 * registers. Sample at $EFC0: 32 x $00 then 32 x $FF, looping. Returns mismatches. */
static int conflict_scan(uint16_t reg, int *checked) {
    static uint8_t cr[16 + 0x8000];
    memset(cr, 0xEA, sizeof cr); memcpy(cr, "NES\x1a\x02", 5); memset(cr + 5, 0, 11);
    uint8_t *q = cr + 16; int o = 0, bad = 0;
    static const uint8_t init[] = { 0x78, 0xA9,1,0x8D,0x16,0x40, 0xA9,0,0x8D,0x16,0x40,
        0xAD,0x16,0x40, 0xAD,0x16,0x40, 0xAD,0x16,0x40, 0xAD,0x16,0x40, 0xAD,0x16,0x40, 0xAD,0x16,0x40, 0xAD,0x16,0x40, 0xAD,0x16,0x40,
        0xA9,0x04,0x8D,0x13,0x40, 0xA9,0xBF,0x8D,0x12,0x40, 0xA9,0x4F,0x8D,0x10,0x40, 0xA9,0x10,0x8D,0x15,0x40 };
    memcpy(q, init, sizeof init); o = (int)sizeof init;
    int loop = 0x8000 + o;
    for (int i = 0; i < 2000; i++) { q[o++] = 0xAD; q[o++] = (uint8_t)reg; q[o++] = (uint8_t)(reg >> 8); }
    q[o++] = 0x4C; q[o++] = (uint8_t)loop; q[o++] = (uint8_t)(loop >> 8);
    for (int k = 0; k < 64; k++) q[0x6FC0 + k] = k < 32 ? 0x00 : 0xFF;
    q[0x7FFC] = 0x00; q[0x7FFD] = 0x80;
    Nes *n = nes_create(); char err[64];
    nes_load_rom(n, cr, sizeof cr, 0, err, sizeof err);
    nt = 0; n->trace = trv;
    for (int i = 0; i < 100000; i++) cpu_step(n);
    *checked = 0;
    for (int i = 0; i < nt; i++) {
        if (K[i] != 'G') continue;
        int h = i; while (h > 0 && K[h] != 'H') h--;
        int r = i + 1; while (r < nt && K[r] != 'R') r++;
        if (AD[h] != reg || r >= nt || AD[r] != reg) continue;
        int k = (AD[i] - 0xEFC0) & 0x7F, lo = k & 0x1F;
        if (k >= 64) continue;
        uint8_t samp = k < 32 ? 0x00 : 0xFF, got = TV[r], want;
        (*checked)++;
        if (reg == 0x4000) {          /* CPU sees the cartridge-side bus the DMA left behind */
            want = lo == 0x16 ? (uint8_t)((samp & 0xE0) | 1) : lo == 0x17 ? (uint8_t)(samp & 0xE0) : samp;
            if (got != want) bad++;
        } else if (lo == 0x15 && (got & 0x20)) bad++;   /* $4015: bit 5 comes from the internal bus only */
    }
    nes_destroy(n);
    return bad;
}

int main(void) {
    char got[512];
    puts("-- load DMA (wiki: halt on the get of the 2nd APU cycle after the $4015 write)");
    for (int par = 0; par < 2; par++) {
        setup(0x0F, 0); if (par) flip(); sta(0x10, 0x4015); nops(20); nes_destroy(run(40));
        int w = nth_write(0x4015, 0), g = (int)(C[w] & 1);
        cyc_str(w, g ? 8 : 7, got);
        expect(g ? "load DMA, write on get" : "load DMA, write on put", got, g ? "Wg Rp Rg Rp Hg dp Gg Rp" : "Wp Rg Rp Hg dp Gg Rp");
    }
    for (int par = 0; par < 2; par++) for (int zp = 0; zp < 2; zp++) {
        setup(0x0F, 0); if (par) flip(); sta(0x10, 0x4015);
        if (zp) { b(0x85); b(0x00); } else { b(0x8D); b(0x00); b(0x00); }
        nops(20); nes_destroy(run(40));
        int w = nth_write(0x4015, 0), g = (int)(C[w] & 1);
        const char *want = g ? (zp ? "Wg Rp Rg Wp Hg dp Gg Rp" : "Wg Rp Rg Rp Wg Hp dg dp Gg Rp")
                             : (zp ? "Wp Rg Rp Wg Hp dg dp Gg Rp" : "Wp Rg Rp Hg dp Gg Rp Wg");
        cyc_str(w, (int)(strlen(want) + 1) / 3, got);
        char nm[64]; snprintf(nm, sizeof nm, "load DMA, write on %s, then STA %s", g ? "get" : "put", zp ? "zp" : "abs");
        expect(nm, got, want);
    }

    puts("-- reload DMAs (wiki: halt on a put; failed halts retry next cycle)");
    static const char *names[4] = { "reload DMAs during NOPs", "reload DMAs during STA zp", "reload DMAs during INC zp (2 writes)",
                                    "reload DMAs during JSR/RTS (stack writes)" };
    for (int s = 0; s < 4; s++) {
        setup(0x0F, 0xFF); sta(0x10, 0x4015);          /* loop starts at $8015 */
        if (s == 0) nops(30);
        else if (s == 1) for (int i = 0; i < 10; i++) { b(0x85); b(0x00); }
        else if (s == 2) for (int i = 0; i < 10; i++) { b(0xE6); b(0x00); }
        else { b(0x20); b(0x1B); b(0x80); b(0x4C); b(0x15); b(0x80); b(0x60); }
        if (s < 3) { b(0x4C); b(0x15); b(0x80); }
        nes_destroy(run(20000));
        validate(names[s]);
    }

    puts("-- explicit stop ($4015=0) vs. the would-be reload at wrap S");
    static int ex[24][3];
    for (int k = 0; k < 230; k++) for (int par = 0; par < 2; par++) {
        setup(0x0F, 0xFF); sta(0x10, 0x4015); if (par) flip(); nops(400 + k); sta(0x00, 0x4015); nops(400);
        nes_destroy(run(k + 830));
        int w = nth_write(0x4015, 1), s = -1, o = 0;
        for (int i = 0; i < nt; i++) if (K[i] == 'w' && C[i] + 8 >= C[w]) { s = i; break; }
        if (w < 0 || s < 0) continue;
        long d = (long)C[s] - (long)C[w];
        for (int i = w + 1; i < nt; i++) if (K[i] == 'H') { int j = i + 1; while (j < nt && !bus(K[j])) j++; o = K[j] == 'd' ? 2 : 1; break; }
        if (d >= -8 && d < 16) ex[d + 8][o]++;
    }
    int exok = 1;
    for (int d = -1; d <= 5; d++) if (!(ex[d+8][0] + ex[d+8][1] + ex[d+8][2])) exok = 0;
    for (int d = -1; d < 16; d++) {
        int want = d >= 3 ? 0 : d >= 1 ? 1 : 2;   /* none / aborted after 1 halt cycle / full */
        for (int o = 0; o < 3; o++) if (o != want && ex[d + 8][o]) exok = 0;
    }
    printf("    S-W:  -1  0  1  2  3  4  5 ..   (N=no DMA, A=aborted 1-cycle halt, F=full DMA)\n    got: ");
    for (int d = -1; d <= 5; d++) { int o = ex[d+8][1] ? 1 : ex[d+8][2] ? 2 : 0; printf("  %c", "NAF"[o]); }
    printf("\n");
    result("write in the APU cycle before S aborts the DMA", exok, NULL);

    puts("-- implicit stop (1-byte sample started k cycles before wrap S)");
    static int im[64][4];
    for (int k = 0; k < 230; k++) for (int par = 0; par < 2; par++) {
        setup(0x0F, 0x00); if (par) flip(); nops(400 + k); sta(0x10, 0x4015); nops(200);
        nes_destroy(run(k + 630));
        int w = nth_write(0x4015, 0), s = -1, full = 0, ab = 0;
        for (int i = w; i < nt; i++) if (K[i] == 'w' || K[i] == 'e') { s = i; break; }
        if (s < 0) continue;
        for (int i = w + 1; i < nt; i++) {   /* count completed fetches and 1-cycle (aborted) halts */
            if (K[i] == 'G') full++;
            if (K[i] == 'H') { int j = i + 1; while (j < nt && !bus(K[j])) j++; if (K[j] == 'R' || K[j] == 'W') ab++; }
        }
        long d = (long)C[s] - (long)C[w];
        int o = (full == 1 && !ab) ? 0 : (full == 1 && ab == 1) ? 1 : (full == 2 && !ab) ? 2 : 3;
        if (d >= 0 && d < 64) im[d][o]++;
    }
    int imok = 1;
    for (int d = 3; d <= 10; d++) if (!(im[d][0] + im[d][1] + im[d][2] + im[d][3])) imok = 0;
    printf("    S-W:   3  4  5  6  7  8  9 10   (N=normal, A=+aborted DMA, U=+unexpected DMA, ?=other)\n    got: ");
    for (int d = 3; d <= 10; d++) { int o = 0; for (int q = 0; q < 4; q++) if (im[d][q]) o = q; printf("  %c", "NAU?"[o]); }
    printf("\n");
    for (int d = 0; d < 64; d++) { int want = (d == 7 || d == 8) ? 1 : (d == 5 || d == 6) ? 2 : 0;
        for (int o = 0; o < 4; o++) if (o != want && im[d][o]) imok = 0; }
    result("implicit stop: aborted at S-8/S-7, unexpected at S-6/S-5", imok, NULL);

    puts("-- $4015 enable right before a DMC timer clock (AccuracyCoin DMC L/M/N)");
    {
        int early = 0, near = 0;
        /* the 1-byte sample's byte sits in the buffer until the first timer wrap (cycle ~753
         * after power-on); re-enable playback in the cycles just before that wrap */
        for (int k = 0; k < 40; k++) for (int par = 0; par < 2; par++) {
            setup(0x0F, 0x00); sta(0x10, 0x4015); if (par) flip(); nops(330 + k); sta(0x10, 0x4015); nops(60);
            nes_destroy(run(k + 420));
            int w = nth_write(0x4015, 1);
            if (w < 0) continue;
            uint64_t ok_from = C[w] + ((C[w] & 1) ? 4 : 3);
            for (int i = w + 1; i < nt; i++) if (K[i] == 'H') { if (C[i] < ok_from) early++; break; }
            for (int i = 0; i < nt; i++) if (K[i] == 'w' && C[i] >= C[w] && C[i] <= C[w] + 2) { near++; break; }
        }
        char ex3[64]; snprintf(ex3, sizeof ex3, "  (%d variants had a wrap 0-2 cycles after)", near);
        result("no DMC DMA before the load delay", !early && near > 0, ex3);
    }
    puts("-- looping 1-byte sample started k cycles before wrap S (Implicit DMA Abort 4)");
    {
        static int lp[64][4];
        for (int k = 0; k < 230; k++) for (int par = 0; par < 2; par++) {
            setup(0x4F, 0x00); if (par) flip(); nops(400 + k); sta(0x10, 0x4015); nops(200);
            nes_destroy(run(k + 630));
            int w = nth_write(0x4015, 0), s = -1, g = 0;
            for (int i = w; i < nt; i++) if (K[i] == 'w' || K[i] == 'e') { s = i; break; }
            if (s < 0) continue;
            for (int i = w + 1; i < nt && C[i] <= C[s] + 12; i++) if (K[i] == 'G') g++;
            long d = (long)C[s] - (long)C[w];
            if (d >= 0 && d < 64) lp[d][g > 3 ? 3 : g]++;
        }
        printf("    S-W:   3  4  5  6  7  8  9 10   (DMC fetches up to S+12)\n    got: ");
        for (int d = 3; d <= 10; d++) { int o = 0; for (int q = 0; q < 4; q++) if (lp[d][q]) o = q; printf("  %d", o); }
        printf("\n");
        /* playback never stops, so nothing aborts: S-6/S-5 get the extra fetch, later starts a normal reload */
        int lok = lp[3][1] && lp[4][1] && !lp[3][2] && !lp[4][2];
        for (int d = 5; d <= 10; d++) if (!lp[d][2] || lp[d][1] || lp[d][3]) lok = 0;
        result("looping: extra fetch at S-6/S-5, no aborted DMAs", lok, NULL);
    }
    puts("-- DMC DMA during OAM DMA");
    static int hist[8]; int base_bad = 0;
    for (int k = 0; k < 240; k++) for (int par = 0; par < 2; par++) {
        setup(0x0F, 0xFF); sta(0x10, 0x4015); if (par) flip(); nops(k); sta(0x02, 0x4014); nops(30);
        nes_destroy(run(k + 60));
        int w = nth_write(0x4014, 0), len = 0, dmc = 0;
        if (w < 0) continue;
        for (int i = w + 1; i < nt; i++) { if (K[i] == 'R' || K[i] == 'W') break; if (bus(K[i])) { len++; if (K[i] == 'G') dmc++; } }
        int base = (C[w] & 1) ? 513 : 514;            /* write on get: no alignment cycle */
        if (!dmc && len != base) base_bad++;
        if (dmc == 1 && len - base >= 0 && len - base < 8) hist[len - base]++;
    }
    printf("    extra cycles when a DMC fetch lands inside: +1:%d +2:%d +3:%d (others: +0:%d +4..:%d)\n",
           hist[1], hist[2], hist[3], hist[0], hist[4] + hist[5] + hist[6] + hist[7]);
    result("OAM DMA alone = 513 (write on get) / 514 (on put)", !base_bad, NULL);
    result("DMC inside OAM DMA costs 2 (1 or 3 at the very end)", hist[2] > 0 && !hist[0] && !(hist[4] + hist[5] + hist[6] + hist[7]), NULL);

    puts("-- DMC fetches that activate 2A03 registers (AccuracyCoin DMC DMA Bus Conflicts / Internal Data Bus)");
    {
        int chk = 0, bad = conflict_scan(0x4000, &chk);
        char ex4[64]; snprintf(ex4, sizeof ex4, "  (%d of %d conflicting fetches wrong)", bad, chk);
        result("LDA $4000 sees sample bits 7-5 + port bits 4-0", !bad && chk >= 64, ex4);
        bad = conflict_scan(0x4015, &chk);
        snprintf(ex4, sizeof ex4, "  (%d of %d conflicting fetches wrong)", bad, chk);
        result("LDA $4015 bit 5 unaffected by a $4015 conflict", !bad && chk >= 64, ex4);
    }
    puts("-- frame IRQ flag (AccuracyCoin Frame Counter IRQ codes E-L, 6, 7)");
    result("flag rises at 29828 and stays (IRQ enabled)", fc_flag(0, 999, -1, 4) == 0x3E, NULL);
    result("inhibited: set at 29828-29829 only (J,K,L)", fc_flag(1, 999, -1, 4) == 0x06, NULL);
    result("E: read 1 cycle before set doesn't clear", (fc_flag(0, -1, -1, 6) >> 7) & 1, NULL);
    result("F: read on the set cycle doesn't clear", (fc_flag(0, 0, -1, 6) >> 7) & 1, NULL);
    result("G: read 1 cycle later doesn't clear", (fc_flag(0, 1, -1, 6) >> 7) & 1, NULL);
    result("H: read 2 cycles later clears", !((fc_flag(0, 2, -1, 6) >> 7) & 1), NULL);
    int m = fc_flag(0, 10, 10, 13);   /* read on a get: survives the get->put edge, clears at put->get */
    result("6/7: read on get clears at the next put->get edge", m == 0x3, NULL);
    m = fc_flag(0, 11, 11, 13);
    result("6/7: read on put clears on the very next edge", m == 0x1, NULL);

    puts("-- controller strobe (AccuracyCoin Controller Strobing 3/4)");
    {
        Nes *n = nes_create(); nes_power(n); n->pad[0] = 0x0F;
        n->s.cpu.cycles = 2000; cpu_bus_write(n, 0x4016, 1);       /* write 1 on a put cycle...      */
        n->s.cpu.cycles++; nes_cpu_clock(n); cpu_bus_write(n, 0x4016, 0);  /* ...and 0 on the next get */
        n->s.cpu.cycles++; nes_cpu_clock(n);
        result("1-cycle strobe from put to get is not seen", n->s.pad_shift[0] == 0, NULL);
        n->s.cpu.cycles = 3001; cpu_bus_write(n, 0x4016, 1);       /* write 1 on a get cycle */
        n->s.cpu.cycles++; nes_cpu_clock(n); cpu_bus_write(n, 0x4016, 0);
        n->s.cpu.cycles++; nes_cpu_clock(n);
        result("1-cycle strobe from get to put is seen", n->s.pad_shift[0] == 0x0F, NULL);
        nes_destroy(n);
    }
    printf("\n%d checks, %d mismatches\n", checks, fails);
    return fails != 0;
}
