#!/usr/bin/env python3
"""Unit tests for T5 Class campaigns (GL model, Unity/Mono checklist, libstdc++/libc conformance)."""

import json
from pathlib import Path
import subprocess
import sys
import unittest

ROOT_DIR = Path(__file__).resolve().parents[2]
TOOLS_DIR = ROOT_DIR / "tools"

sys.path.insert(0, str(TOOLS_DIR))
import unity_mono_checklist  # noqa: E402


class TestGlModel(unittest.TestCase):
    def test_gl_model_document_exists_and_complete(self):
        doc_path = ROOT_DIR / "docs" / "dev-system" / "GL_MODEL.md"
        self.assertTrue(doc_path.exists(), "docs/dev-system/GL_MODEL.md must exist")
        text = doc_path.read_text()
        self.assertIn("Thread Affinity", text)
        self.assertIn("EAGLContext", text)
        self.assertIn("EAGLSharegroup", text)
        self.assertIn("Framebuffer Objects", text)
        self.assertIn("GL_FRAMEBUFFER_COMPLETE", text)
        self.assertIn("GL_FRAMEBUFFER_INCOMPLETE_MISSING_ATTACHMENT", text)
        self.assertIn("Relaunch", text)

    def test_fbo_completeness_enforced_in_fulltest(self):
        hvm_tests = ROOT_DIR / "build" / "hvm_tests"
        if not hvm_tests.exists():
            self.skipTest(f"{hvm_tests} not built")
        p = subprocess.run([str(hvm_tests), "fulltest"], capture_output=True, text=True)
        self.assertEqual(p.returncode, 0)
        self.assertIn("Empty FBO reports GL_FRAMEBUFFER_INCOMPLETE_MISSING_ATTACHMENT", p.stdout)
        self.assertIn("EAGLContext renderbufferStorage:fromDrawable: & FBO complete", p.stdout)


class TestUnityMonoChecklist(unittest.TestCase):
    def test_stat_layout(self):
        res = unity_mono_checklist.check_stat_layout()
        self.assertTrue(res["pass"])
        self.assertEqual(res["st_size_offset"], 48)
        self.assertEqual(res["st_mode_offset"], 4)
        self.assertEqual(res["st_mtime_offset"], 36)

        # Fails on undersized buffer
        bad = unity_mono_checklist.check_stat_layout(b"short")
        self.assertFalse(bad["pass"])

    def test_signals(self):
        res = unity_mono_checklist.check_signals()
        self.assertTrue(res["pass"])
        self.assertIn("SIGSEGV", res["mono_fault_signals"])
        self.assertIn("SIGBUS", res["mono_fault_signals"])
        self.assertIn("SIGILL", res["mono_fault_signals"])
        self.assertIn("SIGFPE", res["mono_fault_signals"])
        self.assertEqual(res["signals"]["SIGSEGV"], 11)

    def test_dlopen_null(self):
        res = unity_mono_checklist.check_dlopen_null()
        self.assertTrue(res["pass"])
        self.assertEqual(res["rtld_default"], 0)
        self.assertIn("_malloc", res["can_resolve"])
        self.assertIn("_objc_msgSend", res["can_resolve"])

    def test_getpw(self):
        res = unity_mono_checklist.check_getpw()
        self.assertTrue(res["pass"])
        self.assertEqual(res["username"], "mobile")
        self.assertEqual(res["uid"], 501)
        self.assertEqual(res["dir"], "/var/mobile")

    def test_audio_format(self):
        res = unity_mono_checklist.check_audio_format()
        self.assertTrue(res["pass"])
        self.assertEqual(res["sample_rate"], 44100)
        self.assertEqual(res["channels"], 2)
        self.assertEqual(res["bits_per_sample"], 16)

    def test_run_checklist(self):
        report = unity_mono_checklist.run_checklist()
        self.assertTrue(report["overall_pass"])
        self.assertEqual(len(report["checks"]), 5)


class TestGlibcAndLibstdcxxProperties(unittest.TestCase):
    def test_property_tests_pass_in_fulltest(self):
        hvm_tests = ROOT_DIR / "build" / "hvm_tests"
        if not hvm_tests.exists():
            self.skipTest(f"{hvm_tests} not built")
        p = subprocess.run([str(hvm_tests), "fulltest"], capture_output=True, text=True)
        self.assertEqual(p.returncode, 0)
        self.assertIn("formatGuest %s with null pointer returns (null)", p.stdout)
        self.assertIn("formatGuest %08x zero-pads hexadecimal numbers", p.stdout)
        self.assertIn("formatGuest %d preserves negative integers", p.stdout)
        self.assertIn("libstdc++ std::string _Rep symbols registered", p.stdout)


if __name__ == "__main__":
    unittest.main()
