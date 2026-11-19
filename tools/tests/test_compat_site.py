import json
import tempfile
import unittest
from pathlib import Path

from tools import compat_site


class CompatSiteTests(unittest.TestCase):
    def test_build_dataset_merges_nightly_and_community_and_hides_private_screenshots(self):
        nightly = {
            "schemaVersion": 1,
            "date": "2026-11-19T04:00:00",
            "commit": "e1f9ac12ede8e01c61c412a2209ac1b693908898",
            "games": [
                {
                    "game": "DeadSpace.apk",
                    "status": "runs",
                    "compatStatus": "Plays",
                    "platform": "Android",
                    "engine": "EA Blast",
                    "frames": 1200,
                    "sound": True,
                    "screenshot": "/home/private/deadspace.png",
                    "issues": [588],
                },
                {
                    "game": "Rayman.ipa",
                    "status": "crash",
                    "platform": "iOS",
                    "engine": "UbiArt",
                    "detail": "NULL pointer at 0x28",
                    "issues": ["#529", 529],
                },
            ],
        }
        community = [
            {
                "game": "Alto.ipa",
                "status": "In-game",
                "platform": "iOS",
                "engine": "Unity",
                "date": "2026-11-18",
                "commit": "abcdef123456",
            }
        ]
        ds = compat_site.build_dataset(nightly, community=community)
        self.assertEqual(ds["total"], 3)
        self.assertEqual([g["game"] for g in ds["games"]], ["DeadSpace.apk", "Alto.ipa", "Rayman.ipa"])
        # Private local screenshot path must not leak unless explicitly allowed
        self.assertEqual(ds["games"][0]["screenshot"], "")
        self.assertEqual(ds["games"][1]["source"], "community")
        # Duplicate issue numbers are deduplicated and linked
        self.assertEqual(len(ds["games"][2]["issues"]), 1)
        self.assertTrue(ds["games"][2]["issues"][0]["url"].endswith("/issues/529"))

    def test_cli_generates_self_contained_html_and_json_with_script_escaping(self):
        nightly = {
            "date": "2026-11-19",
            "commit": "1234567890abcdef",
            "games": [
                {
                    "game": "Test</script><Game>",
                    "status": "runs",
                    "platform": "iOS",
                    "engine": "Cocos2d",
                    "publicScreenshot": "shots/test.png",
                }
            ],
        }
        with tempfile.TemporaryDirectory() as td:
            td_path = Path(td)
            status_path = td_path / "status.json"
            out_dir = td_path / "site"
            status_path.write_text(json.dumps(nightly))
            rc = compat_site.main(["--status", str(status_path), "--out", str(out_dir)])
            self.assertEqual(rc, 0)
            html_text = (out_dir / "index.html").read_text()
            self.assertNotIn("Test</script>", html_text)
            self.assertIn("Test<\\/script>", html_text)
            data = json.loads((out_dir / "compat.json").read_text())
            self.assertEqual(data["games"][0]["status"], "Boots")
            self.assertEqual(data["games"][0]["screenshot"], "shots/test.png")


if __name__ == "__main__":
    unittest.main()
