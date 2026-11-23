#!/usr/bin/env python3
"""run_suite.py - runs NESSYSINTEGER against the pinned test-ROM manifest and gives every case an
auditable verdict.

Standard library only, Python 3.8 or newer. The test ROMs are not part of NESSYSINTEGER: fetch
them from the sources named in the manifest (the exact commits are pinned there) and pass their
folders with --roms NAME=DIR, where NAME is the manifest's source name:

    python3 tests/suite/run_suite.py --roms nes-test-roms=C:/src/nes-test-roms \\
                                     --roms AccuracyCoin=C:/src/AccuracyCoin
    python3 tests/suite/run_suite.py ... --dry-run      # check fixtures and hashes, print commands
    python3 tests/suite/run_suite.py ... --align 0,1,2,3 --ram 0,4 --only apu_reset

No verdict is taken from a process exit code or from words in a ROM's text alone. For blargg's
$6000 protocol the verdict comes from the status byte nessys_cli read from the emulated cartridge
RAM (its -j report), and the exit code, the report and the frame/cycle counts must agree with
each other or the case is a HARNESS_ERROR.

Verdicts
  PASS                        the ROM's own definitive success signal
  FAIL                        a definitive failure code from the ROM
  INCOMPLETE_EMULATED_BUDGET  still running ($80) when the emulated frames ran out
  RESET_REQUESTED             asked for RESET ($81) and the frames ran out before it finished
  RESET_LOOP                  asked for RESET more often than the manifest allows
  NO_SIGNATURE                never wrote the $6000 signature (not started / wrong protocol)
  WRONG_PROTOCOL              a status byte the protocol does not define
  UNVERIFIED_MANUAL           finished, but the result has to be judged by eye or ear
  WALL_TIMEOUT                the host's safety limit stopped the emulator
  CRASH                       the emulator process died
  HARNESS_ERROR               missing/invalid report, inconsistent counts, tool or load error
  MISSING_FIXTURE             the ROM is not in the folder given
  WRONG_REVISION              the ROM's SHA-256 is not the pinned one
  UNSUPPORTED_TARGET          made for hardware the emulator does not model (not run)
  NONDETERMINISTIC            repeated runs (--repeat) disagreed
  REVISION_DEPENDENT          failed a case the manifest marks as written for another chip revision
                              than the one emulated (the reason is in the manifest)
"""
import argparse
import csv
import hashlib
import json
import os
import re
import shlex
import subprocess
import sys
import tempfile
import time

VERDICTS = ("PASS", "FAIL", "INCOMPLETE_EMULATED_BUDGET", "RESET_REQUESTED", "RESET_LOOP", "NO_SIGNATURE",
            "WRONG_PROTOCOL", "UNVERIFIED_MANUAL", "WALL_TIMEOUT", "CRASH", "HARNESS_ERROR",
            "MISSING_FIXTURE", "WRONG_REVISION", "UNSUPPORTED_TARGET", "NONDETERMINISTIC", "REVISION_DEPENDENT")
HERE = os.path.dirname(os.path.abspath(__file__))
CLI_EXIT = {0: "PASS", 1: "FAIL", 3: "RUNNING", 4: "RESET_PENDING", 5: "NO_SIGNATURE", 6: "UNKNOWN_STATUS"}
CYCLES_PER_FRAME = (29000.0, 30500.0)     # NTSC: 29780.5 on average; a held-reset frame is 29781


