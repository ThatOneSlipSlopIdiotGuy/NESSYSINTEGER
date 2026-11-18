#!/usr/bin/env python3
"""mixer_residual.py - how much sound is left in blargg's apu_mixer tests.

Each apu_mixer ROM beeps, plays a channel together with its inverse on the DMC DAC (which should
cancel to near silence), then beeps again. This prints the RMS of the cancelled stretch relative to
the first beep, in dB. Record the audio first:

    nessys_cli apu_mixer/triangle.nes -f 1300 -w triangle.wav -q
    python3 tests/suite/mixer_residual.py triangle.wav

Method: 20 ms windows with their DC removed; the beeps are the first two runs of windows louder
than 1500 after the first second; the measured stretch starts 0.6 s after the first beep and ends
0.2 s before the second. Standard library only. Hardware residuals are unknown, so this measures;
it does not judge."""
import math
import struct
import sys
import wave


def residual(path):
    w = wave.open(path)
    if w.getsampwidth() != 2 or w.getnchannels() != 1:
        raise ValueError("%s: expected 16-bit mono" % path)
    rate = w.getframerate()
    raw = w.readframes(w.getnframes())
    s = struct.unpack("<%dh" % (len(raw) // 2), raw)
    win = rate // 50
    rms = []
    for i in range(0, len(s) - win, win):
        seg = s[i:i + win]
        m = sum(seg) / len(seg)
        rms.append(math.sqrt(sum((x - m) ** 2 for x in seg) / len(seg)))
    runs, start = [], None
    for i, r in enumerate(rms):
        if r > 1500 and i > 50:
            if start is None:
                start = i
        elif start is not None:
            runs.append((start, i))
            start = None
    if len(runs) < 2:
        raise ValueError("%s: couldn't find the two beeps" % path)
    b1, b2 = runs[0], runs[1]
    beep = sum(rms[b1[0] + 2:b1[1] - 2]) / max(1, b1[1] - b1[0] - 4)
    mid = rms[b1[1] + 30:b2[0] - 10]
    tot = math.sqrt(sum(r * r for r in mid) / len(mid))
    return beep, tot, len(mid) / 50.0


def main():
    if len(sys.argv) < 2:
        print(__doc__)
        return 2
    for p in sys.argv[1:]:
        beep, res, secs = residual(p)
        print("%-28s beep %6.0f  residual %6.1f RMS = %6.1f dB  (%.1f s measured)" % (p, beep, res, 20 * math.log10(res / beep), secs))
    return 0


if __name__ == "__main__":
    sys.exit(main())
