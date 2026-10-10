"""Builds NESSYSINTEGER self-test ROMs:
  selftest.nes  - CPU/PPU/APU behaviour suite (NROM), results on screen + $6000 protocol
  mmc3test.nes  - MMC3 PRG/CHR banking and scanline-IRQ split (red-tinted lower part of screen)
  mmc1test.nes  - MMC1 serial-port PRG banking
Usage: python make_test_roms.py [outdir]"""
import os, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from asm6502 import assemble, ines

FONT = {
 '0': '111101101101111', '1': '010110010010111', '2': '111001111100111', '3': '111001111001111', '4': '101101111001001',
 '5': '111100111001111', '6': '111100111101111', '7': '111001001010010', '8': '111101111101111', '9': '111101111001111',
 'A': '010101111101101', 'B': '110101110101110', 'C': '011100100100011', 'D': '110101101101110', 'E': '111100110100111',
 'F': '111100110100100', 'G': '011100101101011', 'H': '101101111101101', 'I': '111010010010111', 'J': '001001001101010',
 'K': '101101110101101', 'L': '100100100100111', 'M': '101111111101101', 'N': '110101101101101', 'O': '010101101101010',
 'P': '110101110100100', 'Q': '010101101110011', 'R': '110101110101101', 'S': '011100010001110', 'T': '111010010010010',
 'U': '101101101101111', 'V': '101101101101010', 'W': '101101111111101', 'X': '101101010101101', 'Y': '101101010010010',
 'Z': '111001010100111', '-': '000000111000000', '=': '000111000111000', '$': '011110010011110', ':': '000010000010000',
 '.': '000000000000010', '/': '001001010100100', '!': '010010010000010', '?': '111001010000010', '(': '001010010010001',
 ')': '100010010010100', '>': '100010001010100', '+': '000010111010000',
}

def font_table():
    """4 KB pattern table: ASCII-indexed glyphs, tile 1 = solid colour 1, tile 2 = solid colour 3."""
    t = bytearray(4096)
    for ch, bits in FONT.items():
        rows = [bits[i * 3:i * 3 + 3] for i in range(5)]
        rows = [rows[0], rows[1], rows[1], rows[2], rows[3], rows[3], rows[4]]   # 3x5 -> 6x7
        for y, r in enumerate(rows):
            b = 0
            for x, c in enumerate(r):
                if c == '1': b |= (0xC0 >> (x * 2))
            t[ord(ch) * 16 + y] = b
    for i in range(8): t[1 * 16 + i] = 0xFF; t[2 * 16 + i] = 0xFF; t[2 * 16 + 8 + i] = 0xFF
    return bytes(t)

DEFS = """
tmp = $00
ptr = $02
curtest = $04
cnt_lo = $05
cnt_hi = $06
nmi_count = $10
irq_count = $11
fails = $12
results = $0300
"""

COMMON = r"""

wait_vbl:
  bit $2002
wv_l:
  lda $2002
  bpl wv_l
  rts
print:
  ldy #0
  lda (ptr),y
  sta $2006
  iny
  lda (ptr),y
  sta $2006
  iny
pr_l:
  lda (ptr),y
  beq pr_d
  sta $2007
  iny
  bne pr_l
pr_d:
  rts
put_hex:
  pha
  lsr a
  lsr a
  lsr a
  lsr a
  tax
  lda hexdig,x
  sta $2007
  pla
  and #$0F
  tax
  lda hexdig,x
  sta $2007
  rts
copy_6004:
  ldy #2
  ldx #0
c64:
  lda (ptr),y
  sta $6004,x
  beq c64d
  iny
  inx
  bne c64
c64d:
  rts
clear_nt:
  lda #$20
  sta $2006
  lda #$00
  sta $2006
  ldx #8
  ldy #0
  lda #0
cn_l:
  sta $2007
  iny
  bne cn_l
  dex
  bne cn_l
  rts
load_palette:
  lda #$3F
  sta $2006
  lda #$00
  sta $2006
  ldx #0
lp_l:
  lda palette,x
  sta $2007
  inx
  cpx #32
  bne lp_l
  rts
init_common:
  lda #$40
  sta $4017
  lda #$00
  sta $4010
  sta $2000
  sta $2001
  sta $4015
  tax
clr:
  sta $00,x
  sta $0300,x
  sta $0400,x
  sta $0500,x
  sta $0600,x
  sta $0700,x
  inx
  bne clr
  jsr wait_vbl
  jsr wait_vbl
  lda #$80
  sta $6000
  lda #$DE
  sta $6001
  lda #$B0
  sta $6002
  lda #$61
  sta $6003
  lda #0
  sta $6004
  jsr load_palette
  jsr clear_nt
  rts
hexdig:
  .byte "0123456789ABCDEF"
palette:
  .byte $0F,$30,$16,$12, $0F,$30,$16,$12, $0F,$30,$16,$12, $0F,$30,$16,$12
  .byte $0F,$2A,$16,$30, $0F,$2A,$16,$30, $0F,$2A,$16,$30, $0F,$2A,$16,$30
"""