def sha256(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for block in iter(lambda: f.read(1 << 16), b""):
            h.update(block)
    return h.hexdigest()


# ---------------------------------------------------------------------------- manifest

class ManifestError(Exception):
    pass


def load_manifest(path):
    try:
        with open(path, encoding="utf-8") as f:
            m = json.load(f)
    except (OSError, ValueError) as e:
        raise ManifestError("can't read manifest %s: %s" % (path, e))
    if not isinstance(m, dict) or m.get("schema") != 1 or not isinstance(m.get("cases"), list):
        raise ManifestError("%s: not a schema-1 manifest" % path)
    sources = m.get("sources", {})
    seen = set()
    for c in m["cases"]:
        for key in ("id", "source", "path", "protocol"):
            if not isinstance(c.get(key), str) or not c[key]:
                raise ManifestError("case %r: missing %s" % (c.get("id"), key))
        if c["id"] in seen:
            raise ManifestError("duplicate case id %s" % c["id"])
        seen.add(c["id"])
        if c["source"] not in sources:
            raise ManifestError("case %s: unknown source %s" % (c["id"], c["source"]))
        if c["protocol"] not in ("blargg6000", "zp_result", "accuracycoin"):
            raise ManifestError("case %s: unknown protocol %s" % (c["id"], c["protocol"]))
        if c.get("target", "NTSC") == "NTSC" and not re.fullmatch(r"[0-9a-f]{64}", c.get("sha256", "")):
            raise ManifestError("case %s: no SHA-256" % c["id"])
        need = {"blargg6000": ("frames",), "zp_result": ("frames", "result_addr", "pass_value"), "accuracycoin": ("asm", "tests")}
        for key in need[c["protocol"]]:
            if key not in c or (key in ("frames", "tests") and (type(c[key]) is not int or c[key] <= 0)):
                raise ManifestError("case %s: %s protocol needs %s" % (c["id"], c["protocol"], key))
    return m


# ---------------------------------------------------------------------------- classification

def _verdict(v, detail, **extra):
    r = {"verdict": v, "detail": detail}
    r.update(extra)
    return r


def _crashed(rc):
    """Exit codes that only a dead process produces: killed by a signal (negative on POSIX) or a
    Windows exception status such as 0xC0000005."""
    return rc is not None and (rc < 0 or rc >= 0xC0000000 or rc in (134, 136, 139))


def _contacts_ideal(conn):
    """The emulator's fast-path flag ("perfect") covers the bus, power and control contacts. The
    lockout chip's and the expansion contacts aren't part of it, so the open and flaky counts,
    which cover every contact that carries a signal, must be zero too."""
    return bool(conn.get("perfect", False)) and conn.get("open_pins", 0) == 0 and conn.get("flaky_pins", 0) == 0


def classify_blargg6000(case, proc, report):
    """proc: {"rc", "timed_out", "error", "stdout", "stderr"}; report: the CLI's parsed JSON, or None,
    or {"_invalid": "message"} when the file existed but wasn't valid JSON."""
    if proc.get("error"):
        return _verdict("HARNESS_ERROR", "could not run the emulator: %s" % proc["error"])
    if proc.get("timed_out"):
        return _verdict("WALL_TIMEOUT", "stopped by the %ss wall-clock limit" % case.get("wall_seconds", "?"))
    rc = proc.get("rc")
    if _crashed(rc):
        return _verdict("CRASH", "emulator process died (exit %s)" % rc)
    if rc == 2:
        return _verdict("HARNESS_ERROR", "emulator refused the ROM or arguments: %s" % (proc.get("stderr") or proc.get("stdout") or "").strip()[:200])
    if report is None:
        return _verdict("HARNESS_ERROR", "no JSON report (exit %s)" % rc)
    if "_invalid" in report:
        return _verdict("HARNESS_ERROR", "JSON report unreadable: %s" % report["_invalid"])
    need = ("signature", "status", "exit_code", "frames_run", "frames_budget", "cpu_cycles", "resets", "connector")
    missing = [k for k in need if k not in report]
    if missing:
        return _verdict("HARNESS_ERROR", "JSON report lacks %s" % ", ".join(missing))
    # the report must agree with the process and with itself
    if report["exit_code"] != rc:
        return _verdict("HARNESS_ERROR", "exit code %s but the report says %s" % (rc, report["exit_code"]))
    st, sig = report["status"], report["signature"]
    if sig is not True and sig is not False or (sig and (type(st) is not int or not 0 <= st <= 255)):
        return _verdict("HARNESS_ERROR", "malformed signature/status in the report: %r, %r" % (sig, st))
    want_rc = (5 if not sig else 0 if st == 0 else 1 if st < 0x80 else 3 if st == 0x80 else 4 if st == 0x81 else 6)
    if want_rc != rc:
        return _verdict("HARNESS_ERROR", "status %r with signature %r doesn't give exit %s" % (st, sig, rc))
    fr, budget, cyc = report["frames_run"], report["frames_budget"], report["cpu_cycles"]
    if not (0 <= fr <= budget) or (case.get("frames") and budget != case["frames"]):
        return _verdict("HARNESS_ERROR", "ran %s of %s frames (manifest budget %s)" % (fr, budget, case.get("frames")))
    if fr >= 10 and not (CYCLES_PER_FRAME[0] * fr <= cyc <= CYCLES_PER_FRAME[1] * (fr + 1)):
        return _verdict("HARNESS_ERROR", "%s CPU cycles is not %s NTSC frames" % (cyc, fr))
    conn = report["connector"]
    if not _contacts_ideal(conn):
        return _verdict("HARNESS_ERROR", "cartridge contacts were not ideal: %s" % json.dumps(conn))
    resets = report["resets"]
    extra = {"status": st, "frames_run": fr, "resets": len(resets), "text": report.get("text", "")}
    if len(resets) > case.get("max_resets", 3):
        return _verdict("RESET_LOOP", "%d RESET requests (manifest allows %d)" % (len(resets), case.get("max_resets", 3)), **extra)
    if not sig:
        return _verdict("NO_SIGNATURE", "no $DE $B0 $61 at $6001 after %d frames" % fr, **extra)
    text = report.get("text", "")
    if st == 0x81 or any(e.get("released_frame", -1) < 0 for e in resets):
        return _verdict("RESET_REQUESTED", "waiting for or inside a RESET when %d frames ran out" % fr, **extra)
    if st == 0x80:
        return _verdict("INCOMPLETE_EMULATED_BUDGET", "still running ($80) after %d frames" % fr, **extra)
    if st == 0:
        last = [l.strip() for l in text.splitlines() if l.strip()]
        if case.get("manual") or (last and last[-1].lower() == "done"):
            return _verdict("UNVERIFIED_MANUAL", case.get("manual") or "the ROM printed 'done': judge it by eye/ear", **extra)
        return _verdict("PASS", "status $00", **extra)
    if st < 0x80:
        return _verdict("FAIL", "status $%02X" % st, **extra)
    return _verdict("WRONG_PROTOCOL", "status $%02X is not defined by the $6000 protocol" % st, **extra)


def classify_zp_result(case, proc, report):
    """Tests from before the $6000 protocol keep their result code in a RAM byte and only show it on
    screen. There is no completion signal, so the byte is read after a fixed emulated budget that
    the manifest sets well past the test's documented run time."""
    if proc.get("error"):
        return _verdict("HARNESS_ERROR", "could not run the emulator: %s" % proc["error"])
    if proc.get("timed_out"):
        return _verdict("WALL_TIMEOUT", "stopped by the %ss wall-clock limit" % case.get("wall_seconds", "?"))
    rc = proc.get("rc")
    if _crashed(rc):
        return _verdict("CRASH", "emulator process died (exit %s)" % rc)
    if report is None or "_invalid" in report or "memory" not in report:
        return _verdict("HARNESS_ERROR", "no usable JSON report with the result byte (exit %s)" % rc)
    if report.get("exit_code") != rc:
        return _verdict("HARNESS_ERROR", "exit code %s but the report says %s" % (rc, report.get("exit_code")))
    if report.get("frames_run") != case.get("frames"):
        return _verdict("HARNESS_ERROR", "ran %s frames, the manifest says %s" % (report.get("frames_run"), case.get("frames")))
    if not _contacts_ideal(report.get("connector", {})):
        return _verdict("HARNESS_ERROR", "cartridge contacts were not ideal")
    mem = report["memory"]
    addr = int(case["result_addr"], 16)
    if mem.get("addr") != addr or not mem.get("bytes"):
        return _verdict("HARNESS_ERROR", "the report holds $%04X, not the result byte $%04X" % (mem.get("addr", 0), addr))
    v = mem["bytes"][0]
    codes = case.get("codes", {})
    extra = {"status": v, "frames_run": report["frames_run"], "text": codes.get(str(v), "")}
    if v == case["pass_value"]:
        return _verdict("PASS", "result byte $%02X" % v, **extra)
    if str(v) in codes:
        return _verdict("FAIL", "result byte $%02X: %s" % (v, codes[str(v)]), **extra)
    return _verdict("WRONG_PROTOCOL", "result byte $%02X is none of the documented codes" % v, **extra)


_AC_TOTAL = re.compile(r"^(\d+)/(\d+) passed \(CPU/PPU alignment (\d), power-on RAM mode (\d), (\d+) frames\)\s*$", re.M)
_AC_FAIL = re.compile(r"^  (Fail \S|\(not run\))\s+(.+?)\s*$", re.M)


def classify_accuracycoin(case, proc, report=None):
    if proc.get("error"):
        return _verdict("HARNESS_ERROR", "could not run accuracycoin_run: %s" % proc["error"])
    if proc.get("timed_out"):
        return _verdict("WALL_TIMEOUT", "stopped by the %ss wall-clock limit" % case.get("wall_seconds", "?"))
    rc = proc.get("rc")
    if _crashed(rc):
        return _verdict("CRASH", "accuracycoin_run died (exit %s)" % rc)
    out = proc.get("stdout") or ""
    m = _AC_TOTAL.search(out)
    if "never started" in (proc.get("stderr") or ""):
        return _verdict("NO_SIGNATURE", "AccuracyCoin's run-all-tests mode never started")
    if not m:
        return _verdict("HARNESS_ERROR", "no 'N/M passed' line (exit %s): %s" % (rc, (proc.get("stderr") or "").strip()[:200]))
    passed, total = int(m.group(1)), int(m.group(2))
    fails = ["%s %s" % (a, b) for a, b in _AC_FAIL.findall(out)]
    extra = {"status": passed, "frames_run": int(m.group(5)), "text": "; ".join(fails)}
    if total != case.get("tests"):
        return _verdict("WRONG_REVISION", "%d tests found, the pinned revision has %s" % (total, case.get("tests")), **extra)
    if (rc == 0) != (passed == total) or rc not in (0, 3):
        return _verdict("HARNESS_ERROR", "exit %s disagrees with %d/%d" % (rc, passed, total), **extra)
    if passed == total and not fails:
        return _verdict("PASS", "%d/%d" % (passed, total), **extra)
    if fails and all(f.startswith("(not run)") for f in fails):
        return _verdict("INCOMPLETE_EMULATED_BUDGET", "%d/%d, the rest never ran" % (passed, total), **extra)
    return _verdict("FAIL", "%d/%d" % (passed, total), **extra)


CLASSIFY = {"blargg6000": classify_blargg6000, "zp_result": classify_zp_result, "accuracycoin": classify_accuracycoin}


# ---------------------------------------------------------------------------- running

def build_command(case, rom, tools, align, ram, json_path, roms_dirs):
    if case["protocol"] == "accuracycoin":
        asm = os.path.join(roms_dirs[case["source"]], case["asm"])
        return list(tools["accuracycoin_run"]) + [rom, asm, "-a", str(align), "-r", str(ram)]
    cmd = list(tools["cli"]) + [rom, "-f", str(case["frames"]), "-a", str(align), "-r", str(ram), "-q", "-j", json_path]
    if case["protocol"] == "zp_result":
        cmd += ["-m", "%s:1" % case["result_addr"]]
    return cmd


def run_process(cmd, wall_seconds):
    t0 = time.monotonic()
    try:
        p = subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=wall_seconds)
    except subprocess.TimeoutExpired as e:
        return {"timed_out": True, "rc": None, "stdout": (e.stdout or b"").decode("latin-1"), "stderr": "", "seconds": time.monotonic() - t0}
    except OSError as e:
        return {"error": str(e), "rc": None, "stdout": "", "stderr": "", "seconds": time.monotonic() - t0}
    return {"rc": p.returncode, "stdout": p.stdout.decode("latin-1"), "stderr": p.stderr.decode("latin-1"),
            "seconds": round(time.monotonic() - t0, 3)}


