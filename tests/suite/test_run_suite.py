#!/usr/bin/env python3
"""Unit tests for run_suite.py: the verdict rules against synthetic reports, and the runner end to
end against a stand-in emulator (fake_cli.py) that plays back scripted behaviour - including
crashing, hanging and writing garbage. Standard library only:

    python3 -m unittest discover -s tests/suite -v
"""
import csv
import hashlib
import json
import os
import shutil
import sys
import tempfile
import unittest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import run_suite as rs  # noqa: E402

FAKE = os.path.join(os.path.dirname(os.path.abspath(__file__)), "fake_cli.py")
CASE = {"id": "t", "source": "s", "path": "t.nes", "protocol": "blargg6000", "frames": 600, "wall_seconds": 20, "sha256": "0" * 64}


def report(status=0, signature=True, frames_run=50, budget=600, text="\nt\n\nPassed\n", resets=(), perfect=True, cycles=None):
    rc = 5 if not signature else 0 if status == 0 else 1 if status < 0x80 else 3 if status == 0x80 else 4 if status == 0x81 else 6
    return {"signature": signature, "status": status if signature else None, "exit_code": rc, "frames_run": frames_run,
            "frames_budget": budget, "cpu_cycles": int(29780.5 * frames_run) + 7 if cycles is None else cycles,
            "resets": [dict(requested_frame=10, pressed_frame=26, released_frame=30) for _ in range(resets)] if isinstance(resets, int) else list(resets),
            "connector": {"perfect": perfect}, "text": text}


def proc(rc=0, **kw):
    d = {"rc": rc, "stdout": "", "stderr": ""}
    d.update(kw)
    return d


def V(case, p, r):
    return rs.classify_blargg6000(case, p, r)["verdict"]


class Blargg6000(unittest.TestCase):
    def test_true_pass(self):
        self.assertEqual(V(CASE, proc(0), report(0)), "PASS")

    def test_true_fail(self):
        r = rs.classify_blargg6000(CASE, proc(1), report(3, text="Failed #3"))
        self.assertEqual((r["verdict"], r["status"]), ("FAIL", 3))

    def test_running_is_never_fail(self):
        self.assertEqual(V(CASE, proc(3), report(0x80, frames_run=600, text="Running test 10 of 16")), "INCOMPLETE_EMULATED_BUDGET")

    def test_reset_then_pass(self):
        r = rs.classify_blargg6000(CASE, proc(0), report(0, resets=1))
        self.assertEqual((r["verdict"], r["resets"]), ("PASS", 1))

    def test_reset_pending_at_budget(self):
        self.assertEqual(V(CASE, proc(4), report(0x81, frames_run=600)), "RESET_REQUESTED")

    def test_reset_in_progress_at_budget(self):
        held = [dict(requested_frame=580, pressed_frame=596, released_frame=-1)]
        self.assertEqual(V(CASE, proc(3), report(0x80, frames_run=600, resets=held)), "RESET_REQUESTED")

    def test_repeated_reset_requests(self):
        self.assertEqual(V(CASE, proc(0), report(0, resets=4)), "RESET_LOOP")
        self.assertEqual(V(dict(CASE, max_resets=4), proc(0), report(0, resets=4)), "PASS")

    def test_no_signature(self):
        self.assertEqual(V(CASE, proc(5), report(signature=False, frames_run=600)), "NO_SIGNATURE")

    def test_text_never_decides(self):
        # "Passed" in the text while the status says running, "failed" in the text of a pass
        self.assertEqual(V(CASE, proc(3), report(0x80, frames_run=600, text="Passed")), "INCOMPLETE_EMULATED_BUDGET")
        self.assertEqual(V(CASE, proc(0), report(0, text="tests that failed: none")), "PASS")
        self.assertEqual(V(CASE, proc(1), report(1, text="All tests passed")), "FAIL")

    def test_exit_zero_without_genuine_result(self):
        r = report(signature=False, frames_run=600)
        self.assertEqual(V(CASE, proc(0), r), "HARNESS_ERROR")          # report says 5
        r = report(0x80, frames_run=600)
        r["exit_code"] = 0
        self.assertEqual(V(CASE, proc(0), r), "HARNESS_ERROR")          # status doesn't give 0

    def test_nonzero_exit_at_frame_limit(self):
        self.assertEqual(V(CASE, proc(3), report(0x80, frames_run=600)), "INCOMPLETE_EMULATED_BUDGET")

    def test_unknown_status(self):
        self.assertEqual(V(CASE, proc(6), report(0x90)), "WRONG_PROTOCOL")

    def test_manual_review(self):
        self.assertEqual(V(CASE, proc(0), report(0, text="\napu_mixer\n\nDone\n")), "UNVERIFIED_MANUAL")
        self.assertEqual(V(dict(CASE, manual="listen for clicks"), proc(0), report(0)), "UNVERIFIED_MANUAL")

    def test_crash_timeout_and_launch_error(self):
        self.assertEqual(V(CASE, proc(-11), None), "CRASH")
        self.assertEqual(V(CASE, proc(0xC0000005), None), "CRASH")
        self.assertEqual(V(CASE, proc(None, timed_out=True), None), "WALL_TIMEOUT")
        self.assertEqual(V(CASE, proc(None, error="No such file"), None), "HARNESS_ERROR")

    def test_bad_reports(self):
        self.assertEqual(V(CASE, proc(0), None), "HARNESS_ERROR")
        self.assertEqual(V(CASE, proc(0), {"_invalid": "Expecting value"}), "HARNESS_ERROR")
        self.assertEqual(V(CASE, proc(0), {"status": 0}), "HARNESS_ERROR")
        r = report(0)
        r["status"] = "0"
        self.assertEqual(V(CASE, proc(0), r), "HARNESS_ERROR")
        self.assertEqual(V(CASE, proc(2, stderr="not an iNES file"), None), "HARNESS_ERROR")

    def test_inconsistent_counts(self):
        self.assertEqual(V(CASE, proc(0), report(0, frames_run=700)), "HARNESS_ERROR")          # over budget
        self.assertEqual(V(CASE, proc(0), report(0, budget=1800)), "HARNESS_ERROR")             # not the manifest's budget
        self.assertEqual(V(CASE, proc(0), report(0, frames_run=100, cycles=1000)), "HARNESS_ERROR")

    def test_contacts_must_be_ideal(self):
        self.assertEqual(V(CASE, proc(0), report(0, perfect=False)), "HARNESS_ERROR")
        for key in ("open_pins", "flaky_pins"):         # e.g. a lifted lockout-chip contact
            r = report(0)
            r["connector"][key] = 1
            self.assertEqual(V(CASE, proc(0), r), "HARNESS_ERROR", key)


