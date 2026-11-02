#!/usr/bin/env python3
"""Unit tests for T5b: One documented GL context/thread/FBO model."""

import json
import os
from pathlib import Path
import re
import subprocess
import tempfile
import sys
import unittest
from unittest import mock

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
        res = subprocess.run(cmd, capture_output=True, text=True)
        if res.returncode != 0:
            gl_err_patterns = [
                "libEGL.so.1 not found",
                "libEGL lacks",
                "eglInitialize failed on the surfaceless platform",
            ]
            combined = (res.stderr or "") + "\n" + (res.stdout or "")
            failed_assertion = re.search(r"^\s*FAIL(?:\s|$)|^fulltest: \d+ passed, [1-9]\d* failed",
                                         res.stdout or "", re.MULTILINE)
            if not failed_assertion and any(p in combined for p in gl_err_patterns):
                self.skipTest(
                    "GL/GPU prerequisite unavailable on host (requires Mesa headless EGL/GLES runtime). "
                    "To provide it, install Mesa / headless EGL: 'sudo apt install libegl1 libgles2 libgl1 libgl1-mesa-dri'."
                )
            self.fail(f"fulltest failed with exit code {res.returncode}:\n{combined}")
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


class TestFulltestPrerequisiteClassification(unittest.TestCase):
    def run_fulltest_result(self, stdout, stderr=""):
        result = subprocess.CompletedProcess([], 1, stdout, stderr)
        test = TestGlModelRuntimeEnforcement("test_fbo_completeness_enforced_in_fulltest")
        with mock.patch.object(subprocess, "run", return_value=result), \
             mock.patch("ctypes.util.find_library", return_value=None), \
             mock.patch("ctypes.CDLL", side_effect=OSError("missing EGL")):
            test.test_fbo_completeness_enforced_in_fulltest()

    def assert_fulltest_fails(self, stdout, stderr=""):
        try:
            self.run_fulltest_result(stdout, stderr)
        except unittest.SkipTest as exc:
            self.fail(f"failure was skipped: {exc}")
        except AssertionError:
            return
        self.fail("fulltest failure was accepted")

    def test_routine_gl_output_and_unavailable_egl_do_not_hide_failures(self):
        for output in (
            "  FAIL EAGLContext renderbufferStorage:fromDrawable: & FBO complete\nfulltest: 4 passed, 1 failed\n",
            "[warn] EAGLContext setCurrentContext: fails because ctx is in use\n  FAIL unrelated check\n",
            "GPU: OpenGL ES calls go to the host driver (affinity-test)\n  FAIL unrelated check\n",
            "HostGL sequential EAGL process lifetime: passed\n  FAIL unrelated check\n",
            "  FAIL unrelated check\n",
        ):
            with self.subTest(output=output):
                self.assert_fulltest_fails(output)

    def test_actual_missing_prerequisite_can_skip(self):
        for error in ("libEGL.so.1 not found", "libEGL lacks eglInitialize",
                      "eglInitialize failed on the surfaceless platform"):
            with self.subTest(error=error), self.assertRaises(unittest.SkipTest):
                self.run_fulltest_result("", error)

    def test_completed_assertion_failure_is_not_hidden_by_prerequisite_message(self):
        self.assert_fulltest_fails("  FAIL unrelated check\nfulltest: 4 passed, 1 failed\n",
                                   "libEGL.so.1 not found")
        self.assert_fulltest_fails("fulltest: 4 passed, 1 failed\n", "libEGL.so.1 not found")


if __name__ == "__main__":
    unittest.main()