def read_report(path):
    if not os.path.exists(path):
        return None
    try:
        with open(path, encoding="utf-8") as f:
            return json.load(f)
    except ValueError as e:
        return {"_invalid": str(e)}
    except OSError as e:
        return {"_invalid": str(e)}


def run_case(case, roms_dirs, tools, align, ram, evidence_dir=None, check_hash=True):
    base = {"id": case["id"], "suite": case.get("suite", case["source"]), "area": case.get("area", ""),
            "align": align, "ram": ram, "source": case["source"], "path": case["path"], "protocol": case["protocol"]}
    if case.get("target", "NTSC") != "NTSC":
        return dict(base, **_verdict("UNSUPPORTED_TARGET", "made for %s" % case["target"]))
    root = roms_dirs.get(case["source"])
    rom = os.path.join(root, case["path"]) if root else None
    if not rom or not os.path.isfile(rom):
        return dict(base, **_verdict("MISSING_FIXTURE", "%s not found%s" % (case["path"], "" if root else " (no --roms %s=DIR)" % case["source"])))
    if check_hash:
        got = sha256(rom)
        if got != case["sha256"]:
            return dict(base, **_verdict("WRONG_REVISION", "SHA-256 %s, pinned %s" % (got, case["sha256"])))
        if case["protocol"] == "accuracycoin" and case.get("asm_sha256"):
            asm = os.path.join(root, case["asm"])
            if not os.path.isfile(asm) or sha256(asm) != case["asm_sha256"]:
                return dict(base, **_verdict("WRONG_REVISION", "%s missing or not the pinned revision" % case["asm"]))
    fd, json_path = tempfile.mkstemp(suffix=".json", prefix="nessys_")
    os.close(fd)
    os.remove(json_path)                                  # the emulator creates it; absent = no report
    try:
        cmd = build_command(case, rom, tools, align, ram, json_path, roms_dirs)
        proc = run_process(cmd, case.get("wall_seconds", 300))
        report = read_report(json_path) if case["protocol"] != "accuracycoin" else None
        try:
            v = CLASSIFY[case["protocol"]](case, proc, report)
        except Exception as e:                            # a malformed report must not stop the run
            v = _verdict("HARNESS_ERROR", "classifying failed: %r" % e)
        if v["verdict"] == "FAIL" and case.get("revision_dependent"):
            v = dict(v, verdict="REVISION_DEPENDENT", detail="%s (%s)" % (v["detail"], case["revision_dependent"]))
        out = dict(base, **v)
        out["seconds"] = proc.get("seconds")
        out["command"] = " ".join(shlex.quote(c) for c in cmd)
        if evidence_dir:
            os.makedirs(evidence_dir, exist_ok=True)
            stem = re.sub(r"[^A-Za-z0-9_.-]+", "_", "%s_a%d_r%d" % (case["id"], align, ram))
            with open(os.path.join(evidence_dir, stem + ".log"), "w", encoding="utf-8") as f:
                f.write("$ %s\nexit %s%s\n--- stdout\n%s\n--- stderr\n%s\n" % (out["command"], proc.get("rc"),
                        " (wall timeout)" if proc.get("timed_out") else "", proc.get("stdout", ""), proc.get("stderr", "")))
            if report is not None:
                with open(os.path.join(evidence_dir, stem + ".json"), "w", encoding="utf-8") as f:
                    json.dump(report, f, indent=1)
        return out
    finally:
        if os.path.exists(json_path):
            os.remove(json_path)


