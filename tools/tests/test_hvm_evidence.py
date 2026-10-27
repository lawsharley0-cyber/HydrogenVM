#!/usr/bin/env python3
"""Synthetic contract tests for shared evidence and failure fingerprinting."""
from __future__ import annotations

import copy
import json
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

TOOLS = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(TOOLS))

from hvm_evidence import (  # noqa: E402
    SCHEMA,
    build_bundle,
    bundle_from_device,
    bundle_from_probe,
    combine_bundles,
    index_artifacts,
    validate_bundle,
    write_bundle,
)
from hvm_fingerprint import fingerprint, normalize_structure  # noqa: E402


def probe_summary(results, *, when="2026-10-26 12:00:00"):
    return {
        "kind": "hvm-probe",
        "time": when,
        "seconds": 60,
        "modes": ["software"],
        "launches": 1,
        "build": {"commit": None, "dirty": None, "configuration": "unknown", "binaryMatch": "unverified"},
        "host": {"kind": "desktop", "os": "test", "pageSize": 4096},
        "results": results,
        "issues": [],
    }


def result(bundle="com.example.game", mode="software", issues=None, status="issues", **extra):
    return {
        "bundleId": bundle,
        "ipa": "private.ipa",
        "mode": mode,
        "status": status,
        "framesPresented": 10,
        "mixer": {"framesRendered": 100, "audibleFrames": 50},
        "issues": issues or [],
        **extra,
    }


def issue(kind="picture", text="screen stays all-black", severity=80):
    return {"kind": kind, "text": text, "severity": severity, "key": f"{kind}: {text}"}


class FingerprintTests(unittest.TestCase):
    def test_volatile_values_do_not_change_identity(self):
        first = fingerprint(
            "crash", "SIGSEGV at 0x12345 in 12000 frames C:/private/game.app",
            stack=["foo", "0x8000"], renderer="software",
        )
        second = fingerprint(
            "crash", "SIGSEGV at 0xabcde in 7 frames C:/private/other.app",
            stack=["foo", "0x9000"], renderer="software",
        )
        self.assertEqual(first["id"], second["id"])
        self.assertNotIn("12345", first["operation"])
        self.assertNotIn("private", first["operation"])

    def test_type_operation_and_renderer_remain_significant(self):
        base = fingerprint("picture", "no frames", renderer="software")
        self.assertNotEqual(base["id"], fingerprint("hang", "no frames", renderer="software")["id"])
        self.assertNotEqual(base["id"], fingerprint("picture", "white screen", renderer="software")["id"])
        self.assertNotEqual(base["id"], fingerprint("picture", "no frames", renderer="gpu")["id"])

    def test_normalization_retains_api_names(self):
        self.assertEqual(normalize_structure("Missing: glGenQueriesEXT 12"), "Missing: glGenQueriesEXT <number>")


