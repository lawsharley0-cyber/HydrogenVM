#!/usr/bin/env python3
"""Unit tests for T5b: One documented GL context/thread/FBO model."""

import json
import os
from pathlib import Path
import subprocess
import tempfile
import sys
import unittest

ROOT_DIR = Path(__file__).resolve().parents[2]
DOC_PATH = ROOT_DIR / "docs" / "dev-system" / "GL_MODEL.md"
TESTLAB_PY = ROOT_DIR / "tools" / "hvm_testlab.py"
HVM_BIN = ROOT_DIR / "build" / "hvm"
HVM_TESTS = ROOT_DIR / "build" / "hvm_tests"


class TestGlModelDocumentation(unittest.TestCase):
    def test_doc_exists_and_covers_model(self):
        self.assertTrue(DOC_PATH.exists(), f"Missing GL model document at {DOC_PATH}")
        content = DOC_PATH.read_text(encoding="utf-8")
        # Invariant 1: Single context per thread & setCurrentContext
        self.assertIn("Single Context per Thread", content)
        self.assertIn("setCurrentContext", content)
        # Invariant 2: Software backend exclusivity
        self.assertIn("Software Backend Exclusivity", content)
        self.assertIn("activeThread_", content)
        # Invariant 3: Sharegroup resource sharing vs context-private FBOs
        self.assertIn("Sharegroup and Resource Sharing", content)
        self.assertIn("Framebuffer Objects", content)
        # Invariant 4: FBO completeness enforcement
        self.assertIn("Completeness Enforcement", content)
        self.assertIn("0x8CD7", content)  # GL_FRAMEBUFFER_INCOMPLETE_MISSING_ATTACHMENT
        self.assertIn("0x8CD5", content)  # GL_FRAMEBUFFER_COMPLETE
        # Invariant 5: Default FBO vs custom FBOs
        self.assertIn("Default FBO vs Custom FBOs", content)


class TestGlModelRuntimeEnforcement(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        if not HVM_BIN.exists() or not HVM_TESTS.exists():
            raise unittest.SkipTest("HydrogenVM test binaries not built yet")

    def test_fbo_completeness_enforced_in_fulltest(self):
        """Fulltest exercises empty FBO -> 0x8CD7, default FBO -> 0x8CD5, and complete FBO -> 0x8CD5."""
        cmd = [str(HVM_TESTS), "fulltest"]
        res = subprocess.run(cmd, capture_output=True, text=True, check=True)
        self.assertIn("passed, 0 failed", res.stdout)
        self.assertNotIn("FAILED", res.stdout)

    @unittest.skipUnless(os.environ.get("HVM_TESTLAB_TOOLCHAIN") == "1",
                         "builds the iOS Test Lab (needs the ARM iOS linker and LLVM tools); set HVM_TESTLAB_TOOLCHAIN=1")
    def test_testlab_tl_r_enforces_fbo_completeness(self):
        """TestLab TL-R validates empty FBO returns 0x8CD7 and attached FBO returns 0x8CD5."""
        with tempfile.TemporaryDirectory() as out:
            cmd = [sys.executable, str(TESTLAB_PY), "run", "--hvm", str(HVM_BIN), "--case", "TL-R", "--out", out]
            subprocess.run(cmd, capture_output=True, text=True, check=True)
            report_path = Path(out) / "testlab_report.json"
            self.assertTrue(report_path.exists())
            rep = json.loads(report_path.read_text())
        cases = rep.get("cases", [])
        self.assertEqual(len(cases), 1)
        c = cases[0]
        self.assertEqual(c["id"], "TL-R")
        self.assertEqual(c["status"], "passed")


if __name__ == "__main__":
    unittest.main()
