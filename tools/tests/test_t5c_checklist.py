#!/usr/bin/env python3
"""Unit tests for T5c: Unity/Mono startup checklist as probe checks."""

from pathlib import Path
import sys
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import hvm_probe  # noqa: E402
import unity_mono_checklist  # noqa: E402


U = unity_mono_checklist


def ok(marker):
    return f"  ok   something; {marker}\n"


def failed(marker):
    return f"  FAIL something; {marker}\n"


class TestUnityMonoChecklist(unittest.TestCase):
    def test_no_evidence_is_unverified_never_pass(self):
        for res in (U.check_stat_layout(None), U.check_signals(None), U.check_dlopen_null(None),
                    U.check_getpw(None), U.check_audio_format(None)):
            self.assertEqual(res["status"], U.UNVERIFIED)
            self.assertFalse(res["pass"])

    def test_assertion_status_reads_the_fulltest_lines(self):
        self.assertEqual(U.assertion_status(ok(U.DLOPEN_MARKER), U.DLOPEN_MARKER), U.PASS)
        self.assertEqual(U.assertion_status(failed(U.DLOPEN_MARKER), U.DLOPEN_MARKER), U.FAIL)
        self.assertEqual(U.assertion_status(ok(U.DLOPEN_MARKER) + failed(U.DLOPEN_MARKER), U.DLOPEN_MARKER), U.FAIL)
        self.assertEqual(U.assertion_status("  ok   unrelated\n", U.DLOPEN_MARKER), U.UNVERIFIED)   # assertion removed
        self.assertEqual(U.assertion_status(None, U.DLOPEN_MARKER), U.UNVERIFIED)                  # no binary

    def test_stat_layout_needs_its_own_assertion(self):
        self.assertEqual(U.check_stat_layout("  ok   unrelated\n")["status"], U.UNVERIFIED)   # an unrelated green run is no proof
        res = U.check_stat_layout(ok(U.STAT_MARKER))
        self.assertEqual(res["status"], U.PASS)
        self.assertEqual((res["legacy_size"], res["inode64_size"], res["st_size_offset"]), (96, 108, 48))
        self.assertEqual(U.check_stat_layout(failed(U.STAT_MARKER))["status"], U.FAIL)

    def test_signals_need_faults_that_reached_the_guest_handler(self):
        self.assertEqual(U.check_signals({"crashed": False, "display": {"faultsToApp": 0}})["status"], U.UNVERIFIED)
        self.assertEqual(U.check_signals({"crashed": False, "display": {"faultsToApp": 3}})["status"], U.PASS)
        self.assertEqual(U.check_signals({"crashed": True, "display": {"faultsToApp": 3}})["status"], U.FAIL)
        self.assertEqual(U.check_signals({"crashed": True, "display": {}})["status"], U.UNVERIFIED)
        for sig in ("SIGSEGV", "SIGBUS", "SIGILL", "SIGFPE"):
            self.assertIn(sig, U.check_signals(None)["signals"])

    def test_dlopen_null(self):
        both = ok(U.DLOPEN_MARKER) + ok(U.DLSYM_MARKER)
        self.assertEqual(U.check_dlopen_null(both)["status"], U.PASS)
        self.assertEqual(U.check_dlopen_null(ok(U.DLOPEN_MARKER))["status"], U.UNVERIFIED)   # export resolution not shown
        self.assertEqual(U.check_dlopen_null(ok(U.DLOPEN_MARKER) + failed(U.DLSYM_MARKER))["status"], U.FAIL)
        self.assertEqual(U.check_dlopen_null(failed(U.DLOPEN_MARKER) + ok(U.DLSYM_MARKER))["status"], U.FAIL)
        self.assertEqual(U.check_dlopen_null("")["status"], U.UNVERIFIED)
        self.assertEqual(U.check_dlopen_null(None)["rtld_default"], 0xFFFFFFFE)   # the handle the runtime test checks

    def test_getpw_needs_both_lookups(self):
        self.assertEqual(U.check_getpw(ok(U.GETPWNAM_MARKER))["status"], U.UNVERIFIED)   # getpwuid(501) not covered
        both = ok(U.GETPWNAM_MARKER) + ok(U.GETPWUID_MARKER)
        res = U.check_getpw(both)
        self.assertEqual((res["status"], res["user"], res["uid"], res["dir"]), (U.PASS, "mobile", 501, "/var/mobile"))
        self.assertEqual(U.check_getpw(ok(U.GETPWNAM_MARKER) + failed(U.GETPWUID_MARKER))["status"], U.FAIL)

    def test_audio_format_needs_its_assertion_not_a_frame_count(self):
        self.assertEqual(U.check_audio_format("  ok   unrelated\n")["status"], U.UNVERIFIED)
        res = U.check_audio_format(ok(U.AUDIO_MARKER))
        self.assertEqual((res["status"], res["rate"], res["channels"], res["bits"]), (U.PASS, 44100, 2, 16))
        self.assertEqual(U.check_audio_format(failed(U.AUDIO_MARKER))["status"], U.FAIL)

    def test_checklist_without_the_binary_is_not_a_pass(self):
        res = U.run_checklist(Path("/nonexistent/hvm_tests"))
        self.assertFalse(res["overall_pass"])
        self.assertFalse(res["fulltest_available"])
        self.assertEqual(res["overall_status"], U.UNVERIFIED)

    def test_unusable_test_binary_is_unverified_not_a_traceback(self):
        import tempfile
        with tempfile.TemporaryDirectory() as td:
            self.assertIsNone(U.run_fulltest(Path(td)))              # a directory
            plain = Path(td) / "not_executable"
            plain.write_text("x")
            self.assertIsNone(U.run_fulltest(plain))                 # no execute permission
            self.assertEqual(U.run_checklist(plain)["overall_status"], U.UNVERIFIED)

    def test_startup_lookup_failures_make_the_run_not_ok(self):
        for log in ("dlsym(RTLD_DEFAULT): symbol not found: mono_x\n", "getpwnam failed: no mobile\n"):
            found = hvm_probe.issues_from({"mixer": {}}, log, Path("/nonexistent"), False)
            worst = max(i["severity"] for i in found)
            self.assertGreaterEqual(worst, 45, log)   # >= 45 is status 'issues', not 'ok'

    def test_cli_exit_codes(self):
        import io
        import contextlib
        with contextlib.redirect_stdout(io.StringIO()) as out:
            code = U.main(["--tests-bin", "/nonexistent/hvm_tests"])
        self.assertEqual(code, 2)
        self.assertIn("UNVERIFIED", out.getvalue())
        self.assertNotIn("PASSED", out.getvalue())

    def test_probe_check_clean_run(self):
        rep = {"mixer": {"framesRendered": 100, "audibleFrames": 100}}
        issues = unity_mono_checklist.check_probe_run(rep, "Application started normally")
        self.assertEqual(len(issues), 0)

    def test_probe_check_stat_failure(self):
        rep = {}
        log = "Error reading asset bundle: Could not load Manifest asset!\n"
        issues = unity_mono_checklist.check_probe_run(rep, log)
        self.assertEqual(len(issues), 1)
        self.assertEqual(issues[0]["check"], "manifest_asset")   # a missing asset is not proof of a stat problem
        self.assertEqual(issues[0]["kind"], "error")

    def test_probe_check_stat_layout_failure(self):
        issues = unity_mono_checklist.check_probe_run({}, "stat struct size mismatch: 104 != 96\n")
        self.assertEqual([i["check"] for i in issues], ["stat_layout"])

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
        self.assertTrue(any("Manifest asset" in k for k in keys))

    def test_log_checks_run_when_no_report_was_written(self):
        found = hvm_probe.issues_from(None, "AudioQueue: unsupported format 0x1234\n", Path("/nonexistent"), False)
        kinds = {item["kind"] for item in found}
        self.assertIn("crash", kinds)                                   # the generic no-report crash is kept
        self.assertTrue(any("AudioQueue" in item["text"] for item in found))

    def test_checklist_integration_failure_is_reported(self):
        from unittest import mock
        with mock.patch.object(unity_mono_checklist, "check_probe_run", side_effect=RuntimeError("boom")):
            found = hvm_probe.issues_from({"mixer": {}}, "", Path("/nonexistent"), False)
        self.assertTrue(any("integration failed" in item["text"] and "boom" in item["text"] for item in found))


if __name__ == "__main__":
    unittest.main()