def run_all(manifest, roms_dirs, tools, aligns, rams, only=None, evidence_dir=None, repeat=1, log=print):
    results = []
    for case in manifest["cases"]:
        if only and not any(o in case["id"] for o in only):
            continue
        a_list = aligns if case.get("matrix", True) else aligns[:1]
        r_list = rams if case.get("matrix", True) else rams[:1]
        for a in a_list:
            for r in r_list:
                runs = [run_case(case, roms_dirs, tools, a, r, evidence_dir) for _ in range(max(1, repeat))]
                res = runs[0]
                if len({(x["verdict"], x.get("status")) for x in runs}) > 1:
                    res = dict(res, verdict="NONDETERMINISTIC", detail="; ".join("%s %s" % (x["verdict"], x.get("status")) for x in runs))
                results.append(res)
                log("%-28s %-58s a%d r%d  %s" % (res["verdict"], res["id"][:58], a, r, res["detail"][:80]))
    return results


# ---------------------------------------------------------------------------- reports

def summarize(results):
    by = {}
    for r in results:
        by.setdefault(r["suite"], {}).setdefault(r["verdict"], 0)
        by[r["suite"]][r["verdict"]] += 1
    totals = {}
    for r in results:
        totals[r["verdict"]] = totals.get(r["verdict"], 0) + 1
    return by, totals


