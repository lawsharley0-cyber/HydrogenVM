import json
import os
import sys
import tempfile
import unittest

sys.path.insert(0, os.path.abspath(os.path.join(os.path.dirname(__file__), "..", "..")))

from compat_harness.cli import main as cli_main  # noqa: E402
from compat_harness.history import (  # noqa: E402
    HistoryError,
    index_run,
    previous_report_path,
    record_report,
)


def _report(when: str, short: str, *, ok: bool = True, frames: int = 10) -> dict:
    return {
        "schemaVersion": 1,
        "kind": "hvm-compat-report",
        "generatedAt": when,
        "git": {
            "commit": short + "c" * (40 - len(short)),
            "short": short,
            "branch": "test",
            "dirty": False,
        },
        "host": {"system": "Linux", "machine": "x86_64", "pageSize": 4096},
        "summary": {
            "ok": ok,
            "unitFailed": 0,
            "pythonFailed": 0,
            "appsCrashed": 0,
            "appsSkipped": 1,
            "clusterCount": 0,
        },
        "suites": {"apps": {"apps": [{
            "id": "sample",
            "status": "ran",
            "launchSuccess": True,
            "audio": {"signal": "active", "frames": frames, "nonSilentFrames": frames},
        }]}},
    }


class HistoryTests(unittest.TestCase):
    def test_record_writes_versioned_report_index_and_latest(self):
        with tempfile.TemporaryDirectory() as tmp:
            report = _report("2026-10-21T18:45:30+00:00", "a4b0f8b12345")
            entry = record_report(report, tmp)
            rel = "linux-x86_64/20261021T184530Z-a4b0f8b12345.json"
            self.assertEqual(entry["path"], rel)
            self.assertEqual(entry["sha"], report["git"]["commit"])
            self.assertEqual(entry["date"], "2026-10-21T18:45:30+00:00")
            self.assertEqual(entry["schemaVersion"], 1)
            self.assertEqual(entry["summary"]["ok"], True)
            stored = os.path.join(tmp, "docs", "dev-system", "reports", rel)
            with open(stored, encoding="utf-8") as fh:
                body = json.load(fh)
            self.assertEqual(body["schemaVersion"], 1)
            self.assertEqual(body["kind"], "hvm-compat-report")
            self.assertNotIn("png", "".join(os.listdir(os.path.dirname(stored))))
            index_path = os.path.join(tmp, "docs", "dev-system", "reports", "index.json")
            latest_path = os.path.join(tmp, "docs", "dev-system", "reports", "latest.json")
            with open(index_path, encoding="utf-8") as fh:
                index = json.load(fh)
            with open(latest_path, encoding="utf-8") as fh:
                latest = json.load(fh)
            self.assertEqual(index["schemaVersion"], 1)
            self.assertEqual(len(index["runs"]), 1)
            self.assertEqual(index["runs"][0]["sha"], entry["sha"])
            self.assertEqual(index["runs"][0]["schemaVersion"], 1)
            self.assertEqual(latest["path"], rel)
            self.assertEqual(latest["reportSchemaVersion"], 1)
            self.assertEqual(latest["sha"], entry["sha"])

    def test_second_record_appends_and_previous_is_the_first(self):
        with tempfile.TemporaryDirectory() as tmp:
            first = record_report(_report("2026-10-21T18:45:30+00:00", "aaaaaaaaaaaa"), tmp)
            second = record_report(_report("2026-10-21T19:00:00+00:00", "bbbbbbbbbbbb", ok=False), tmp)
            index_path = os.path.join(tmp, "docs", "dev-system", "reports", "index.json")
            with open(index_path, encoding="utf-8") as fh:
                index = json.load(fh)
            self.assertEqual([row["path"] for row in index["runs"]], [first["path"], second["path"]])
            latest_path = os.path.join(tmp, "docs", "dev-system", "reports", "latest.json")
            with open(latest_path, encoding="utf-8") as fh:
                latest = json.load(fh)
            self.assertEqual(latest["path"], second["path"])
            current = os.path.join(tmp, "docs", "dev-system", "reports", second["path"])
            previous = previous_report_path(tmp, current)
            self.assertEqual(previous, os.path.join(tmp, "docs", "dev-system", "reports", first["path"]))

    def test_unindexed_report_diffs_against_the_latest_indexed_one(self):
        with tempfile.TemporaryDirectory() as tmp:
            first = record_report(_report("2026-10-21T18:45:30+00:00", "aaaaaaaaaaaa"), tmp)
            outsider = os.path.join(tmp, "build", "report.json")
            os.makedirs(os.path.dirname(outsider), exist_ok=True)
            with open(outsider, "w", encoding="utf-8") as fh:
                json.dump(_report("2026-10-21T19:10:00+00:00", "cccccccccccc"), fh)
            previous = previous_report_path(tmp, outsider)
            self.assertEqual(previous, os.path.join(tmp, "docs", "dev-system", "reports", first["path"]))
        with tempfile.TemporaryDirectory() as empty:
            self.assertIsNone(previous_report_path(empty, os.path.join(empty, "report.json")))

    def test_empty_index_has_no_previous(self):
        with tempfile.TemporaryDirectory() as tmp:
            self.assertIsNone(previous_report_path(tmp, os.path.join(tmp, "report.json")))

    def test_same_bytes_are_not_indexed_twice(self):
        with tempfile.TemporaryDirectory() as tmp:
            report = _report("2026-10-21T18:45:30+00:00", "aaaaaaaaaaaa")
            record_report(report, tmp)
            record_report(report, tmp)
            with open(os.path.join(tmp, "docs", "dev-system", "reports", "index.json"), encoding="utf-8") as fh:
                index = json.load(fh)
            self.assertEqual(len(index["runs"]), 1)

    def test_collision_keeps_both_reports(self):
        with tempfile.TemporaryDirectory() as tmp:
            record_report(_report("2026-10-21T18:45:30+00:00", "aaaaaaaaaaaa", frames=1), tmp)
            second = record_report(_report("2026-10-21T18:45:30+00:00", "aaaaaaaaaaaa", frames=2), tmp)
            self.assertTrue(second["path"].endswith("-2.json"))
            with open(os.path.join(tmp, "docs", "dev-system", "reports", "index.json"), encoding="utf-8") as fh:
                index = json.load(fh)
            self.assertEqual(len(index["runs"]), 2)

    def test_record_refuses_a_report_without_schema_version(self):
        with tempfile.TemporaryDirectory() as tmp:
            report = _report("2026-10-21T18:45:30+00:00", "aaaaaaaaaaaa")
            del report["schemaVersion"]
            with self.assertRaises(HistoryError):
                record_report(report, tmp)

    def test_index_run_diffs_previous_then_records(self):
        with tempfile.TemporaryDirectory() as tmp:
            record_report(_report("2026-10-21T18:45:30+00:00", "aaaaaaaaaaaa", frames=10), tmp)
            current = _report("2026-10-21T19:10:00+00:00", "cccccccccccc", frames=0)
            current["suites"]["apps"]["apps"][0]["launchSuccess"] = False
            current["suites"]["apps"]["apps"][0]["status"] = "crashed"
            current["suites"]["apps"]["apps"][0]["crashed"] = True
            result = index_run(
                current, tmp, record=True, diff_previous=True,
                current_path=os.path.join(tmp, "build", "report.json"),
            )
            self.assertFalse(result["previousMissing"])
            self.assertIsNotNone(result["previousDiff"])
            self.assertEqual(result["recorded"]["schemaVersion"], 1)
            with open(os.path.join(tmp, "docs", "dev-system", "reports", "index.json"), encoding="utf-8") as fh:
                index = json.load(fh)
            self.assertEqual(len(index["runs"]), 2)
            self.assertEqual(index["runs"][0]["sha"].startswith("aaaaaaaaaaaa"), True)

    def test_cli_diff_previous_uses_the_index_not_a_baseline_file(self):
        with tempfile.TemporaryDirectory() as tmp:
            record_report(_report("2026-10-21T18:45:30+00:00", "aaaaaaaaaaaa"), tmp)
            current_path = os.path.join(tmp, "current.json")
            later = _report("2026-10-21T19:10:00+00:00", "cccccccccccc", ok=False)
            later["suites"]["unit"] = {"cases": [{"id": "fulltest", "status": "fail"}]}
            later["suites"]["python"] = {"cases": [{"id": "verify_ipa", "status": "pass"}]}
            # The first indexed report has no unit cases, so a new failure is not a
            # regression of a previously passing case. Flip launch instead.
            later["suites"]["apps"]["apps"][0]["launchSuccess"] = False
            later["suites"]["apps"]["apps"][0]["status"] = "crashed"
            later["suites"]["apps"]["apps"][0]["crashed"] = True
            with open(current_path, "w", encoding="utf-8") as fh:
                json.dump(later, fh)
            out = os.path.join(tmp, "diff.json")
            code = cli_main(["diff", "--previous", "--repo", tmp, current_path, "--out", out])
            self.assertEqual(code, 1)
            with open(out, encoding="utf-8") as fh:
                diff = json.load(fh)
            self.assertFalse(diff["ok"])
            self.assertTrue(any(item["kind"] == "crash" for item in diff["regressions"]))

    def test_cli_record_and_diff_previous_of_indexed_file(self):
        with tempfile.TemporaryDirectory() as tmp:
            first_path = os.path.join(tmp, "first.json")
            second_path = os.path.join(tmp, "second.json")
            with open(first_path, "w", encoding="utf-8") as fh:
                json.dump(_report("2026-10-21T18:45:30+00:00", "aaaaaaaaaaaa"), fh)
            crashed = _report("2026-10-21T19:10:00+00:00", "bbbbbbbbbbbb")
            crashed["suites"]["apps"]["apps"][0]["launchSuccess"] = False
            crashed["suites"]["apps"]["apps"][0]["status"] = "crashed"
            crashed["suites"]["apps"]["apps"][0]["crashed"] = True
            with open(second_path, "w", encoding="utf-8") as fh:
                json.dump(crashed, fh)
            self.assertEqual(cli_main(["record", "--repo", tmp, first_path]), 0)
            self.assertEqual(cli_main(["record", "--repo", tmp, second_path]), 0)
            with open(os.path.join(tmp, "docs", "dev-system", "reports", "latest.json"), encoding="utf-8") as fh:
                latest = json.load(fh)
            indexed = os.path.join(tmp, "docs", "dev-system", "reports", latest["path"])
            # The indexed path is the row just recorded. Previous is the first run,
            # so the crash is a regression. Diffing the file against itself would be clean.
            out = os.path.join(tmp, "diff.json")
            self.assertEqual(cli_main(["diff", "--previous", "--repo", tmp, indexed, "--out", out]), 1)
            with open(out, encoding="utf-8") as fh:
                diff = json.load(fh)
            self.assertTrue(any(item["kind"] == "crash" for item in diff["regressions"]))


if __name__ == "__main__":
    unittest.main()
