"""Unit tests for testlab_schema/validate_result.py."""
import sys
import os
import unittest

sys.path.insert(0, os.path.abspath(os.path.join(os.path.dirname(__file__), "..", "..")))
from testlab_schema.validate_result import validate

MINIMAL_CASE = {
    "id": "TL-A",
    "version": 1,
    "status": "passed",
    "expected": "importable",
    "actual": "importable",
}

MINIMAL_REPORT = {
    "schemaVersion": 1,
    "kind": "hvm-testlab-result",
    "generatedAt": "2026-10-22T00:00:00Z",
    "runId": "abc123",
    "appVersion": "1.0",
    "cases": [MINIMAL_CASE],
}


class TestValidate(unittest.TestCase):
    def _ok(self, data):
        errs = validate(data)
        self.assertEqual(errs, [], msg=f"expected no errors, got: {errs}")

    def _bad(self, data, fragment):
        errs = validate(data)
        self.assertTrue(
            any(fragment in e for e in errs),
            msg=f"expected error containing '{fragment}', got: {errs}",
        )

    # --- happy path ---

    def test_minimal_report_passes(self):
        self._ok(MINIMAL_REPORT)

    def test_all_statuses(self):
        for s in ("passed", "failed", "skipped", "interrupted"):
            reason = "explanation" if s != "passed" else None
            c = dict(MINIMAL_CASE, status=s)
            if reason:
                c["reason"] = reason
            r = dict(MINIMAL_REPORT, cases=[c])
            self._ok(r)

    def test_host_metadata_optional(self):
        r = dict(
            MINIMAL_REPORT,
            host={
                "system": "Linux",
                "pageSize": 4096,
                "emulatorCommit": "39904cb",
                "emulatorDirty": False,
            },
        )
        self._ok(r)

    def test_null_artifact_sha(self):
        r = dict(MINIMAL_REPORT, artifactSha256=None)
        self._ok(r)

    def test_valid_artifact_sha(self):
        r = dict(
            MINIMAL_REPORT,
            artifactSha256="a" * 64,
        )
        self._ok(r)

    def test_host_report_kind(self):
        r = dict(MINIMAL_REPORT, kind="hvm-testlab-host-report")
        self._ok(r)

    def test_duration_and_clock(self):
        c = dict(MINIMAL_CASE, durationMs=42.5, clockDefinition="mach_absolute_time")
        r = dict(MINIMAL_REPORT, cases=[c])
        self._ok(r)

    # --- required-field failures ---

    def test_missing_schema_version(self):
        r = {k: v for k, v in MINIMAL_REPORT.items() if k != "schemaVersion"}
        self._bad(r, "schemaVersion")

    def test_missing_cases(self):
        r = {k: v for k, v in MINIMAL_REPORT.items() if k != "cases"}
        self._bad(r, "cases")

    def test_wrong_schema_version(self):
        r = dict(MINIMAL_REPORT, schemaVersion=2)
        self._bad(r, "schemaVersion")

    def test_bad_kind(self):
        r = dict(MINIMAL_REPORT, kind="something-else")
        self._bad(r, "kind")

    def test_missing_case_id(self):
        c = {k: v for k, v in MINIMAL_CASE.items() if k != "id"}
        r = dict(MINIMAL_REPORT, cases=[c])
        self._bad(r, "id")

    def test_bad_status(self):
        c = dict(MINIMAL_CASE, status="unknown")
        r = dict(MINIMAL_REPORT, cases=[c])
        self._bad(r, "status")

    def test_bad_sha256(self):
        r = dict(MINIMAL_REPORT, artifactSha256="not-a-sha")
        self._bad(r, "artifactSha256")

    def test_failed_without_reason(self):
        c = dict(MINIMAL_CASE, status="failed")
        r = dict(MINIMAL_REPORT, cases=[c])
        self._bad(r, "reason")

    def test_interrupted_without_reason(self):
        c = dict(MINIMAL_CASE, status="interrupted")
        r = dict(MINIMAL_REPORT, cases=[c])
        self._bad(r, "reason")

    def test_skipped_without_reason(self):
        c = dict(MINIMAL_CASE, status="skipped")
        r = dict(MINIMAL_REPORT, cases=[c])
        self._bad(r, "reason")

    def test_bad_duration_type(self):
        c = dict(MINIMAL_CASE, durationMs="fast")
        r = dict(MINIMAL_REPORT, cases=[c])
        self._bad(r, "durationMs")

    def test_bad_version_type(self):
        c = dict(MINIMAL_CASE, version="one")
        r = dict(MINIMAL_REPORT, cases=[c])
        self._bad(r, "version")


if __name__ == "__main__":
    unittest.main()
