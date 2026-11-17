"""tools/nightly/compare.py: classifying a night's run and spotting what got worse."""
import sys
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools" / "nightly"))

import compare  # noqa: E402

RUNNING = {"perf": {"framesPresented": 500, "timeToFirstFrameMs": 1200}, "mixer": {"audibleFrames": 9000},
           "missingApis": [], "defaultedApis": [{"name": "placeholder JNI a/B.c()V"}]}


def game(name, status="runs", sound=True, missing=(), shot=""):
    return {"game": name, "status": status, "detail": "", "frames": 10, "sound": sound,
            "missingApis": list(missing), "defaultedApis": [], "screenshot": shot}


def night(*games, commit="b" * 40):
    return {"date": "2026-11-17T03:00:00", "commit": commit, "games": list(games)}


class ClassifyTests(unittest.TestCase):
    def test_running_game(self):
        r = compare.classify(RUNNING, 0, "")
        self.assertEqual((r["status"], r["frames"], r["sound"], r["firstFrameMs"]), ("runs", 500, True, 1200))
        self.assertEqual(r["defaultedApis"], ["placeholder JNI a/B.c()V"])

    def test_failures(self):
        self.assertEqual(compare.classify({}, 3, "x\ncan't run: no binary\n")["status"], "load-fail")
        self.assertEqual(compare.classify({}, 3, "x\ncan't run: no binary\n")["detail"], "can't run: no binary")
        self.assertEqual(compare.classify({**RUNNING, "crashed": True, "crashReport": "\nSIGSEGV at 0x0\nmore"}, 0, "")["detail"],
                         "SIGSEGV at 0x0")
        self.assertEqual(compare.classify(RUNNING, None, "")["status"], "hang")
        self.assertEqual(compare.classify({"perf": {"framesPresented": 0}}, 0, "")["status"], "no-picture")


class CompareTests(unittest.TestCase):
    def test_worse_better_and_new(self):
        before = night(game("a.apk"), game("b.ipa", "crash"), game("gone.ipa"), commit="a" * 40)
        after = night(game("a.apk", "crash"), game("b.ipa"), game("new.apk"))
        s = compare.compare(before, after, lambda *_: None)
        self.assertEqual([(r["game"], r["what"]) for r in s["regressions"]], [("a.apk", "runs → crash")])
        self.assertEqual([f["game"] for f in s["fixes"]], ["b.ipa"])
        self.assertEqual((s["newGames"], s["removedGames"]), (["new.apk"], ["gone.ipa"]))
        md = compare.summary_markdown(s)
        self.assertIn("aaaaaaaaaa..bbbbbbbbbb", md)
        self.assertIn("**a.apk**: runs → crash", md)

    def test_sound_and_missing_apis(self):
        before = night(game("a.apk", missing=["x"]))
        after = night(game("a.apk", sound=False, missing=["x", "socket"]))
        whats = [r["what"] for r in compare.compare(before, after, lambda *_: None)["regressions"]]
        self.assertEqual(whats, ["sound stopped", "new missing APIs"])

    def test_screenshot_change_is_a_notice_not_a_regression(self):
        s = compare.compare(night(game("a.apk", shot="1")), night(game("a.apk", shot="2")), lambda *_: 0.6)
        self.assertEqual(s["regressions"], [])
        self.assertEqual(len(s["notices"]), 1)

    def test_first_night(self):
        s = compare.compare(None, night(game("a.apk")))
        self.assertEqual((s["regressions"], s["newGames"]), ([], ["a.apk"]))
        self.assertIn("1 runs", compare.summary_markdown(s))


if __name__ == "__main__":
    unittest.main()