def text(name, row, col, s):
    a = 0x2000 + row * 32 + col
    return '%s:\n  .byte $%02X, $%02X, "%s", 0\n' % (name, a >> 8, a & 0xFF, s)

# ------------------------------------------------------------------ suite
TESTS = [
 ('t_ram', 'RAM MIRRORING'), ('t_jmpind', 'JMP () PAGE BUG'), ('t_unoff', 'UNOFFICIAL OPS'),
 ('t_openbus', 'CPU OPEN BUS'), ('t_ppubus', 'PPU OPEN BUS'), ('t_palette', 'PALETTE MIRROR'),
 ('t_readbuf', '2007 READ BUFFER'), ('t_mirror', 'NT MIRRORING'), ('t_vblclr', 'VBL FLAG CLEAR'),
 ('t_nmi', 'NMI EVERY FRAME'), ('t_dummy', 'DUMMY READ 2002'), ('t_oam', 'OAM READ/MASK'),
 ('t_spr0', 'SPRITE 0 HIT'), ('t_ovf', 'SPRITE OVERFLOW'), ('t_pad', 'JOYPAD SHIFT'),
 ('t_len', 'APU LENGTH CTR'), ('t_firq', 'APU FRAME IRQ'), ('t_dmc', 'DMC IRQ + DMA'),
]

