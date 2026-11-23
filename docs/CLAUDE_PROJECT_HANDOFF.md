# Project handoff (for the next Claude session)

**Last verified:** 2026-11-23 (UTC), README audit. The accuracy audit was 2026-11-18.

**Repository:** https://github.com/ThatOneSlipSlopIdiotGuy/NESSYSINTEGER

- **`main`:** the owner merged `accuracy-audit-1` (0.7.3) as `8d4b0fc`.
- **Latest work:** branch `readme-audit`, based on `8d4b0fc`, offered as a pull request. Nothing is pushed to `main` directly.
- **Worktree:** clean after the final commit.

## 2026-11-22/23: README audit (verified)

Every claim in `README.md` was checked against the code, the test results, or a fresh run:

- **Mapper list:** exactly 50 numbers are accepted, confirmed by trying all 4096 with `nes_load_rom`.
- **Pinout tables:** match `PIN72`/`PIN60` in `src/cart.c` and NESdev's pinout.
- **Menus, keys, defaults and file names:** read from `win/main.c`.
- **"Things to try":** the pin 26 High/Low results and the lockout from contact 35 were reproduced with `nessys_cli`.
- **Self-test:** passes at all four alignments.

**Fixed:**

- **The license:** `scoop/nessysinteger.json` said MIT, but the repository is PolyForm Noncommercial 1.0.0. The release zip now carries `LICENSE` and `NOTICE` (`package.ps1`), and the README has a License section.
- **Visual Studio builds:** they needed the VC++ runtime DLL despite the README's "no DLLs". They now link it statically: `/MT` in `build.bat`, `CMAKE_MSVC_RUNTIME_LIBRARY` in `CMakeLists.txt` (CMake 3.15+). The CMake change was checked on Linux; MSVC itself was not available.
- **Transparency in the README:**
  - a platform table (Windows 10 played on; 0.7.3 tested only on Linux; macOS and Windows 7 and older untested);
  - the GUI's defaults: *Wear and dirt* is on. Simulating 100,000 default console + cartridge pairs gave about 0.4% with a flaky contact;
  - which parts of the connector physics and the power-off corruption odds are a model, not measurements;
  - the colour palette not being compared with real consoles;
  - the earlier CPU fuzz test having no harness in the repository;
  - /IRQ sampled with /NMI.
- **The runner's contact check:** it now also rejects runs with any open or flaky contact. `perfect` doesn't cover the lockout chip's contacts. The full suite re-ran at alignment 0 with the same result (120 PASS, 4 UNVERIFIED_MANUAL, 2 REVISION_DEPENDENT).
- **`tools/mapper_census.py`:** reads ROM headers and reports board coverage.
  - On the owner's 5,827-cartridge collection: 4,400 supported (75.5%), 1,427 on 217 missing boards; the top 10 would give 88.9%; 457 dumps are PAL; no .fds images.
  - Missing boards, most-needed first: 30 (367 dumps), 176 (96), 268 (81), 163 (71), 224 (39), 28 (35), 178 (33), 150 (20), 90 (18), 148 (18).

**Observed but not changed:** the Windows front end already offers the four picture shapes the owner asked about for a Minecraft TV: 8:7 aspect and overscan crop, each switchable.

## Owner's decisions (keep these)

- **Keep:** the classic Windows GUI and the physical cartridge/connector simulation.
- **Order:** core hardware accuracy first; colour and video fidelity next; cartridge chips and pin physics after the core is trustworthy.
- **Tests:**
  - They "have to pass".
  - When one fails, first rule out the virtual cartridge: ideal contacts and a clean connector. The runner enforces `perfect: true` and no open or flaky contact.
  - Then find the real cause. Never special-case a ROM.
- **Skipping:** allowed. Always say what was skipped and why.
- **Credits:** "Emulator design and programming: Claude"; "Logo artwork and play testing: ThatOneSlipSlopIdiotGuy".
- **Workflow:** changes go through a branch and a pull request the owner merges with GitHub Desktop.
- **Owner's backlog:** fix SOROM saves, add the MMC3A, about 30 more boards.

## Build and test

