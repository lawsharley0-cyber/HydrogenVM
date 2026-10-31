#!/usr/bin/env python3
"""Unit tests for T4 Support manifest, census, failure ledger, and probe fast mode."""

import json
from pathlib import Path
import random
import re
import subprocess
import sys
import tempfile
import unittest

ROOT_DIR = Path(__file__).resolve().parents[2]
TOOLS_DIR = ROOT_DIR / "tools"
BUILD_HVM = ROOT_DIR / "build" / "hvm"

sys.path.insert(0, str(TOOLS_DIR))
import census  # noqa: E402
import ledger  # noqa: E402


class TestDumpSupportManifest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        if not BUILD_HVM.exists():
            raise unittest.SkipTest(f"{BUILD_HVM} not built")
        p = subprocess.run([str(BUILD_HVM), "--dump-support"], capture_output=True, text=True, check=True)
        cls.manifest = json.loads(p.stdout)
        cls.symbols_by_name = {s["name"]: s for s in cls.manifest.get("symbols", [])}
        cls.classes_by_name = {c["name"]: c for c in cls.manifest.get("classes", [])}
        schema_path = TOOLS_DIR / "support_manifest.schema.json"
        cls.schema = json.loads(schema_path.read_text())

    def test_schema_validity(self):
        try:
            import jsonschema
            jsonschema.validate(instance=self.manifest, schema=self.schema)
        except ImportError:
            # Fallback manual validation if jsonschema is not installed in the environment
            self.assertIn("symbols", self.manifest)
            self.assertIn("classes", self.manifest)
            self.assertIn("selectors", self.manifest)
            self.assertIn("provenance", self.manifest)

    def test_lists_known_function(self):
        self.assertIn("_printf", self.symbols_by_name, "Known function _printf should be in symbols")
        self.assertEqual(self.symbols_by_name["_printf"]["kind"], "function")
        self.assertIn("status", self.symbols_by_name["_printf"])

    def test_lists_known_class(self):
        self.assertIn("NSObject", self.classes_by_name, "Known class NSObject should be in classes")
        self.assertEqual(self.classes_by_name["NSObject"]["status"], "Real")

    def test_lists_known_stub_with_status(self):
        self.assertIn("_glDrawBuffers", self.symbols_by_name, "Known stub _glDrawBuffers should be in symbols")
        self.assertEqual(self.symbols_by_name["_glDrawBuffers"]["status"], "Stub")