def write_reports(results, out_dir, manifest_path):
    os.makedirs(out_dir, exist_ok=True)
    with open(os.path.join(out_dir, "results.json"), "w", encoding="utf-8") as f:
        json.dump({"manifest": manifest_path, "manifest_sha256": sha256(manifest_path), "results": results}, f, indent=1)
    cols = ["id", "suite", "area", "align", "ram", "verdict", "status", "detail", "resets", "frames_run", "seconds", "text", "command"]
    with open(os.path.join(out_dir, "results.csv"), "w", newline="", encoding="utf-8") as f:
        w = csv.DictWriter(f, fieldnames=cols, extrasaction="ignore")
        w.writeheader()
        for r in results:
            w.writerow(r)
    by, totals = summarize(results)
    lines = ["# Test results", "", "Manifest: `%s` (SHA-256 `%s`)" % (os.path.basename(manifest_path), sha256(manifest_path)), "",
             "| verdict | cases |", "|---|---|"]
    lines += ["| %s | %d |" % (v, totals[v]) for v in VERDICTS if v in totals]
    lines += ["", "| suite | " + " | ".join(v for v in VERDICTS if v in totals) + " |", "|---|" + "---|" * len([v for v in VERDICTS if v in totals])]
    for s in sorted(by):
        lines.append("| %s | " % s + " | ".join(str(by[s].get(v, "")) for v in VERDICTS if v in totals) + " |")
    lines += ["", "Everything that is not PASS:", ""]
    lines += ["- `%s` (a%s r%s): **%s** - %s" % (r["id"], r["align"], r["ram"], r["verdict"], r["detail"]) for r in results if r["verdict"] != "PASS"]
    with open(os.path.join(out_dir, "results.md"), "w", encoding="utf-8") as f:
        f.write("\n".join(lines) + "\n")
    return totals


