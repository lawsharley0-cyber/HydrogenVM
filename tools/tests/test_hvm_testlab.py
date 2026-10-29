"""Unit tests for tools/hvm_testlab.py.

Tests cover:
- IPA content verification (pass and fail cases)
- Case output parsing (passed, failed, interrupted by timeout, exit-code-2)
- Interrupted-case detection via exit code
- Host report assembly and schema validation
- Correct failure/interrupted detection for wrong expected output
- TL-J baseline ceiling logic
"""
import json
import os
import sys
import tempfile
import unittest
import zipfile
from pathlib import Path
from unittest.mock import MagicMock, patch

# Add tools/ to path so we can import hvm_testlab
ROOT = Path(__file__).parent.parent.parent
sys.path.insert(0, str(ROOT / "tools"))

from hvm_testlab import (
    _apply_tj_ceiling,
    _baseline_mflops_ceiling,
    _parse_case_output,
    make_host_report,
    run_case,
    verify_ipa_contents,
)
from testlab_schema.validate_result import validate


def _minimal_report(**kwargs):
    base = {
        "schemaVersion": 1,
        "kind":          "hvm-testlab-host-report",
        "generatedAt":   "2026-10-22T00:00:00Z",
        "runId":         "test-run-1",
        "appVersion":    "1.0",
        "cases":         [],
    }
    base.update(kwargs)
    return base


def _minimal_case(case_id="TL-A", status="passed", **kwargs):
    c = {"id": case_id, "version": 1, "status": status}
    if status in ("failed", "skipped", "interrupted"):
        c["reason"] = kwargs.pop("reason", f"test reason for {status}")
    c.update(kwargs)
    return c


# ─────────────────────────── IPA content checks ──────────────────────────────

class TestVerifyIpaContents(unittest.TestCase):

    def _make_ipa(self, entries: list[tuple[str, bytes]], path: str) -> None:
        with zipfile.ZipFile(path, "w", zipfile.ZIP_DEFLATED) as zf:
            for name, data in entries:
                zf.writestr(name, data)

    def test_valid_ipa_passes(self):
        with tempfile.TemporaryDirectory() as d:
            ipa = os.path.join(d, "TestLab.ipa")
            self._make_ipa([
                ("Payload/TestLab.app/TestLab",    b"\xcf\xfa\xed\xfe"),  # fake Mach-O
                ("Payload/TestLab.app/Info.plist", b"<?xml?>"),
                ("Payload/TestLab.app/tone.wav",   b"RIFF"),
                ("Payload/TestLab.app/empty.dat",  b""),
            ], ipa)
            errors = verify_ipa_contents(ipa)
            self.assertEqual(errors, [])

    def test_missing_executable_fails(self):
        with tempfile.TemporaryDirectory() as d:
            ipa = os.path.join(d, "bad.ipa")
            self._make_ipa([
                ("Payload/TestLab.app/Info.plist", b"<?xml?>"),
                ("Payload/TestLab.app/tone.wav",   b"RIFF"),
                ("Payload/TestLab.app/empty.dat",  b""),
            ], ipa)
            errors = verify_ipa_contents(ipa)
            self.assertTrue(any("executable" in e for e in errors))

    def test_missing_tone_fails(self):
        with tempfile.TemporaryDirectory() as d:
            ipa = os.path.join(d, "bad.ipa")
            self._make_ipa([
                ("Payload/TestLab.app/TestLab",    b"\xcf\xfa\xed\xfe"),
                ("Payload/TestLab.app/Info.plist", b"<?xml?>"),
                ("Payload/TestLab.app/empty.dat",  b""),
            ], ipa)
            errors = verify_ipa_contents(ipa)
            self.assertTrue(any("tone.wav" in e for e in errors))

    def test_missing_empty_dat_fails(self):
        with tempfile.TemporaryDirectory() as d:
            ipa = os.path.join(d, "bad.ipa")
            self._make_ipa([
                ("Payload/TestLab.app/TestLab",    b"\xcf\xfa\xed\xfe"),
                ("Payload/TestLab.app/Info.plist", b"<?xml?>"),
                ("Payload/TestLab.app/tone.wav",   b"RIFF"),
            ], ipa)
            errors = verify_ipa_contents(ipa)
            self.assertTrue(any("empty.dat" in e for e in errors))

    def test_corrupt_zip_reports_error(self):
        with tempfile.TemporaryDirectory() as d:
            ipa = os.path.join(d, "corrupt.ipa")
            Path(ipa).write_bytes(b"not a zip file")
            errors = verify_ipa_contents(ipa)
            self.assertTrue(len(errors) > 0)


# ─────────────────────────── output parsing ──────────────────────────────────

