import subprocess
import io
import json
import sys
import tempfile
import unittest
from pathlib import Path
from unittest.mock import patch
from contextlib import redirect_stdout, redirect_stderr

sys.path.insert(0, str(Path(__file__).resolve().parents[2]))

from compat_harness.doctor import inspect
from compat_harness.cli import main


class DoctorTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        docs = self.root / "docs/dev-system"
        docs.mkdir(parents=True)
        for name in ["BACKLOG.md", "STATUS.md", "IMPLEMENTATION_PLAN.md"]:
            (docs / name).write_text("No commit references.\n", encoding="utf-8")
        self.backlog = docs / "BACKLOG.md"

    def report(self, statuses=("pass",)):
        return {"git": {"commit": "a" * 40, "dirty": False}, "summary": {"ok": True},
                "suites": {"unit": {"cases": [{"id": "B-108: regex", "status": s} for s in statuses]}}}

    def test_no_mutations(self):
        self.backlog.write_text("| B-108 | regex | Planned |\n")
        before = self.backlog.read_bytes()
        with patch("compat_harness.doctor._git", return_value=True):
            self.assertEqual(inspect(self.root, self.report())[0]["kind"], "review-stale-status")
        self.assertEqual(self.backlog.read_bytes(), before)

    def test_failing_or_skipped_case_is_not_completion(self):
        self.backlog.write_text("| B-108 | regex | Planned |\n")
        for status in ["fail", "skip", "unknown"]:
            with self.subTest(status=status), patch("compat_harness.doctor._git", return_value=True):
                self.assertEqual(inspect(self.root, self.report(("pass", status))), [])

    def test_unrelated_fulltest_is_not_completion(self):
        self.backlog.write_text("| B-108 | regex | Planned |\n")
        report = self.report()
        report["suites"]["unit"]["cases"][0]["id"] = "fulltest"
        with patch("compat_harness.doctor._git", return_value=True):
            self.assertEqual(inspect(self.root, report), [])

    def test_dirty_or_failing_evidence_is_rejected(self):
        for field in ["dirty", "failed"]:
            report = self.report()
            if field == "dirty": report["git"]["dirty"] = True
            else: report["summary"]["ok"] = False
            with self.subTest(field=field), patch("compat_harness.doctor._git", return_value=True):
                self.assertEqual(inspect(self.root, report)[0]["kind"], "unusable-evidence")

    def test_uses_git_to_validate_commit_references(self):
        subprocess.run(["git", "init", "-q", str(self.root)], check=True)
        subprocess.run(["git", "-C", str(self.root), "-c", "user.name=test", "-c", "user.email=test@example.invalid",
                        "commit", "--allow-empty", "-qm", "fixture"], check=True)
        commit = subprocess.check_output(["git", "-C", str(self.root), "rev-parse", "HEAD"], text=True).strip()
        self.backlog.write_text(f"Valid `{commit}` and invalid `deadbee`. Binary hash without commit markup: 1234567890abcdef.\n")
        findings = inspect(self.root)
        self.assertEqual(len(findings), 1)
        self.assertIn("deadbee", findings[0]["detail"])

    def test_missing_document_is_reported(self):
        self.backlog.unlink()
        self.assertEqual(inspect(self.root)[0]["kind"], "missing-document")

    def test_cli_dispatches_read_only_doctor(self):
        before = self.backlog.read_bytes()
        with redirect_stdout(io.StringIO()):
            self.assertEqual(main(["doctor", "--repo", str(self.root)]), 0)
        self.assertEqual(self.backlog.read_bytes(), before)

    def test_cli_rejects_invalid_report_schema(self):
        report = self.root / "invalid.json"
        report.write_text(json.dumps({"summary": {"ok": True}}))
        with redirect_stderr(io.StringIO()):
            self.assertEqual(main(["doctor", "--repo", str(self.root), "--report", str(report)]), 2)