SUITE = r"""
NTESTS = %d
.org $8000
reset:
  sei
  cld
  ldx #$FF
  txs
  jsr init_common
  ldx #0
run_l:
  stx curtest
  txa
  asl a
  tay
  lda tests,y
  sta ptr
  lda tests+1,y
  sta ptr+1
  jsr call_ptr
  ldx curtest
  sta results,x
  cmp #0
  beq run_ok
  inc fails
run_ok:
  ldx curtest
  inx
  cpx #NTESTS
  bne run_l
  jsr wait_vbl
  lda #0
  sta $2001
  jsr clear_nt
  lda #<title
  sta ptr
  lda #>title
  sta ptr+1
  jsr print
  ldx #0
show_l:
  stx curtest
  txa
  asl a
  tay
  lda names,y
  sta ptr
  lda names+1,y
  sta ptr+1
  jsr print
  ldx curtest
  lda results,x
  bne show_fail
  lda #'O'
  sta $2007
  lda #'K'
  sta $2007
  jmp show_next
show_fail:
  pha
  lda #'F'
  sta $2007
  lda #'A'
  sta $2007
  lda #'I'
  sta $2007
  lda #'L'
  sta $2007
  lda #' '
  sta $2007
  pla
  jsr put_hex
show_next:
  ldx curtest
  inx
  cpx #NTESTS
  bne show_l
  lda #<firq_txt
  sta ptr
  lda #>firq_txt
  sta ptr+1
  jsr print
  lda cnt_hi
  jsr put_hex
  lda cnt_lo
  jsr put_hex
  lda fails
  bne sum_fail
  lda #<pass_txt
  sta ptr
  lda #>pass_txt
  sta ptr+1
  jsr print
  jsr copy_6004
  lda #0
  sta $6000
  jmp show_done
sum_fail:
  lda #<fail_txt
  sta ptr
  lda #>fail_txt
  sta ptr+1
  jsr print
  jsr copy_6004
  lda #1
  sta $6000
show_done:
  jsr wait_vbl
  lda #0
  sta $2005
  sta $2005
  sta $2000
  lda #$0A
  sta $2001
forever:
  jmp forever

call_ptr:
  jmp (ptr)
nmi:
  inc nmi_count
  rti
irq:
  pha
  lda $4015
  inc irq_count
  pla
  rti
reset_scroll:
  lda #0
  sta $2005
  sta $2005
  sta $2000
  rts
oam_clear:
  lda #$FF
  ldx #0
oc_l:
  sta $0200,x
  inx
  bne oc_l
  rts
fail1:
  lda #1
  rts
fail2:
  lda #2
  rts
fail3:
  lda #3
  rts
fail4:
  lda #4
  rts
pass:
  lda #0
  rts

t_ram:
  lda #$A5
  sta $0123
  lda $0923
  cmp #$A5
  bne fail1
  lda #$3C
  sta $1FFF
  lda $07FF
  cmp #$3C
  bne fail2
  jmp pass
t_jmpind:
  lda #<ji_ok
  sta $05FF
  lda #>ji_ok
  sta $0500
  lda #>ji_bad
  sta $0600
  jmp ($05FF)
t_unoff:
  lda #$5A
  sta $20
  ldx #0
  .byte $A7, $20
  cpx #$5A
  bne fail1
  lda #$F0
  ldx #$3C
  .byte $87, $21
  lda $21
  cmp #$30
  bne fail2
  lda #$10
  sta $22
  lda #$0F
  .byte $C7, $22
  bne fail3
  jmp pass
t_openbus:
  lda $5000
  cmp #$50
  bne fail1
  lda $4000
  cmp #$40
  bne fail2
  lda $4016
  and #$E0
  cmp #$40
  bne fail3
  jmp pass
t_ppubus:
  lda #$5C
  sta $2003
  lda $2000
  cmp #$5C
  bne fail1
  jmp pass
t_palette:
  lda #$3F
  sta $2006
  lda #$1C
  sta $2006
  lda #$21
  sta $2007
  lda #$3F
  sta $2006
  lda #$0C
  sta $2006
  lda $2007
  and #$3F
  cmp #$21
  bne fail1
  lda #$3F
  sta $2006
  lda #$1D
  sta $2006
  lda #$11
  sta $2007
  lda #$3F
  sta $2006
  lda #$0D
  sta $2006
  lda $2007
  and #$3F
  cmp #$11
  beq fail2
  jsr load_palette
  jmp pass
t_readbuf:
  lda #$24
  sta $2006
  lda #$00
  sta $2006
  lda #$5A
  sta $2007
  lda #$A5
  sta $2007
  lda #$24
  sta $2006
  lda #$00
  sta $2006
  lda $2007
  lda $2007
  cmp #$5A
  bne fail1
  lda $2007
  cmp #$A5
  bne fail2
  jsr clear_nt
  jmp pass
t_mirror:
  lda #$20
  sta $2006
  lda #$00
  sta $2006
  lda #$11
  sta $2007
  lda #$24
  sta $2006
  lda #$00
  sta $2006
  lda #$22
  sta $2007
  lda #$28
  sta $2006
  lda #$00
  sta $2006
  lda $2007
  lda $2007
  cmp #$11
  bne fail1
  lda #$2C
  sta $2006
  lda #$00
  sta $2006
  lda $2007
  lda $2007
  cmp #$22
  bne fail2
  jsr clear_nt
  jmp pass
t_vblclr:
  bit $2002
vc_l:
  bit $2002
  bpl vc_l
  lda $2002
  and #$80
  bne fail1
  jmp pass
t_nmi:
  lda #0
  sta nmi_count
  lda #$80
  sta $2000
  ldx #0
  ldy #0
nm_l:
  lda nmi_count
  cmp #4
  bcs nm_ok
  dex
  bne nm_l
  dey
  bne nm_l
  lda #0
  sta $2000
  jmp fail1
nm_ok:
  lda #0
  sta $2000
  jmp pass
t_dummy:
  lda #0
  sta nmi_count
  lda #$80
  sta $2000
du_l:
  lda nmi_count
  beq du_l
  lda #0
  sta $2000
  ldx #$10
  lda $20F2,x
  and #$80
  bne fail1
  jmp pass
t_oam:
  lda #$10
  sta $2003
  lda #$FF
  sta $2004
  lda #$12
  sta $2003
  lda #$FF
  sta $2004
  lda #$10
  sta $2003
  lda $2004
  cmp #$FF
  bne fail1
  lda $2004
  cmp #$FF
  bne fail2
  lda #$12
  sta $2003
  lda $2004
  cmp #$E3
  bne fail3
  jmp pass
t_spr0:
  jsr oam_clear
  lda #79
  sta $0200
  lda #$01
  sta $0201
  lda #$00
  sta $0202
  lda #80
  sta $0203
  jsr wait_vbl
  lda #$21
  sta $2006
  lda #$4A
  sta $2006
  lda #$01
  sta $2007
  lda #0
  sta $2003
  lda #$02
  sta $4014
  jsr reset_scroll
  lda #$1E
  sta $2001
  jsr wait_vbl
  jsr wait_vbl
  and #$40
  beq sp_f1
  lda #200
  sta $0203
  lda #0
  sta $2003
  lda #$02
  sta $4014
  jsr wait_vbl
  jsr wait_vbl
  and #$40
  bne sp_f2
  lda #0
  sta $2001
  jsr clear_nt
  jmp pass
sp_f1:
  lda #0
  sta $2001
  jmp fail1
sp_f2:
  lda #0
  sta $2001
  jmp fail2
t_ovf:
  jsr oam_clear
  ldx #0
  ldy #0
ov_l:
  lda #100
  sta $0200,x
  lda #$02
  sta $0201,x
  lda #0
  sta $0202,x
  tya
  sta $0203,x
  clc
  adc #20
  tay
  inx
  inx
  inx
  inx
  cpx #36
  bne ov_l
  jsr wait_vbl
  lda #0
  sta $2003
  lda #2
  sta $4014
  jsr reset_scroll
  lda #$18
  sta $2001
  jsr wait_vbl
  jsr wait_vbl
  and #$20
  beq ov_f1
  lda #$FF
  sta $0220
  lda #0
  sta $2003
  lda #2
  sta $4014
  jsr wait_vbl
  jsr wait_vbl
  and #$20
  bne ov_f2
  lda #0
  sta $2001
  jmp pass
ov_f1:
  lda #0
  sta $2001
  jmp fail1
ov_f2:
  lda #0
  sta $2001
  jmp fail2
t_pad:
  lda #1
  sta $4016
  lda #0
  sta $4016
  ldx #8
pd_l:
  lda $4016
  and #1
  bne pd_f1
  dex
  bne pd_l
  lda $4016
  and #1
  beq pd_f2
  jmp pass
pd_f1:
  jmp fail1
pd_f2:
  jmp fail2
t_len:
  lda #$40
  sta $4017
  lda #$01
  sta $4015
  lda #$10
  sta $4000
  lda #$18
  sta $4003
  lda $4015
  and #1
  beq ln_f1
  jsr wait_vbl
  jsr wait_vbl
  jsr wait_vbl
  lda $4015
  and #1
  bne ln_f2
  lda #0
  sta $4015
  jmp pass
ln_f1:
  jmp fail1
ln_f2:
  jmp fail2
t_firq:
  lda #0
  sta irq_count
  ldx #0
  ldy #0
  lda $4015
  lda #$00
  sta $4017
  cli
fq_l:
  lda irq_count
  bne fq_d
  inx
  bne fq_l
  iny
  bne fq_l
  sei
  jmp fail1
fq_d:
  sei
  stx cnt_lo
  sty cnt_hi
  lda #$40
  sta $4017
  cpy #$0B
  bne fq_f2
  jmp pass
fq_f2:
  jmp fail2
t_dmc:
  lda #$8F
  sta $4010
  lda #$00
  sta $4012
  sta $4013
  lda #$10
  sta $4015
  ldx #0
dm_l:
  nop
  nop
  dex
  bne dm_l
  lda $4015
  and #$90
  cmp #$80
  bne dm_f1
  lda #$00
  sta $4010
  lda $4015
  and #$80
  bne dm_f2
  lda #0
  sta $4015
  jmp pass
dm_f1:
  jmp fail1
dm_f2:
  jmp fail2

tests:
%s
names:
%s
%s
{COMMON}
.org $9000
ji_ok:
  lda #0
  rts
.org $9100
ji_bad:
  lda #1
  rts
.org $FFFA
  .word nmi, reset, irq
"""

