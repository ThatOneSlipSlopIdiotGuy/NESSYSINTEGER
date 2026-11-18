#!/usr/bin/env python3
"""Stand-in for nessys_cli in the runner's own tests (test_run_suite.py). The "ROM" is a text file
naming what to act out: a pass, a failure, a crash, a hang, a lying exit code, broken JSON..."""
import json
import os
import signal
import sys
import time


def main():
    args = sys.argv[1:]
    rom = args[0]
    opt = {args[i]: args[i + 1] for i in range(1, len(args) - 1) if args[i] in ("-f", "-a", "-r", "-j", "-m")}
    budget = int(opt.get("-f", 600))
    with open(rom) as f:
        scenario = f.read().strip()
    if scenario == "flaky":                                  # alternates between pass and fail
        mark = rom + ".count"
        n = int(open(mark).read()) if os.path.exists(mark) else 0
        with open(mark, "w") as f:
            f.write(str(n + 1))
        scenario = "pass" if n % 2 == 0 else "fail"
    if scenario == "crash":
        sys.stdout.flush()
        if os.name == "nt":
            os._exit(0xC0000005)
        os.kill(os.getpid(), signal.SIGKILL)
    if scenario == "hang":
        time.sleep(60)
    if scenario == "load_error":
        print("not an iNES file", file=sys.stderr)
        return 2
    status, sig, frames, resets = {
        "pass": (0, True, 50, 0), "fail": (3, True, 50, 0), "running": (0x80, True, budget, 0),
        "reset_then_pass": (0, True, 80, 1), "no_signature": (None, False, budget, 0),
        "lie_exit_zero": (None, False, budget, 0), "garbage_json": (0, True, 50, 0), "no_json": (0, True, 50, 0),
    }[scenario]
    rc = 5 if not sig else 0 if status == 0 else 1 if status < 0x80 else 3 if status == 0x80 else 4
    rep = {"tool": "fake_cli", "signature": sig, "status": status, "exit_code": rc, "frames_run": frames, "frames_budget": budget,
           "cpu_cycles": int(29780.5 * frames) + 7, "text": "\nfake\n\nPassed\n" if status == 0 else "",
           "resets": [{"requested_frame": 10, "pressed_frame": 26, "released_frame": 30}] * resets,
           "connector": {"perfect": True}}
    if "-j" in opt and scenario != "no_json":
        with open(opt["-j"], "w") as f:
            f.write("{not json" if scenario == "garbage_json" else json.dumps(rep))
    return 0 if scenario == "lie_exit_zero" else rc


if __name__ == "__main__":
    sys.exit(main())
