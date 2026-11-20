"""tools/nightly/compare.py: classifying a night's run and spotting what got worse."""
import io
import json
import sys
import tempfile
from contextlib import redirect_stdout
import unittest
from pathlib import Path
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools"))
sys.path.insert(0, str(ROOT / "tools" / "nightly"))

import android_gaps  # noqa: E402
import compare  # noqa: E402
import fix_agent  # noqa: E402
import nightly  # noqa: E402

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


class MultiFolderAndSchemaTests(unittest.TestCase):
    def test_real_ledger_and_nightly_output_feed_the_repair_planner(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            (root / "example.apk").touch()
            binary = root / "hvm"
            binary.touch()
            state = root / "state"
            state.mkdir()
            latest = state / "latest.json"
            latest.write_text(json.dumps(night(game("example.apk"))), encoding="utf-8")
            ledger = root / "ledger.json"
            ledger.write_text(json.dumps(android_gaps.build_ledger([{
                "package": "com.test.example",
                "engine": "fixture",
                "support": "driver",
                "nativeMissing": [{"symbol": "missing_fixture_api"}],
                "runtimeMissing": {
                    "native": [],
                    "jni": [{"class": "test/Class", "name": "start", "sig": "()V"}],
                },
            }])), encoding="utf-8")
            crashed = {**game("example.apk", "crash"), "detail": "fixture crashed"}
            argv = ["nightly", "--fixtures", str(root), "--hvm", str(binary), "--state", str(state)]
            with patch.object(nightly, "run_game", return_value=crashed), \
                 patch.object(sys, "argv", argv), redirect_stdout(io.StringIO()):
                code = nightly.main()
            self.assertEqual(code, 1)
            candidates = fix_agent.load_candidates(ledger_path=ledger, nightly_path=latest)
            self.assertCountEqual([c.key for c in candidates],
                                  ["regression:example.apk:runs → crash",
                                   "gap:missing_fixture_api", "gap:test/Class.start()V"])
            self.assertEqual(candidates[0].evidence["detail"], "fixture crashed")

    def test_compat_status_and_engine(self):
        r = compare.classify({**RUNNING, "engine": "Unity (Mono)"}, 0, "")
        self.assertEqual(r["compatStatus"], "Boots")
        self.assertEqual(r["engine"], "Unity (Mono)")
        self.assertEqual(compare.classify({**RUNNING, "crashed": True}, 0, "")["compatStatus"], "Crashes")
        self.assertEqual(compare.classify(RUNNING, None, "")["compatStatus"], "Hangs")
        self.assertEqual(compare.classify({"perf": {"framesPresented": 0}}, 0, "")["compatStatus"], "No picture")

    def test_find_all_games_keeps_names_unique_across_folders(self):
        import tempfile
        import nightly  # noqa: E402
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            ipa = root / "IPA"
            apk = root / "APK"
            ipa.mkdir()
            apk.mkdir()
            (ipa / "MegaJump.ipa").write_bytes(b"ipa")
            (ipa / "Shared.ipa").write_bytes(b"ipa")
            (ipa / "Skipped.ipa").write_bytes(b"ipa")
            (ipa / "Skipped.skip").write_text("")
            (apk / "DeadSpace.apk").write_bytes(b"apk")
            (apk / "Shared.ipa").write_bytes(b"dup")

            found = nightly.find_all_games([ipa, apk])
            names = [name for _, name, _ in found]
            stems = [stem for _, _, stem in found]
            self.assertEqual(names, ["MegaJump.ipa", "IPA/Shared.ipa", "DeadSpace.apk", "APK/Shared.ipa"])
            self.assertEqual(len(set(names)), 4)
            self.assertEqual(len(set(stems)), 4)


if __name__ == "__main__":
    unittest.main()