class TestLifecycleResult(unittest.TestCase):
    def test_passing_guest_line_requires_clean_exit_and_persisted_save(self):
        for exit_code, saved, expected in [(0, True, "passed"), (1, True, "failed"), (0, False, "failed")]:
            with self.subTest(exit_code=exit_code, saved=saved), tempfile.TemporaryDirectory() as home:
                if saved:
                    path = Path(home) / "com.hydrogenvm.test.testlab" / "Documents" / "lifecycle-save.txt"
                    path.parent.mkdir(parents=True)
                    path.write_text("saved before termination completed")
                process = MagicMock(returncode=exit_code, stdout="TESTLAB TL-L passed\n", stderr="")
                with patch("hvm_testlab.subprocess.run", return_value=process):
                    result = run_case("hvm", "TestLab.ipa", "TL-L", home, 10)
                self.assertEqual(result["status"], expected)

    def test_eagl_affinity_case_uses_host_gpu(self):
        process = MagicMock(returncode=0, stdout="TESTLAB TL-N passed\n", stderr="")
        with tempfile.TemporaryDirectory() as home, patch("hvm_testlab.subprocess.run", return_value=process) as run:
            result = run_case("hvm", "TestLab.ipa", "TL-N", home, 10)
        self.assertEqual(result["status"], "passed")
        command = run.call_args.args[0]
        self.assertIn("--gpu", command)
        self.assertLess(command.index("--gpu"), command.index("--"))


class TestParseCaseOutput(unittest.TestCase):

    def test_passed_line(self):
        stdout = "TESTLAB TL-A passed\n"
        r = _parse_case_output(stdout, "TL-A", 0.5, 0, False, "")
        self.assertEqual(r["status"], "passed")
        self.assertNotIn("reason", r)

    def test_failed_line(self):
        stdout = "TESTLAB TL-B failed\n  reason: expected 0x12345678, got 0xDEADBEEF\n"
        r = _parse_case_output(stdout, "TL-B", 1.0, 1, False, "")
        self.assertEqual(r["status"], "failed")
        self.assertIn("reason", r)
        self.assertIn("0xDEADBEEF", r["reason"])

    def test_interrupted_by_timeout(self):
        r = _parse_case_output("", "TL-C", 30.0, -1, True, "")
        self.assertEqual(r["status"], "interrupted")
        self.assertIn("timeout", r.get("reason", "").lower())

    def test_interrupted_by_exit_code_2(self):
        r = _parse_case_output("", "TL-E", 2.0, 2, False, "")
        self.assertEqual(r["status"], "interrupted")

    def test_no_output_is_failed(self):
        r = _parse_case_output("", "TL-D", 0.1, 1, False, "")
        self.assertEqual(r["status"], "failed")

    def test_wrong_expected_is_failed(self):
        # Simulates a deliberate wrong-expectation test
        stdout = "TESTLAB TL-B failed\n  reason: expected 0x00000000, got 0xA05A0848\n"
        r = _parse_case_output(stdout, "TL-B", 1.0, 1, False, "")
        self.assertEqual(r["status"], "failed")
        self.assertIn("0xA05A0848", r["reason"])

    def test_skipped_line(self):
        stdout = "TESTLAB TL-E skipped\n  reason: phase 1 written — relaunch to verify\n"
        r = _parse_case_output(stdout, "TL-E", 0.2, 0, False, "")
        self.assertEqual(r["status"], "skipped")

    def test_duration_is_recorded(self):
        stdout = "TESTLAB TL-A passed\n"
        r = _parse_case_output(stdout, "TL-A", 1.234, 0, False, "")
        self.assertAlmostEqual(r["durationMs"], 1234.0, delta=1.0)


# ─────────────────────────── host report / schema ────────────────────────────

class TestHostReport(unittest.TestCase):

    def test_schema_validation_passes_for_minimal_report(self):
        r = _minimal_report()
        errs = validate(r)
        self.assertEqual(errs, [])

    def test_schema_validation_passes_for_all_statuses(self):
        for status in ("passed", "failed", "skipped", "interrupted"):
            case = _minimal_case(status=status)
            r = _minimal_report(cases=[case])
            errs = validate(r)
            self.assertEqual(errs, [], msg=f"status={status}: {errs}")

    def test_host_report_kind_validates(self):
        r = _minimal_report(kind="hvm-testlab-host-report")
        errs = validate(r)
        self.assertEqual(errs, [])

    def test_make_host_report_validates(self):
        cases = [_minimal_case("TL-A"), _minimal_case("TL-B")]
        git = {"commit": "abcdef1234567890" * 2 + "abcdef12", "dirty": False}
        report = make_host_report(cases, "a" * 64, "run-001", git)
        errs = validate(report)
        self.assertEqual(errs, [])

    def test_failed_without_reason_is_invalid(self):
        case = {"id": "TL-C", "version": 1, "status": "failed"}
        r = _minimal_report(cases=[case])
        errs = validate(r)
        self.assertTrue(any("reason" in e for e in errs))

    def test_interrupted_without_reason_is_invalid(self):
        case = {"id": "TL-E", "version": 1, "status": "interrupted"}
        r = _minimal_report(cases=[case])
        errs = validate(r)
        self.assertTrue(any("reason" in e for e in errs))

    def test_bad_sha256_is_invalid(self):
        r = _minimal_report(artifactSha256="not-a-hex-string")
        errs = validate(r)
        self.assertTrue(any("artifactSha256" in e for e in errs))

    def test_null_sha256_is_valid(self):
        r = _minimal_report(artifactSha256=None)
        errs = validate(r)
        self.assertEqual(errs, [])

    def test_emulator_commit_null_is_valid(self):
        r = _minimal_report(host={"emulatorCommit": None, "emulatorDirty": None})
        errs = validate(r)
        self.assertEqual(errs, [])