class OtherProtocols(unittest.TestCase):
    ZP = {"id": "z", "source": "s", "path": "z.nes", "protocol": "zp_result", "frames": 300, "result_addr": "00F0",
          "pass_value": 1, "codes": {"2": "too soon", "3": "too late"}, "sha256": "0" * 64}

    def zp(self, v, frames=300, rc=5):
        r = report(signature=False, frames_run=frames, budget=300)
        r["memory"] = {"addr": 0xF0, "bytes": [v]}
        return rs.classify_zp_result(self.ZP, proc(rc), r)

    def test_zp(self):
        self.assertEqual(self.zp(1)["verdict"], "PASS")
        self.assertEqual(self.zp(3)["verdict"], "FAIL")
        self.assertEqual(self.zp(0x77)["verdict"], "WRONG_PROTOCOL")
        self.assertEqual(self.zp(1, frames=100)["verdict"], "HARNESS_ERROR")
        self.assertEqual(self.zp(1, rc=0)["verdict"], "HARNESS_ERROR")

    AC = {"id": "ac", "source": "s", "path": "AccuracyCoin.nes", "protocol": "accuracycoin", "tests": 3, "sha256": "0" * 64}

    def ac(self, out, rc, err=""):
        return rs.classify_accuracycoin(self.AC, proc(rc, stdout=out, stderr=err))

    def test_accuracycoin(self):
        ok = "3/3 passed (CPU/PPU alignment 0, power-on RAM mode 4, 4320 frames)\n"
        bad = "[CPU Behavior]\n  Fail 2      Open Bus\n2/3 passed (CPU/PPU alignment 0, power-on RAM mode 4, 4320 frames)\n"
        self.assertEqual(self.ac(ok, 0)["verdict"], "PASS")
        r = self.ac(bad, 3)
        self.assertEqual((r["verdict"], r["text"]), ("FAIL", "Fail 2 Open Bus"))
        self.assertEqual(self.ac(ok, 3)["verdict"], "HARNESS_ERROR")
        self.assertEqual(self.ac(ok.replace("3/3", "4/4"), 0)["verdict"], "WRONG_REVISION")
        self.assertEqual(self.ac("", 1, "the test run never started")["verdict"], "NO_SIGNATURE")
        self.assertEqual(self.ac("garbage", 0)["verdict"], "HARNESS_ERROR")


