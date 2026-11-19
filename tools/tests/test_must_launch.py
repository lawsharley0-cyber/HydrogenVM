"""The must-launch check's decisions, with runs written out by hand."""
import os
import sys
import unittest

sys.path.insert(0, os.path.join(os.path.dirname(__file__), ".."))
import must_launch  # noqa: E402


def run(frames=100, lit=0.8, audible=5000, crashed=False, signature=""):
    return {"crashed": crashed, "frames": frames, "lit": lit, "audibleFrames": audible, "signature": signature}


class Evaluate(unittest.TestCase):
    expect = {"name": "Game", "folder": "game", "frames": 50, "picture": True, "sound": True}

    def test_a_working_run_passes(self):
        self.assertEqual(must_launch.evaluate(self.expect, run()), [])

    def test_exactly_the_expected_frames_is_enough(self):
        self.assertEqual(must_launch.evaluate(self.expect, run(frames=50)), [])

    def test_one_frame_short_fails(self):
        self.assertEqual(must_launch.evaluate(self.expect, run(frames=49)), ["presented 49 frames, needs 50"])

    def test_a_crash_is_named_with_its_signature(self):
        self.assertEqual(must_launch.evaluate(self.expect, run(crashed=True, signature="NULL at game+12")),
                         ["crashed: NULL at game+12"])

    def test_a_black_screen_fails_even_with_frames(self):
        self.assertEqual(must_launch.evaluate(self.expect, run(lit=0.01)), ["black screen (1% of the picture lit)"])

    def test_a_missing_screenshot_is_not_a_pass(self):
        self.assertEqual(must_launch.evaluate(self.expect, run(lit=None)), ["no screenshot to check the picture"])

    def test_silence_fails_only_where_sound_is_expected(self):
        self.assertEqual(must_launch.evaluate(self.expect, run(audible=0)), ["silent, used to play sound"])
        quiet = dict(self.expect, sound=False)
        self.assertEqual(must_launch.evaluate(quiet, run(audible=0)), [])

    def test_no_report_fails(self):
        self.assertEqual(must_launch.evaluate(self.expect, None), ["the run produced no report"])

    def test_every_problem_is_listed(self):
        got = run(frames=0, lit=0.0, audible=0, crashed=True)
        self.assertEqual(must_launch.evaluate(self.expect, got),
                         ["crashed", "presented 0 frames, needs 50", "black screen (0% of the picture lit)", "silent, used to play sound"])


class Record(unittest.TestCase):
    def test_a_new_working_game_is_listed_with_three_tenths_of_its_frames(self):
        new = must_launch.expectation({"name": "G", "folder": "g"}, run(frames=101, audible=0))
        self.assertEqual(new, {"name": "G", "folder": "g", "frames": 30, "picture": True, "sound": False})

    def test_a_new_game_that_does_not_launch_is_not_listed(self):
        self.assertIsNone(must_launch.expectation({"name": "G", "folder": "g"}, run(crashed=True)))
        self.assertIsNone(must_launch.expectation({"name": "G", "folder": "g"}, run(lit=0.0)))
        self.assertIsNone(must_launch.expectation({"name": "G", "folder": "g"}, None))

    def test_a_listed_game_that_breaks_keeps_its_entry(self):
        old = {"name": "G", "folder": "g", "frames": 80, "picture": True, "sound": True}
        self.assertEqual(must_launch.expectation(old, run(crashed=True)), old)
        self.assertEqual(must_launch.expectation(old, None), old)

    def test_recording_never_lowers_frames_or_drops_sound(self):
        old = {"name": "G", "folder": "g", "frames": 80, "picture": True, "sound": True}
        new = must_launch.expectation(old, run(frames=60, audible=0))   # slower and silent today
        self.assertEqual((new["frames"], new["sound"]), (80, True))

    def test_recording_raises_frames_and_adds_sound(self):
        old = {"name": "G", "folder": "g", "frames": 10, "picture": True, "sound": False}
        new = must_launch.expectation(old, run(frames=400, audible=9))
        self.assertEqual((new["frames"], new["sound"]), (120, True))


class Listed(unittest.TestCase):
    def test_a_folder_that_was_not_run_is_not_written(self):
        entries = [{"name": "b", "folder": "b", "frames": 3}, {"name": "A", "folder": "a", "frames": 9},
                   {"name": "unrun", "folder": "unrun"}]
        self.assertEqual([g["name"] for g in must_launch.listed(entries)], ["A", "b"])


class Measure(unittest.TestCase):
    def test_reads_the_runner_report(self):
        report = {"crashed": False, "perf": {"framesPresented": 71}, "mixer": {"audibleFrames": 1091584}, "signature": None}
        self.assertEqual(must_launch.measure(report, 0.5),
                         {"crashed": False, "frames": 71, "lit": 0.5, "audibleFrames": 1091584, "signature": ""})

    def test_a_load_error_counts_as_a_crash(self):
        self.assertTrue(must_launch.measure({"crashed": False, "loadError": "bad Mach-O"}, None)["crashed"])

    def test_an_empty_report_is_zero_frames_and_silent(self):
        got = must_launch.measure({}, None)
        self.assertEqual((got["frames"], got["audibleFrames"], got["crashed"]), (0, 0, False))


class BuildCmd(unittest.TestCase):
    def test_device_is_passed_to_hvm(self):
        cmd = must_launch.build_cmd("hvm", "/app", "/home", 30, "/r.json", "/s.png", device="iPhone4,1")
        self.assertEqual(cmd[cmd.index("--device") + 1], "iPhone4,1")

    def test_no_device_flag_when_the_entry_has_none(self):
        cmd = must_launch.build_cmd("hvm", "/app", "/home", 30, "/r.json", "/s.png")
        self.assertNotIn("--device", cmd)

    def test_tap_and_device_can_be_combined(self):
        cmd = must_launch.build_cmd("hvm", "/app", "/home", 30, "/r.json", "/s.png",
                                    tap="169.5,217.0@8", device="iPhone4,1")
        self.assertEqual(cmd[cmd.index("--tap") + 1], "169.5,217.0@8")
        self.assertEqual(cmd[cmd.index("--device") + 1], "iPhone4,1")


class RecordDevice(unittest.TestCase):
    def test_recording_keeps_a_device_entry(self):
        old = {"name": "G", "folder": "g", "frames": 80, "picture": True, "sound": True, "device": "iPhone4,1"}
        new = must_launch.expectation(old, run(frames=200, audible=9))
        self.assertEqual(new["device"], "iPhone4,1")
        self.assertGreaterEqual(new["frames"], 80)




if __name__ == "__main__":
    unittest.main()


def test_damaged_list_stops_without_rewriting(tmp_path, capsys):
    import must_launch
    manifest = tmp_path / "must_launch.json"
    manifest.write_text("{ not json")
    apps = tmp_path / "apps"
    apps.mkdir()
    assert must_launch.main(["--apps", str(apps), "--manifest", str(manifest), "--record"]) == 2
    assert manifest.read_text() == "{ not json"
    assert "cannot read" in capsys.readouterr().out
