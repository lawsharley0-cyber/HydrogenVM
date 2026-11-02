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
from unittest import mock

ROOT_DIR = Path(__file__).resolve().parents[2]
TESTLAB_PY = ROOT_DIR / "tools" / "hvm_testlab.py"
HVM_BIN = ROOT_DIR / "build" / "hvm"


def run_testlab(cmd, timeout=240):
    # A timed-out runner can leave build/hvm holding the output pipes open.
    # Own the process group so the runner and its children can be killed together.
    with subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                          text=True, start_new_session=True) as process:
        try:
            stdout, stderr = process.communicate(timeout=timeout)
        except subprocess.TimeoutExpired:
            try:
                os.killpg(process.pid, signal.SIGKILL)
            except ProcessLookupError:
                pass
            process.communicate()
            raise
    # Like subprocess.run(check=False), retain nonzero exits for the report.
    return subprocess.CompletedProcess(cmd, process.returncode, stdout, stderr)


class TestTestLabInvocation(unittest.TestCase):
    def test_default_timeout_and_nonzero_exit(self):
        process = mock.MagicMock()
        process.communicate.return_value = ("out", "err")
        process.returncode = 1
        with mock.patch("subprocess.Popen") as popen:
            popen.return_value.__enter__.return_value = process
            result = run_testlab(["testlab"])
        popen.assert_called_once_with(["testlab"], stdout=subprocess.PIPE,
                                      stderr=subprocess.PIPE, text=True, start_new_session=True)
        process.communicate.assert_called_once_with(timeout=240)
        self.assertEqual((result.returncode, result.stdout, result.stderr), (1, "out", "err"))

    def test_nonzero_exit_keeps_output(self):
        result = run_testlab([sys.executable, "-c", "import sys; print('out'); print('err', file=sys.stderr); sys.exit(1)"])
        self.assertEqual((result.returncode, result.stdout, result.stderr), (1, "out\n", "err\n"))

    def test_timeout_kills_runner_and_descendant_holding_output(self):
        # The descendant inherits stdout, as hvm or a build command can.
        child = "import time; time.sleep(5)"
        runner = ("import subprocess, sys, time; "
                  f"child = subprocess.Popen([sys.executable, '-c', {child!r}]); "
                  "print(child.pid, flush=True); time.sleep(5)")
        start = time.monotonic()
        with self.assertRaises(subprocess.TimeoutExpired) as raised:
            run_testlab([sys.executable, "-c", runner], timeout=1)
        self.assertLess(time.monotonic() - start, 4)
        child_pid = int(raised.exception.output.strip())
        try:
            status = subprocess.run(["ps", "-o", "stat=", "-p", str(child_pid)],
                                    capture_output=True, text=True, check=False).stdout.strip()
            # A killed orphan can remain a zombie until its new parent reaps it.
            self.assertTrue(not status or status.startswith("Z"), status)
        finally:
            try:
                os.kill(child_pid, signal.SIGKILL)
            except ProcessLookupError:
                pass


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


if __name__ == "__main__":
    unittest.main()
