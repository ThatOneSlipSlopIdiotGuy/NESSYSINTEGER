#!/usr/bin/env python3
"""make_manifest.py - rebuilds manifest.json from local clones of the pinned test-ROM sources.

    python3 tests/suite/make_manifest.py --nes-test-roms DIR --accuracycoin DIR > tests/suite/manifest.json

The clones must be at the commits named in SOURCES below (the script checks with git when it can).
What each suite is for, how long it may run and what to expect is written down here, not guessed:
budgets are about three times what the ROM needed on NESSYSINTEGER 0.7.3, rounded up, and never
under 600 frames (10 emulated seconds). Standard library only."""
import argparse
import hashlib
import json
import os
import re
import subprocess
import sys

SOURCES = {
    "nes-test-roms": {
        "url": "https://github.com/christopherpow/nes-test-roms",
        "commit": "95d8f621ae55cee0d09b91519a8989ae0e64753b",
        "date": "2022-03-02",
        "license": "No licence file in the collection; the ROMs are blargg's (Shay Green) and other authors' "
                   "public test programs. NESSYSINTEGER does not redistribute them.",
    },
    "AccuracyCoin": {
        "url": "https://github.com/100thCoin/AccuracyCoin",
        "commit": "48e596ff468ecb06e22e24dd34089bd7df77ecd7",
        "date": "2026-11-13",
        "license": "MIT (Chris Siebert). NESSYSINTEGER does not redistribute it.",
    },
}

# suite folder -> (area, notes, frames override, extra fields)
SUITES = {
    "apu_mixer": ("APU", "Cancels each channel against an inverted copy on the DMC DAC; pass = near silence between the "
                  "beeps. The status byte only says the program finished.", None,
                  {"manual": "listen: near silence between the beeps (noise fades in and out); see ACCURACY_TEST_LEDGER.md for measured residuals"}),
    "apu_reset": ("APU/reset", "Needs RESET presses through the $81 handshake.", None, {"max_resets": 2}),
    "apu_test": ("APU", "", None, {}),
    "cpu_dummy_writes": ("CPU", "", None, {}),
    "cpu_exec_space": ("CPU", "Executes code from the I/O register space (open bus).", None, {}),
    "cpu_interrupts_v2": ("CPU", "", None, {}),
    "cpu_reset": ("CPU/reset", "Needs one RESET press through the $81 handshake.", None, {"max_resets": 1}),
    "instr_misc": ("CPU", "", None, {}),
    "instr_test-v3": ("CPU", "Older revision of instr_test; overlaps instr_test-v5, not independent evidence.", None, {}),
    "instr_test-v5": ("CPU", "all_instrs/official_only combine the singles; not independent evidence.", None, {}),
    "instr_timing": ("CPU", "", None, {}),
    "mmc3_test": ("Mapper/MMC3", "Older revision of mmc3_test_2.", None, {}),
    "mmc3_test_2": ("Mapper/MMC3", "MMC3 scanline counter driven by PPU A12.", None, {}),
    "nes_instr_test": ("CPU", "Older instruction tests; overlap instr_test-v5.", None, {}),
    "oam_read": ("PPU", "", None, {}),
    "oam_stress": ("PPU", "", None, {}),
    "ppu_open_bus": ("PPU", "", None, {}),
    "ppu_read_buffer": ("PPU", "", None, {}),
    "ppu_vbl_nmi": ("PPU", "ppu_vbl_nmi.nes combines the singles.", None, {}),
    "sprdma_and_dmc_dma": ("DMA", "", None, {}),
}
# individual ROMs made for a chip revision NESSYSINTEGER doesn't emulate
REVISION_DEPENDENT = {
    "mmc3_test_2/rom_singles/6-MMC3_alt.nes": "tests the MMC3A-style IRQ (reload to 0 raises no IRQ); MMC3B/C is emulated",
    "mmc3_test/6-MMC6.nes": "tests MMC6 IRQ behaviour, but its iNES 1.0 header can't say MMC6, so it runs as an MMC3B/C",
}
# frames each ROM needed on 0.7.3 (frames_run in the JSON report); budgets are derived from these
NEEDED = {}
SKIP = {"other/Streemerz_bundle.nes": "a homebrew game bundle (mapper 28), not a test"}


def sha256(path):
    with open(path, "rb") as f:
        return hashlib.sha256(f.read()).hexdigest()


