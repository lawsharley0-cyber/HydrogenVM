"""T2a (#257): hvm_probe lists tainted runs separately.

A tainted run called at least one missing or approximated API. It can still
finish with exit 0 and a healthy picture, so it must not be read as a clean
pass. These tests use synthetic run reports only; no game or build is needed.
"""
from __future__ import annotations

import sys
import unittest
from pathlib import Path

TOOLS = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(TOOLS))

import hvm_probe  # noqa: E402


def api(name, count=1, first=1.0):
    return {"name": name, "count": count, "firstSeconds": first, "callerPc": 0x4000,
            "caller": "Game+16384", "backtrace": ["0x00004000 Game+16384"]}


def report(stub_hits=None, apis=None, **extra):
    """A run report as `hvm run --report` writes it. Reports from older builds have no taint fields."""
    rep = {"crashed": False, "missingFunctions": [], "perf": {"framesPresented": 30}}
    if stub_hits is not None:
        rep["tainted"] = stub_hits > 0
        rep["stubHits"] = stub_hits
        rep["missingApis"] = apis or []
    rep.update(extra)
    return rep


def run(bundle_id, rep, status="ok", mode="software"):
    tainted, hits, distinct, top = hvm_probe.taint_info(rep)
    return {"bundleId": bundle_id, "mode": mode, "status": status, "tainted": tainted,
            "stubHits": hits, "distinctMissing": distinct, "topMissing": top, "dir": f"/runs/{bundle_id}-{mode}"}


class TaintInfoTests(unittest.TestCase):
    def test_reads_flag_hits_and_top_apis_by_count(self):
        rep = report(12, [api("_a", 2), api("_b", 9), api("_c", 1)])
        tainted, hits, distinct, top = hvm_probe.taint_info(rep)
        self.assertTrue(tainted)
        self.assertEqual(hits, 12)
        self.assertEqual(distinct, 3)
        self.assertEqual(top, ["_b", "_a", "_c"])

    def test_top_list_is_short(self):
        rep = report(50, [api(f"_f{i}", i + 1) for i in range(20)])
        self.assertEqual(len(hvm_probe.taint_info(rep)[3]), 3)

    def test_report_without_taint_fields_is_not_tainted(self):
        # An older build, a crashed run with no report, and a malformed value all read as "no information".
        self.assertEqual(hvm_probe.taint_info(report())[:3], (False, 0, 0))
        self.assertEqual(hvm_probe.taint_info(None)[:3], (False, 0, 0))
        self.assertEqual(hvm_probe.taint_info({"stubHits": "many", "missingApis": "x"})[:3], (False, 0, 0))

    def test_hits_without_flag_still_count_as_tainted(self):
        # The flag is derived from the hit count, so a report that only carries the count is still tainted.
        self.assertTrue(hvm_probe.taint_info({"stubHits": 3, "missingApis": [api("_x", 3)]})[0])


class TaintedRunListTests(unittest.TestCase):
    def setUp(self):
        self.results = [
            run("com.clean.game", report()),
            run("com.few.stubs", report(2, [api("_a", 2)])),
            run("com.many.stubs", report(40, [api("_b", 30), api("_c", 10)]), status="issues"),
        ]

    def test_only_tainted_runs_are_listed_most_hits_first(self):
        listed = hvm_probe.tainted_runs(self.results)
        self.assertEqual([r["bundleId"] for r in listed], ["com.many.stubs", "com.few.stubs"])
        self.assertEqual(listed[0]["stubHits"], 40)
        self.assertEqual(listed[0]["topMissing"], ["_b", "_c"])

    def test_import_failed_run_without_report_fields_is_ignored(self):
        results = self.results + [{"bundleId": "broken", "mode": "software", "status": "import-failed", "issues": []}]
        self.assertNotIn("broken", [r["bundleId"] for r in hvm_probe.tainted_runs(results)])

    def test_markdown_section_names_tainted_runs_and_leaves_clean_ones_out(self):
        text = "\n".join(hvm_probe.tainted_md(hvm_probe.tainted_runs(self.results)))
        self.assertIn("Tainted runs", text)
        self.assertIn("com.many.stubs", text)
        self.assertIn("com.few.stubs", text)
        self.assertIn("_b", text)
        self.assertNotIn("com.clean.game", text)

    def test_api_names_with_line_breaks_stay_inside_their_table_row(self):
        # Guest-supplied names (for example a KVC key) can hold CR, LF and '|'. They must not split the row.
        rows = hvm_probe.tainted_runs([run("com.odd.names", report(3, [api("key\nsecond line\rthird|pipe", 3)]))])
        md = hvm_probe.tainted_md(rows)
        self.assertEqual(len(md), len(hvm_probe.tainted_md([])) - 1 + 3, md)   # section header lines + table header, divider, one row
        row = md[-1]
        self.assertNotIn("\n", row)
        self.assertNotIn("\r", row)
        self.assertIn("\\n", row)
        self.assertIn("\\r", row)
        self.assertEqual(row.count("|"), 8, row)   # 7 cells, and the name's own '|' is replaced

    def test_backticks_in_api_names_cannot_end_the_code_span(self):
        # A backtick in a guest-supplied name would close the code span and let the rest render as Markdown.
        rows = hvm_probe.tainted_runs([run("com.odd.names", report(2, [api("evil`[link](http://x)`name", 2)]))])
        row = hvm_probe.tainted_md(rows)[-1]
        cell = row.split(" | ")[5]            # the "Most called" cell
        self.assertEqual(cell.count("`"), 2, cell)   # only the wrapping pair is left
        self.assertTrue(cell.startswith("`") and cell.endswith("`"), cell)
        self.assertIn("evil'[link](http://x)'name", cell)

    def test_hostile_game_mode_status_and_folder_cannot_break_the_row(self):
        # bundleId comes from the imported app, mode from the CLI, dir from input-derived paths.
        r = run("evil|id`x\nnext", report(2, [api("_a", 2)]), status="ok|bad", mode="mo|de`\r")
        r["dir"] = "/runs/a|b`c\nd"
        md = hvm_probe.tainted_md(hvm_probe.tainted_runs([r]))
        row = md[-1]
        self.assertNotIn("\n", row)
        self.assertNotIn("\r", row)
        self.assertEqual(row.count("|"), 8, row)            # 7 cells; every hostile '|' was replaced
        self.assertEqual(row.count("`"), 4, row)            # only the two code spans (Most called, Run folder)

    def test_markdown_says_so_when_nothing_is_tainted(self):
        text = "\n".join(hvm_probe.tainted_md(hvm_probe.tainted_runs([run("com.clean.game", report())])))
        self.assertIn("Tainted runs", text)
        self.assertIn("No tainted runs", text)
        self.assertNotIn("com.clean.game", text)

    def test_tainted_ok_run_is_still_status_ok(self):
        # Taint is reported beside the status, it does not change how a run is ranked.
        listed = hvm_probe.tainted_runs([run("com.few.stubs", report(2, [api("_a", 2)]), status="ok")])
        self.assertEqual(listed[0]["status"], "ok")


if __name__ == "__main__":
    unittest.main()