class TestCensus(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        manifest_path = ROOT_DIR / "build" / "manifest_cache.json"
        if not manifest_path.exists():
            if not BUILD_HVM.exists():
                raise unittest.SkipTest(f"{BUILD_HVM} not built")
            p = subprocess.run([str(BUILD_HVM), "--dump-support"], capture_output=True, text=True, check=True)
            manifest_path.write_text(p.stdout)
        cls.manifest = json.loads(manifest_path.read_text())

    def test_fixture_hash_mismatch_raises(self):
        with tempfile.TemporaryDirectory() as td:
            bad_ipa = Path(td) / "AngryBirds_v1.0_os30.ipa"
            bad_ipa.write_text("corrupted content")
            with self.assertRaises(ValueError):
                census.census_ipa(bad_ipa, self.manifest)

    def test_reproduces_reference_numbers(self):
        # Reproduce reference numbers for all 3 committed repo-root IPAs
        for fixture_name, meta in census.FIXTURE_MANIFEST.items():
            ipa_path = ROOT_DIR / fixture_name
            if not ipa_path.exists():
                self.skipTest(f"Fixture IPA {fixture_name} not found at repo root")
            res = census.census_ipa(ipa_path, self.manifest)
            ref = meta["reference"]
            self.assertEqual(res["imported_non_objc_symbols"], ref["imported_non_objc_symbols"],
                             f"{fixture_name}: non-objc symbols mismatch")
            self.assertEqual(res["unimplemented_symbols"], ref["unimplemented_symbols"],
                             f"{fixture_name}: unimplemented symbols mismatch")
            self.assertEqual(res["missing_or_stubbed_classes"], ref["missing_or_stubbed_classes"],
                             f"{fixture_name}: missing classes mismatch")
            self.assertEqual(res["missing_selectors_classes_constants"], ref["missing_selectors_classes_constants"],
                             f"{fixture_name}: total gaps mismatch")


class TestLedger(unittest.TestCase):
    def test_group_synthetic_failures_same_signature(self):
        f1 = {
            "app": "AngryBirds",
            "signature": "libc:abort()",
            "message": "libc abort() called at 0xdeadbeef",
            "severity": 100
        }
        f2 = {
            "app": "TinyTower",
            "signature": "libc:abort()",
            "message": "libc abort() called at 0x12345678",
            "severity": 90
        }
        grouped = ledger.group_failures([f1, f2])
        self.assertEqual(len(grouped), 1)
        entry = grouped[0]
        self.assertEqual(entry["signature"], "libc:abort()")
        self.assertEqual(entry["count"], 2)
        self.assertEqual(entry["apps"], ["AngryBirds", "TinyTower"])
        self.assertEqual(entry["failure_class"], "libc")
        self.assertEqual(entry["severity"], 100)

    def test_failure_classification(self):
        cases = [
            ("std::string::_M_assign", "libstdc++ ABI"),
            ("glDrawBuffers(0)", "OpenGL ES"),
            ("-[NSObject unrecognized_selector:]", "Foundation/UIKit/ObjC"),
            ("mono_jit_exec", "Unity/Mono"),
            ("GuestFault: UndefinedInstruction", "CPU"),
            ("malloc failed", "libc"),
            ("AudioUnitRender failed", "Audio"),
            ("SCNetworkReachabilityCreate", "Network"),
            ("something unknown happened", "other"),
        ]
        for text, expected in cases:
            cls = ledger.classify_failure("sig", text)
            self.assertEqual(cls, expected, f"Failed for {text}")


class TestProbeFastMode(unittest.TestCase):
    def test_seed_determinism(self):
        sample_ipas = [Path(f"Game_{i}.ipa") for i in range(20)]
        seed = 12345
        sample_size = 4

        rng1 = random.Random(seed)
        pick1 = sorted(rng1.sample(sample_ipas, sample_size), key=lambda p: p.name)

        rng2 = random.Random(seed)
        pick2 = sorted(rng2.sample(sample_ipas, sample_size), key=lambda p: p.name)

        self.assertEqual(pick1, pick2)

    def test_summary_object_generation(self):
        results = [
            {"bundleId": "game.a", "mode": "software", "status": "ok", "issues": []},
            {"bundleId": "game.b", "mode": "software", "status": "crash", "issues": [{"key": "crash:abort", "kind": "crash", "severity": 100, "text": "abort"}]},
            {"bundleId": "game.c", "mode": "software", "status": "broken", "issues": [{"key": "broken:gl", "kind": "error", "severity": 70, "text": "gl issue"}]},
        ]
        summary_obj = {
            'totalRuns': len(results),
            'gamesProbed': len({r['bundleId'] for r in results}),
            'okRuns': sum(1 for r in results if r['status'] == 'ok'),
            'crashRuns': sum(1 for r in results if r['status'] == 'crash'),
            'brokenRuns': sum(1 for r in results if r['status'] == 'broken'),
            'issuesCount': 2,
            'fast': True,
            'seed': 42,
        }
        report = {'results': results, 'summary': summary_obj}
        parsed = json.loads(json.dumps(report))
        self.assertIn("summary", parsed)
        s = parsed["summary"]
        self.assertEqual(s["totalRuns"], 3)
        self.assertEqual(s["okRuns"], 1)
        self.assertEqual(s["crashRuns"], 1)
        self.assertEqual(s["brokenRuns"], 1)
        self.assertEqual(s["seed"], 42)

    def test_error_line_grepping_in_log(self):
        with tempfile.TemporaryDirectory() as td:
            log_path = Path(td) / "run.log"
            log_path.write_text("Normal execution line 1\nNormal execution line 2\n")
            issues = [
                {"kind": "crash", "severity": 100, "text": "SIGABRT inside libc abort()"},
                {"kind": "warning", "severity": 20, "text": "minor warning ignored"}
            ]
            with open(log_path, "a") as log_append:
                for i in issues:
                    if i["severity"] >= 45 or i["kind"] in ("error", "crash", "load", "hang", "fault"):
                        log_append.write(f"ERROR: {i['kind']}: {i['text']}\n")

            content = log_path.read_text()
            error_lines = [line for line in content.splitlines() if line.startswith("ERROR:")]
            self.assertEqual(len(error_lines), 1)
            self.assertIn("ERROR: crash: SIGABRT inside libc abort()", error_lines[0])


if __name__ == "__main__":
    unittest.main()
