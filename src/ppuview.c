/* PPU viewer: draws the PPU's memory the way the chip uses it - the four nametables with
 * each scanline's real scroll origin outlined, both pattern tables, the 32 palette entries
 * and all 64 sprites. Everything is read without bus side effects (no mapper latches, no
 * $2007 buffer), so looking never disturbs the game. Layout (VIEW_W x VIEW_H):
 *   nametables 512x480 at (0,0) | pattern tables 256x128 | palettes 256x32 | sprites 128x128 */
#include <stdio.h>
#include <string.h>
#include "nes.h"

#define C_BG     0x202228u
#define C_SCROLL 0xFFE040u
#define C_GRID   0x707080u
#define C_SEL    0xFFFFFFu
#define C_CELL   0x34363Eu

static uint8_t pal_entry(const Ppu *p, int i) {           /* $3F10/14/18/1C mirror $3F00/04/08/0C */
    i &= 0x1F;
    if ((i & 0x13) == 0x10) i &= 0x0F;
    return (uint8_t)(p->pal[i] & 0x3F);
}
static uint8_t nt_byte(Nes *n, uint16_t a) { uint8_t *q = cart_vram_ptr(n, a); return q ? *q : 0; }
typedef struct { const uint32_t *off; uint8_t src; } Banks;
static int chr_px(Nes *n, Banks b, uint16_t a, int x) {   /* a = tile row address */
    uint8_t lo = cart_chr_peek_banks(n, a, b.off, b.src), hi = cart_chr_peek_banks(n, (uint16_t)(a + 8), b.off, b.src);
    return ((lo >> (7 - x)) & 1) | (((hi >> (7 - x)) & 1) << 1);
}
/* CHR banks: those of the first scanline that rendered (the top of the picture), or the live ones */
static Banks top_banks(Nes *n, int end_banks) {
    Ppu *p = &n->s.ppu; Banks b = { n->s.cart.chr_off, n->s.cart.chr_src };
    if (!end_banks) for (int l = 0; l < 240; l++) if (p->line_fx[l] & 0x80) { b.off = p->line_chr[l]; b.src = p->line_src[l]; break; }
    return b;
}
static void put(uint32_t *rgb, int x, int y, uint32_t c) { if (x >= 0 && x < VIEW_W && y >= 0 && y < VIEW_H) rgb[y * VIEW_W + x] = c; }
static void box(uint32_t *rgb, int x0, int y0, int w, int h, uint32_t c) {
    for (int i = 0; i < w; i++) { put(rgb, x0 + i, y0, c); put(rgb, x0 + i, y0 + h - 1, c); }
    for (int j = 0; j < h; j++) { put(rgb, x0, y0 + j, c); put(rgb, x0 + w - 1, y0 + j, c); }
}
static void hline_wrap(uint32_t *rgb, int x, int y, uint32_t c) { for (int i = 0; i < 256; i++) put(rgb, (x + i) % 512, y % 480, c); }
static void line_origin(const Ppu *p, int l, int *x, int *y) {
    uint16_t v = p->line_v[l];
    *x = ((v >> 10) & 1) * 256 + (v & 31) * 8 + (p->line_fx[l] & 7);
    *y = (((v >> 11) & 1) * 240 + ((v >> 5) & 31) * 8 + ((v >> 12) & 7)) % 480;
}

