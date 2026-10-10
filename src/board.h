/* board.h - interface between the cartridge connector (cart.c) and the boards that
 * implement newer mappers. Every hook is called by the connector code with the
 * address and data as the board actually sees them through its contacts. */
#ifndef BOARD_H
#define BOARD_H
#include "nes.h"

enum { PPU_DEFAULT = 0, PPU_DRIVE, PPU_CIRAM0, PPU_CIRAM1, PPU_OPEN };
typedef struct Board {
    int mapper;
    void  (*power)(Nes *n);
    int   (*cpu_read)(Nes *n, uint16_t a, int m2, uint8_t *v);   /* returns the data bits it drives */
    void  (*cpu_write)(Nes *n, uint16_t a, int m2, uint8_t v);
    int   (*ppu_read)(Nes *n, uint16_t a, uint8_t *v);            /* PPU_* code; NULL = default */
    int   (*ppu_write)(Nes *n, uint16_t a, uint8_t v);
    void  (*ppu_addr)(Nes *n, uint16_t a);                         /* PPU address bus changes (A12 etc.) */
    void  (*m2)(Nes *n);                                           /* once per CPU cycle */
    float (*audio)(Nes *n);                                        /* cartridge audio output, APU units */
} Board;
const Board *board_find(int mapper);

/* banking helpers (cart.c) */
enum { MIR_H, MIR_V, MIR_A, MIR_B, MIR_4 };
void prg8(Nes *n, int slot, int bank);
void prg16(Nes *n, int slot, int bank);
void prg32(Nes *n, int bank);
void chr1(Nes *n, int slot, int bank);
void chr2(Nes *n, int slot, int bank);
void chr4(Nes *n, int slot, int bank);
void chr8(Nes *n, int bank);
void chr1_ram(Nes *n, int slot, int bank);   /* window from CHR-RAM on a CHR-ROM board */
void mirror(Nes *n, int m);
uint32_t chr_total(Nes *n);
uint8_t prg_read(Nes *n, uint16_t a);        /* ROM byte at $8000-$FFFF as currently mapped */
uint8_t prg_abs(Nes *n, uint32_t off);       /* ROM byte at an absolute PRG offset */
uint8_t chr_abs(Nes *n, uint32_t off);       /* CHR byte at an absolute offset */
int nprg8(Nes *n);                           /* number of 8 KB PRG banks */
#define CS CartState *cs = &n->s.cart
static inline void irq(Nes *n, int on) { n->s.cart.irq_line = (uint8_t)(on != 0); }
static inline int wram_ok(Nes *n) { return n->wram_size != 0; }
static inline uint8_t *wram_at(Nes *n, uint32_t off) { return &n->s.cart.wram[off % (n->wram_size ? n->wram_size : 0x2000)]; }

extern const Board B_VRC1, B_VRC2_4_21, B_VRC2_4_22, B_VRC2_4_23, B_VRC2_4_25, B_VRC3, B_VRC6_24, B_VRC6_26, B_VRC7;
extern const Board B_MMC5;
extern const Board B_13, B_16, B_159, B_18, B_19, B_210, B_32, B_33, B_40, B_142, B_64, B_65, B_67, B_68, B_69, B_78, B_80, B_82, B_185;
#endif
