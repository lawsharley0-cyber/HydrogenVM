import importlib.util
import json
from pathlib import Path
import unittest

spec = importlib.util.spec_from_file_location(
    "hvm_memory_report", Path(__file__).resolve().parents[1] / "hvm_memory_report.py")
module = importlib.util.module_from_spec(spec)
spec.loader.exec_module(module)


class MemoryReportTests(unittest.TestCase):
    def test_rates_peaks_and_unsupported_fields(self):
        result = module.summarize([
            {"elapsedSeconds": 0, "residentBytes": 100, "physicalFootprintBytes": None},
            {"elapsedSeconds": 5, "residentBytes": 200},
            {"elapsedSeconds": 10, "residentBytes": 150}])
        counter = result["counters"]["residentBytes"]
        self.assertEqual((counter["peak"], counter["delta"], counter["perSecond"]), (200, 50, 5))
        self.assertNotIn("physicalFootprintBytes", result["counters"])

    def test_legacy_available_memory_samples(self):
        result = module.summarize([
            {"utc": "2026-10-27T15:10:04Z", "availableBytes": 2402},
            {"utc": "2026-10-27T15:15:04Z", "availableBytes": 983}])
        self.assertEqual(result["durationSeconds"], 300)
        self.assertAlmostEqual(result["counters"]["availableBytes"]["perSecond"], -1419 / 300)

    def test_single_sample_and_reversed_time(self):
        result = module.summarize([{"elapsedSeconds": 0, "residentBytes": 100}])
        self.assertIsNone(result["counters"]["residentBytes"]["perSecond"])
        with self.assertRaises(ValueError):
            module.summarize([{"elapsedSeconds": 1}, {"elapsedSeconds": 0}])

    def test_runtime_samples_survive_shell_available_history(self):
        rows = [{"elapsedSeconds": 0, "residentBytes": 42}]
        shell = {"memorySamples": [{"utc": "2026-10-27T15:10:04Z", "availableBytes": 7}],
                 "logs": ["unrelated", "[memory] " + json.dumps(rows[0])]}
        self.assertEqual(module.read_samples(json.dumps(shell)), rows)
        shell["runtimeMemorySamples"] = rows
        self.assertEqual(module.read_samples(json.dumps(shell)), rows)
        self.assertEqual(module.read_samples("[info] [memory] " + json.dumps(rows[0]) + "\nnoise\n"), rows)


if __name__ == "__main__":
    unittest.main()
