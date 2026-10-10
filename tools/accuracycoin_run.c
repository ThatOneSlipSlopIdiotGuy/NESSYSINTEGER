/* accuracycoin_run - runs AccuracyCoin's "run every test" mode headlessly and prints the
 * results, the same way the ROM's own results screen reports them.
 *   usage: accuracycoin_run AccuracyCoin.nes AccuracyCoin.asm [-a 0-3] [-r ram] [-v]
 * The .asm (from the AccuracyCoin repository, matching the ROM) supplies each test's
 * name and result address; nothing from it is built into this program.
 *   -a  CPU/PPU clock alignment (0-3)     -r  power-on RAM: 0 zero, 1 $FF, 2 random, 3 stripes, 4 real console (default)
 *   -v  list passing tests too */
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../src/nes.h"

typedef struct { char suite[64], name[64], res[64]; unsigned addr; } Test;
static Test T[512]; static int nt;
static struct { char name[64]; unsigned val; } K[1024]; static int nk;

static char *trim(char *s) {
    while (isspace((unsigned char)*s)) s++;
    char *e = s + strlen(s);
    while (e > s && isspace((unsigned char)e[-1])) *--e = 0;
    return s;
}
static void quoted(const char *s, char *out) {           /* copy the first "..." string */
    const char *a = strchr(s, '"'), *b = a ? strchr(a + 1, '"') : NULL;
    size_t l = (a && b) ? (size_t)(b - a - 1) : 0;
    if (l > 63) l = 63;
    memcpy(out, a ? a + 1 : "", l); out[l] = 0;
}
static int parse_asm(const char *path) {
    FILE *f = fopen(path, "r");
    if (!f) return 0;
    char line[2048], suite[64] = "?";
    int pages = 0;
    while (fgets(line, sizeof line, f)) {
        char *s = trim(line), nm[64];
        unsigned v;
        if (!strncmp(s, "result_", 7) && nk < 1024 && sscanf(s, "%63[A-Za-z0-9_] = $%x", nm, &v) == 2) { strcpy(K[nk].name, nm); K[nk].val = v; nk++; }
        if (!strncmp(s, "TestPages:", 10)) pages = 1;
        if (!pages) continue;
        if (!strncmp(s, "TableTable", 10) || !strncmp(s, "EndTableTable", 13)) break;
        if (!strncmp(s, ".byte", 5) && strchr(s, '"')) quoted(s, suite);
        else if (!strncmp(s, "table", 5) && isspace((unsigned char)s[5]) && nt < 512) {
            char *q = strchr(s, '"'); q = q ? strchr(q + 1, '"') : NULL;
            char *p = q ? strchr(q, ',') : NULL; p = p ? strchr(p + 1, ',') : NULL;   /* skip the $FF */
            if (!p || sscanf(p + 1, " %63[A-Za-z0-9_]", T[nt].res) != 1) continue;
            quoted(s, T[nt].name); strcpy(T[nt].suite, suite); nt++;
        }
    }
    fclose(f);
    for (int i = 0; i < nt; i++) for (int k = 0; k < nk; k++) if (!strcmp(K[k].name, T[i].res)) T[i].addr = K[k].val;
    return nt;
}

int main(int argc, char **argv) {
    if (argc < 3) { fprintf(stderr, "usage: %s AccuracyCoin.nes AccuracyCoin.asm [-a 0-3] [-r 0-4] [-v]\n", argv[0]); return 2; }
    int align = 0, ram = 4, verbose = 0;
    for (int i = 3; i < argc; i++) {
        if (!strcmp(argv[i], "-a") && i + 1 < argc) align = atoi(argv[++i]) & 3;
        else if (!strcmp(argv[i], "-r") && i + 1 < argc) { ram = atoi(argv[++i]); if (ram < 0 || ram > 4) ram = 4; }
        else if (!strcmp(argv[i], "-v")) verbose = 1;
    }
    FILE *f = fopen(argv[1], "rb");
    if (!f) { fprintf(stderr, "can't open %s\n", argv[1]); return 1; }
    fseek(f, 0, SEEK_END); long sz = ftell(f); rewind(f);
    uint8_t *rom = (uint8_t *)malloc((size_t)sz);
    if (!rom || fread(rom, 1, (size_t)sz, f) != (size_t)sz) { fclose(f); return 1; }
    fclose(f);
    if (!parse_asm(argv[2])) { fprintf(stderr, "no tests found in %s\n", argv[2]); return 1; }

    Nes *n = nes_create();
    n->cpu_align = align; n->ram_init = ram;
    char err[128];
    if (!nes_load_rom(n, rom, (size_t)sz, 0, err, sizeof err)) { fprintf(stderr, "%s\n", err); return 1; }
    const int press = 120;
    int started = 0, fr;
    for (fr = 0; fr < 60000; fr++) {
        n->pad[0] = (fr >= press && fr < press + 3) ? 0x08 : 0;      /* Start on the main menu = run every test */
        nes_run_frame(n);
        if (fr >= press && n->s.ram[0x35] == 1) started = 1;           /* RunningAllTests */
        if (!started && fr > press + 300) { fprintf(stderr, "the test run never started\n"); return 1; }
        if (started && !(fr % 30)) {
            int pending = 0;
            for (int i = 0; i < nt; i++) {
                if (!T[i].addr || (T[i].addr >> 8) == 3) continue;     /* page-3 results are pre-tests */
                uint8_t q = n->s.ram[T[i].addr & 0x7FF] & 3;
                if (q != 1 && q != 2) pending++;
            }
            if (!pending) break;
        }
    }
    int pass = 0, total = 0;
    const char *last = "";
    for (int i = 0; i < nt; i++) {
        if (!T[i].addr || (T[i].addr >> 8) == 3) continue;
        uint8_t v = n->s.ram[T[i].addr & 0x7FF], c = v >> 2;
        int ok = (v & 3) == 1;
        total++; pass += ok;
        if (ok && !verbose) continue;
        if (strcmp(last, T[i].suite)) { printf("[%s]\n", T[i].suite); last = T[i].suite; }
        if (ok) printf("  Pass        %s\n", T[i].name);
        else if ((v & 3) == 2) printf("  Fail %c      %s\n", c < 10 ? '0' + c : 'A' + c - 10, T[i].name);
        else printf("  (not run)   %s\n", T[i].name);
    }
    printf("%d/%d passed (CPU/PPU alignment %d, power-on RAM mode %d, %d frames)\n", pass, total, align, ram, fr);
    nes_destroy(n);
    return pass == total ? 0 : 3;
}