def git_head(d):
    try:
        return subprocess.run(["git", "-C", d, "rev-parse", "HEAD"], stdout=subprocess.PIPE, stderr=subprocess.DEVNULL,
                              timeout=20).stdout.decode().strip()
    except (OSError, subprocess.SubprocessError):
        return ""


def blargg_codes_2005(tests_txt):
    """'NN.name' sections of blargg_apu_2005.07.30/tests.txt with their numbered result codes."""
    out, cur = {}, None
    for line in open(tests_txt, encoding="latin-1"):
        m = re.match(r"^(\d\d\.\w+)\s*$", line)
        if m:
            cur = m.group(1)
            out[cur] = {}
        m = re.match(r"^(\d+)\) (.+?)\s*$", line)
        if m and cur:
            out[cur][m.group(1)] = m.group(2)
    return out


def budget(needed):
    return max(600, -(-needed * 3 // 100) * 100)


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--nes-test-roms", required=True)
    ap.add_argument("--accuracycoin", required=True)
    ap.add_argument("--needed", help="JSON {path: frames} measured with nessys_cli -j (frames_run)")
    a = ap.parse_args()
    for name, d in (("nes-test-roms", a.nes_test_roms), ("AccuracyCoin", a.accuracycoin)):
        head = git_head(d)
        if head and head != SOURCES[name]["commit"]:
            sys.exit("%s is at %s, the manifest pins %s" % (d, head, SOURCES[name]["commit"]))
    needed = json.load(open(a.needed)) if a.needed else NEEDED
    cases = []
    root = a.nes_test_roms
    for suite in sorted(SUITES):
        area, notes, frames, extra = SUITES[suite]
        for dirpath, _, files in sorted(os.walk(os.path.join(root, suite))):
            for f in sorted(files):
                if not f.lower().endswith(".nes"):
                    continue
                full = os.path.join(dirpath, f)
                rel = os.path.relpath(full, root).replace(os.sep, "/")
                if rel in SKIP:
                    continue
                data = open(full, "rb").read()
                if b"\xde\xb0\x61" not in data and not (b"\xa9\xde" in data and b"\xa9\xb0" in data and b"\xa9\x61" in data):
                    continue                                  # not a $6000-protocol ROM
                c = {"id": rel[:-4], "suite": suite, "area": area, "source": "nes-test-roms", "path": rel,
                     "sha256": sha256(full), "protocol": "blargg6000",
                     "frames": frames or budget(needed.get(rel, 1000) + 300 * extra.get("max_resets", 0)),
                     "notes": notes}
                c["wall_seconds"] = max(60, c["frames"] // 100)
                c.update(extra)
                if rel in REVISION_DEPENDENT:
                    c["revision_dependent"] = REVISION_DEPENDENT[rel]
                cases.append(c)
    codes = blargg_codes_2005(os.path.join(root, "blargg_apu_2005.07.30", "tests.txt"))
    for f in sorted(os.listdir(os.path.join(root, "blargg_apu_2005.07.30"))):
        if not f.endswith(".nes"):
            continue
        rel = "blargg_apu_2005.07.30/" + f
        cases.append({"id": rel[:-4], "suite": "blargg_apu_2005.07.30", "area": "APU", "source": "nes-test-roms", "path": rel,
                      "sha256": sha256(os.path.join(root, rel)), "protocol": "zp_result", "frames": 300, "wall_seconds": 60,
                      "result_addr": "00F0", "pass_value": 1, "codes": codes.get(f[:-4], {}),
                      "notes": "Shows its result code on screen and keeps it at $F0 (sta <result in the source); "
                               "no completion signal, so $F0 is read after 300 frames. Each finishes in under 2 s."})
    ac = a.accuracycoin
    cases.append({"id": "AccuracyCoin", "suite": "AccuracyCoin", "area": "Integration", "source": "AccuracyCoin",
                  "path": "AccuracyCoin.nes", "sha256": sha256(os.path.join(ac, "AccuracyCoin.nes")),
                  "asm": "AccuracyCoin.asm", "asm_sha256": sha256(os.path.join(ac, "AccuracyCoin.asm")),
                  "protocol": "accuracycoin", "tests": 147, "wall_seconds": 300,
                  "notes": "accuracycoin_run presses Start on the menu (run every test) and reads each result byte the .asm names."})
    json.dump({"schema": 1, "generated_by": "tests/suite/make_manifest.py", "sources": SOURCES, "skipped": SKIP,
               "cases": cases}, sys.stdout, indent=1)
    sys.stdout.write("\n")


if __name__ == "__main__":
    main()
