import importlib.util
import json
import math
import os
import sys
import tempfile
import unittest
from pathlib import Path
from unittest import mock

spec = importlib.util.spec_from_file_location(
    "private_fixture_parity", Path(__file__).resolve().parents[1] / "private_fixture_parity.py")
module = importlib.util.module_from_spec(spec)
spec.loader.exec_module(module)


class FrameCountTests(unittest.TestCase):
    def test_positive_frames_rejects_non_counts(self):
        for bad in (True, False, None, "60", 0, -1, 0.0, math.nan, math.inf, -math.inf, 0.5, 59.9, [], {}):
            self.assertFalse(module.positive_frames(bad), repr(bad))
        self.assertFalse(module.positive_frames(10**1000))   # oversized ints are rejected without OverflowError
        self.assertFalse(module.positive_frames(2**53 + 1))
        self.assertTrue(module.positive_frames(2**53))
        self.assertFalse(module.positive_frames(-10**1000))
        self.assertFalse(module.positive_frames(float("1e308") * 10))
        for good in (1, 60, 600, 60.0):
            self.assertTrue(module.positive_frames(good), repr(good))

    def test_frames_presented_tolerates_malformed_rows(self):
        self.assertEqual(module.frames_presented({"perf": {"framesPresented": 7}}), 7)
        for row in (None, [], "row", {}, {"perf": None}, {"perf": []}, {"perf": "x"}, {"perf": {}}):
            self.assertEqual(module.frames_presented(row), 0, repr(row))


class MainTests(unittest.TestCase):
    def run_main(self, linux_impl):
        with tempfile.TemporaryDirectory() as d:
            out, summary = Path(d) / "out.json", Path(d) / "summary.md"
            env = {"GITHUB_SHA": "0" * 40, "GITHUB_STEP_SUMMARY": str(summary),
                   "HVM_FIXTURE_ROOT": d, "PARITY_build": "success"}
            with mock.patch.dict(os.environ, env), mock.patch.object(module, "linux", linux_impl), \
                    mock.patch.object(sys, "argv", ["x", "--platform", "linux", "--out", str(out)]):
                code = module.main()
            return code, json.loads(out.read_text()), out.read_text()

    def test_import_error_gives_not_verified_result(self):
        def broken(root):
            raise ImportError("compat_harness missing")
        code, result, _ = self.run_main(broken)
        self.assertEqual(code, 1)
        self.assertFalse(result["parityGatePassed"])
        self.assertIn("reason", result)

    def test_unrelated_exception_is_not_swallowed(self):
        def boom(root):
            raise RuntimeError("bug")
        with self.assertRaises(RuntimeError):
            self.run_main(boom)

    def test_output_is_strict_json(self):
        code, result, text = self.run_main(lambda root: {"requiredApps": [], "baselinePassed": True})
        self.assertEqual(code, 0)
        self.assertTrue(result["parityGatePassed"])
        self.assertNotIn("NaN", text)
        with self.assertRaises(ValueError):   # a non-finite number must never be written
            self.run_main(lambda root: {"framesPresented": math.nan})


if __name__ == "__main__":
    unittest.main()
