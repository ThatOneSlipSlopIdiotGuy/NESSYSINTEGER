/* nessys_cli - headless NESSYSINTEGER runner, for test ROMs and experiments.
 *
 *   nessys_cli rom.nes [options]
 *     -f N            run N frames (default 600)
 *     -s shot.ppm     save the final frame as a PPM image
 *     -w out.wav      record audio (44.1 kHz mono)
 *     -i SCRIPT       controller 1 input, e.g. "150-155:S;300-420:R;400-410:A"
 *                     (frame or from-to : buttons, A B s=Select S=Start U D L R)
 *     -V view.ppm[:N] save the PPU viewer picture at the end (N = palette 0-7 for the tiles)
 *     -T trace.txt[:N] log PPU/mapper writes and interrupts for the first N frames (default 150)
 *     -c auto|nes|nes101|fc   console model
 *     -p PIN=MODE     force a cartridge pin: MODE = off | on | flaky | auto   (repeatable)
 *     -t S,R,P,D      cart pose: seat, roll, pitch, dirt (e.g. -t 1,0.3,0,0)
 *     -F real|high|low|noise|hold   what floating inputs read as (default real)
 *     -a N            CPU/PPU clock alignment 0..3
 *     -m 0300:16      dump CPU memory (hex address : byte count) at the end
 *     -g CODE         Game Genie code (repeatable)
 *     -q              quiet (only the result line)
 * Exit code: 0 if the ROM reports success through the $6000 protocol. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../src/nes.h"

static void put32(FILE *f, uint32_t v) { fputc(v & 255, f); fputc((v >> 8) & 255, f); fputc((v >> 16) & 255, f); fputc(v >> 24, f); }
static void put16(FILE *f, uint16_t v) { fputc(v & 255, f); fputc(v >> 8, f); }

/* -T trace: PPU register traffic, mapper writes and interrupt entries, frame/line/dot stamped */
static FILE *tf; static int tf_frames = 150, cur_frame, run_kind, run_n; static uint16_t run_v;
static void tflush(void) {
    if (run_n) fprintf(tf, "                  $2007 %s x%d (VRAM $%04X)\n", run_kind == 'W' ? "writes" : "reads", run_n, run_v);
    run_n = 0;
}
static void trace_cb(Nes *n, char k, uint16_t a, uint8_t v) {
    if (!tf || cur_frame >= tf_frames || (k != 'W' && k != 'R')) return;
    Ppu *p = &n->s.ppu; char line[80];
    if (a >= 0x2000 && a < 0x4000 && (a & 7) == 7) {                     /* collapse $2007 runs */
        if (run_n && run_kind == k) { run_n++; return; }
        tflush(); run_kind = k; run_n = 1; run_v = (uint16_t)(p->v & 0x3FFF); return;
    }
    if (k == 'W' && a >= 0x2000 && a < 0x4000) snprintf(line, sizeof line, "PPU $200%d = $%02X", a & 7, v);
    else if (k == 'W' && a >= 0x4020) snprintf(line, sizeof line, "cart $%04X = $%02X", a, v);
    else if (k == 'R' && a == 0xFFFA) snprintf(line, sizeof line, "NMI");
    else if (k == 'R' && a == 0xFFFE) snprintf(line, sizeof line, "IRQ/BRK  (cart IRQ line %d)", n->s.cart.irq_line);
    else return;
    tflush();
    fprintf(tf, "f%-4d L%-3d d%-3d %s\n", cur_frame, p->scanline, p->dot, line);
}

