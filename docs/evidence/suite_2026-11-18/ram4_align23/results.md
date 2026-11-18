# Test results

Manifest: `manifest.json` (SHA-256 `631125d9e84a7d5ffcb687c3b2f65d02ba7636cbe3728430b2ea727de18d3829`)

| verdict | cases |
|---|---|
| PASS | 240 |
| UNVERIFIED_MANUAL | 8 |
| REVISION_DEPENDENT | 4 |

| suite | PASS | UNVERIFIED_MANUAL | REVISION_DEPENDENT |
|---|---|---|---|
| AccuracyCoin | 2 |  |  |
| apu_mixer |  | 8 |  |
| apu_reset | 12 |  |  |
| apu_test | 18 |  |  |
| blargg_apu_2005.07.30 | 22 |  |  |
| cpu_dummy_writes | 4 |  |  |
| cpu_exec_space | 4 |  |  |
| cpu_interrupts_v2 | 12 |  |  |
| cpu_reset | 4 |  |  |
| instr_misc | 10 |  |  |
| instr_test-v3 | 34 |  |  |
| instr_test-v5 | 36 |  |  |
| instr_timing | 6 |  |  |
| mmc3_test | 10 |  | 2 |
| mmc3_test_2 | 10 |  | 2 |
| nes_instr_test | 22 |  |  |
| oam_read | 2 |  |  |
| oam_stress | 2 |  |  |
| ppu_open_bus | 2 |  |  |
| ppu_read_buffer | 2 |  |  |
| ppu_vbl_nmi | 22 |  |  |
| sprdma_and_dmc_dma | 4 |  |  |

Everything that is not PASS:

- `apu_mixer/dmc` (a2 r4): **UNVERIFIED_MANUAL** - listen: near silence between the beeps (noise fades in and out); see ACCURACY_TEST_LEDGER.md for measured residuals
- `apu_mixer/dmc` (a3 r4): **UNVERIFIED_MANUAL** - listen: near silence between the beeps (noise fades in and out); see ACCURACY_TEST_LEDGER.md for measured residuals
- `apu_mixer/noise` (a2 r4): **UNVERIFIED_MANUAL** - listen: near silence between the beeps (noise fades in and out); see ACCURACY_TEST_LEDGER.md for measured residuals
- `apu_mixer/noise` (a3 r4): **UNVERIFIED_MANUAL** - listen: near silence between the beeps (noise fades in and out); see ACCURACY_TEST_LEDGER.md for measured residuals
- `apu_mixer/square` (a2 r4): **UNVERIFIED_MANUAL** - listen: near silence between the beeps (noise fades in and out); see ACCURACY_TEST_LEDGER.md for measured residuals
- `apu_mixer/square` (a3 r4): **UNVERIFIED_MANUAL** - listen: near silence between the beeps (noise fades in and out); see ACCURACY_TEST_LEDGER.md for measured residuals
- `apu_mixer/triangle` (a2 r4): **UNVERIFIED_MANUAL** - listen: near silence between the beeps (noise fades in and out); see ACCURACY_TEST_LEDGER.md for measured residuals
- `apu_mixer/triangle` (a3 r4): **UNVERIFIED_MANUAL** - listen: near silence between the beeps (noise fades in and out); see ACCURACY_TEST_LEDGER.md for measured residuals
- `mmc3_test/6-MMC6` (a2 r4): **REVISION_DEPENDENT** - status $03 (tests MMC6 IRQ behaviour, but its iNES 1.0 header can't say MMC6, so it runs as an MMC3B/C)
- `mmc3_test/6-MMC6` (a3 r4): **REVISION_DEPENDENT** - status $03 (tests MMC6 IRQ behaviour, but its iNES 1.0 header can't say MMC6, so it runs as an MMC3B/C)
- `mmc3_test_2/rom_singles/6-MMC3_alt` (a2 r4): **REVISION_DEPENDENT** - status $02 (tests the MMC3A-style IRQ (reload to 0 raises no IRQ); MMC3B/C is emulated)
- `mmc3_test_2/rom_singles/6-MMC3_alt` (a3 r4): **REVISION_DEPENDENT** - status $02 (tests the MMC3A-style IRQ (reload to 0 raises no IRQ); MMC3B/C is emulated)
