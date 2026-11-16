import math
from pathlib import Path
import sys
import unittest
sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import perf_summary


class PerfSummaryTests(unittest.TestCase):
    def setUp(self):
        self.report = {"exitStatus": 0, "crashed": False, "perf": {
            "wallSeconds": 60, "framesPresented": 2000, "avgFrameMs": 21,
            "p95FrameMs": 25, "p99FrameMs": 29, "guestMIPS": 90}}
        self.host = "  12.50%  [.] Hydrogen::CPU::handler\n   1.25% [.] memcpy\n"
        self.coremark = "Total time (secs): 12.123\nIterations/Sec: 2100.1\nCorrect operation validated.\n"

    def test_validated_baseline_preserves_metrics_without_speedup_claim(self):
        result = perf_summary.summarize(self.report, self.host, [self.coremark])
        self.assertEqual(result["coremark"][0]["iterations_per_second"], 2100.1)
        self.assertEqual(result["game_metrics"]["p99FrameMs"], 29)
        self.assertEqual(result["top_host_samples"][0]["percent"], 12.5)
        self.assertIn("no speedup", result["interpretation"])

    def test_coremark_requires_crc_validation_and_full_duration(self):
        for text in ("Iterations/Sec: 100", self.coremark.replace("12.123", "1.0"),
                     self.coremark.replace("2100.1", "0"), self.coremark.replace("Correct operation validated", "ERROR")):
            with self.subTest(text=text), self.assertRaises(ValueError):
                perf_summary.coremark(text)

    def test_crashes_and_bad_metrics_are_rejected(self):
        for extra in ({"crashed": True}, {"loadError": "bad input"}, {"exitStatus": 1}, {"crashReport": "fault"}):
            with self.subTest(extra=extra), self.assertRaises(ValueError):
                perf_summary.summarize(self.report | extra, self.host, [])
        for bad in (None, math.nan, math.inf, -1, True):
            with self.subTest(bad=bad), self.assertRaises(ValueError):
                perf_summary.summarize(self.report | {"perf": self.report["perf"] | {"p95FrameMs": bad}}, self.host, [])

    def test_missing_perf_or_frames_is_not_a_measurement(self):
        with self.assertRaises(ValueError):
            perf_summary.host_samples("permission denied")
        with self.assertRaises(ValueError):
            perf_summary.host_samples("101% [.] invalid")
        with self.assertRaises(ValueError):
            perf_summary.summarize(self.report | {"perf": self.report["perf"] | {"framesPresented": 0}}, self.host, [])

    def test_perf_ipc_columns_do_not_become_part_of_demangled_symbols(self):
        rows = perf_summary.host_samples(" 5.69% [.] drawImage(Surface&, unsigned int)       -      -\n")
        self.assertEqual(rows[0]["symbol"], "drawImage(Surface&, unsigned int)")


if __name__ == "__main__":
    unittest.main()
