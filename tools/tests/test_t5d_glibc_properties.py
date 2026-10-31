#!/usr/bin/env python3
"""Unit tests for T5d: glibc property tests for printf/string/memory helpers."""

from pathlib import Path
import subprocess
import unittest

ROOT_DIR = Path(__file__).resolve().parents[2]
HVM_TESTS = ROOT_DIR / "build" / "hvm_tests"


class TestGlibcProperties(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        if not HVM_TESTS.exists():
            raise unittest.SkipTest(f"{HVM_TESTS} binary not built yet")

    def test_property_tests_pass(self):
        cmd = [str(HVM_TESTS), "propertytest"]
        res = subprocess.run(cmd, capture_output=True, text=True)
        self.assertEqual(res.returncode, 0, f"propertytest failed:\n{res.stdout}\n{res.stderr}")
        self.assertIn("glibc property tests: 6 passed, 0 failed", res.stdout)
        self.assertNotIn("FAIL", res.stdout)

    def test_planted_off_by_one_strlen_caught(self):
        cmd = [str(HVM_TESTS), "propertytest", "--plant-off-by-one", "strlen"]
        res = subprocess.run(cmd, capture_output=True, text=True)
        self.assertNotEqual(res.returncode, 0, "Planted off-by-one in strlen was not caught!")
        self.assertIn("FAIL strlen and strnlen", res.stdout)

    def test_planted_off_by_one_memcmp_caught(self):
        cmd = [str(HVM_TESTS), "propertytest", "--plant-off-by-one", "memcmp"]
        res = subprocess.run(cmd, capture_output=True, text=True)
        self.assertNotEqual(res.returncode, 0, "Planted off-by-one in memcmp was not caught!")
        self.assertIn("FAIL memcmp", res.stdout)

    def test_planted_off_by_one_strlcpy_caught(self):
        cmd = [str(HVM_TESTS), "propertytest", "--plant-off-by-one", "strlcpy"]
        res = subprocess.run(cmd, capture_output=True, text=True)
        self.assertNotEqual(res.returncode, 0, "Planted off-by-one in strlcpy was not caught!")
        self.assertIn("FAIL strlcpy", res.stdout)

    def test_planted_off_by_one_sprintf_caught(self):
        cmd = [str(HVM_TESTS), "propertytest", "--plant-off-by-one", "sprintf"]
        res = subprocess.run(cmd, capture_output=True, text=True)
        self.assertNotEqual(res.returncode, 0, "Planted off-by-one in formatGuest sprintf was not caught!")
        self.assertIn("FAIL formatGuest", res.stdout)


if __name__ == "__main__":
    unittest.main()