class EndToEnd(unittest.TestCase):
    """The runner against fake_cli.py, which acts out the scenario named by the ROM's contents."""

    def setUp(self):
        self.dir = tempfile.mkdtemp(prefix="suite_test_")

    def tearDown(self):
        shutil.rmtree(self.dir, ignore_errors=True)

    def case(self, scenario, **kw):
        path = os.path.join(self.dir, scenario + ".nes")
        with open(path, "w") as f:
            f.write(scenario)
        c = dict(CASE, id=scenario, path=scenario + ".nes", sha256=hashlib.sha256(scenario.encode()).hexdigest(), wall_seconds=5)
        c.update(kw)
        return c

    def run1(self, c):
        return rs.run_case(c, {"s": self.dir}, {"cli": [sys.executable, FAKE]}, 0, 4)

    def test_scenarios(self):
        want = {"pass": "PASS", "fail": "FAIL", "running": "INCOMPLETE_EMULATED_BUDGET", "reset_then_pass": "PASS",
                "no_signature": "NO_SIGNATURE", "crash": "CRASH", "hang": "WALL_TIMEOUT", "garbage_json": "HARNESS_ERROR",
                "no_json": "HARNESS_ERROR", "lie_exit_zero": "HARNESS_ERROR", "load_error": "HARNESS_ERROR"}
        for scenario, verdict in want.items():
            c = self.case(scenario, wall_seconds=3 if scenario == "hang" else 20)
            self.assertEqual(self.run1(c)["verdict"], verdict, scenario)

    def test_fixture_problems(self):
        c = self.case("pass")
        self.assertEqual(self.run1(dict(c, sha256="1" * 64))["verdict"], "WRONG_REVISION")
        self.assertEqual(self.run1(dict(c, path="missing.nes"))["verdict"], "MISSING_FIXTURE")
        self.assertEqual(self.run1(dict(c, target="PAL"))["verdict"], "UNSUPPORTED_TARGET")
        r = rs.run_case(c, {"s": self.dir}, {"cli": [os.path.join(self.dir, "no_such_emulator")]}, 0, 4)
        self.assertEqual(r["verdict"], "HARNESS_ERROR")

    def test_revision_dependent(self):
        c = self.case("fail", revision_dependent="written for MMC3A; MMC3B/C is emulated")
        self.assertEqual(self.run1(c)["verdict"], "REVISION_DEPENDENT")

    def test_nondeterministic(self):
        c = self.case("flaky")
        res = rs.run_all({"cases": [c]}, {"s": self.dir}, {"cli": [sys.executable, FAKE]}, [0], [4], repeat=3, log=lambda *a: None)
        self.assertEqual(res[0]["verdict"], "NONDETERMINISTIC")

    def test_manifest_validation(self):
        p = os.path.join(self.dir, "m.json")
        for bad in ("{", json.dumps({"schema": 2, "cases": []}), json.dumps({"schema": 1, "sources": {}, "cases": [{"id": "x"}]}),
                    json.dumps({"schema": 1, "sources": {"s": {}}, "cases": [dict(CASE, sha256="zz")]}),
                    json.dumps({"schema": 1, "sources": {"s": {}}, "cases": [CASE, CASE]})):
            with open(p, "w") as f:
                f.write(bad)
            with self.assertRaises(rs.ManifestError):
                rs.load_manifest(p)

    def test_corrupted_or_foreign_csv(self):
        p = os.path.join(self.dir, "old.csv")
        with open(p, "wb") as f:
            f.write(b"\xff\xfe\x00garbage")
        self.assertIsNotNone(rs.compare_with_original(p, [])["error"])
        with open(p, "w", newline="") as f:
            csv.writer(f).writerows([["name", "result"], ["a", "PASS"]])
        self.assertIsNotNone(rs.compare_with_original(p, [])["error"])
        with open(p, "w", newline="") as f:
            csv.writer(f).writerows([["test", "status", "output_excerpt"], ["nes-test-roms/t.nes", "FAIL", "result $80"]])
        res = [dict(CASE, protocol="blargg6000", verdict="PASS", detail="status $00")]
        cmp_ = rs.compare_with_original(p, res)
        self.assertEqual((cmp_["error"], cmp_["rows"][0]["original"], cmp_["rows"][0]["now"]), (None, "FAIL", "PASS"))


if __name__ == "__main__":
    unittest.main()
