import io
import json
import os
import stat
import sys
import tempfile
import textwrap
import unittest
from contextlib import redirect_stdout

sys.path.insert(0, os.path.abspath(os.path.join(os.path.dirname(__file__), "..", "..")))

from compat_harness import runner  # noqa: E402
from compat_harness.cli import _diff_count_line, main as cli_main  # noqa: E402
from compat_harness.issues import generate  # noqa: E402
from compat_harness.schema_check import validate_diff, validate_report  # noqa: E402
from compat_harness.diffing import diff_reports  # noqa: E402
from compat_harness.frame import encode_rgba_png  # noqa: E402


ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", "..", ".."))


class HarnessFlowTests(unittest.TestCase):
    def test_case_list_matches_run_tests_script(self):
        with open(os.path.join(ROOT, "tools", "run_tests.sh"), encoding="utf-8") as fh:
            text = fh.read()
        for name in runner.INT_GUESTS + runner.FP_GUESTS:
            self.assertIn(name, text)
        self.assertIn("fulltest", text)
        self.assertIn("selftest", text)

    def test_fake_hvm_run_validates_and_writes_no_issue_files_on_dry_run(self):
        with tempfile.TemporaryDirectory() as tmp:
            ipa = os.path.join(tmp, "sample.ipa")
            with open(ipa, "wb") as f:
                f.write(b"fixture-bytes")
            hvm = os.path.join(tmp, "hvm")
            png = encode_rgba_png(2, 2, bytes([0, 0, 0, 255]) * 4)
            script = textwrap.dedent(f"""\
                #!/usr/bin/env python3
                import json, sys
                args = sys.argv
                def flag(name):
                    return args[args.index(name) + 1]
                report = {{
                    "exitStatus": 0,
                    "stillRunning": True,
                    "crashed": False,
                    "crashReport": "",
                    "loadError": "",
                    "missingFunctions": [],
                    "unimplementedImports": ["_tmpnam"],
                    "warnings": [{{"message": "Missing class FooBar", "count": 1}}],
                    "errors": [],
                    "audio": {{"enabled": True, "frames": 120, "nonSilentFrames": 0, "peak": 0.0,
                               "droppedBlocks": 0, "transportError": 0}},
                    "perf": {{"wallSeconds": 3.0, "guestInstructions": 10, "guestMIPS": 1.5,
                              "frames": 4, "framesPresented": 4, "avgFrameMs": 12.0,
                              "avgCompositeMs": 1.0, "maxFrameMs": 14.0, "slowFrames": 0,
                              "blocksDecoded": 2}}
                }}
                with open(flag("--report"), "w", encoding="utf-8") as f:
                    json.dump(report, f)
                with open(flag("--screenshot"), "wb") as f:
                    f.write({png!r})
            """)
            with open(hvm, "w", encoding="utf-8") as f:
                f.write(script)
            os.chmod(hvm, os.stat(hvm).st_mode | stat.S_IEXEC)
            manifest = {"apps": [{
                "id": "sample-app",
                "name": "Sample",
                "version": "1",
                "sha256": "abc",
                "optional": False,
                "seconds": 3,
            }]}
            resolved = [{
                "id": "sample-app",
                "status": "resolved",
                "source": "fixture-root",
                "path": ipa,
                "displayPath": "ipas/sample.ipa",
                "sha256": "b" * 64,
                "actualSha256": "b" * 64,
                "reason": "",
                "optional": False,
            }]
            apps = runner.run_apps(manifest, resolved, hvm=hvm, out_dir=os.path.join(tmp, "out"), seconds=3)
            self.assertEqual(apps["apps"][0]["status"], "ran")
            self.assertTrue(apps["apps"][0]["launchSuccess"])
            self.assertEqual(apps["apps"][0]["frame"]["signal"], "all-black")
            self.assertEqual(apps["apps"][0]["audio"]["signal"], "silent")
            self.assertIn("--audio-fd", apps["apps"][0]["command"])
            report = runner.build_report(
                root=ROOT,
                unit={"status": "pass", "passed": 1, "failed": 0, "cases": [
                    {"id": "fulltest", "status": "pass", "exitCode": 0, "durationSec": 0.1, "summary": "ok"}
                ]},
                python={"status": "pass", "passed": 1, "failed": 0, "cases": [
                    {"id": "verify_ipa", "status": "pass", "exitCode": 0, "durationSec": 0.1, "summary": "ok"}
                ]},
                fuzz={"status": "not-run", "cases": []},
                apps=apps,
                manifest_path=os.path.join(ROOT, "tools", "compat_harness", "manifests", "apps.json"),
                fixture_root=None,
                allow_legacy=True,
            )
            # The fake hash is not 64 hex chars in the manifest spec above; the
            # resolved record is. Schema requires 64 hex on the app row.
            errors = validate_report(report)
            self.assertEqual(errors, [], errors)
            keys = {c["key"] for c in report["clusters"]}
            self.assertIn("_tmpnam", keys)
            self.assertIn("all-black", keys)
            before = json.loads(json.dumps(report))
            before["suites"]["apps"]["apps"][0]["unimplementedImports"] = []
            before["clusters"] = []
            diff = diff_reports(before, report)
            self.assertFalse(diff["ok"])
            self.assertEqual(validate_diff(diff), [])
            docs = generate(report, write_dir=None)
            self.assertTrue(docs)
            blob = "\n".join(doc["body"] for doc in docs)
            self.assertIn("Dry-run issue", blob)
            self.assertIn("python3 tools/hvm_compat.py apps", blob)
            self.assertNotIn("gh issue create", blob)
            self.assertFalse(os.path.exists(os.path.join(tmp, "ISSUES.md")))
            written = generate(report, write_dir=os.path.join(tmp, "issues"))
            self.assertTrue(os.path.isfile(written[0]["path"]))
            self.assertFalse(any(doc["filed"] for doc in written))

    def test_log_redaction_strips_keys_and_uuids(self):
        text = runner._redact(
            "Application's key is: abcdef0123456789-dead\n"
            "UUID is: 0A7969C8-CAAD-45CB-8C38-0D643B269414\n"
            "file s_0A7969C8-CAAD-45CB-8C38-0D643B269414.yml\n"
            "path /workspace/build/x\n",
            "/workspace",
        )
        self.assertNotIn("abcdef0123456789", text)
        self.assertNotIn("0A7969C8", text)
        self.assertIn("<redacted>", text)
        self.assertIn("<uuid>", text)
        self.assertNotIn("/workspace", text)

    def test_schema_required_keys_are_enforced(self):
        errors = validate_report({"kind": "hvm-compat-report"})
        self.assertTrue(any("summary" in err for err in errors))

    def test_schema_rejects_draft_violations_and_accepts_baseline(self):
        baseline_path = os.path.join(ROOT, "docs", "dev-system", "baselines", "linux-headless.json")
        with open(baseline_path, encoding="utf-8") as fh:
            baseline = json.load(fh)
        self.assertEqual(validate_report(baseline), [])
        broken = json.loads(json.dumps(baseline))
        del broken["git"]["dirty"]
        broken["summary"]["unitFailed"] = True
        app = broken["suites"]["apps"]["apps"][0]
        app["sha256"] = "not-a-hash"
        app["frame"]["signal"] = "purple"
        app["audio"]["signal"] = "loud"
        errors = validate_report(broken)
        blob = "\n".join(errors)
        self.assertIn("git.dirty", blob)
        self.assertIn("unitFailed", blob)
        self.assertIn("sha256", blob)
        self.assertIn("frame.signal", blob)
        self.assertIn("audio.signal", blob)

    def test_missing_required_fixture_fails_without_require_resolved(self):
        with tempfile.TemporaryDirectory() as tmp:
            empty = os.path.join(tmp, "fixtures")
            os.makedirs(empty)
            manifest = os.path.join(tmp, "apps.json")
            with open(manifest, "w", encoding="utf-8") as fh:
                json.dump({
                    "schemaVersion": 1,
                    "apps": [{
                        "id": "needs-ipa",
                        "name": "Needs IPA",
                        "sha256": "a" * 64,
                        "fixtureRelPath": "ipas/missing.ipa",
                        "optional": False,
                    }],
                }, fh)
            out = os.path.join(tmp, "out")
            code = cli_main([
                "apps",
                "--manifest", manifest,
                "--fixture-root", empty,
                "--no-legacy-repo",
                "--hvm", "/bin/true",
                "--out", out,
                "--build-dir", tmp,
            ])
            self.assertEqual(code, 1)
            with open(os.path.join(out, "report.json"), encoding="utf-8") as fh:
                report = json.load(fh)
            self.assertNotIn("schemaErrors", report)
            self.assertFalse(report["summary"]["ok"])
            self.assertEqual(report["suites"]["apps"]["status"], "fail")
            self.assertEqual(report["suites"]["apps"]["apps"][0]["status"], "skipped")

    def test_optional_skip_is_partial_and_does_not_fail(self):
        with tempfile.TemporaryDirectory() as tmp:
            empty = os.path.join(tmp, "fixtures")
            os.makedirs(empty)
            manifest = os.path.join(tmp, "apps.json")
            with open(manifest, "w", encoding="utf-8") as fh:
                json.dump({
                    "schemaVersion": 1,
                    "apps": [{
                        "id": "clash-royale-2-1306-1",
                        "name": "Clash Royale",
                        "sha256": "b" * 64,
                        "fixtureRelPath": "ipas/missing.ipa",
                        "optional": True,
                    }],
                }, fh)
            out = os.path.join(tmp, "out")
            code = cli_main([
                "apps",
                "--manifest", manifest,
                "--fixture-root", empty,
                "--no-legacy-repo",
                "--hvm", "/bin/true",
                "--out", out,
                "--build-dir", tmp,
            ])
            self.assertEqual(code, 0)
            with open(os.path.join(out, "report.json"), encoding="utf-8") as fh:
                report = json.load(fh)
            self.assertNotIn("schemaErrors", report)
            self.assertTrue(report["summary"]["ok"])
            self.assertEqual(report["suites"]["apps"]["status"], "partial")
            self.assertEqual(report["suites"]["apps"]["apps"][0]["status"], "skipped")

    def test_run_diff_line_includes_the_informational_count(self):
        with tempfile.TemporaryDirectory() as tmp:
            manifest = os.path.join(tmp, "apps.json")
            with open(manifest, "w", encoding="utf-8") as fh:
                json.dump({
                    "schemaVersion": 1,
                    "apps": [{
                        "id": "optional-app",
                        "name": "Optional",
                        "sha256": "c" * 64,
                        "fixtureRelPath": "missing.ipa",
                        "optional": True,
                    }],
                }, fh)
            baseline = os.path.join(tmp, "baseline.json")
            with open(baseline, "w", encoding="utf-8") as fh:
                json.dump({"host": {"pageSize": 16384}, "suites": {}}, fh)
            out = os.path.join(tmp, "out")
            buf = io.StringIO()
            with redirect_stdout(buf):
                cli_main([
                    "run", "--skip-unit", "--skip-python", "--skip-apps",
                    "--manifest", manifest,
                    "--no-legacy-repo",
                    "--out", out,
                    "--build-dir", tmp,
                    "--baseline", baseline,
                ])
            text = buf.getvalue()
            self.assertRegex(text, r"diff ok=\S+ regressions=\d+ attention=\d+ informational=\d+")

    def test_previous_diff_line_includes_the_informational_count(self):
        line = _diff_count_line("previous ", {
            "ok": True,
            "regressions": [],
            "attention": [{}],
            "informational": [{}, {}],
        })
        self.assertEqual(line, "previous ok=True regressions=0 attention=1 informational=2")


if __name__ == "__main__":
    unittest.main()
