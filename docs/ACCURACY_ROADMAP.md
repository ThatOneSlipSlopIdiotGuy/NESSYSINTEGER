# Accuracy roadmap

Prioritised work, kept current rather than rewritten each session. The owner's order:

1. The console core (CPU, PPU, APU, DMA, reset/power, I/O).
2. Picture and sound fidelity.
3. Cartridge chips and the physical cartridge simulation.

Every item needs evidence before it is called done.

**How each item is written up:**

- **Cost:** a rough session count.
- **Risk:** what could regress.
- **Done when:** the evidence that closes the item.

## Done (2026-11-18, 0.7.3)

| Item | Evidence |
|---|---|
| A reliable test runner: verdicts from the status byte, pinned ROMs, RESET handshake | [MEGAPACK_ERRATA.md](MEGAPACK_ERRATA.md), `tests/suite`, 25 unit tests |
| RESET leaves `$4015` = 0 | apu_reset/4015_cleared; `dma_trace` |
| Frame counter held during RESET, restarted 9–10 cycles before the first instruction | apu_reset/4017_timing reads 9/10; blargg_apu_2005 09.reset_timing passes at power and after RESET |
| Mixer uses the NESdev formula | apu_mixer/triangle residual −23.2 → −35.3 dB |
| MMC3 PRG-RAM no longer switches off at the first register write | 12 MMC3 test ROMs report through `$6000` again |
| /IRQ sampled at the same point as /NMI | mmc3_test_2 4-scanline_timing passes at all four alignments (failed at 1–3) |

## Open, in priority order

### 1. Screen-only test suites (core coverage)

- **The gap:** these suites report only on screen, or through RAM bytes not yet identified, so the runner can't judge them:
  - `sprite_hit_tests_2005.10.05`, `sprite_overflow_tests`, `blargg_ppu_tests_2005.09.15b`, `vbl_nmi_timing`;
  - `branch_timing_tests`, `cpu_timing_test6`, `dmc_dma_during_read4`, `read_joy3`;
  - `mmc3_irq_tests`, `cpu_dummy_reads`, `dmc_tests`.
- **Cost:** 1 session.
- **Risk:** none to the emulator. Each suite's result location must be proven from its source or assembled ROM, as was done for `$F0` in the 2005 APU tests.
- **Done when:** each suite is in the manifest with a documented protocol (`zp_result` or a new nametable-text reader in `nessys_cli`), or listed as `UNVERIFIED_MANUAL` with the reason.

### 2. nestest and SingleStepTests `nes6502` (CPU bus-level proof)

- **What:** compare every instruction's registers and every cycle's bus access.
  - nestest: `other/nestest.nes`, with `nestest.log` in the same collection. Needs a true per-instruction trace adapter; `-T` is not one.
  - SingleStepTests: the `nes6502` set only. Needs a small harness that runs `cpu.c` on a flat 64 KB bus.
- **Cost:** 1–2 sessions.
- **Risk:** low.
- **Done when:** 0 mismatches. Unstable unofficial opcodes are listed where the reference has no data.

### 3. MesenCE differential comparison

- **What:** a version-pinned MesenCE build, comparing CPU bus logs and frame pixels on scripted input.
- **Cost:** 2 sessions.
- **Risk:** disagreements need hardware evidence to settle; a famous emulator isn't ground truth.

### 4. `apu_mixer` objective oracle

- **Residuals now:**
  - square −31.5 dB, triangle −35.3, noise −16.0, DMC −33.3, all relative to the beeps;
  - square and DMC are spikes at each transition, where the `$4011` write lands a few cycles off the channel's step.
- **Open question:** whether hardware has the same spikes. That needs a recording of a real console, or a sound argument from the test's cycle counts.
- **Done when:** thresholds are justified and the four ROMs move from `UNVERIFIED_MANUAL` to a measured verdict.

### 5. Reset details

- **What:** blargg saw 11–12 in `4017_timing`; NESSYSINTEGER gives 9–10. "IRQ inhibit is sometimes cleared" at RESET is modelled as always.
- **Open question:** the cold power-on APU phase (blargg: usually 9; NESSYSINTEGER: 10).
- **Cost:** research only, until there's a die-level or measured source (Visual 2A03 / breaknes).
- **Risk:** moving the power-on phase moves every DMA's get/put alignment at power-on.

### 6. Colour and video signal (owner's request)

- **What:** the palette is already generated from a composite-signal model.
- **Next:** compare against measured palettes (2C02G captures), offer decoder choices (ideal vs. a consumer TV's decoder), and keep raw PPU output separate from TV simulation.
- **Done when:** documented measured references exist and the user can choose a decoder.

### 7. Real-time play (owner's request)

- **What:** frame pacing, VRR, input latency, audio underruns.
- **Measured so far:** Super Mario Bros. runs 3600 frames in about 14 s headless on this Linux container (about 4.3× real time, with another job running).
- **Not measured:** Windows, and Windows 7 SP1 was never tested.

### 8. Cartridge chips (milestone 4, after the core)

- **MMC3 `$A001` power-on value:** UNKNOWN; 0.7.3 starts it enabled.
- **MMC3A IRQ:** submapper 4.
- **MMC6 on iNES 1.0 headers:** can't be detected.
- **SOROM save-RAM banking.**
- **About 30 more boards:** see the README's roadmap, plus mapper 28 (Action 53; `other/Streemerz_bundle.nes` in nes-test-roms is one).
- **Board-level contact physics research:** only after this list.
