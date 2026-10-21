import os
import sys
import unittest

sys.path.insert(0, os.path.abspath(os.path.join(os.path.dirname(__file__), "..", "..")))

from compat_harness.diffing import diff_reports  # noqa: E402


def _report(apps, cases=None):
    return {
        "host": {"pageSize": 4096},
        "suites": {
            "unit": {"cases": cases if cases is not None else [{"id": "fulltest", "status": "pass"}]},
            "python": {"cases": [{"id": "verify_ipa", "status": "pass"}]},
            "apps": {"apps": apps},
        },
    }


def _app(**kwargs):
    base = {
        "id": "angry-birds",
        "status": "ran",
        "launchSuccess": True,
        "crashed": False,
        "loadError": "",
        "reason": "",
        "unimplementedImports": ["_tmpnam"],
        "missingFunctions": [],
        "perf": {"framesPresented": 100, "avgFrameMs": 20.0, "guestMIPS": 10.0},
        "frame": {"signal": "varied"},
        "audio": {"measured": True, "signal": "silent"},
    }
    base.update(kwargs)
    return base


class DiffTests(unittest.TestCase):
    def test_identical_is_clean(self):
        report = _report([_app()])
        diff = diff_reports(report, report)
        self.assertTrue(diff["ok"])
        self.assertEqual(diff["regressions"], [])

    def test_new_crash_and_import_are_regressions(self):
        before = _report([_app()])
        after = _report([_app(crashed=True, launchSuccess=False, status="crashed",
                              unimplementedImports=["_tmpnam", "regcomp"])])
        diff = diff_reports(before, after)
        kinds = {item["kind"] for item in diff["regressions"]}
        self.assertIn("crash", kinds)
        self.assertIn("launch", kinds)
        self.assertIn("unimplemented-import", kinds)
        self.assertFalse(diff["ok"])

    def test_mips_and_frame_time_are_not_failures(self):
        before = _report([_app()])
        after = _report([_app(perf={"framesPresented": 90, "avgFrameMs": 40.0, "guestMIPS": 4.0})])
        diff = diff_reports(before, after)
        self.assertTrue(diff["ok"])
        self.assertTrue(any(item["kind"] == "perf" for item in diff["informational"]))

    def test_varied_to_black_is_a_regression(self):
        before = _report([_app()])
        after = _report([_app(frame={"signal": "all-black"})])
        diff = diff_reports(before, after)
        self.assertFalse(diff["ok"])
        self.assertTrue(any(item["kind"] == "render" for item in diff["regressions"]))

    def test_runtime_miss_is_attention(self):
        before = _report([_app()])
        after = _report([_app(missingFunctions=["_regcomp"])])
        diff = diff_reports(before, after)
        self.assertTrue(diff["ok"])
        self.assertEqual(diff["attention"][0]["kind"], "runtime-miss")

    def test_unit_failure_and_frames_dropped(self):
        before = _report([_app()])
        after = _report(
            [_app(perf={"framesPresented": 0, "avgFrameMs": 0, "guestMIPS": 0})],
            cases=[{"id": "fulltest", "status": "fail"}],
        )
        diff = diff_reports(before, after)
        kinds = {item["kind"] for item in diff["regressions"]}
        self.assertIn("test", kinds)
        self.assertIn("frames", kinds)

    def test_skip_after_a_real_run_is_a_regression(self):
        before = _report([_app()])
        after = _report([_app(status="skipped", launchSuccess=False, reason="missing fixture")])
        diff = diff_reports(before, after)
        self.assertFalse(diff["ok"])
        self.assertEqual(diff["regressions"][0]["kind"], "fixture-skipped")

    def test_page_size_note(self):
        before = _report([_app()])
        after = _report([_app()])
        after["host"] = {"pageSize": 16384}
        diff = diff_reports(before, after)
        self.assertTrue(diff["ok"])
        self.assertEqual(diff["informational"][0]["kind"], "host-page-size")


if __name__ == "__main__":
    unittest.main()
