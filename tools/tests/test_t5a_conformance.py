#!/usr/bin/env python3
"""Unit tests for T5a: libstdc++ 4.2 ABI conformance guests."""

import json
import os
from pathlib import Path
import signal
import subprocess
import sys
import tempfile
import time
import unittest

ROOT_DIR = Path(__file__).resolve().parents[2]
TESTLAB_PY = ROOT_DIR / "tools" / "hvm_testlab.py"
HVM_BIN = ROOT_DIR / "build" / "hvm"


def run_testlab(cmd, timeout=240):
    with subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                          text=True, start_new_session=True) as proc:
        try:
            stdout, stderr = proc.communicate(timeout=timeout)
        except subprocess.TimeoutExpired as exc:
            try:
                os.killpg(proc.pid, signal.SIGKILL)
            except ProcessLookupError:
                pass
            exc.stdout, exc.stderr = proc.communicate()
            raise
        return subprocess.CompletedProcess(cmd, proc.returncode, stdout, stderr)


class TestLibStdcxxConformance(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        if not HVM_BIN.exists():
            raise unittest.SkipTest(f"{HVM_BIN} not built")
        if os.environ.get("HVM_TESTLAB_TOOLCHAIN") != "1":
            raise unittest.SkipTest("builds the iOS Test Lab (needs the ARM iOS linker and LLVM tools); set HVM_TESTLAB_TOOLCHAIN=1")
        cls._out = tempfile.TemporaryDirectory()
        cls.addClassCleanup(cls._out.cleanup)
        cmd = [sys.executable, str(TESTLAB_PY), "run", "--hvm", str(HVM_BIN), "--case", "TL-Y", "--out", cls._out.name]
        # No check=True: a failing TL-Y exits 1, and the report (with the guest's reason) is what explains it.
        cls.testlab_run = run_testlab(cmd)
        cls.out_dir = Path(cls._out.name)
        report_path = cls.out_dir / "testlab_report.json"
        if not report_path.exists():
            raise AssertionError(f"Test Lab produced no report (exit {cls.testlab_run.returncode}):\n{cls.testlab_run.stdout}\n{cls.testlab_run.stderr}")
        cls.report = json.loads(report_path.read_text())

    def test_tly_passed(self):
        cases = self.report.get("cases", [])
        self.assertEqual(len(cases), 1)
        c = cases[0]
        self.assertEqual(c["id"], "TL-Y")
        self.assertEqual(c["status"], "passed")

    def test_independent_expected_values_present(self):
        # Verify guest-side report recorded expected values
        guest_report_path = self.out_dir / "home" / "com.hydrogenvm.test.testlab" / "Documents" / "testlab_result.json"
        self.assertTrue(guest_report_path.exists(), f"guest result missing: {guest_report_path}")
        guest_report = json.loads(guest_report_path.read_text())
        c = guest_report["cases"][0]
        self.assertEqual(c["id"], "TL-Y")
        self.assertEqual(c["status"], "passed")
        self.assertIn("empty rep (0,0)", c["expected"])
        self.assertIn("refcount +1 on copy", c["expected"])
        self.assertIn("circular list hook/unhook", c["expected"])
        self.assertIn("RB-tree root black", c["expected"])
        self.assertIn("SJLJ chain link", c["expected"])
        self.assertEqual(c["actual"], "string=1 hook=1 unhook=1 rbtree=1 sjlj=1")


class TestTestLabProcess(unittest.TestCase):
    def test_nonzero_exit_keeps_output(self):
        cmd = [sys.executable, "-c", "import sys; print('out'); print('err', file=sys.stderr); sys.exit(1)"]
        result = run_testlab(cmd, timeout=5)
        self.assertEqual((result.returncode, result.stdout, result.stderr), (1, "out\n", "err\n"))

    def test_timeout_kills_descendants_and_keeps_output(self):
        with tempfile.TemporaryDirectory() as directory:
            marker = Path(directory) / "child-survived"
            child = "import pathlib, sys, time; time.sleep(2); pathlib.Path(sys.argv[1]).touch()"
            parent = (
                "import subprocess, sys, time; "
                "subprocess.Popen([sys.executable, '-c', sys.argv[1], sys.argv[2]], "
                "stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL); "
                "print('started', flush=True); print('diagnostic', file=sys.stderr, flush=True); time.sleep(10)"
            )
            cmd = [sys.executable, "-c", parent, child, str(marker)]
            with self.assertRaises(subprocess.TimeoutExpired) as raised:
                run_testlab(cmd, timeout=1)
            self.assertEqual(raised.exception.stdout, "started\n")
            self.assertEqual(raised.exception.stderr, "diagnostic\n")
            time.sleep(2)
            self.assertFalse(marker.exists(), "a descendant survived the timeout")


if __name__ == "__main__":
    unittest.main()
