"""Smoke failure oracles, using synthetic processes/images only."""
import contextlib
import io
import json
import os
from pathlib import Path
import plistlib
import sys
import tempfile
import unittest
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[2]))
from tools.smoke import run


class SmokeTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.root = Path(self.tmp.name)
        self.spec = {"schema": 1, "id": "fake", "bundle_id": "org.test.app",
                     "version": "1", "ipa": "test.app", "source": "synthetic",
                     "run_seconds": .2, "rss_limit_mb": 64, "gpu": False,
                     "taps": [[1, 2, .1]], "screenshots": []}
        self.execution = {"exit_code": 0, "timeout": False, "rss_exceeded": False, "sampled_rss_bytes": 1024}
        self.report = {"stillRunning": True, "crashed": False}

    def fixture(self):
        app = self.root / "test.app"
        app.mkdir()
        (app / "Info.plist").write_bytes(plistlib.dumps({"CFBundleIdentifier": "org.test.app", "CFBundleVersion": "1"}))

    def test_missing_games_and_fixture_skip(self):
        for games in (None, self.root):
            with self.subTest(games=games):
                result = run.run_scenario(self.spec, Path("missing-hvm"), games, None, self.root / "out")
                self.assertEqual(result["status"], "SKIPPED")

    def test_identity_mismatch_fails_before_execution(self):
        self.fixture()
        self.spec["version"] = "2"
        with patch.object(run, "execute") as execute:
            with self.assertRaisesRegex(ValueError, "identity"):
                run.run_scenario(self.spec, Path("hvm"), self.root, None, self.root / "out")
            execute.assert_not_called()

    def test_each_failure_oracle(self):
        cases = [("Crash: guest bad access", {}, {}, "crash"),
                 ("[hvm] Stall: main waiting", {}, {}, "stall"),
                 ("", {"crashed": True}, {}, "crash"),
                 ("", {"loadError": "bad IPA"}, {}, "error"),
                 ("", {"stillRunning": False}, {}, "duration"),
                 ("", {}, {"exit_code": 1}, "exit"),
                 ("", {}, {"timeout": True}, "timeout"),
                 ("", {}, {"rss_exceeded": True}, "RSS"),
                 ("", {}, {"sampled_rss_bytes": None}, "unavailable"),
                 ("", {"runtimeMemorySamples": [{"residentBytes": 100 * 1024 * 1024}]}, {}, "RSS")]
        for log, report, execution, expected in cases:
            with self.subTest(expected=expected, log=log):
                failures, _ = run.evaluate(self.spec, log, self.report | report, self.execution | execution)
                self.assertTrue(any(expected in f for f in failures), failures)
        self.assertEqual(run.evaluate(self.spec, "Snapshot: requested stop", self.report, self.execution)[0], [])

    def test_required_log(self):
        self.spec["required_log"] = ["expected request"]
        self.assertIn("required log absent", run.evaluate(self.spec, "", self.report, self.execution)[0][0])
        self.assertFalse(run.evaluate(self.spec, "expected request", self.report, self.execution)[0])

    def test_paths_reject_traversal_and_escape_symlink(self):
        with self.assertRaises(ValueError):
            run.within(self.root, "../outside")
        if os.name == "posix":
            (self.root / "link").symlink_to(self.root.parent)
            with self.assertRaises(ValueError):
                run.within(self.root, "link/outside")

    def test_schema_and_checked_in_scenarios(self):
        for path in (Path(run.__file__).parent / "scenarios").glob("*.json"):
            run.load_scenario(path)
        for update in ({"run_seconds": -1}, {"rss_limit_mb": float("nan")}, {"taps": [[1, 2, 100]]}, {"ipa": "../x"}):
            path = self.root / "bad.json"
            path.write_text(json.dumps(self.spec | update))
            with self.assertRaises(ValueError):
                run.load_scenario(path)

    def test_checkpoint_replays_are_isolated_and_offline(self):
        self.fixture()
        self.spec["screenshots"] = [{"at": .1, "reference": "a.png"}]
        commands = []
        def execute(command, log_path, timeout, rss_limit):
            commands.append(command)
            home = Path(command[command.index("--home") + 1])
            self.assertFalse((home / "save").exists())
            (home / "save").write_text("modified")
            log_path.write_text("")
            (log_path.parent / "report.json").write_text(json.dumps(self.report))
            (log_path.parent / "screen.png").write_bytes(b"fake-image")
            return self.execution
        with patch.object(run, "execute", execute):
            result = run.run_scenario(self.spec, Path("hvm"), self.root, None, self.root / "out")
        self.assertEqual(result["status"], "PASS")
        self.assertEqual(result["visual_status"], "SKIPPED")
        self.assertEqual(len(commands), 2)
        self.assertNotEqual(commands[0][4], commands[1][4])
        self.assertNotIn("--internet", commands[0])
        self.assertNotIn("--tap", commands[0])
        self.assertIn("--tap", commands[1])

    def test_reference_required_and_missing_screenshot_fail(self):
        self.fixture()
        self.spec["screenshots"] = [{"at": .2, "reference": "a.png"}]
        def execute(command, log_path, timeout, rss_limit):
            log_path.write_text("")
            (log_path.parent / "report.json").write_text(json.dumps(self.report))
            return self.execution
        with patch.object(run, "execute", execute):
            result = run.run_scenario(self.spec, Path("hvm"), self.root, None, self.root / "out", True)
        self.assertEqual(result["status"], "FAILED")
        self.assertIn("screenshot missing", result["runs"][0]["failures"])

    def test_missing_report_is_failure_and_reference_skip_can_be_required(self):
        self.fixture()
        self.spec["screenshots"] = [{"at": .2, "reference": "a.png"}]
        def execute(command, log_path, timeout, rss_limit):
            log_path.write_text("")
            (log_path.parent / "screen.png").write_bytes(b"image")
            return self.execution
        with patch.object(run, "execute", execute):
            result = run.run_scenario(self.spec, Path("hvm"), self.root, None, self.root / "out", True)
        self.assertIn("runner report missing", result["runs"][0]["failures"])
        self.assertIn("screenshot reference missing", result["runs"][0]["failures"])

    def test_cli_records_skips_and_rejects_unknown_ids(self):
        with patch.dict(os.environ, {}, clear=True), contextlib.redirect_stdout(io.StringIO()):
            self.assertEqual(run.main(["--out", str(self.root / "runs")]), 0)
        summary = json.loads(next((self.root / "runs").glob("*/summary.json")).read_text())
        self.assertEqual(summary["summary"], {"PASS": 0, "FAILED": 0, "SKIPPED": 4})
        with contextlib.redirect_stderr(io.StringIO()), self.assertRaises(SystemExit) as error:
            run.main(["--only", "typo", "--out", str(self.root / "other")])
        self.assertEqual(error.exception.code, 2)

    def test_tolerant_screenshot_and_dimension_guard(self):
        try:
            from PIL import Image
        except ImportError:
            self.skipTest("Pillow not installed")
        a, b = self.root / "a.png", self.root / "b.png"
        Image.new("RGB", (100, 80), (100, 100, 100)).save(a)
        Image.new("RGB", (100, 80), (102, 102, 102)).save(b)
        self.assertLess(run.screenshot_difference(a, b), .01)
        Image.new("RGB", (100, 80), "white").save(b)
        self.assertGreater(run.screenshot_difference(a, b), .5)
        Image.new("RGB", (80, 100)).save(b)
        with self.assertRaisesRegex(ValueError, "dimensions"):
            run.screenshot_difference(a, b)

    @unittest.skipUnless(sys.platform.startswith("linux"), "Linux bench process accounting")
    def test_real_process_timeout_and_exit(self):
        result = run.execute([sys.executable, "-c", "import time; time.sleep(10)"], self.root / "timeout.log", .15, 100 * 1024 * 1024)
        self.assertTrue(result["timeout"])
        self.assertIsNotNone(result["sampled_rss_bytes"])
        self.assertLess(result["elapsed_seconds"], 3)
        result = run.execute([sys.executable, "-c", "raise SystemExit(7)"], self.root / "exit.log", 2, 100 * 1024 * 1024)
        self.assertEqual(result["exit_code"], 7)


if __name__ == "__main__":
    unittest.main()