| What | Command |
|---|---|
| Build (Linux/macOS: CLI tools only) | `make` |
| Build (Windows) | `build.bat` (Scoop gcc or MSVC), `make` (w64devkit/MSYS2) or CMake |
| Built-in tests | `make test`: selftest/mmc3test/mmc1test ROMs, `dma_trace` (34), `mapper_test` (773) |
| Runner's own unit tests | `python3 -m unittest discover -s tests/suite` (25 tests) |
| Test-ROM battery | `python3 tests/suite/run_suite.py --roms nes-test-roms=DIR --roms AccuracyCoin=DIR [--align 0,1,2,3] [--ram 0,4] [--only NAME]` |
| AccuracyCoin directly | `./accuracycoin_run AccuracyCoin.nes AccuracyCoin.asm -a N -r M` |

**Test ROM sources:** not in the repository; clone them.

- `github.com/christopherpow/nes-test-roms` at `95d8f621ae55cee0d09b91519a8989ae0e64753b`.
- `github.com/100thCoin/AccuracyCoin` at `48e596ff468ecb06e22e24dd34089bd7df77ecd7`.
- The manifest holds each ROM's SHA-256.

**Minimum Windows:**

- The README claims none beyond "Windows". The owner reported a Windows 7 SP1 x64 floor from an earlier discussion.
- That floor has never been tested. Nothing in this session touched the Windows front end or its imports.

## What this session did (verified)

Details and evidence are in [MEGAPACK_ERRATA.md](MEGAPACK_ERRATA.md) and [ACCURACY_TEST_LEDGER.md](ACCURACY_TEST_LEDGER.md).

1. **Audited GPT-6's MegaPack Core CSV** (41 rows: 38 PASS, 3 FAIL).
   - The 3 FAILs were two budget truncations (`$80` = still running) and one RESET-handshake bug in `nessys_cli` 0.7.2.
   - One PASS (`4015_cleared`) was false and hid a real APU bug.
   - The original CSV is preserved unmodified in `docs/evidence/megapack_2026-11-18/`, with a corrected interpretation beside it.
2. **`nessys_cli` (`tools/nessys_cli.c`):**
   - One RESET per new `$81`, through the held-button path (`reset_pulse`).
   - Distinct exit codes (0 pass, 1 fail, 2 usage, 3 running, 4 RESET pending, 5 no signature, 6 unknown status).
   - `-j` JSON report (status, text, resets with frames and cycles, frames, cycles, PC, contacts, `-m` bytes).
   - `-r` power-on RAM mode.
   - `NESSYS_VERSION` "0.7.3" in `src/nes.h`.
3. **APU reset (`src/apu.c`, `src/nes.c`):**
   - RESET clears `$4015` (enable bits and pending length loads).
   - The frame counter is held while RESET is down (`FrameCtr.held`, which fits in old padding, so `NesState` stays 117792 bytes and save states are compatible).
   - `apu_reset_line()` is shared by the GUI, `nes_reset` and the CLI.
   - `fc_restart()` restarts the frame counter as if `$4017` were written on the cycle before the last held cycle: 9 or 10 cycles before the first instruction.
4. **Mixer (`src/apu.c`):** NESdev's `tnd_out` formula (weights 1/8227, 1/12241, 1/22638) and the 95.88 pulse numerator, replacing the 3:2:1 lookup table. No measurable speed cost.
5. **MMC3 (`src/cart.c`):** `$A001` starts at `$80`. Before, PRG-RAM switched off at the first MMC3 register write. The MMC6 is unchanged.
6. **CPU (`src/cpu.c`):** /IRQ is sampled at `NMI_PT` (9 master clocks into the cycle), like /NMI. NESdev "CPU interrupts" says both are polled during φ2. This fixed `mmc3_test_2/4-scanline_timing` at alignments 1–3.
7. **Test runner `tests/suite/`:**
   - `run_suite.py`: verdict state machine, consistency checks, evidence mode, comparison with a historical CSV.
   - `manifest.json`: 126 pinned ROMs.
   - `make_manifest.py`: regenerates the manifest.
   - `test_run_suite.py` with `fake_cli.py`: 25 unit tests.
   - `run_suite.bat`: the Windows launcher (CRLF).
   - `mixer_residual.py`: measures how much sound `apu_mixer` leaves between its beeps.

