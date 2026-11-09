"""Home Screen icons (#279). Existing "Play <game>" shortcuts only reach the app if
Info.plist declares their activity type; new icons come from a web clip profile."""
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

    def test_home_screen_uses_a_web_clip_profile(self):
        """Add to Home Screen installs a web clip (LiveContainer's "Create App Clip"):
        the profile's URL is the game link, opened in Safari as a data: URL."""
        sheet = (ROOT / "app/HydrogenVM/AddToHomeScreen.swift").read_text()
        link = (ROOT / "app/HydrogenVM/HomeScreenLink.swift").read_text()
        self.assertIn("SFSafariViewController", sheet)
        self.assertIn("data:application/x-apple-aspen-config;base64,", link)
        self.assertIn("com.apple.webClip.managed", link)
        self.assertNotIn("shortcuts://", sheet)

if __name__ == "__main__":
    unittest.main()
