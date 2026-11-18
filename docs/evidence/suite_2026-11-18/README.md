# Test-suite evidence, 2026-11-18

Raw output of `tests/suite/run_suite.py` for NESSYSINTEGER 0.7.3 (branch `accuracy-audit-1`), on
Linux x86-64, with the ROMs pinned in `tests/suite/manifest.json`. Each folder holds that run's
`results.md` (summary), `results.csv` and `results.json` (every case, with its command line).

| Folder | Run |
|---|---|
| `ram4_align01/` | All 126 cases at CPU/PPU alignments 0 and 1, power-on RAM mode 4. `comparison_with_megapack_core.json` lines the result up with the MegaPack's original Core CSV |
| `ram4_align23/` | All 126 cases at alignments 2 and 3, RAM mode 4 |
| `ram0_timing_suites_part1/`, `ram0_timing_suites_part2/` | The timing-sensitive suites at all four alignments with RAM mode 0 (all zeros) |
| `before_irq_fix_matrix/` | The PPU/MMC3 matrix before /IRQ was sampled with /NMI: `mmc3_test_2/4-scanline_timing` failed (`$02`) at alignments 1-3 |
| `accuracycoin_matrix.txt` | `accuracycoin_run` at 4 alignments x RAM modes 0-4, for the 147-test upstream ROM (`new_`) and the owner's 144-test copy (`old_`) |

The paths inside the command lines are the ones on the machine that ran them.