**Results:** see the ledger.

- Pinned battery: every case with a machine-readable verdict passes at all four alignments.
- 4 `apu_mixer` cases are UNVERIFIED_MANUAL.
- 2 MMC3 cases are REVISION_DEPENDENT.
- AccuracyCoin 147/147 and 144/144: 40/40 configurations.
- `make test` is green.
- The seven games' pictures are identical to 0.7.2.

## Confidence by subsystem

| Subsystem | Confidence | Why |
|---|---|---|
| CPU | High | instr_test v3/v5, instr_misc, instr_timing, cpu_interrupts_v2, cpu_exec_space, cpu_dummy_writes and AccuracyCoin pass at 4 alignments. Not yet compared bus-cycle by bus-cycle with SingleStepTests or nestest |
| PPU | High for what's tested | ppu_vbl_nmi, ppu_open_bus, ppu_read_buffer, oam_read, oam_stress and AccuracyCoin pass. The sprite-hit and overflow suites (screen-only) aren't in the battery yet |
| APU | High for timing, medium for analogue output | apu_test, apu_reset, blargg_apu_2005 and AccuracyCoin pass. Mixer levels follow the NESdev formula; no hardware recording to compare |
| DMA | High | sprdma_and_dmc_dma, `dma_trace`, AccuracyCoin's DMA tests |
| Reset and I/O | High for APU and CPU reset | apu_reset and cpu_reset pass through a real handshake. "Usually 9" and 11–12 are open (roadmap 5) |
| GUI | Not run since 0.7.2 (no Windows here) | Menus, keys and defaults were read from `win/main.c` for the README audit; behaviour depends on the owner's play testing |
| Mappers and contacts | `mapper_test` 773 green; MMC3 blargg tests pass | The MMC3 `$A001` power-on value is UNKNOWN |
| Save states | Layout unchanged (117792 bytes) | `in_reset`/`reset_pulse` live outside `NesState` (front-end state), as before |

## Open problems and hypotheses

- **`4017_timing`:** hardware reads 9–12; the model gives 9–10, and 10 at power-on where blargg usually saw 9. UNKNOWN cause.
- **`apu_mixer`:** square and DMC residuals are transition spikes. Whether hardware has them too is unverified.
- **Screen-only suites:** sprite_hit_tests_2005, sprite_overflow_tests, blargg_ppu_tests_2005, vbl_nmi_timing and others aren't in the manifest yet.
- **Not run:** nestest, SingleStepTests and MesenCE comparisons.

## Most important files

- **Core:** `src/cpu.c` (`start_cycle`/`end_cycle` timing points), `src/apu.c` (`fc_restart`, `apu_reset_line`, `tnd_out`), `src/nes.c` (`nes_run_frame` reset line), `src/cart.c` (MMC3).
- **Tools:** `tools/nessys_cli.c`, `tools/accuracycoin_run.c`, `tests/suite/run_suite.py`, `tests/suite/manifest.json`.
- **Docs:** this file, `ACCURACY_TEST_LEDGER.md`, `ACCURACY_RESEARCH.md`, `ACCURACY_ROADMAP.md`, `MEGAPACK_ERRATA.md`.

## START HERE NEXT SESSION

1. Clone the repository. Check out `main` if the owner merged `readme-audit`, otherwise that branch.
2. Run `make && make test` and expect everything green.
3. Clone nes-test-roms and AccuracyCoin at the commits above, then run `python3 tests/suite/run_suite.py --roms nes-test-roms=... --roms AccuracyCoin=... --dry-run`. All 126 lines should say `ok`.
4. Without `--dry-run` it should report only PASS, the 4 `apu_mixer` UNVERIFIED_MANUAL and the 2 MMC3 REVISION_DEPENDENT cases.
5. Then take roadmap item 1:
   - Add the screen-only suites, starting with `sprite_hit_tests_2005.10.05` and `sprite_overflow_tests`.
   - Find where each ROM keeps its result code: grep its source for `sta <result` and confirm the byte in the assembled ROM, as was done for `$F0`.
   - Add the suites to `make_manifest.py`, regenerate the manifest, and run them at all four alignments.
