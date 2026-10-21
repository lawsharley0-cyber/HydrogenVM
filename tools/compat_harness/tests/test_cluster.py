import os
import sys
import unittest

sys.path.insert(0, os.path.abspath(os.path.join(os.path.dirname(__file__), "..", "..")))

from compat_harness.cluster import aggregate, clusters_for_app, owner_for_symbol  # noqa: E402


class ClusterTests(unittest.TestCase):
    def test_symbol_owners(self):
        self.assertEqual(owner_for_symbol("glPointSizePointerOES")[0], "gl")
        self.assertEqual(owner_for_symbol("_tmpnam")[0], "libc")
        self.assertEqual(owner_for_symbol("CGFontGetAscent")[2], "D")
        self.assertEqual(owner_for_symbol("-[UIView layoutSubviews]"), ("framework", "guest-frameworks", "D"))

    def test_missing_import_and_blank_frame(self):
        clusters = clusters_for_app({
            "status": "ran",
            "launchSuccess": True,
            "crashed": False,
            "loadError": "",
            "unimplementedImports": ["_tmpnam", "glPointSizePointerOES"],
            "missingFunctions": [],
            "warnings": [{"message": "Missing class SKPaymentQueue", "count": 1}],
            "frame": {"signal": "all-black", "blackFraction": 1, "whiteFraction": 0, "otherFraction": 0},
            "audio": {"measured": True, "transportError": 0},
        })
        kinds = {(c["kind"], c["key"], c["likelyOwner"], c["severity"]) for c in clusters}
        self.assertIn(("missing_symbol", "_tmpnam", "guest-frameworks", "failure"), kinds)
        self.assertIn(("gl_feature", "glPointSizePointerOES", "graphics-audio-input", "failure"), kinds)
        self.assertIn(("missing_class", "SKPaymentQueue", "guest-frameworks", "signal"), kinds)
        self.assertIn(("render", "all-black", "graphics-audio-input", "signal"), kinds)

    def test_null_fault_is_memory_group(self):
        report = "\n".join([
            "Crash: bad memory access at 0x00000004 (NULL pointer)",
            "  pc 0x00000000",
            "  lr 0x00001234",
        ])
        clusters = clusters_for_app({
            "status": "crashed",
            "crashed": True,
            "crashReport": report,
            "loadError": "",
            "unimplementedImports": [],
            "missingFunctions": [],
            "warnings": [],
            "frame": {"signal": "no-frames"},
            "audio": {},
        })
        fault = next(c for c in clusters if c["kind"] == "fault")
        self.assertEqual(fault["area"], "null")
        self.assertEqual(fault["group"], "A")
        self.assertEqual(fault["likelyOwner"], "runtime-cpu-memory")
        render = next(c for c in clusters if c["kind"] == "render")
        self.assertEqual(render["key"], "no-frames")
        self.assertEqual(render["severity"], "failure")

    def test_hle_gl_fault_goes_to_graphics(self):
        report = "Crash: bad memory access at 0xffff0010\n  pc [HydrogenVM] glDrawArrays\n"
        fault = next(c for c in clusters_for_app({
            "crashed": True,
            "crashReport": report,
            "unimplementedImports": [],
            "missingFunctions": [],
            "warnings": [],
            "frame": {},
            "audio": {},
        }) if c["kind"] == "fault")
        self.assertEqual(fault["likelyOwner"], "graphics-audio-input")
        self.assertEqual(fault["group"], "B")

    def test_aggregate_groups_apps(self):
        def app(ident, symbol):
            row = {
                "id": ident,
                "clusters": clusters_for_app({
                    "unimplementedImports": [symbol],
                    "missingFunctions": [],
                    "warnings": [],
                    "frame": {"signal": "varied"},
                    "audio": {},
                    "crashed": False,
                    "loadError": "",
                }),
            }
            return row
        grouped = aggregate([app("angry-birds", "_tmpnam"), app("tiny-tower", "_tmpnam")])
        tmp = next(c for c in grouped if c["key"] == "_tmpnam")
        self.assertEqual(tmp["apps"], ["angry-birds", "tiny-tower"])


if __name__ == "__main__":
    unittest.main()
