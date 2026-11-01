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
            self.skipTest("jsonschema is not installed; the manifest cannot be validated against its schema")

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


class TestCensusWithoutBinary(unittest.TestCase):
    """These need no hvm binary, so they run everywhere."""

    def test_missing_manifest_path_is_an_error(self):
        with self.assertRaises(FileNotFoundError):
            census.get_support_manifest(manifest_path="/nonexistent/manifest.json")

    def test_fixture_hash_mismatch_raises(self):
        with tempfile.TemporaryDirectory() as td:
            bad_ipa = Path(td) / "AngryBirds_v1.0_os30.ipa"
            bad_ipa.write_text("corrupted content")
            with self.assertRaises(ValueError):
                census.census_ipa(bad_ipa, {"symbols": [], "classes": []})


class TestCensus(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        if not BUILD_HVM.exists():
            raise unittest.SkipTest(f"{BUILD_HVM} not built")
        p = subprocess.run([str(BUILD_HVM), "--dump-support"], capture_output=True, text=True, check=True)
        cls.manifest = json.loads(p.stdout)   # dumped fresh for every run: no cache that outlives a rebuild

    def test_reproduces_reference_numbers(self):
        # Reproduce reference numbers for all 3 committed repo-root IPAs
        for fixture_name, meta in census.FIXTURE_MANIFEST.items():
            ipa_path = ROOT_DIR / fixture_name
            if not ipa_path.exists():
                self.skipTest(f"Fixture IPA {fixture_name} not found at repo root")
            res = census.census_ipa(ipa_path, self.manifest)
            self.assertEqual(res["slice"], meta["slice"], fixture_name)
            ref = meta["reference"]
            self.assertTrue(res["is_fixture"])
            # Computed from the IPA and the live manifest, never copied from the reference. The import count is a
            # property of the binary and must match exactly; the gap counts can only shrink as support is added.
            self.assertEqual(res["imported_non_objc_symbols"], ref["imported_non_objc_symbols"],
                             f"{fixture_name}: non-objc symbols mismatch")
            for key in ("unimplemented_symbols", "missing_or_stubbed_classes", "missing_selectors_classes_constants"):
                self.assertLessEqual(res[key], ref[key], f"{fixture_name}: {key} grew past the recorded reference")
            self.assertEqual(res["reference"], ref)


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
    @classmethod
    def setUpClass(cls):
        import hvm_probe
        cls.probe = hvm_probe

    def test_seed_determinism(self):
        ipas = [Path(f"Game_{i}.ipa") for i in range(20)]
        pick1 = self.probe.sample_ipas(ipas, 12345, 4)
        pick2 = self.probe.sample_ipas(ipas, 12345, 4)
        self.assertEqual(pick1, pick2)
        self.assertEqual(len(pick1), 4)
        self.assertEqual(pick1, sorted(pick1, key=lambda p: p.name))
        self.assertEqual(len(self.probe.sample_ipas(ipas[:3], 1, 10)), 3)   # fewer apps than the sample size

    def test_summary_object_counts_every_status(self):
        results = [
            {"bundleId": "game.a", "status": "ok"}, {"bundleId": "game.b", "status": "crash"},
            {"bundleId": "game.c", "status": "broken"}, {"bundleId": "game.d", "status": "issues"},
            {"bundleId": "game.e", "status": "import-failed"},
        ]
        s = self.probe.summarize_runs(results, 2, True, 42)
        self.assertEqual((s["totalRuns"], s["gamesProbed"], s["issuesCount"], s["seed"]), (5, 5, 2, 42))
        self.assertEqual((s["okRuns"], s["crashRuns"], s["brokenRuns"], s["issuesRuns"], s["importFailedRuns"]),
                         (1, 1, 1, 1, 1))

    def test_summary_counts_tainted_runs_whatever_their_status(self):
        results = [{"bundleId": "a", "status": "ok", "tainted": True}, {"bundleId": "b", "status": "ok"}]
        self.assertEqual(self.probe.summarize_runs(results, 0, False, None)["taintedRunCount"], 1)

    def test_baseline_settings_must_match_for_fixed_claims(self):
        base = {"seconds": 30, "launches": 2}
        self.assertTrue(self.probe.baseline_settings_match(base, 30, 2))
        self.assertFalse(self.probe.baseline_settings_match(base, 10, 2))   # shorter run than the baseline
        self.assertFalse(self.probe.baseline_settings_match(base, 30, 1))   # single launch vs multi-launch baseline
        self.assertTrue(self.probe.baseline_settings_match({"seconds": 30}, 30, 1))   # old baselines are single launch
        self.assertFalse(self.probe.baseline_settings_match([], 30, 1))

    def test_baseline_coverage_is_ipa_hash_and_mode(self):
        base = {"results": [{"sha256": "h1", "mode": "software"}, {"sha256": "h2", "mode": "gpu"}, {"mode": "gpu"}]}
        self.assertEqual(self.probe.baseline_coverage(base), {("h1", "software"), ("h2", "gpu")})
        self.assertEqual(self.probe.baseline_coverage({}), set())
        failed = {"results": [{"sha256": "h1", "mode": "software", "status": "import-failed"}]}
        self.assertEqual(self.probe.baseline_coverage(failed), set())   # an import failure is not coverage
        # malformed baselines are non-comparable (empty), not a crash
        self.assertEqual(self.probe.baseline_coverage({"results": [None, "x", {"sha256": "h3", "mode": "gpu"}]}), {("h3", "gpu")})
        self.assertEqual(self.probe.baseline_coverage({"results": "oops"}), set())
        self.assertEqual(self.probe.baseline_coverage([]), set())

    def test_error_lines_start_on_their_own_line(self):
        with tempfile.TemporaryDirectory() as td:
            log_path = Path(td) / "run.log"
            log_path.write_text("Normal line\nlast line without a newline")
            self.probe.tag_errors(log_path, [
                {"kind": "crash", "severity": 100, "text": "SIGABRT inside libc abort()"},
                {"kind": "warning", "severity": 20, "text": "minor warning ignored"}])
            lines = log_path.read_text().splitlines()
            self.assertEqual([l for l in lines if l.startswith("ERROR:")], ["ERROR: crash: SIGABRT inside libc abort()"])
            self.assertIn("last line without a newline", lines)

    def test_error_tagging_survives_undecodable_log_bytes(self):
        with tempfile.TemporaryDirectory() as td:
            log_path = Path(td) / "run.log"
            log_path.write_bytes(b"binary \xff\xfe tail")
            self.probe.tag_errors(log_path, [{"kind": "crash", "severity": 100, "text": "caf\u00e9 crash"}])
            self.assertIn("ERROR: crash: caf\u00e9 crash", log_path.read_text(encoding="utf-8", errors="replace"))


class TestLedgerProbeReports(unittest.TestCase):
    def test_classification_uses_the_whole_diagnostic(self):
        f = {"app": "g", "signature": "sig1", "severity": 100,
             "crashReport": "The game crashed\nGuestFault: memory fault at 0x10"}
        self.assertEqual(ledger.group_failures([f])[0]["failure_class"], ledger.classify_failure("sig1", f["crashReport"]))
        self.assertNotEqual(ledger.group_failures([f])[0]["failure_class"], "other")

    def test_reads_non_ascii_json_as_utf8(self):
        # Run under a non-UTF-8 default encoding, where reading without an explicit encoding fails.
        import os
        with tempfile.TemporaryDirectory() as td:
            path = Path(td) / "f.json"
            path.write_bytes(json.dumps([{"app": "caf\u00e9", "signature": "s", "severity": 1, "message": "\u00e9"}],
                                        ensure_ascii=False).encode("utf-8"))
            env = {**os.environ, "LC_ALL": "C", "LANG": "C", "PYTHONUTF8": "0", "PYTHONCOERCECLOCALE": "0"}
            env.pop("PYTHONIOENCODING", None)
            res = subprocess.run([sys.executable, "-X", "utf8=0", str(TOOLS_DIR / "ledger.py"), str(path), "--json"],
                                 capture_output=True, env=env)
        self.assertEqual(res.returncode, 0, res.stderr.decode("utf-8", "replace"))
        self.assertEqual(json.loads(res.stdout.decode("ascii"))[0]["apps"], ["caf\u00e9"])



if __name__ == "__main__":
    unittest.main()