def compare_with_original(csv_path, results):
    """Lines up a historical CSV (the MegaPack 'Core' format: test,status,output_excerpt,...) with
    these results. A damaged CSV is reported, not trusted."""
    try:
        with open(csv_path, newline="", encoding="utf-8") as f:
            rows = list(csv.DictReader(f))
    except (OSError, UnicodeDecodeError, csv.Error) as e:
        return {"error": "can't read %s: %s" % (csv_path, e), "rows": []}
    if not rows or not {"test", "status"} <= set(rows[0].keys()):
        return {"error": "%s has no test/status columns" % csv_path, "rows": []}
    out = []
    for row in rows:
        test = (row.get("test") or "").replace("\\", "/")
        mine = [r for r in results if test.endswith(r["path"]) or (r["protocol"] == "accuracycoin" and "AccuracyCoin" in test)]
        new = mine[0] if mine else None
        out.append({"test": test, "original": row.get("status"), "original_excerpt": (row.get("output_excerpt") or "")[:80],
                    "now": new["verdict"] if new else "NOT_IN_MANIFEST", "detail": new["detail"] if new else ""})
    return {"error": None, "rows": out}


# ---------------------------------------------------------------------------- main

def find_tool(explicit, name):
    if explicit:
        return [explicit]
    repo = os.path.dirname(os.path.dirname(HERE))
    for cand in (name + ".exe", name):
        p = os.path.join(repo, cand)
        if os.path.isfile(p):
            return [p]
    return [name]


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--manifest", default=os.path.join(HERE, "manifest.json"))
    ap.add_argument("--roms", action="append", default=[], metavar="SOURCE=DIR", help="folder holding a manifest source")
    ap.add_argument("--cli", help="nessys_cli command (default: the one built in the repository)")
    ap.add_argument("--accuracycoin-run", dest="acrun", help="accuracycoin_run command")
    ap.add_argument("--align", default="0", help="CPU/PPU alignments, e.g. 0,1,2,3")
    ap.add_argument("--ram", default="4", help="power-on RAM modes, e.g. 0,4")
    ap.add_argument("--only", action="append", help="run cases whose id contains this (repeatable)")
    ap.add_argument("--out", default="suite_results", help="folder for results.json/.csv/.md")
    ap.add_argument("--evidence", help="also keep every command's output and JSON report here")
    ap.add_argument("--repeat", type=int, default=1, help="run each case N times; disagreement = NONDETERMINISTIC")
    ap.add_argument("--compare", help="a historical results CSV to line up with this run")
    ap.add_argument("--dry-run", action="store_true", help="check fixtures and hashes and print commands only")
    a = ap.parse_args(argv)
    try:
        manifest = load_manifest(a.manifest)
    except ManifestError as e:
        print("run_suite: %s" % e, file=sys.stderr)
        return 2
    roms_dirs = {}
    for spec in a.roms:
        if "=" not in spec:
            print("run_suite: --roms wants SOURCE=DIR, got %r" % spec, file=sys.stderr)
            return 2
        k, v = spec.split("=", 1)
        roms_dirs[k] = v
    try:
        aligns = [int(x) for x in a.align.split(",")]
        rams = [int(x) for x in a.ram.split(",")]
    except ValueError:
        print("run_suite: --align/--ram want comma-separated numbers", file=sys.stderr)
        return 2
    if any(x not in range(4) for x in aligns) or any(x not in range(5) for x in rams):
        print("run_suite: alignments are 0-3 and RAM modes 0-4", file=sys.stderr)
        return 2
    tools = {"cli": find_tool(a.cli, "nessys_cli"), "accuracycoin_run": find_tool(a.acrun, "accuracycoin_run")}
    if a.dry_run:
        bad = 0
        for case in manifest["cases"]:
            if a.only and not any(o in case["id"] for o in a.only):
                continue
            root = roms_dirs.get(case["source"])
            rom = os.path.join(root, case["path"]) if root else None
            if case.get("target", "NTSC") != "NTSC":
                state = "UNSUPPORTED_TARGET"
            elif not rom or not os.path.isfile(rom):
                state, bad = "MISSING_FIXTURE", bad + 1
            elif sha256(rom) != case["sha256"]:
                state, bad = "WRONG_REVISION", bad + 1
            else:
                state = "ok"
            print("%-18s %s" % (state, case["id"]))
            if state == "ok":
                print("    " + " ".join(shlex.quote(c) for c in build_command(case, rom, tools, aligns[0], rams[0], "result.json", roms_dirs)))
        return 1 if bad else 0
    results = run_all(manifest, roms_dirs, tools, aligns, rams, a.only, a.evidence, a.repeat)
    totals = write_reports(results, a.out, a.manifest)
    if a.compare:
        cmp_ = compare_with_original(a.compare, results)
        with open(os.path.join(a.out, "comparison.json"), "w", encoding="utf-8") as f:
            json.dump(cmp_, f, indent=1)
        if cmp_["error"]:
            print("run_suite: %s" % cmp_["error"], file=sys.stderr)
    print("\n" + ", ".join("%s %d" % (v, totals[v]) for v in VERDICTS if v in totals))
    print("reports in %s" % os.path.abspath(a.out))
    return 0 if set(totals) <= {"PASS", "UNSUPPORTED_TARGET", "REVISION_DEPENDENT"} else 1


if __name__ == "__main__":
    sys.exit(main())