void nes_view_render(Nes *n, uint32_t *rgb, const uint32_t pal_rgb[512], int chr_pal, int grid, int end_banks) {
    Ppu *p = &n->s.ppu;
    Banks top = top_banks(n, end_banks), row_b[480];
    for (int y = 0; y < 480; y++) row_b[y] = top;            /* rows a scanline displayed use that line's banks */
    for (int l = 239; l >= 0; l--) if (p->line_fx[l] & 0x80) { int lx, ly; line_origin(p, l, &lx, &ly); row_b[ly].off = p->line_chr[l]; row_b[ly].src = p->line_src[l]; }
    for (int i = 0; i < VIEW_W * VIEW_H; i++) rgb[i] = C_BG;
    /* nametables, drawn with the background pattern table PPUCTRL selects */
    uint16_t bg = (p->ctrl & 0x10) ? 0x1000 : 0x0000;
    for (int nt = 0; nt < 4; nt++) {
        int ox = (nt & 1) * 256, oy = (nt >> 1) * 240;
        uint16_t base = (uint16_t)(0x2000 + nt * 0x400);
        for (int ty = 0; ty < 30; ty++)
            for (int tx = 0; tx < 32; tx++) {
                uint8_t tile = nt_byte(n, (uint16_t)(base + ty * 32 + tx));
                uint8_t at = nt_byte(n, (uint16_t)(base + 0x3C0 + (ty >> 2) * 8 + (tx >> 2)));
                int pl = (at >> (((ty & 2) << 1) | (tx & 2))) & 3;
                for (int y = 0; y < 8; y++)
                    for (int x = 0; x < 8; x++) {
                        int px = chr_px(n, row_b[oy + ty * 8 + y], (uint16_t)(bg + tile * 16 + y), x);
                        rgb[(oy + ty * 8 + y) * VIEW_W + ox + tx * 8 + x] = pal_rgb[pal_entry(p, px ? pl * 4 + px : 0)];
                    }
            }
    }
    if (grid)                                                  /* 16x16 attribute areas, dotted */
        for (int y = 0; y < 480; y++)
            for (int x = 0; x < 512; x++)
                if (((x % 16 == 0) || ((y % 240) % 16 == 0)) && ((x + y) & 1)) rgb[y * VIEW_W + x] = C_GRID;
    /* what each scanline actually showed: its scroll origin, outlined (splits show as steps) */
    for (int l = 0; l < 240; l++) {
        if (!(p->line_fx[l] & 0x80)) continue;
        int x, y, px, py, nx, ny;
        line_origin(p, l, &x, &y);
        put(rgb, x % 512, y, C_SCROLL); put(rgb, (x + 255) % 512, y, C_SCROLL);
        int top = l == 0 || !(p->line_fx[l - 1] & 0x80);
        if (!top) { line_origin(p, l - 1, &px, &py); top = px != x || (py + 1) % 480 != y; }
        int bot = l == 239 || !(p->line_fx[l + 1] & 0x80);
        if (!bot) { line_origin(p, l + 1, &nx, &ny); bot = nx != x || (y + 1) % 480 != ny; }
        if (top || bot) hline_wrap(rgb, x, y, C_SCROLL);
    }
    /* pattern tables $0000 and $1000, in the chosen palette */
    for (int t = 0; t < 2; t++)
        for (int tile = 0; tile < 256; tile++)
            for (int y = 0; y < 8; y++)
                for (int x = 0; x < 8; x++) {
                    int px = chr_px(n, top, (uint16_t)(t * 0x1000 + tile * 16 + y), x);
                    put(rgb, VIEW_CHR_X + t * 128 + (tile & 15) * 8 + x, VIEW_CHR_Y + (tile >> 4) * 8 + y,
                        pal_rgb[pal_entry(p, px ? (chr_pal & 7) * 4 + px : 0)]);
                }
    /* palette RAM: background row, sprite row */
    for (int i = 0; i < 32; i++)
        for (int y = 1; y < 15; y++)
            for (int x = 1; x < 15; x++) put(rgb, VIEW_PAL_X + (i & 15) * 16 + x, VIEW_PAL_Y + (i >> 4) * 16 + y, pal_rgb[pal_entry(p, i)]);
    box(rgb, VIEW_PAL_X + (chr_pal & 3) * 64, VIEW_PAL_Y + ((chr_pal >> 2) & 1) * 16, 64, 16, C_SEL);
    /* sprites, as OAM describes them (8x16 mode picks the table from bit 0 of the tile number) */
    int tall = (p->ctrl & 0x20) != 0;
    for (int s = 0; s < 64; s++) {
        const uint8_t *o = &p->oam[s * 4];
        int cx = VIEW_OAM_X + (s & 7) * 16, cy = VIEW_OAM_Y + (s >> 3) * 16, h = tall ? 16 : 8;
        for (int y = 1; y < 15; y++) for (int x = 1; x < 15; x++) put(rgb, cx + x, cy + y, C_CELL);
        uint16_t base = tall ? (uint16_t)(((o[1] & 1) ? 0x1000 : 0) + (o[1] & 0xFE) * 16) : (uint16_t)(((p->ctrl & 8) ? 0x1000 : 0) + o[1] * 16);
        for (int y = 0; y < h; y++)
            for (int x = 0; x < 8; x++) {
                int sy = (o[2] & 0x80) ? h - 1 - y : y, sx = (o[2] & 0x40) ? 7 - x : x;
                int px = chr_px(n, top, (uint16_t)(base + (sy >= 8 ? 16 : 0) + (sy & 7)), sx);
                if (px) put(rgb, cx + 4 + x, cy + (tall ? 0 : 4) + y, pal_rgb[pal_entry(p, 16 + (o[2] & 3) * 4 + px)]);
            }
    }
}