def build_suite():
    tests = '\n'.join('  .word %s' % t for t, _ in TESTS)
    names = '\n'.join('  .word nm_%d' % i for i in range(len(TESTS)))
    strs = ''.join(text('nm_%d' % i, 3 + i, 2, n.ljust(18)) for i, (_, n) in enumerate(TESTS))
    strs += text('title', 1, 4, 'NESSYSINTEGER SELF TEST')
    strs += text('firq_txt', 23, 2, 'FRAME IRQ LOOPS: $')
    strs += text('pass_txt', 25, 2, 'ALL TESTS PASSED')
    strs += text('fail_txt', 25, 2, 'SOME TESTS FAILED')
    src = DEFS + (SUITE % (len(TESTS), tests, names, strs)).replace('{COMMON}', COMMON)
    prg, syms = assemble(src, 0x8000, 0x8000)
    return ines(prg, font_table() * 2, mapper=0, mirror_v=True)

# ------------------------------------------------------------------ MMC3
MMC3 = r"""
.org $E000
reset:
  sei
  cld
  ldx #$FF
  txs
  ldx #0
m3init:
  lda m3regs,x
  stx $8000
  sta $8001
  inx
  cpx #8
  bne m3init
  lda #$00
  sta $A000
  lda #$80
  sta $A001
  jsr init_common
  lda #$FF
  ldx #0
m3oam:
  sta $0200,x
  inx
  bne m3oam
  lda #0
  sta $2003
  lda #2
  sta $4014
  ; ---- PRG banking: R6 -> $8000, R7 -> $A000 ----
  ldx #0
pb_l:
  lda #6
  sta $8000
  stx $8001
  lda #7
  sta $8000
  stx $8001
  cpx $8000
  bne bad1
  cpx $A000
  bne bad1
  inx
  cpx #6
  bne pb_l
  lda #$46
  sta $8000
  lda #3
  sta $8001
  lda $C000
  cmp #3
  bne bad2
  lda $8000
  cmp #6
  bne bad2
  lda #$06
  sta $8000
  lda #0
  sta $8001
  ; ---- CHR banking: R2 = bank 9 at $1000 ----
  lda #2
  sta $8000
  lda #9
  sta $8001
  lda #$10
  sta $2006
  lda #$00
  sta $2006
  lda $2007
  lda $2007
  cmp #9
  bne bad3
  lda #2
  sta $8000
  lda #4
  sta $8001
  ; ---- screen ----
  lda #<l1
  sta ptr
  lda #>l1
  sta ptr+1
  jsr print
  lda #<l2
  sta ptr
  lda #>l2
  sta ptr+1
  jsr print
  lda #<l3
  sta ptr
  lda #>l3
  sta ptr+1
  jsr print
  lda #0
  sta $6000
  lda #'O'
  sta $6004
  lda #'K'
  sta $6005
  lda #0
  sta $6006
  ; ---- IRQ split: latch 100, BG $0000, sprites $1000 ----
  jsr wait_vbl
frame:
  lda #0
  sta $2005
  sta $2005
  lda #$08
  sta $2000
  lda #$1E
  sta $2001
  lda #100
  sta $C000
  sta $C001
  sta $E001
  cli
  jsr wait_vbl
  sei
  jmp frame
bad1:
  lda #1
  jmp bad
bad2:
  lda #2
  jmp bad
bad3:
  lda #3
bad:
  sta $6000
  lda #'F'
  sta $6004
  lda #0
  sta $6005
hang:
  jmp hang
irq:
  pha
  sta $E000
  lda #$FE
  sta $2001
  inc irq_count
  pla
  rti
nmi:
  rti
m3regs:
  .byte 0, 2, 4, 5, 6, 7, 0, 1
%s
{COMMON}
.org $FFFA
  .word nmi, reset, irq
"""

