# Accuracy test ledger

Every test result NESSYSINTEGER claims, with the exact ROM, source, setup, command and verdict. The
machine-readable side is in two places:

- [`tests/suite/manifest.json`](../tests/suite/manifest.json): the contract. For each ROM it gives the source commit, SHA-256, protocol, frame budget, wall-clock limit and expected behaviour.
- [`evidence/suite_2026-11-18/`](evidence/suite_2026-11-18/): what a run produced (`results.csv`, `results.json` and `results.md` for each configuration).

Verdict names are defined at the top of [`tests/suite/run_suite.py`](../tests/suite/run_suite.py).

## Baseline configuration

| | |
|---|---|
| Build | NESSYSINTEGER 0.7.3, branch `accuracy-audit-1` (base `3cd1598`), `make` with gcc on Linux x86-64 (`-std=c99 -O2 -Wall`, no warnings) |
| Console | NTSC NES-001 front-loader (`console: NES` in every report) |
| CPU/PPU clock alignment | 0–3 (the matrix below) |
| Power-on RAM | Mode 4, "like a real console" (the CLI's fixed console fingerprint, `console_id` 1313166129). Mode 0 (all zeros) for the timing-sensitive suites |
| Cartridge contacts | Ideal. Every JSON report must say `"perfect": true` or the runner rejects the case |
| Lockout chip | Enabled; the handshake succeeded in every report (`cic_ok`) |
| RESET | One press per new `$81`, 15 frames after the request, held 4 frames, through the console's reset line |

Tested on Linux only. The Windows build was not built or run in this session.

**Reproduce:**

```
make && make test
python3 tests/suite/run_suite.py --roms nes-test-roms=DIR --roms AccuracyCoin=DIR --align 0,1 --ram 4
python3 tests/suite/run_suite.py --roms ... --align 2,3 --ram 4
python3 tests/suite/run_suite.py --roms ... --align 0,1,2,3 --ram 0 --only apu_reset --only ...
```

## Results, 2026-11-18

### Pinned manifest (126 ROMs), all four alignments, RAM mode 4

| Suite | Area | Protocol | ROMs | Runs (ROMs × 4 alignments) | Verdicts |
|---|---|---|---|---|---|
| AccuracyCoin | Integration | accuracycoin | 1 | 4 | PASS 4 |
| apu_mixer | APU | blargg6000 | 4 | 16 | UNVERIFIED_MANUAL 16 |
| apu_reset | APU/reset | blargg6000 | 6 | 24 | PASS 24 |
| apu_test | APU | blargg6000 | 9 | 36 | PASS 36 |
| blargg_apu_2005.07.30 | APU | zp_result | 11 | 44 | PASS 44 |
| cpu_dummy_writes | CPU | blargg6000 | 2 | 8 | PASS 8 |
| cpu_exec_space | CPU | blargg6000 | 2 | 8 | PASS 8 |
| cpu_interrupts_v2 | CPU | blargg6000 | 6 | 24 | PASS 24 |
| cpu_reset | CPU/reset | blargg6000 | 2 | 8 | PASS 8 |
| instr_misc | CPU | blargg6000 | 5 | 20 | PASS 20 |
| instr_test-v3 | CPU | blargg6000 | 17 | 68 | PASS 68 |
| instr_test-v5 | CPU | blargg6000 | 18 | 72 | PASS 72 |
| instr_timing | CPU | blargg6000 | 3 | 12 | PASS 12 |
| mmc3_test | Mapper/MMC3 | blargg6000 | 6 | 24 | PASS 20, REVISION_DEPENDENT 4 |
| mmc3_test_2 | Mapper/MMC3 | blargg6000 | 6 | 24 | PASS 20, REVISION_DEPENDENT 4 |
| nes_instr_test | CPU | blargg6000 | 11 | 44 | PASS 44 |
| oam_read | PPU | blargg6000 | 1 | 4 | PASS 4 |
| oam_stress | PPU | blargg6000 | 1 | 4 | PASS 4 |
| ppu_open_bus | PPU | blargg6000 | 1 | 4 | PASS 4 |
| ppu_read_buffer | PPU | blargg6000 | 1 | 4 | PASS 4 |
| ppu_vbl_nmi | PPU | blargg6000 | 11 | 44 | PASS 44 |
| sprdma_and_dmc_dma | DMA | blargg6000 | 2 | 8 | PASS 8 |
| **Total** | | | 126 | 504 | PASS 480, REVISION_DEPENDENT 8, UNVERIFIED_MANUAL 16 |

The non-PASS cases are explained in the sections below. Group by suite before counting: several
suites overlap and are not independent evidence.

- `instr_test-v5`'s combined ROMs repeat its singles.
- `instr_test-v3` and `nes_instr_test` are older versions of the same tests.
- `ppu_vbl_nmi.nes` combines its singles.
- `mmc3_test` is an older version of `mmc3_test_2`.

### Timing-sensitive suites, all four alignments, RAM mode 0

| Suite | ROMs | Runs (ROMs × 4 alignments) | Verdicts |
|---|---|---|---|
| apu_reset | 6 | 24 | PASS 24 |
| apu_test | 9 | 36 | PASS 36 |
| blargg_apu_2005.07.30 | 11 | 44 | PASS 44 |
| cpu_exec_space | 2 | 8 | PASS 8 |
| cpu_interrupts_v2 | 5 | 20 | PASS 20 |
| cpu_reset | 2 | 8 | PASS 8 |
| instr_misc | 4 | 16 | PASS 16 |
| instr_timing | 2 | 8 | PASS 8 |
| mmc3_test_2 | 6 | 24 | PASS 20, REVISION_DEPENDENT 4 |
| oam_read | 1 | 4 | PASS 4 |
| ppu_open_bus | 1 | 4 | PASS 4 |
| ppu_read_buffer | 1 | 4 | PASS 4 |
| ppu_vbl_nmi | 10 | 40 | PASS 40 |
| sprdma_and_dmc_dma | 2 | 8 | PASS 8 |
| **Total** | 62 | 248 | PASS 244, REVISION_DEPENDENT 4 |

### AccuracyCoin

| ROM | SHA-256 | Configurations | Result |
|---|---|---|---|
| AccuracyCoin, upstream `48e596f` (147 tests) | `10e4cbbb…bad` | 4 alignments × RAM modes 0–4 | 20/20 runs 147/147 |
| AccuracyCoin, the owner's 144-test copy | `5de89b27…f31` | 4 alignments × RAM modes 0–4 | 20/20 runs 144/144 |

### Built-in suites (`make test`)

| Suite | Result |
|---|---|
| `tests/roms/selftest.nes` | `$00` ALL TESTS PASSED (18 checks) |
| `tests/roms/mmc3test.nes`, `mmc1test.nes` | `$00` OK |
| `dma_trace` | 34 checks, 0 mismatches. The 4 reset-line checks are new |
| `mapper_test` | 773 checks, 0 failures |

### Not PASS, and why

| Case | Verdict | Why |
|---|---|---|
| `apu_mixer/{square,triangle,noise,dmc}` | UNVERIFIED_MANUAL | The ROM's status only says it finished. The real check is "near silence between the beeps"; residuals are measured below |
| `mmc3_test_2/rom_singles/6-MMC3_alt` | REVISION_DEPENDENT (`$02`) | Tests the MMC3A's IRQ behaviour; NESSYSINTEGER emulates the MMC3B/C |
| `mmc3_test/6-MMC6` | REVISION_DEPENDENT (`$03`) | Tests the MMC6, but its iNES 1.0 header can't say MMC6 |

### `apu_mixer` residuals

**How they were measured:**

- RMS of the cancelled section between the two beeps, relative to the beep's RMS.
- Audio from `nessys_cli -w` (44.1 kHz, after the NES output filters).
- Script: [`tests/suite/mixer_residual.py`](../tests/suite/mixer_residual.py) (20 ms windows, DC removed), for example `nessys_cli apu_mixer/triangle.nes -f 1300 -w t.wav -q`, then `python3 tests/suite/mixer_residual.py t.wav`.

| ROM | 0.7.3 before the mixer change (3:2:1 table) | 0.7.3 (NESdev formula) |
|---|---|---|
| square | −31.4 dB | −31.5 dB |
| triangle | −23.2 dB | **−35.3 dB** |
| noise | −15.4 dB | −16.0 dB (the readme says noise fades in and out) |
| dmc | −30.8 dB | −33.3 dB |

**What the residuals look like:**

- Before the change, the triangle residual was a steady ±280-sample tone: the triangle wasn't cancelled.
- After it, only a small ripple is left.
- The square and DMC residuals are spikes at each transition, where the `$4011` write lands a few cycles off the channel's step.

No hardware recording was available to say what residual a real console leaves.

## Before and after, for each change made in this session

| Change | Before (0.7.2 core) | After (0.7.3) |
|---|---|---|
| `nessys_cli` RESET handshake | `apu_reset/4017_written`: three presses (frames 28, 44, 60), `$03` | Two presses (requests at frames 13 and 46), `$00` |
| RESET clears `$4015` | `apu_reset/4015_cleared` with one correct press: `$03` | `$00` |
| Frame counter restart | `4017_timing` prints 8 at power, 8 or 9 after RESET; `blargg_apu_2005.07.30/09.reset_timing`: `$04` at power | 10 at power, 9 or 10 after RESET; 09.reset_timing: `$01` at power at all 4 alignments and after 144 RESETs covering both APU phases |
| Mixer formula | See the residual table | See the residual table |
| MMC3 `$A001` power-on | All 12 `mmc3_test`/`mmc3_test_2` ROMs: NO_SIGNATURE. The screen showed "Passed", but PRG-RAM was switched off | 10 PASS, 2 REVISION_DEPENDENT |
| /IRQ sampled with /NMI | `mmc3_test_2/4-scanline_timing`: `$02` at alignments 1, 2, 3 (RAM 0 and 4, 6 runs) | PASS at all four |

**Games.** Seven games were run on 0.7.2 and 0.7.3 with the same scripted input and compared at the last frame:

- **The games:** Super Mario Bros., SMB3, Kirby's Adventure, Zelda, Lagrange Point (VRC7), and the two SMB2j conversions (mappers 40 and 142).
- **Pictures:** identical (0 of 61440 pixels differ).
- **Sound:** the difference is 28–45 dB below the music. That is the mixer change; the timing of the sound is the same.

## Not covered yet

- **Screen-only suites:** in nes-test-roms but not in the manifest, because the runner can't read their verdict yet. See [ACCURACY_ROADMAP.md](ACCURACY_ROADMAP.md) item 1.
- **Other protocols:** `pal_apu_tests` (PAL: would be UNSUPPORTED_TARGET), `other/nestest.nes` (needs a per-instruction trace adapter), and the demos and games in `other/`.
- **SingleStepTests `nes6502`:** not run.
- **MesenCE comparison:** not run.
- **The MegaPack's "Overkill" results:** not supplied.
- **Windows:** the Windows build, Windows 7 SP1, and real-time pacing and latency were not measured in this session.
