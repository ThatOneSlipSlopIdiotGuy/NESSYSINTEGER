# Test results

Manifest: `manifest.json` (SHA-256 `631125d9e84a7d5ffcb687c3b2f65d02ba7636cbe3728430b2ea727de18d3829`)

| verdict | cases |
|---|---|
| PASS | 160 |
| REVISION_DEPENDENT | 4 |

| suite | PASS | REVISION_DEPENDENT |
|---|---|---|
| apu_reset | 24 |  |
| apu_test | 36 |  |
| blargg_apu_2005.07.30 | 44 |  |
| cpu_interrupts_v2 | 20 |  |
| cpu_reset | 8 |  |
| mmc3_test_2 | 20 | 4 |
| sprdma_and_dmc_dma | 8 |  |

Everything that is not PASS:

- `mmc3_test_2/rom_singles/6-MMC3_alt` (a0 r0): **REVISION_DEPENDENT** - status $02 (tests the MMC3A-style IRQ (reload to 0 raises no IRQ); MMC3B/C is emulated)
- `mmc3_test_2/rom_singles/6-MMC3_alt` (a1 r0): **REVISION_DEPENDENT** - status $02 (tests the MMC3A-style IRQ (reload to 0 raises no IRQ); MMC3B/C is emulated)
- `mmc3_test_2/rom_singles/6-MMC3_alt` (a2 r0): **REVISION_DEPENDENT** - status $02 (tests the MMC3A-style IRQ (reload to 0 raises no IRQ); MMC3B/C is emulated)
- `mmc3_test_2/rom_singles/6-MMC3_alt` (a3 r0): **REVISION_DEPENDENT** - status $02 (tests the MMC3A-style IRQ (reload to 0 raises no IRQ); MMC3B/C is emulated)
