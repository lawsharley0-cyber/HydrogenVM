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

    def test_active_audio_to_silent_is_a_regression(self):
        before = _report([_app(audio={"measured": True, "signal": "active", "frames": 100, "nonSilentFrames": 40})])
        after = _report([_app(audio={"measured": True, "signal": "silent", "frames": 80, "nonSilentFrames": 0})])
        diff = diff_reports(before, after)
        self.assertFalse(diff["ok"])
        self.assertTrue(any(item["kind"] == "audio" for item in diff["regressions"]))
        back = diff_reports(after, before)
        self.assertTrue(back["ok"])
        self.assertTrue(any(item["kind"] == "audio" for item in back["improvements"]))

    def test_mostly_one_color_to_blank_is_a_regression(self):
        before = _report([_app(frame={"signal": "mostly-one-color"})])
        for blank in ("all-white", "all-black"):
            with self.subTest(blank=blank):
                diff = diff_reports(before, _report([_app(frame={"signal": blank})]))
                self.assertFalse(diff["ok"])
                self.assertTrue(any(item["kind"] == "render" for item in diff["regressions"]))
        varied = diff_reports(before, _report([_app(frame={"signal": "varied"})]))
        self.assertTrue(varied["ok"])
        self.assertTrue(any(item["kind"] == "render-signal" for item in varied["informational"]))

    def test_changed_load_error_is_a_regression(self):
        before = _report([_app(loadError="old loader failure")])
        after = _report([_app(loadError="different loader failure")])
        diff = diff_reports(before, after)
        self.assertFalse(diff["ok"])
        self.assertTrue(any(item["kind"] == "load" and "different" in item["message"] for item in diff["regressions"]))
        cleared = diff_reports(before, _report([_app(loadError="")]))
        self.assertTrue(cleared["ok"])
        self.assertTrue(any(item["kind"] == "load" for item in cleared["improvements"]))

    def test_skipped_baseline_to_real_run_is_new_coverage(self):
        before = _report([_app(
            id="clash-royale-2-1306-1",
            status="skipped",
            launchSuccess=False,
            unimplementedImports=[],
            missingFunctions=[],
            perf={},
            frame={"signal": "not-run"},
            audio={"measured": False, "signal": "not-measured"},
            reason="fixture not present",
        )])
        after = _report([_app(
            id="clash-royale-2-1306-1",
            status="ran",
            launchSuccess=True,
            unimplementedImports=["SomeNewImport"],
            frame={"signal": "varied"},
            audio={"measured": True, "signal": "active"},
        )])
        diff = diff_reports(before, after)
        self.assertTrue(diff["ok"], diff["regressions"])
        self.assertEqual(diff["regressions"], [])
        self.assertTrue(any(item["kind"] == "new-coverage" for item in diff["informational"]))

    def test_page_size_note(self):
        before = _report([_app()])
        after = _report([_app()])
        after["host"] = {"pageSize": 16384}
        diff = diff_reports(before, after)
        self.assertTrue(diff["ok"])
        self.assertEqual(diff["informational"][0]["kind"], "host-page-size")

    def test_active_sample_count_drop_is_informational(self):
        cases = (
            ("frames", 1000, 400, 400, 400, "frames"),
            ("nonSilentFrames", 1000, 400, 1000, 100, "nonSilentFrames"),
        )
        for name, b_frames, b_hot, a_frames, a_hot, key in cases:
            with self.subTest(name=name):
                before = _report([_app(audio={
                    "measured": True, "signal": "active", "frames": b_frames, "nonSilentFrames": b_hot,
                })])
                after = _report([_app(audio={
                    "measured": True, "signal": "active", "frames": a_frames, "nonSilentFrames": a_hot,
                })])
                diff = diff_reports(before, after)
                self.assertTrue(diff["ok"], diff["regressions"])
                notes = [item for item in diff["informational"] if item["kind"] == "audio-samples"]
                self.assertEqual(len(notes), 1)
                self.assertIn(key, notes[0]["message"])

    def test_active_sample_count_jump_is_informational(self):
        before = _report([_app(audio={
            "measured": True, "signal": "active", "frames": 400, "nonSilentFrames": 100,
        })])
        after = _report([_app(audio={
            "measured": True, "signal": "active", "frames": 1000, "nonSilentFrames": 100,
        })])
        diff = diff_reports(before, after)
        self.assertTrue(diff["ok"], diff["regressions"])
        notes = [item for item in diff["informational"] if item["kind"] == "audio-samples"]
        self.assertEqual(len(notes), 1)
        self.assertIn("frames", notes[0]["message"])

    def test_small_active_sample_change_is_not_noted(self):
        before = _report([_app(audio={
            "measured": True, "signal": "active", "frames": 1000, "nonSilentFrames": 400,
        })])
        after = _report([_app(audio={
            "measured": True, "signal": "active", "frames": 990, "nonSilentFrames": 390,
        })])
        diff = diff_reports(before, after)
        self.assertTrue(diff["ok"])
        self.assertFalse(any(item["kind"] == "audio-samples" for item in diff["informational"]))


if __name__ == "__main__":
    unittest.main()
