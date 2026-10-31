#!/usr/bin/env python3
"""Unit tests for T5c: Unity/Mono startup checklist as probe checks."""

from pathlib import Path
import sys
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import hvm_probe  # noqa: E402
import unity_mono_checklist  # noqa: E402


class TestUnityMonoChecklist(unittest.TestCase):
    def test_stat_layout(self):
        res = unity_mono_checklist.check_stat_layout()
        self.assertTrue(res["pass"])
        self.assertEqual(res["legacy_size"], 96)
        self.assertEqual(res["inode64_size"], 108)
        self.assertEqual(res["st_size_offset"], 48)

    def test_signals(self):
        res = unity_mono_checklist.check_signals()
        self.assertTrue(res["pass"])
        self.assertIn("SIGSEGV", res["signals"])
        self.assertIn("SIGBUS", res["signals"])
        self.assertIn("SIGILL", res["signals"])
        self.assertIn("SIGFPE", res["signals"])

    def test_dlopen_null(self):
        res = unity_mono_checklist.check_dlopen_null()
        self.assertTrue(res["pass"])
        self.assertEqual(res["rtld_default"], 0)

    def test_getpw(self):
        res = unity_mono_checklist.check_getpw()
        self.assertTrue(res["pass"])
        self.assertEqual(res["user"], "mobile")
        self.assertEqual(res["uid"], 501)
        self.assertEqual(res["dir"], "/var/mobile")

    def test_audio_format(self):
        res = unity_mono_checklist.check_audio_format()
        self.assertTrue(res["pass"])
        self.assertEqual(res["rate"], 44100)
        self.assertEqual(res["channels"], 2)
        self.assertEqual(res["bits"], 16)

    def test_probe_check_clean_run(self):
        rep = {"mixer": {"framesRendered": 100, "audibleFrames": 100}}
        issues = unity_mono_checklist.check_probe_run(rep, "Application started normally")
        self.assertEqual(len(issues), 0)

    def test_probe_check_stat_failure(self):
        rep = {}
        log = "Error reading asset bundle: Could not load Manifest asset!\n"
        issues = unity_mono_checklist.check_probe_run(rep, log)
        self.assertEqual(len(issues), 1)
        self.assertEqual(issues[0]["check"], "stat_layout")
        self.assertEqual(issues[0]["kind"], "error")

    def test_probe_check_signal_failure(self):
        rep = {"crashed": True}
        log = "Received SIGSEGV in mono_sigsegv_signal_handler\n"
        issues = unity_mono_checklist.check_probe_run(rep, log)
        self.assertEqual(len(issues), 1)
        self.assertEqual(issues[0]["check"], "signals")
        self.assertEqual(issues[0]["kind"], "crash")

    def test_probe_check_dlopen_failure(self):
        rep = {}
        log = "dlsym(RTLD_DEFAULT): symbol not found: mono_thread_attach\n"
        issues = unity_mono_checklist.check_probe_run(rep, log)
        self.assertEqual(len(issues), 1)
        self.assertEqual(issues[0]["check"], "dlopen_null")

    def test_probe_check_getpw_failure(self):
        rep = {}
        log = "getpwnam failed: user not found\n"
        issues = unity_mono_checklist.check_probe_run(rep, log)
        self.assertEqual(len(issues), 1)
        self.assertEqual(issues[0]["check"], "getpw")

    def test_probe_check_audio_failure(self):
        rep = {}
        log = "AudioQueue: unsupported format 0x1234\n"
        issues = unity_mono_checklist.check_probe_run(rep, log)
        self.assertEqual(len(issues), 1)
        self.assertEqual(issues[0]["check"], "audio_format")

    def test_hvm_probe_issues_from_integration(self):
        rep = {"mixer": {"framesRendered": 50, "audibleFrames": 50}}
        log = "Could not load Manifest asset!\n"
        found = hvm_probe.issues_from(rep, log, Path("/nonexistent"), False)
        keys = [item["key"] for item in found]
        self.assertTrue(any("stat layout failure" in k for k in keys))


if __name__ == "__main__":
    unittest.main()
