"""The "Play <game>" shortcut (#279) only reaches the app if Info.plist declares its
activity type. Guards the Swift constant and the plist from drifting apart."""
import plistlib
import re
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]


class GameShortcutActivityTest(unittest.TestCase):
    def test_info_plist_declares_the_shortcut_activity(self):
        source = (ROOT / "app/HydrogenVM/HomeScreenLink.swift").read_text()
        match = re.search(r'static let activityType = "([^"]+)"', source)
        self.assertIsNotNone(match, "GameShortcut.activityType not found")
        with open(ROOT / "app/HydrogenVM/Info.plist", "rb") as f:
            info = plistlib.load(f)
        self.assertIn(match.group(1), info.get("NSUserActivityTypes", []))

    def test_no_configuration_profile_is_written(self):
        for path in (ROOT / "app/HydrogenVM").glob("*.swift"):
            self.assertNotIn("mobileconfig", path.read_text(), path.name)


if __name__ == "__main__":
    unittest.main()
