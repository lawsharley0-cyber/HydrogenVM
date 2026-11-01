#!/usr/bin/env python3
"""Unit tests for T5a: libstdc++ 4.2 ABI conformance guests."""

import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

ROOT_DIR = Path(__file__).resolve().parents[2]
TESTLAB_PY = ROOT_DIR / "tools" / "hvm_testlab.py"
HVM_BIN = ROOT_DIR / "build" / "hvm"


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
        subprocess.run(cmd, capture_output=True, text=True, check=True)
        cls.out_dir = Path(cls._out.name)
        cls.report = json.loads((cls.out_dir / "testlab_report.json").read_text())

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