static int pad_at(const char *sc, int f) {     /* "60-65:S;200:A" -> buttons held on frame f */
    int m = 0;
    while (sc && *sc) {
        int a = atoi(sc), b = a; const char *p = sc; while (*p >= '0' && *p <= '9') p++;
        if (*p == '-') { b = atoi(++p); while (*p >= '0' && *p <= '9') p++; }
        int mm = 0; if (*p == ':') for (p++; *p && *p != ';'; p++)
            mm |= *p == 'A' ? 1 : *p == 'B' ? 2 : *p == 's' ? 4 : *p == 'S' ? 8 : *p == 'U' ? 16 : *p == 'D' ? 32 : *p == 'L' ? 64 : *p == 'R' ? 128 : 0;
        if (f >= a && f <= b) m |= mm;
        sc = *p ? p + 1 : p;
    }
    return m;
}
int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "usage: %s rom.nes [-f frames] [-s shot.ppm] [-w out.wav] [-i input-script] [-V view.ppm[:pal]]\n"
                        "       [-T trace.txt[:frames]] [-c auto|nes|nes101|fc] [-p pin=off|on|flaky|auto] [-t seat,roll,pitch,dirt]\n"
                        "       [-F real|high|low|noise|hold] [-a 0-3] [-m addr:len] [-g code] [-q]\n", argv[0]);
        return 2;
    }
    FILE *f = fopen(argv[1], "rb");
    if (!f) { perror(argv[1]); return 2; }
    fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0, SEEK_SET);
    uint8_t *d = (uint8_t *)malloc((size_t)sz);
    if (fread(d, 1, (size_t)sz, f) != (size_t)sz) { fclose(f); return 2; }
    fclose(f);
    Nes *n = nes_create();
    int frames = 600, quiet = 0, dump_addr = -1, dump_len = 0;
    const char *shot = NULL, *wav = NULL, *script = NULL, *view = NULL;
    for (int i = 2; i < argc; i++) {
        const char *a = argv[i], *v = i + 1 < argc ? argv[i + 1] : "";
        if (!strcmp(a, "-f")) { frames = atoi(v); i++; }
        else if (!strcmp(a, "-s")) { shot = v; i++; }
        else if (!strcmp(a, "-i")) { script = v; i++; }                    /* controller script */
        else if (!strcmp(a, "-V")) { view = v; i++; }                      /* PPU viewer picture */
        else if (!strcmp(a, "-w")) { wav = v; i++; }
        else if (!strcmp(a, "-a")) { n->cpu_align = atoi(v) & 3; i++; }
        else if (!strcmp(a, "-q")) quiet = 1;
        else if (!strcmp(a, "-c")) {                                       /* console model */
            n->console_setting = !strcmp(v, "nes") ? CON_NES : !strcmp(v, "nes101") ? CON_NES101 : (!strcmp(v, "fc") || !strcmp(v, "famicom")) ? CON_FAMICOM : CON_AUTO; i++;
        }
        else if (!strcmp(a, "-T")) {                                       /* trace.txt or trace.txt:frames */
            static char path[1024]; snprintf(path, sizeof path, "%s", v);
            char *c = strrchr(path, ':');
            if (c && c[1] && strspn(c + 1, "0123456789") == strlen(c + 1)) { tf_frames = atoi(c + 1); *c = 0; }
            if (!(tf = fopen(path, "w"))) perror(path);
            i++;
        }
        else if (!strcmp(a, "-m")) { unsigned ad, ln; if (sscanf(v, "%x:%u", &ad, &ln) == 2) { dump_addr = (int)ad; dump_len = (int)ln; } i++; }
        else if (!strcmp(a, "-g")) {
            uint16_t ad; uint8_t va, cm; int hc;
            if (n->ngenie < 16 && nes_genie_decode(v, &ad, &va, &cm, &hc)) {
                n->genie[n->ngenie].addr = ad; n->genie[n->ngenie].val = va;
                n->genie[n->ngenie].cmp = cm; n->genie[n->ngenie].has_cmp = (uint8_t)hc; n->ngenie++;
            } else fprintf(stderr, "bad Game Genie code %s\n", v);
            i++;
        } else if (!strcmp(a, "-F")) {
            n->conn.float_mode = !strcmp(v, "low") ? FLOAT_LOW : !strcmp(v, "noise") ? FLOAT_NOISE : !strcmp(v, "hold") ? FLOAT_HOLD :
                                 !strcmp(v, "high") ? FLOAT_HIGH : FLOAT_REAL; i++;
        } else if (!strcmp(a, "-p")) {
            int pin; char mode[16];
            if (sscanf(v, "%d=%15s", &pin, mode) == 2 && pin >= 1 && pin <= PINS)
                n->conn.mode[pin] = !strcmp(mode, "off") ? PIN_OFF : !strcmp(mode, "on") ? PIN_ON : !strcmp(mode, "flaky") ? PIN_FLAKY : PIN_AUTO;
            i++;
        } else if (!strcmp(a, "-t")) { sscanf(v, "%f,%f,%f,%f", &n->conn.seat, &n->conn.roll, &n->conn.pitch, &n->conn.dirt); i++; }
    }
    conn_update(n);
    char err[256];
    if (!nes_load_rom(n, d, (size_t)sz, 0, err, sizeof err)) { fprintf(stderr, "%s\n", err); return 2; }
    if (!quiet && !n->conn.perfect) {
        int off = 0; for (int p = 1; p <= PINS; p++) off += !n->conn.conn[p];
        printf("connector: %d pin(s) open, %d flaky, cart power %s, CIC %s\n", off, n->conn.nflaky,
               n->conn.powered ? "ok" : "LOST", n->conn.cic_ok ? "ok" : "LOCKOUT");
    }
    FILE *wf = NULL; uint32_t nsamp = 0;
    if (wav && (wf = fopen(wav, "wb"))) { fwrite("RIFF\0\0\0\0WAVEfmt ", 1, 16, wf); put32(wf, 16); put16(wf, 1); put16(wf, 1);
        put32(wf, AUDIO_RATE); put32(wf, AUDIO_RATE * 2); put16(wf, 2); put16(wf, 16); fwrite("data\0\0\0\0", 1, 8, wf); }
    int reset_at = -1, i;
    if (tf) { n->trace = trace_cb; fprintf(tf, "%s: mapper %d (%s), submapper %d, console %s\n", argv[1], n->mapper, nes_mapper_name(n->mapper), n->submapper, nes_console_name(n->console)); }
    for (i = 0; i < frames; i++) {
        cur_frame = i;
        n->pad[0] = (uint8_t)pad_at(script, i);
        nes_run_frame(n);
        if (wf) { fwrite(n->audio, 2, (size_t)n->audio_len, wf); nsamp += (uint32_t)n->audio_len; }
        uint8_t *w = n->s.cart.wram;
        if (n->wram_size && w[1] == 0xDE && w[2] == 0xB0 && w[3] == 0x61) {
            if (w[0] == 0x81 && reset_at < 0) reset_at = i + 15;   /* test asks for a reset */
            if (reset_at == i) { nes_reset(n); reset_at = -2; }
            if (w[0] < 0x80 && i > 10 && !shot && !wf) break;
        }
    }
    if (wf) { fseek(wf, 4, SEEK_SET); put32(wf, 36 + nsamp * 2); fseek(wf, 40, SEEK_SET); put32(wf, nsamp * 2); fclose(wf); }
    uint8_t *w = n->s.cart.wram;
    int ok = n->wram_size && w[1] == 0xDE && w[2] == 0xB0 && w[3] == 0x61;
    if (ok) printf("result $%02X: %s\n", w[0], (char *)w + 4);
    else printf("no $6000 result (frames=%d cycles=%llu pc=$%04X)\n", i, (unsigned long long)n->s.cpu.cycles, n->s.cpu.pc);
    if (dump_addr >= 0) {
        for (int k = 0; k < dump_len; k++) printf("%s%02X", k % 16 ? " " : (k ? "\n" : ""), nes_peek(n, (uint16_t)(dump_addr + k)));
        printf("\n");
    }
    if (shot) {
        uint32_t pal[512]; nes_palette(pal);
        FILE *o = fopen(shot, "wb");
        if (o) {
            fprintf(o, "P6\n256 240\n255\n");
            for (int k = 0; k < 256 * 240; k++) { uint32_t c = pal[n->fb[k] & 511]; fputc(c >> 16, o); fputc((c >> 8) & 255, o); fputc(c & 255, o); }
            fclose(o);
        }
    }
    if (view) {                                         /* -V file.ppm[:palette 0-7] */
        char vp[512]; int cp = 0; snprintf(vp, sizeof vp, "%s", view);
        char *colon = strrchr(vp, ':'); if (colon && colon[1] >= '0' && colon[1] <= '7' && !colon[2]) { cp = colon[1] - '0'; *colon = 0; }
        uint32_t pal[512]; nes_palette(pal);
        uint32_t *rgb = malloc(sizeof(uint32_t) * VIEW_W * VIEW_H);
        FILE *o = rgb ? fopen(vp, "wb") : NULL;
        if (o) {
            nes_view_render(n, rgb, pal, cp, 0, 0);
            fprintf(o, "P6\n%d %d\n255\n", VIEW_W, VIEW_H);
            for (int k = 0; k < VIEW_W * VIEW_H; k++) { fputc(rgb[k] >> 16, o); fputc((rgb[k] >> 8) & 255, o); fputc(rgb[k] & 255, o); }
            fclose(o);
        }
        free(rgb);
    }
    int code = (ok && w[0] == 0) ? 0 : 1;
    nes_destroy(n); free(d);
    if (tf) { tflush(); fclose(tf); }
    return code;
}
