# Accuracy research

Claim-by-claim sources for NESSYSINTEGER's hardware behaviour.

**What counts as a source here:**

- An entry is listed only if it was actually read. Where a source couldn't be reached, that is said.
- "Measured" means someone measured a real console.
- "Test author" means the behaviour is documented by the person whose test ROM was verified on hardware.
- "Inference" means it was worked out, not measured.

**How the sources were read:**

- All access dates are 2026-11-18 (UTC) unless noted.
- Test-ROM sources were read in local clones at the commits pinned in [`tests/suite/manifest.json`](../tests/suite/manifest.json):
  - nes-test-roms `95d8f621ae55cee0d09b91519a8989ae0e64753b` (2022-03-02);
  - AccuracyCoin `48e596ff468ecb06e22e24dd34089bd7df77ecd7` (2026-11-13).

## Sources read

| # | Source | Kind | Read |
|---|---|---|---|
| S1 | NESdev Wiki, [CPU power up state](https://nesdev.org/wiki/CPU_power_up_state) (revision 22091) | Measured: NTSC front-loader, RP2A03G, NES-CPU-07 board | Fetched |
| S2 | blargg, `apu_reset/readme.txt` and `apu_reset/source/*.s` | Test author | Local clone |
| S3 | blargg, `blargg_apu_2005.07.30/readme.txt`, `tests.txt` and `source/*.asm` | Test author | Local clone, and [a mirror of tests.txt](https://rust-digger.code-maven.com/src/reznez-0.0.0/tests/roms/blargg_apu_2005-07-30/tests.txt) |
| S4 | NESdev Wiki, [APU Mixer](https://nesdev.org/wiki/APU_Mixer) (page last edited 27 October 2026) | Formula; cites blargg's `apu_ref.txt` | Fetched |
| S5 | blargg, `apu_mixer/readme.txt` and `apu_mixer/source/triangle.s` | Test author | Local clone |
| S6 | NESdev Wiki, [Talk:MMC3](https://www.nesdev.org/wiki/Talk:MMC3) | Discussion | Fetched |
| S7 | blargg, `mmc3_test/` and `mmc3_test_2/` readmes and sources | Test author | Local clone |
| S8 | blargg, `instr_test-v5/readme.txt` | Test author | Local clone |
| S9 | 100thCoin, [AccuracyCoin](https://github.com/100thCoin/AccuracyCoin) (MIT) | Test author | Local clone |

**Not consulted in this session.** These are named in the owner's directive and remain on the roadmap:

- MesenCE source;
- SingleStepTests `nes6502`;
- the nestest log comparison;
- Visual 2A03 / breaknes die analysis;
- the NESdev pages on DMA, PPU rendering and controller reading.

The behaviour those cover was implemented in earlier sessions, from sources not re-read today.

**Pages that couldn't be fetched:**

- The `www.nesdev.org` host is blocked by this environment's fetch allowlist.
- The same pages on `nesdev.org` (no `www`) work once a search returns them.
- The `nesdev-wiki.nes.science` mirror returned empty pages.

## Claims

### Reset and power-on (APU)

| Claim | Source | Kind | In NESSYSINTEGER |
|---|---|---|---|
| After RESET, `$4015` = 0: all channels disabled | S1 ("APU was silenced ($4015 = 0)"), S2 (4015_cleared: "At power and reset, $4015 is cleared") | Measured + test author | `apu_power` clears the enable bits and any pending length load (0.7.3). Before 0.7.3 only the length counters were zeroed |
| After RESET, the `$4017` mode is unchanged | S1, S2 (4017_written: "At reset, $4017 mode is unchanged, but IRQ inhibit flag is sometimes cleared") | Measured + test author | Mode kept; the inhibit flag is always cleared (the "sometimes" is not modelled) |
| At power and reset the APU acts as if `$4017` were written, then 9–12 clocks later the reset vector runs ("usually 9" after a minute powered off) | S2 (4017_timing), S3 ("After reset or power-up, APU acts as if $4017 were written with $00 from 9 to 12 clocks before first instruction begins") | Test author, measured on his console | The frame counter restarts at the release of RESET as if written on the cycle before the last held cycle: 9 or 10 by `4017_timing`'s count. 0.7.2 gave 8 at power and 8 or 9 after RESET |
| "At power-up and reset, the APU is randomly in an odd or even cycle with respect to the first clock of the first instruction" | S3 readme ("Clock Jitter") | Test author | The APU's even/odd phase is the CPU cycle counter's parity. After RESET it depends on how long the button was held; at power-on it is fixed (10 by `4017_timing`) |
| Holding RESET keeps the frame counter still; the delay to the first instruction doesn't depend on how long it is held | S2 (4017_timing gives the same range at power and at reset) | Inference from the test | `FrameCtr.held`; checked by `dma_trace` ("no frame IRQ while RESET is held", "restart: flag at W+29831 / W+29830") |
| Frame IRQ flag "set three times in a row 29831 clocks after writing $4017 with $00" | S3 (`07.irq_flag_timing.asm`) | Test author | Matches (checked by 07.irq_flag_timing, 08.irq_timing and AccuracyCoin) |

**Unresolved:**

- blargg also saw 11 and 12, which the model can't produce. Their cause is UNKNOWN.
- The power-on value differs from blargg's "usually 9" (it is 10). See [MEGAPACK_ERRATA.md](MEGAPACK_ERRATA.md#remaining-doubts).

### Mixer

| Claim | Source | Kind | In NESSYSINTEGER |
|---|---|---|---|
| `pulse_out = 95.88 / (8128 / (p1 + p2) + 100)`; `tnd_out = 159.79 / (1 / (t/8227 + n/12241 + d/22638) + 100)` | S4 | Formula from blargg's `apu_ref.txt` (S4 doesn't say how it was obtained) | Used directly since 0.7.3 (`tnd_out()` in `src/apu.c`) |
| The 203-entry table indexed by `3t + 2n + d` approximates `tnd_out` "within 4%" with adjusted numerators | S4 | Wiki | Used until 0.7.2 |
| One triangle step equals about 2.75 DMC steps, not 3 | S5: `triangle.s` cancels the triangle's 15 steps with DMC levels 0…41 (2.73 per step). S4's weights give 22638/8227 = 2.75 | Test author (hardware-verified cancellation test) | With the formula, the triangle residual in `apu_mixer/triangle` drops from −23.2 dB to −35.3 dB relative to the beeps (see the ledger) |

### Cartridge: MMC3 `$A001`

| Claim | Source | Kind | In NESSYSINTEGER |
|---|---|---|---|
| `$A001` bits 7/6 enable and write-protect PRG-RAM; emulators often ignore them because the MMC6 uses the register differently; FCEUX ignores it for mapper 4 | S6 | Discussion | Implemented |
| Power-on value of `$A001` | Not found in S6 or S7 | UNKNOWN | 0.7.3 starts it at `$80` (enabled, writable). 0.7.2 started with the RAM enabled but turned it off at the first write to any MMC3 register. blargg's MMC3 tests never write `$A001` and expect `$6000` output to work |
| `6-MMC3_alt` tests the MMC3A-style IRQ; `6-MMC6` tests MMC6 IRQ behaviour | S7 | Test author | REVISION_DEPENDENT: the MMC3A isn't modelled, and an iNES 1.0 header can't select the MMC6 |

### Test protocols

| Claim | Source | Kind |
|---|---|---|
| `$6000`: `$80` running, `$81` press RESET at least 100 ms later, `$00`–`$7F` result; text from `$6004`; signature at `$6001-$6003` | S2 and every readme of that framework | Test author |
| The readmes' `$DE $B0 $G1` is `DE B0 61` | All 114 `$6000` ROMs in the pinned collection contain those bytes (or the matching `LDA #imm`s) and report through them | Checked against the ROMs |
| Only a test that prints "done" needs watching or listening; one that prints "passed" passed | S2, S5 readmes ("Flashes, clicks, other glitches") | Test author |
| `all_instrs.nes` and `official_only.nes` combine the singles | S8 | Test author |
| The 2005 APU tests keep their result code at `$F0` (`sta <result`) and show it on screen | S3 sources (`09.reset_timing.asm`), and the assembled ROM (`85 F0` after `LDA #2`) | Checked against the ROMs |