# ─────────────────────────── deliberate-failure checks ───────────────────────

class TestDeliberateFailure(unittest.TestCase):
    """These tests ensure failure/interrupted detection works correctly."""

    def test_wrong_cpu_checksum_is_failed(self):
        # Produce output with a wrong actual value — should be 'failed' not 'passed'
        stdout = "TESTLAB TL-B failed\n  reason: expected 0xA05A0848, got 0x00000000\n"
        r = _parse_case_output(stdout, "TL-B", 1.0, 1, False, "")
        self.assertEqual(r["status"], "failed")
        self.assertNotEqual(r.get("status"), "passed")

    def test_external_timeout_is_interrupted_not_failed(self):
        r = _parse_case_output("", "TL-C", 30.0, -1, True, "")
        self.assertEqual(r["status"], "interrupted")
        # Must NOT report as passed or failed when timed out
        self.assertNotEqual(r["status"], "passed")
        self.assertNotEqual(r["status"], "failed")

    def test_no_output_after_crash_is_not_passed(self):
        # rc=134 = SIGABRT, no TESTLAB lines
        r = _parse_case_output("", "TL-D", 0.5, 134, False, "")
        self.assertNotEqual(r["status"], "passed")


# ─────────────────────────── TL-J ceiling tests ──────────────────────────────

class TestTJCeiling(unittest.TestCase):
    """Tests for the host-side TL-J baseline regression ceiling."""

    def _make_passed_tj(self, mflops: float) -> dict:
        return {
            "id":          "TL-J",
            "version":     1,
            "status":      "passed",
            "diagnostics": {"stdoutTail": f"mflops={mflops:.1f}"},
        }

    def test_no_ceiling_when_baseline_absent(self):
        """When baseline is not present, _baseline_mflops_ceiling returns None."""
        import unittest.mock as mock
        with mock.patch("hvm_testlab.Path.read_text", side_effect=FileNotFoundError):
            result = _baseline_mflops_ceiling()
        self.assertIsNone(result)

    def test_ceiling_applied_above_threshold_passes(self):
        """A result with MFLOPS well above the ceiling is left as passed."""
        cr = self._make_passed_tj(9999.0)
        import unittest.mock as mock
        with mock.patch("hvm_testlab._baseline_mflops_ceiling", return_value=100.0):
            result = _apply_tj_ceiling(cr)
        self.assertEqual(result["status"], "passed")

    def test_ceiling_applied_below_threshold_fails(self):
        """A result with MFLOPS below the ceiling is changed to failed."""
        cr = self._make_passed_tj(10.0)
        import unittest.mock as mock
        with mock.patch("hvm_testlab._baseline_mflops_ceiling", return_value=100.0):
            result = _apply_tj_ceiling(cr)
        self.assertEqual(result["status"], "failed")
        self.assertIn("MFLOPS", result.get("reason", ""))

    def test_ceiling_not_applied_to_already_failed(self):
        """The ceiling function does not change a case that already failed."""
        cr = {"id": "TL-J", "version": 1, "status": "failed",
              "reason": "hash mismatch", "diagnostics": {}}
        import unittest.mock as mock
        with mock.patch("hvm_testlab._baseline_mflops_ceiling", return_value=100.0):
            result = _apply_tj_ceiling(cr)
        self.assertEqual(result["status"], "failed")
        self.assertEqual(result["reason"], "hash mismatch")

    def test_ceiling_skipped_when_no_mflops_in_output(self):
        """If no mflops= token in stdout tail, ceiling is not applied."""
        cr = {"id": "TL-J", "version": 1, "status": "passed",
              "diagnostics": {"stdoutTail": "no timing info here"}}
        import unittest.mock as mock
        with mock.patch("hvm_testlab._baseline_mflops_ceiling", return_value=100.0):
            result = _apply_tj_ceiling(cr)
        self.assertEqual(result["status"], "passed")

    def test_ceiling_records_observed_mflops(self):
        """When ceiling is applied, observed_mflops is recorded in diagnostics."""
        cr = self._make_passed_tj(500.0)
        import unittest.mock as mock
        with mock.patch("hvm_testlab._baseline_mflops_ceiling", return_value=100.0):
            result = _apply_tj_ceiling(cr)
        self.assertIn("observed_mflops", result["diagnostics"])
        self.assertAlmostEqual(result["diagnostics"]["observed_mflops"], 500.0)


if __name__ == "__main__":
    unittest.main()
