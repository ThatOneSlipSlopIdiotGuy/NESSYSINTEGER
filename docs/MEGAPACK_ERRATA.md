# MegaPack errata

What was wrong in the "NESSYSINTEGER Accuracy MegaPack" results that ChatGPT (GPT-6) produced on
2026-11-18, how each error was reproduced, and what the corrected result is. Some of the errors were
in NESSYSINTEGER's own tools, not the MegaPack; those are listed too, because the MegaPack trusted
them.

- **Original evidence, unmodified:** [`evidence/megapack_2026-11-18/CORE_RESULTS_ORIGINAL_2026-11-18.csv`](evidence/megapack_2026-11-18/CORE_RESULTS_ORIGINAL_2026-11-18.csv).
  - 41 rows.
  - SHA-256 `292742fd3900953cf632921f2afcb57cc5c79db029e6b069a4e6475fca3bb729`, the same as the copy in the owner's zip.
- **Corrected interpretation, one row per original row:** [`evidence/megapack_2026-11-18/CORE_RESULTS_CORRECTED_2026-11-18.csv`](evidence/megapack_2026-11-18/CORE_RESULTS_CORRECTED_2026-11-18.csv).
- **What could not be inspected:**
  - The MegaPack's `scripts/run_nessys.py`, its download scripts, its batch launchers and its "Overkill" results were not in the zip the owner supplied. They are not in this repository either.
  - The descriptions of `run_nessys.py` below are therefore taken from the owner's master prompt: a fixed `--frames 1800`, and `classify(mode, rc, out)` returning FAIL for any nonzero exit.
  - That code has not been read. The errors attributed to it are the ones its recorded outputs prove.

## Summary

| | Original (MegaPack) | Corrected (2026-11-18, NESSYSINTEGER 0.7.3) |
|---|---|---|
| Rows | 41 | 41 |
| PASS | 38 | 41 |
| FAIL | 3 | 0 |
| Valid as evidence | — | 37 of the 38 original PASS rows; 4015_cleared's was a false pass |

The three FAILs were two time-budget truncations and one harness reset bug. One original PASS was
not valid evidence, and behind it was a real emulator bug, now fixed.

## Errors

### E1. `$80` (still running) was classified as FAIL

- **Rows:** `instr_test-v5/all_instrs.nes` (`result $80: Running test 10 of 16`) and `instr_test-v5/official_only.nes` (`result $80: Running test 16 of 16`).
- **Error:**
  - blargg's `$6000` protocol defines `$80` as "the test is running".
  - The MegaPack stopped every ROM after 1800 frames and called the nonzero exit a failure.
  - Exit 1 was also what `nessys_cli` 0.7.2 returned for *any* non-success, running included. So the exit code alone couldn't tell running from failed. That was a NESSYSINTEGER tool weakness.
- **Reproduced:**
  - `nessys_cli instr_test-v5/all_instrs.nes -f 1800` still shows `$80`.
  - With an adequate budget, both ROMs end with status `$00` and "All 16 tests passed". all_instrs needs 2399 frames; official_only needs 1872.
- **Corrected:** both PASS. The manifest gives them 7200 and 5700 frames.
- **Fix:**
  - `nessys_cli` now has distinct exit codes: 0 passed, 1 failed, 3 still running, 4 waiting for RESET, 5 no signature, 6 undefined status.
  - It also writes a JSON report (`-j`).
  - `tests/suite/run_suite.py` never derives a verdict from the exit code. A still-running ROM at the end of its budget is `INCOMPLETE_EMULATED_BUDGET`.

### E2. `4017_written` "Failed #3": the RESET handshake pressed RESET two or three times

