# Test results

Manifest: `manifest.json` (SHA-256 `631125d9e84a7d5ffcb687c3b2f65d02ba7636cbe3728430b2ea727de18d3829`)

| verdict | cases |
|---|---|
| PASS | 202 |
| FAIL | 6 |
| REVISION_DEPENDENT | 8 |

| suite | PASS | FAIL | REVISION_DEPENDENT |
|---|---|---|---|
| cpu_exec_space | 16 |  |  |
| instr_misc | 32 |  |  |
| instr_timing | 16 |  |  |
| mmc3_test_2 | 34 | 6 | 8 |
| oam_read | 8 |  |  |
| ppu_open_bus | 8 |  |  |
| ppu_read_buffer | 8 |  |  |
| ppu_vbl_nmi | 80 |  |  |

Everything that is not PASS:

- `mmc3_test_2/rom_singles/4-scanline_timing` (a1 r0): **FAIL** - status $02
- `mmc3_test_2/rom_singles/4-scanline_timing` (a1 r4): **FAIL** - status $02
- `mmc3_test_2/rom_singles/4-scanline_timing` (a2 r0): **FAIL** - status $02
- `mmc3_test_2/rom_singles/4-scanline_timing` (a2 r4): **FAIL** - status $02
- `mmc3_test_2/rom_singles/4-scanline_timing` (a3 r0): **FAIL** - status $02
- `mmc3_test_2/rom_singles/4-scanline_timing` (a3 r4): **FAIL** - status $02
- `mmc3_test_2/rom_singles/6-MMC3_alt` (a0 r0): **REVISION_DEPENDENT** - status $02 (tests the MMC3A-style IRQ (reload to 0 raises no IRQ); MMC3B/C is emulated)
- `mmc3_test_2/rom_singles/6-MMC3_alt` (a0 r4): **REVISION_DEPENDENT** - status $02 (tests the MMC3A-style IRQ (reload to 0 raises no IRQ); MMC3B/C is emulated)
- `mmc3_test_2/rom_singles/6-MMC3_alt` (a1 r0): **REVISION_DEPENDENT** - status $02 (tests the MMC3A-style IRQ (reload to 0 raises no IRQ); MMC3B/C is emulated)
- `mmc3_test_2/rom_singles/6-MMC3_alt` (a1 r4): **REVISION_DEPENDENT** - status $02 (tests the MMC3A-style IRQ (reload to 0 raises no IRQ); MMC3B/C is emulated)
- `mmc3_test_2/rom_singles/6-MMC3_alt` (a2 r0): **REVISION_DEPENDENT** - status $02 (tests the MMC3A-style IRQ (reload to 0 raises no IRQ); MMC3B/C is emulated)
- `mmc3_test_2/rom_singles/6-MMC3_alt` (a2 r4): **REVISION_DEPENDENT** - status $02 (tests the MMC3A-style IRQ (reload to 0 raises no IRQ); MMC3B/C is emulated)
- `mmc3_test_2/rom_singles/6-MMC3_alt` (a3 r0): **REVISION_DEPENDENT** - status $02 (tests the MMC3A-style IRQ (reload to 0 raises no IRQ); MMC3B/C is emulated)
- `mmc3_test_2/rom_singles/6-MMC3_alt` (a3 r4): **REVISION_DEPENDENT** - status $02 (tests the MMC3A-style IRQ (reload to 0 raises no IRQ); MMC3B/C is emulated)