def build_mmc3():
    strs = text('l1', 4, 4, 'MMC3 BANKING OK') + text('l2', 8, 4, 'IRQ SPLIT AT LINE 100') + \
           text('l3', 20, 4, 'THIS PART IS TINTED')
    last, syms = assemble(DEFS + (MMC3 % strs).replace('{COMMON}', COMMON), 0x2000, 0xE000)
    prg = b''.join(bytes([k]) * 0x2000 for k in range(7)) + last
    font = font_table()
    chr_ = font + b''.join(bytes([k]) * 0x400 for k in range(4, 16))
    return ines(prg, chr_, mapper=4)

# ------------------------------------------------------------------ MMC1
MMC1 = r"""
.org $C000
reset:
  sei
  cld
  ldx #$FF
  txs
  lda #$80
  sta $8000
  lda #$0E
  jsr mmc1_ctrl
  jsr init_common
  ldx #0
b_l:
  txa
  jsr mmc1_prg
  cpx $8000
  bne b_bad
  inx
  cpx #7
  bne b_l
  lda #<ok_txt
  sta ptr
  lda #>ok_txt
  sta ptr+1
  jsr print
  lda #0
  sta $6000
  lda #'O'
  sta $6004
  lda #'K'
  sta $6005
  lda #0
  sta $6006
  jmp show
b_bad:
  stx $6005
  lda #1
  sta $6000
  lda #<bad_txt
  sta ptr
  lda #>bad_txt
  sta ptr+1
  jsr print
show:
  jsr wait_vbl
  lda #0
  sta $2005
  sta $2005
  sta $2000
  lda #$0A
  sta $2001
spin:
  jmp spin
mmc1_ctrl:
  sta $8000
  lsr a
  sta $8000
  lsr a
  sta $8000
  lsr a
  sta $8000
  lsr a
  sta $8000
  rts
mmc1_prg:
  sta $E000
  lsr a
  sta $E000
  lsr a
  sta $E000
  lsr a
  sta $E000
  lsr a
  sta $E000
  rts
nmi:
  rti
irq:
  rti
%s
{COMMON}
.org $FFFA
  .word nmi, reset, irq
"""

def build_mmc1():
    strs = text('ok_txt', 6, 4, 'MMC1 PRG BANKING OK') + text('bad_txt', 6, 4, 'MMC1 PRG BANKING FAILED')
    last, syms = assemble(DEFS + (MMC1 % strs).replace('{COMMON}', COMMON), 0x4000, 0xC000)
    prg = b''.join(bytes([k]) * 0x4000 for k in range(7)) + last
    return ines(prg, font_table() * 2, mapper=1)

if __name__ == '__main__':
    out = sys.argv[1] if len(sys.argv) > 1 else '.'
    os.makedirs(out, exist_ok=True)
    for name, data in [('selftest.nes', build_suite()), ('mmc3test.nes', build_mmc3()), ('mmc1test.nes', build_mmc1())]:
        open(os.path.join(out, name), 'wb').write(data)
        print('wrote', name, len(data), 'bytes')