- **Row:** `apu_reset/4017_written.nes`, `result $03`, reported FAIL.
- **Root cause:** a bug in `nessys_cli` 0.7.2 (NESSYSINTEGER's tool, written by Claude), not in the APU.
  - The CLI scheduled a RESET whenever `$6000` read `$81` and no press was pending.
  - After a reset, the test keeps the old `$81` in `$6000` for about six frames before rewriting it.
  - So the CLI pressed RESET again 16 frames later (frame 44, only 2 frames after the real request), and again at frame 60, in the middle of the test.
- **Reproduced:** with a scratch copy of the CLI loop (`resettrace.c`), the presses landed at frames 28, 44 and 60.
- **Corrected:**
  - The CLI now presses RESET once per *new* `$81`.
  - It presses 15 frames (250 ms) after the request, longer than the protocol's minimum of 100 ms, through the same held-button path as the Windows program (4 frames).
  - The ROM passes with exactly 2 resets (requests at frames 13 and 46).
  - The press and release frames and cycles are in the JSON report.
- **The APU's `$4017` handling was already right:** the reset keeps the mode and clears the IRQ inhibit flag.

### E3. `4015_cleared` was a false PASS, and behind it was a real emulator bug

- **Row:** `apu_reset/4015_cleared.nes`, reported PASS.
- **Why the pass was invalid:** the spurious second RESET from E2 arrived after the test program had already cleared `$4015` itself. The ROM therefore saw `$4015` = 0 for the wrong reason.
- **Reproduced:**
  - With one correct press, NESSYSINTEGER 0.7.2 fails #3: "At reset, $4015 should be cleared".
  - The soft reset zeroed the length counters but left the channel enable bits set, so a length load right after RESET was accepted.
- **Fixed in 0.7.3** (`src/apu.c`, `apu_power`):
  - Power and RESET both leave every channel disabled.
  - A length-counter load still pending from the cycle before is dropped.
- **Hardware basis:**
  - NESdev "CPU power up state": after reset, `$4015` = 0, all channels disabled. That page's figures were measured on an RP2A03G front-loader.
  - blargg's readme: "At power and reset, $4015 is cleared."
- **Regression:** a new check in `dma_trace`, "RESET disables the channels ($4015 = 0)".

### E4. `4017_timing` passed with a value real hardware doesn't give

- **Row:** `apu_reset/4017_timing.nes`, PASS, printing "Delay after effective $4017 write: 8".
- **Problem:** the test accepts 6–12, but blargg measured "9 to 12 clocks" on hardware ("usually 9" after a cold power-on).
- **Confirmed as a real 1-cycle error:**
  - blargg's older `blargg_apu_2005.07.30/09.reset_timing.nes` was not in the MegaPack.
  - On 0.7.2 it fails at power-on with code `$04`, "Fourth step occurs too late".
- **Fixed in 0.7.3:** the frame counter restarts as if `$4017` were written on the cycle before the last held cycle.
  - `4017_timing` now reads 10 at power and 9 or 10 after RESET, depending on the APU's even/odd phase.
  - `09.reset_timing` passes at power and after RESET in both phases (144 RESET runs, all `$01`).

### E5. Classification from stdout text and exit codes

- **Problem:** the MegaPack's evidence column says "explicit pass text + success exit" and "trusted $6000 suite returned nonzero". Its verdicts came from words in the printed text and from the exit code.
- **Two rows show why that's unsafe:**
  - The `cpu_dummy_writes` rows' excerpts don't contain the status line at all.
  - Words like "passed" or "failed" can appear in a running test's text.
- **Corrected:** `run_suite.py` takes the verdict from the status byte in the emulator's JSON report. It requires the exit code, the report and the frame/cycle counts to agree, otherwise the case is `HARNESS_ERROR`. Unit tests cover the cases where text and status disagree.

### E6. No revisions, hashes or budgets were recorded

- **Problem:**
  - The CSV names no source commit and no ROM hash.
  - The AccuracyCoin row doesn't say which AccuracyCoin. It changed on 2026-11-13, from 144 to 147 tests.
  - Every ROM got the same 1800-frame budget.
- **Corrected:**
  - `tests/suite/manifest.json` pins nes-test-roms at `95d8f62` and AccuracyCoin at `48e596f`.
  - It holds a SHA-256 for every ROM (and for AccuracyCoin's `.asm`), and a budget of about 3× each ROM's measured need.
  - The runner refuses a ROM whose hash differs (`WRONG_REVISION`).

### E7. Overlapping ROMs counted as independent evidence

- **Problem:** 38/41 counts `instr_test-v5`'s two combined ROMs and its 16 singles as 18 results. They test the same instructions.
- **Corrected:** the ledger groups results by suite and marks combined ROMs and older revisions (instr_test-v3, nes_instr_test) as overlapping.

### E8. The `$DE $B0 $G1` signature in blargg's readmes

- **Fact:** the readmes write the `$6001-$6003` signature as `$DE $B0 $G1`. `$G1` is not hexadecimal.
- **Bytes actually written:** `DE B0 61`, checked three ways:
  - The assembled ROMs contain `DE B0 61`, or the immediates `A9 DE`, `A9 B0`, `A9 61`.
  - All 114 `$6000`-protocol ROMs in the pinned collection are recognised by those bytes.
  - Every one of them reports through them.
- **Conclusion:** `$G1` is a typo for `$61`. Nothing to fix; recorded so nobody "corrects" the code to the typo.

## Remaining doubts

- **`4017_timing`:** blargg also saw 11 and 12 on hardware. NESSYSINTEGER only produces 9 and 10, the two values that come from the APU's even/odd phase. Where the extra 1–2 cycles come from on hardware (the reset circuit, or the 2A03's /RES synchroniser) is UNKNOWN.
- **Power-on value:** the power-on value is 10. blargg's "usually 9" for a cold console means the APU's phase at power-on favours the other value on his console. The power-on phase isn't changed, because that would move every DMA's get/put alignment at power-on, and nothing in the evidence says which one a given console picks.
- **The MegaPack's Overkill results** were not supplied, so they have not been reviewed.