class AdapterTests(unittest.TestCase):
    def test_probe_distinguishes_failure_classes(self):
        rows = [
            result("import", issues=[issue("load", "bad Mach-O", 100)]),
            result("guest", issues=[issue("crash", "SIGSEGV", 100)], exceptionKind="signal"),
            result("host", issues=[issue("crash", "host crash in renderer", 100)]),
            result("hang", issues=[issue("hang", "watchdog", 90)]),
            result("picture", issues=[issue()]),
            result("runner", status="infrastructure-error", issues=[]),
        ]
        bundle = bundle_from_probe(probe_summary(rows))
        classes = {run["app"]["bundleId"]: run["outcome"]["class"] for run in bundle["runs"]}
        self.assertEqual(classes, {
            "import": "import_failure",
            "guest": "guest_crash",
            "host": "host_crash",
            "hang": "liveness_failure",
            "picture": "behavioral_failure",
            "runner": "infrastructure_error",
        })
        self.assertFalse(bundle["provenanceComplete"])

    def test_device_adapter_preserves_not_run_and_extracts_missing_calls(self):
        report = {
            "timestamp": "2026-10-26T12:00:00Z",
            "build": {"commit": "abc", "dirty": False, "configuration": "Release"},
            "host": {"kind": "iphone", "os": "iOS test", "pageSize": 16384},
            "suites": {"apps": {"apps": [{
                "bundleId": "com.example.device",
                "name": "Device Game",
                "version": "1.2",
                "mode": "gpu",
                "crashed": False,
                "summary": "completed",
                "logs": "Missing: glGenQueriesEXT (called from 0x12345)\nMissing: glGenQueriesEXT\nMissing: AudioUnitAddRenderNotify",
                "explore": {"seed": 42},
                "checks": [
                    {"name": "Launches", "status": "pass", "detail": "process launched"},
                    {"name": "Music", "status": "skip", "detail": "stopped early"},
                    {"name": "Shows a picture", "status": "fail", "detail": "all black"},
                ],
            }]}},
        }
        bundle = bundle_from_device(report)
        run = bundle["runs"][0]
        music = next(item for item in run["observations"] if item["kind"] == "Music")
        self.assertEqual(music["status"], "not_run")
        self.assertEqual(run["scenario"]["seed"], 42)
        self.assertEqual(run["host"]["pageSize"], 16384)
        self.assertEqual(run["outcome"]["class"], "behavioral_failure")
        self.assertEqual([api["name"] for api in bundle["missingApis"]], ["AudioUnitAddRenderNotify", "glGenQueriesEXT"])
        self.assertTrue(all(api["reachability"] == "observed_call" for api in bundle["missingApis"]))

    def test_device_host_crash_and_teardown_are_not_conflated(self):
        def app(bundle, exception):
            return {"bundleId": bundle, "mode": "software", "crashed": True,
                    "exceptionKind": exception, "summary": exception, "checks": []}
        bundle = bundle_from_device({"suites": {"apps": {"apps": [
            app("host", "host_fault"), app("teardown", "engine-teardown-timeout"),
        ]}}})
        classes = {run["app"]["bundleId"]: run["outcome"]["class"] for run in bundle["runs"]}
        self.assertEqual(classes["host"], "host_crash")
        self.assertEqual(classes["teardown"], "lifecycle_failure")

    def test_device_timestamps_preserve_attempts_across_batches(self):
        app = {"bundleId": "same", "mode": "software", "crashed": False, "checks": []}
        first = bundle_from_device({"timestamp": "first", "suites": {"apps": {"apps": [app]}}})
        second = bundle_from_device({"timestamp": "second", "suites": {"apps": {"apps": [app]}}})
        combined = combine_bundles([first, second])
        self.assertEqual(len(combined["runs"]), 2)
        self.assertNotEqual(first["runs"][0]["runId"], second["runs"][0]["runId"])

    def test_infrastructure_failure_does_not_create_game_check_failures(self):
        app = {
            "bundleId": "runner", "mode": "software", "crashed": True,
            "infrastructureError": True, "summary": "runner disk full",
            "checks": [{"name": "Shows a picture", "status": "fail", "detail": "not run"}],
        }
        bundle = bundle_from_device({"suites": {"apps": {"apps": [app]}}})
        self.assertEqual(len(bundle["runs"][0]["issues"]), 1)
        self.assertEqual(bundle["runs"][0]["outcome"]["class"], "infrastructure_error")

    def test_missing_api_ranking_prefers_observed_shared_calls(self):
        rows = [
            result("one", issues=[issue("missing", "calledApi", 40), issue("unimplemented", "importedOnly", 30)]),
            result("two", issues=[issue("missing", "calledApi", 40)]),
        ]
        ranked = bundle_from_probe(probe_summary(rows))["missingApis"]
        self.assertEqual(ranked[0]["name"], "calledApi")
        self.assertEqual(ranked[0]["affectedApps"], ["one", "two"])
        self.assertEqual(ranked[0]["observedRuns"], 2)
        self.assertIsNone(ranked[0]["observedCalls"])
        self.assertEqual(ranked[1]["reachability"], "imported_not_observed")


class ClusterTests(unittest.TestCase):
    def test_distinct_games_are_not_inflated_by_renderer_runs(self):
        rows = [
            result("same", "software", [issue()]),
            result("same", "gpu", [issue()]),
        ]
        bundle = bundle_from_probe(probe_summary(rows))
        self.assertEqual(len(bundle["clusters"]), 2)
        self.assertEqual(len(bundle["runs"]), 2)
        for cluster in bundle["clusters"]:
            self.assertEqual(cluster["affectedApps"], ["same"])
            self.assertEqual(len(cluster["affectedRuns"]), 1)

    def test_retry_after_failure_is_flaky_not_clean_pass(self):
        failing = bundle_from_probe(probe_summary([result(issues=[issue()])], when="first"))
        passing = bundle_from_probe(probe_summary([result(issues=[], status="ok")], when="second"))
        combined = combine_bundles([failing, passing])
        cluster = combined["clusters"][0]
        self.assertEqual(cluster["reproducibility"]["status"], "flaky")
        self.assertEqual(cluster["reproducibility"]["perApp"]["com.example.game"]["attempts"], 2)
        self.assertEqual(len(combined["runs"]), 2)

    def test_repeated_failure_is_reproduced(self):
        first = bundle_from_probe(probe_summary([result(issues=[issue()])], when="first"))
        second = bundle_from_probe(probe_summary([result(issues=[issue()])], when="second"))
        cluster = combine_bundles([first, second])["clusters"][0]
        self.assertEqual(cluster["reproducibility"]["status"], "reproduced")

    def test_manual_merge_and_split_are_reviewable(self):
        rows = [
            result("one", issues=[issue()]),
            result("two", issues=[issue("hang", "stuck", 90)]),
            result("three", issues=[issue()]),
        ]
        original = bundle_from_probe(probe_summary(rows))
        by_app = {run["app"]["bundleId"]: run for run in original["runs"]}
        picture_fp = by_app["one"]["issues"][0]["fingerprint"]["id"]
        hang_fp = by_app["two"]["issues"][0]["fingerprint"]["id"]
        merged = build_bundle(copy.deepcopy(original["runs"]), overrides={"merge": {picture_fp: "shared", hang_fp: "shared"}})
        self.assertEqual(len(merged["clusters"]), 1)
        self.assertEqual(len(merged["clusters"][0]["fingerprintIds"]), 2)
        self.assertTrue(merged["clusters"][0]["manual"])
        split_key = f"{by_app['one']['runId']}:{picture_fp}"
        split = build_bundle(copy.deepcopy(original["runs"]), overrides={"split": {split_key: "only-this-run"}})
        self.assertEqual(len(split["clusters"]), 3)
        self.assertEqual(sum(cluster["manual"] for cluster in split["clusters"]), 1)


