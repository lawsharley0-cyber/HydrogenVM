"""missing_ledger: parsing each input kind, folding apps, ranking and diff, on synthetic logs."""
import io
import json
from pathlib import Path
import sys
import tempfile
import unittest
import zipfile

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import missing_ledger as ml

PLAY_LOG = """HydrogenVM 1.0.0 (2) on iOS 27.3
guest: /private/var/mobile/Containers/Data/Application/X/Documents/Apps/com.example.racer/Racer.app/

Loaded Racer: armv7, 100 imports, 1 initializers, entry 0x1000 Racer+0 _main
Missing class CAGradientLayer (created a placeholder subclass of NSObject)
Missing data symbol vtable for std::out_of_range (virtual calls through it report the slot)
6 imported functions aren't implemented yet (the app may still run until it calls one): sem_open, std::__1::foo(int, char), getnameinfo, SecTrustSetAn
Missing: sem_open
Missing: -[NSNetServiceBrowser setDelegate:] (returned nil)
Missing: StoreKit (in-app purchases are disabled)
"""

OTHER_LOG = """Loaded Racer: armv7, 100 imports
Missing: sem_open
Missing: getnameinfo
"""


def hvm_report(bundle, called, imports, crashed=False):
    return {"bundle": bundle, "missingApis": [{"name": n, "count": 2} for n in called],
            "unimplementedImports": imports, "missingFunctions": [], "crashed": crashed,
            "signature": "Crash: bad memory access" if crashed else ""}


def batch_report(entries):
    return {"suites": {"apps": {"apps": entries}}, "build": {}, "kind": "batch"}


class Parsing(unittest.TestCase):
    def test_play_log(self):
        app = ml.parse_log_text(PLAY_LOG, "x.log")
        self.assertEqual(app["id"], "com.example.racer")
        self.assertEqual(app["exe"], "Racer")
        self.assertIn(("function", "sem_open"), app["called"])
        self.assertIn(("selector", "-[NSNetServiceBrowser setDelegate:]"), app["called"])   # note stripped
        self.assertIn(("limitation", "StoreKit (in-app purchases are disabled)"), app["called"])
        self.assertIn(("class", "CAGradientLayer"), app["imported"])
        self.assertIn(("data", "vtable for std::out_of_range"), app["imported"])
        # commas inside a C++ signature do not split it; the cut-off last name is dropped
        self.assertIn(("function", "std::__1::foo(int, char)"), app["imported"])
        self.assertNotIn(("function", "SecTrustSetAn"), app["imported"])

    def test_hvm_report_and_batch_report(self):
        rep = ml.parse_bytes(json.dumps(hvm_report("/x/Racer.app", ["sem_open"], ["sem_open", "fork"])).encode(), "r.json")
        self.assertEqual([a["id"] for a in rep], ["Racer"])
        self.assertEqual(rep[0]["called"][("function", "sem_open")], 2)
        batch = ml.parse_bytes(json.dumps(batch_report([
            {"bundleId": "com.example.racer", "logs": OTHER_LOG, "crashed": True, "signature": "Crash: x"}])).encode(), "report.json")
        self.assertEqual(batch[0]["id"], "com.example.racer")
        self.assertTrue(batch[0]["crashed"])

    def test_zip_export_and_name_folding(self):
        buf = io.BytesIO()
        with zipfile.ZipFile(buf, "w") as z:
            z.writestr("export/play-logs/1-Racer.log", PLAY_LOG)
            z.writestr("export/logs/com.example.racer_gpu.log", OTHER_LOG)
        with tempfile.TemporaryDirectory() as d:
            Path(d, "e.zip").write_bytes(buf.getvalue())
            Path(d, "r.json").write_text(json.dumps(hvm_report("/b/Racer.app", ["getnameinfo"], [])))
            apps = ml.merge_apps(ml.collect([d]))
        # the play log, the export's gpu log and the bench report (executable name) are one app
        self.assertEqual(list(apps), ["com.example.racer"])
        self.assertEqual(apps["com.example.racer"]["called"][("function", "sem_open")], 2)


class Ranking(unittest.TestCase):
    def ledger(self, support=None):
        apps = ml.merge_apps([
            ml.parse_log_text(PLAY_LOG, "a.log"),
            ml.parse_log_text("Loaded Jumper: armv7\nMissing: sem_open\n3 imported functions aren't implemented yet (x): sem_open, fork, getnameinfo\n", "b.log"),
            ml.parse_log_text("Loaded Flyer: armv7\n2 imported functions aren't implemented yet (x): fork, getnameinfo\n", "c.log"),
        ])
        return ml.build_ledger(apps, support or {})

    def test_called_outranks_imported(self):
        led = self.ledger()
        top = led["apis"][0]
        self.assertEqual(top["name"], "sem_open")
        self.assertEqual(top["calledBy"], ["Jumper", "com.example.racer"])
        fork = next(r for r in led["apis"] if r["name"] == "fork")
        self.assertEqual(fork["calledBy"], [])
        self.assertEqual(len(fork["importedBy"]), 2)
        self.assertLess(fork["score"], top["score"])

    def test_support_marks_fixed(self):
        support = {("function", "sem_open"): "Real"}
        led = self.ledger(support)
        self.assertEqual(next(r for r in led["apis"] if r["name"] == "sem_open")["nowStatus"], "Real")
        jumper = next(a for a in led["apps"] if a["id"] == "Jumper")
        self.assertEqual(jumper["openCalled"], [])
        md = ml.markdown(led, 10)
        self.assertIn("Implemented since these logs", md)
        self.assertIn("StoreKit (in-app purchases are disabled)", md)

    def test_norm_matches_demangler_spacing(self):
        self.assertEqual(ml.norm("std::__1::basic_string<char, std::__1::allocator<char> > const&"),
                         ml.norm("std::__1::basic_string<char, std::__1::allocator<char>> const&"))
        self.assertEqual(ml.norm("_sem_open"), "sem_open")
        self.assertEqual(ml.norm("__cxa_throw"), "__cxa_throw")

    def test_diff(self):
        old = self.ledger()
        new = self.ledger({("function", "sem_open"): "Real"})
        text = ml.diff(old, new)
        self.assertIn("Resolved called APIs: 1", text)
        self.assertIn("- sem_open (2 apps)", text)
        self.assertIn("* Jumper", text)

    def test_deterministic_json(self):
        a = json.dumps(self.ledger(), sort_keys=True)
        b = json.dumps(self.ledger(), sort_keys=True)
        self.assertEqual(a, b)


if __name__ == "__main__":
    unittest.main()
