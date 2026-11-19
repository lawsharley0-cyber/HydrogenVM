"""Home Screen icons (#279). Existing "Play <game>" shortcuts only reach the app if
Info.plist declares their activity type; new icons come from the "Play Game" App Intent."""
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

    def test_home_screen_goes_through_shortcuts(self):
        """Add to Home Screen goes through Shortcuts: each runnable game is a "Play Game"
        App Intent whose run opens the game through the same game link; the sheet opens
        Shortcuts and no longer installs a configuration profile."""
        sheet = (ROOT / "app/HydrogenVM/AddToHomeScreen.swift").read_text()
        intents = (ROOT / "app/HydrogenVM/GameIntents.swift").read_text()
        self.assertIn("shortcuts://", sheet)
        self.assertNotIn("SFSafariViewController", sheet)
        self.assertNotIn("application/x-apple-aspen-config", sheet)
        self.assertIn("struct PlayGameIntent: AppIntent", intents)
        self.assertIn("AppShortcutsProvider", intents)
        self.assertIn("GameLink.url(forGameID:", intents)
        self.assertIn("LaunchRouter.shared.open", intents)
        self.assertIn("(.applicationName)", intents)

    def test_saving_the_icon_has_a_photos_reason(self):
        """Saving the icon to Photos (for the shortcut picture) needs the add-only reason."""
        with open(ROOT / "app/HydrogenVM/Info.plist", "rb") as f:
            info = plistlib.load(f)
        self.assertTrue(info.get("NSPhotoLibraryAddUsageDescription"))

if __name__ == "__main__":
    unittest.main()