class ArtifactAndCliTests(unittest.TestCase):
    def test_artifact_index_hashes_without_embedding_content(self):
        with tempfile.TemporaryDirectory() as temporary:
            path = Path(temporary) / "run.log"
            path.write_text("private game capture", encoding="utf-8")
            artifact = index_artifacts([path])[0]
            self.assertEqual(artifact["privacy"], "private")
            self.assertEqual(len(artifact["sha256"]), 64)
            self.assertNotIn("private game capture", json.dumps(artifact))

    def test_probe_run_artifacts_are_linked_by_hash(self):
        with tempfile.TemporaryDirectory() as temporary:
            run_dir = Path(temporary) / "game-software"
            run_dir.mkdir()
            for name in ("run.log", "report.json", "shot.png", "script.txt"):
                (run_dir / name).write_bytes((name + " private").encode("utf-8"))
            bundle = bundle_from_probe(probe_summary([result(issues=[issue()], dir=str(run_dir))]))
            self.assertEqual(len(bundle["artifacts"]), 4)
            self.assertEqual(len(bundle["runs"][0]["artifacts"]), 4)
            self.assertTrue(all(item["privacy"] == "private" for item in bundle["artifacts"]))
            self.assertNotIn("run.log private", json.dumps(bundle["artifacts"]))
            validate_bundle(bundle)

    def test_cli_round_trip_and_no_overwrite(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            source = root / "summary.json"
            source.write_text(json.dumps(probe_summary([result(issues=[issue()])])), encoding="utf-8")
            out = root / "new-evidence"
            command = [sys.executable, "-I", str(TOOLS / "hvm_evidence.py"),
                       "--probe-summary", str(source), "--out", str(out)]
            completed = subprocess.run(command, capture_output=True, text=True, timeout=30)
            self.assertEqual(completed.returncode, 0, completed.stderr)
            bundle = json.loads((out / "evidence.json").read_text(encoding="utf-8"))
            self.assertEqual(bundle["schema"], SCHEMA)
            validate_bundle(bundle)
            self.assertEqual(len(bundle["artifacts"]), 1)
            self.assertTrue(all(run["artifacts"] for run in bundle["runs"]))
            self.assertTrue((out / "summary.md").is_file())
            second = subprocess.run(command, capture_output=True, text=True, timeout=30)
            self.assertNotEqual(second.returncode, 0)
            self.assertIn("refusing to overwrite", second.stderr)

    def test_validator_rejects_unknown_artifact_and_wrong_schema(self):
        bundle = bundle_from_probe(probe_summary([result(issues=[issue()])]))
        wrong = copy.deepcopy(bundle)
        wrong["schema"] = "hvm-evidence/v0"
        with self.assertRaises(ValueError):
            validate_bundle(wrong)
        dangling = copy.deepcopy(bundle)
        dangling["runs"][0]["artifacts"] = ["sha256:" + "0" * 64]
        with self.assertRaises(ValueError):
            validate_bundle(dangling)

    def test_schema_file_is_valid_json_and_current(self):
        schema = json.loads((TOOLS / "hvm_evidence.schema.json").read_text(encoding="utf-8"))
        self.assertEqual(schema["properties"]["schema"]["const"], SCHEMA)
        self.assertEqual(schema["$defs"]["missingApi"]["properties"]["observedCalls"]["type"], ["integer", "null"])


if __name__ == "__main__":
    unittest.main()
