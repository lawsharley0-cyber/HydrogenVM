"""Synthetic fail-closed checks for the private parity evidence boundary."""
import json
import os
from pathlib import Path
import sys
import tempfile
import unittest
from unittest.mock import patch

TOOLS = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(TOOLS))
import private_fixture_parity as parity


class PrivateParityTests(unittest.TestCase):
    def populate(self, root):
        for name, marker in parity.MARKERS.items():
            folder = root / "work" / ("sim-" + name)
            folder.mkdir(parents=True)
            (folder / "console.log").write_text(marker + "\n")
            (folder / "screen.png").write_bytes(b"\x89PNG\r\n\x1a\nsynthetic")

    def test_all_scenarios_required_and_only_bounded_metadata_returned(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            self.populate(root)
            result = parity.macos(root)
            self.assertEqual(len(result["simulatorScenarios"]), 4)
            self.assertTrue(all(row["markerObserved"] for row in result["simulatorScenarios"]))
            self.assertNotIn("console.log", json.dumps(result))
            self.assertTrue(all(len(row["screenshotSha256"]) == 64 for row in result["simulatorScenarios"]))

    def test_missing_landscape_marker_is_not_a_pass(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            self.populate(root)
            (root / "work/sim-angry/console.log").write_text("GPU: presented frame 60 directly\n")
            with self.assertRaises(ValueError):
                parity.macos(root)

    def test_missing_screenshot_is_not_a_pass(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            self.populate(root)
            (root / "work/sim-tiny-emu/screen.png").unlink()
            with self.assertRaises(OSError):
                parity.macos(root)

    def test_skipped_prerequisite_produces_failure_summary(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            destination = root / "bounded.json"
            env = {"GITHUB_SHA": "a" * 40, "GITHUB_STEP_SUMMARY": str(root / "summary"),
                   "HVM_FIXTURE_ROOT": str(root), "PARITY_BUILD": "skipped"}
            with patch.dict(os.environ, env, clear=True), patch.object(sys, "argv", ["parity", "--platform", "linux", "--out", str(destination)]):
                self.assertEqual(parity.main(), 1)
            result = json.loads(destination.read_text())
            self.assertFalse(result["parityGatePassed"])
            self.assertEqual(result["steps"]["BUILD"], "skipped")


if __name__ == "__main__":
    unittest.main()