int nes_view_describe(Nes *n, int x, int y, int end_banks, char *buf, int len) {
    Ppu *p = &n->s.ppu;
    Banks top = top_banks(n, end_banks);
    buf[0] = 0;
    if (x >= 0 && x < 512 && y >= 0 && y < 480) {
        int nt = (y / 240) * 2 + x / 256, tx = (x % 256) / 8, ty = (y % 240) / 8;
        uint16_t a = (uint16_t)(0x2000 + nt * 0x400 + ty * 32 + tx), aa = (uint16_t)(0x2000 + nt * 0x400 + 0x3C0 + (ty >> 2) * 8 + (tx >> 2));
        uint8_t at = nt_byte(n, aa);
        snprintf(buf, (size_t)len, "Nametable $%04X  column %d row %d  tile $%02X (CHR $%04X)  palette %d (attribute $%04X = $%02X)",
                 a, tx, ty, nt_byte(n, a), ((p->ctrl & 0x10) ? 0x1000 : 0) + nt_byte(n, a) * 16, (at >> (((ty & 2) << 1) | (tx & 2))) & 3, aa, at);
    } else if (x >= VIEW_CHR_X && x < VIEW_CHR_X + 256 && y >= VIEW_CHR_Y && y < VIEW_CHR_Y + 128) {
        int t = (x - VIEW_CHR_X) / 128, tile = ((y - VIEW_CHR_Y) / 8) * 16 + ((x - VIEW_CHR_X) % 128) / 8;
        int a = t * 0x1000 + tile * 16, slot = a >> 10;
        uint32_t off = top.off[slot] + (uint32_t)(a & 0x3FF);
        int ram = n->chr_is_ram || ((top.src >> slot) & 1);
        snprintf(buf, (size_t)len, "Pattern table $%04X  tile $%02X  PPU $%04X-$%04X  =  %s offset $%05X (1 KB bank %u)", t * 0x1000, tile, a, a + 15,
                 ram ? "CHR-RAM" : "CHR-ROM", (unsigned)(ram ? off % (n->chr_ram_size ? n->chr_ram_size : 0x2000) : (n->chr_size ? off % n->chr_size : 0)), (unsigned)(top.off[slot] / 0x400));
    } else if (x >= VIEW_PAL_X && x < VIEW_PAL_X + 256 && y >= VIEW_PAL_Y && y < VIEW_PAL_Y + 32) {
        int i = ((y - VIEW_PAL_Y) / 16) * 16 + (x - VIEW_PAL_X) / 16;
        snprintf(buf, (size_t)len, "Palette $3F%02X = colour $%02X  (%s palette %d, entry %d%s)  - click to use it for the pattern tables",
                 i, pal_entry(p, i), i < 16 ? "background" : "sprite", (i >> 2) & 3, i & 3, (i & 3) ? "" : ", backdrop");
    } else if (x >= VIEW_OAM_X && x < VIEW_OAM_X + 128 && y >= VIEW_OAM_Y && y < VIEW_OAM_Y + 128) {
        int s = ((y - VIEW_OAM_Y) / 16) * 8 + (x - VIEW_OAM_X) / 16;
        const uint8_t *o = &p->oam[s * 4];
        snprintf(buf, (size_t)len, "Sprite %d: X %d  Y %d  tile $%02X  palette %d%s%s%s", s, o[3], o[0] + 1, o[1], o[2] & 3,
                 (o[2] & 0x20) ? "  behind background" : "", (o[2] & 0x40) ? "  flipped H" : "", (o[2] & 0x80) ? "  flipped V" : "");
    } else return 0;
    return 1;
}
