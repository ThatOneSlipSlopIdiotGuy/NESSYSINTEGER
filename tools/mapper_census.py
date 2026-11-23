#!/usr/bin/env python3
"""mapper_census.py - which boards does a ROM collection need, and which ones can NESSYSINTEGER
run today?

Reads only the 16-byte iNES/NES 2.0 header of every .nes file (also inside .zip files) in a
folder and its subfolders. Nothing from the games themselves is
copied or printed; only file names and header numbers.

    python tools/mapper_census.py D:\\Roms\\NES
    python tools/mapper_census.py D:\\Roms\\NES --csv census.csv

Prints a table of every mapper number found, how many games use it, and whether NESSYSINTEGER
supports it, ordered so the unsupported boards that unlock the most games come first.
Standard library only, Python 3.8 or newer."""
import argparse
import collections
import csv
import os
import sys
import zipfile

# Mapper numbers NESSYSINTEGER 0.7.3 emulates: the README's "Supported mappers" table, and exactly the
# numbers nes_load_rom accepted when all 4096 were tried (2026-11-22). Update it with new boards.
SUPPORTED = {0, 1, 2, 3, 4, 5, 7, 9, 10, 11, 13, 16, 18, 19, 21, 22, 23, 24, 25, 26, 32, 33, 34, 40,
             64, 65, 66, 67, 68, 69, 71, 73, 75, 76, 78, 79, 80, 82, 85, 87, 94, 118, 119, 140, 142,
             159, 180, 185, 206, 210}


def parse_header(h):
    """Returns (mapper, submapper, nes2, tv) or None if this isn't an iNES file."""
    if len(h) < 16 or h[:4] != b"NES\x1a":
        return None
    nes2 = (h[7] & 0x0C) == 0x08
    mapper = (h[6] >> 4) | (h[7] & 0xF0)
    sub = 0
    if nes2:
        mapper |= (h[8] & 0x0F) << 8
        sub = h[8] >> 4
        tv = ("NTSC", "PAL", "multi", "Dendy")[h[12] & 3]
    else:
        # Old dumps often carry junk in bytes 8-15 ("DiskDude!"); then only the low nibble is real.
        if any(h[12:16]):
            mapper &= 0x0F
        tv = "PAL" if h[9] & 1 else "NTSC?"
    return mapper, sub, nes2, tv


def scan(root):
    for dirpath, _, files in os.walk(root):
        for f in sorted(files):
            p = os.path.join(dirpath, f)
            low = f.lower()
            try:
                if low.endswith(".nes"):
                    with open(p, "rb") as fh:
                        yield os.path.relpath(p, root), fh.read(16)
                elif low.endswith(".fds"):
                    yield os.path.relpath(p, root), b"FDS"
                elif low.endswith(".zip"):
                    with zipfile.ZipFile(p) as z:
                        for name in z.namelist():
                            n = name.lower()
                            if n.endswith(".nes"):
                                with z.open(name) as fh:
                                    yield os.path.relpath(p, root) + "/" + name, fh.read(16)
                            elif n.endswith(".fds"):
                                yield os.path.relpath(p, root) + "/" + name, b"FDS"
            except (OSError, zipfile.BadZipFile) as e:
                print("skipped %s: %s" % (p, e), file=sys.stderr)


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("folder")
    ap.add_argument("--csv", help="also write one line per game here")
    a = ap.parse_args()
    games, bad, fds = [], 0, 0
    for name, head in scan(a.folder):
        if head == b"FDS":
            fds += 1
            games.append((name, "FDS", "", "", "Famicom Disk System (not emulated yet)"))
            continue
        r = parse_header(head)
        if r is None:
            bad += 1
            continue
        m, sub, nes2, tv = r
        games.append((name, m, sub, tv, "yes" if m in SUPPORTED else "no"))
    count = collections.Counter(g[1] for g in games if g[1] != "FDS")
    total = sum(count.values())
    ok = sum(n for m, n in count.items() if m in SUPPORTED)
    pal = sum(1 for g in games if g[3] == "PAL")
    print("%d cartridge ROMs, %d disk images, %d files without an iNES header" % (total, fds, bad))
    print("NESSYSINTEGER runs the board of %d of %d cartridges (%.1f%%)" % (ok, total, 100.0 * ok / max(1, total)))
    print("%d marked PAL in their header (PAL timing isn't emulated yet)\n" % pal)
    print("Unsupported boards, most games first:")
    print("  mapper  games")
    for m, n in sorted(((m, n) for m, n in count.items() if m not in SUPPORTED), key=lambda x: (-x[1], x[0])):
        print("  %6d  %5d" % (m, n))
    print("\nSupported boards found:")
    print("  mapper  games")
    for m, n in sorted(((m, n) for m, n in count.items() if m in SUPPORTED), key=lambda x: (-x[1], x[0])):
        print("  %6d  %5d" % (m, n))
    if a.csv:
        with open(a.csv, "w", newline="", encoding="utf-8") as f:
            w = csv.writer(f)
            w.writerow(["file", "mapper", "submapper", "tv", "supported"])
            w.writerows(games)
        print("\none line per game in %s" % a.csv)
    return 0


if __name__ == "__main__":
    sys.exit(main())
