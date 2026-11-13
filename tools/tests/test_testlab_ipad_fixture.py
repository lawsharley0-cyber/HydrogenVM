import importlib.util
import plistlib
import tempfile
import unittest
import zipfile
from pathlib import Path


spec = importlib.util.spec_from_file_location("tablet_testlab_host", Path(__file__).parents[1] / "hvm_testlab.py")
host = importlib.util.module_from_spec(spec)
spec.loader.exec_module(host)


class TabletFixtureTests(unittest.TestCase):
    def setUp(self):
        self._dir = tempfile.TemporaryDirectory()
        self.tmp_path = Path(self._dir.name)

    def tearDown(self):
        self._dir.cleanup()

    def test_tablet_fixture_preserves_original_archive_and_all_non_metadata_bytes(self):
        source = self.tmp_path / "TestLab.ipa"
        with zipfile.ZipFile(source, "w") as z:
            z.writestr("Payload/TestLab.app/Info.plist", plistlib.dumps({"UIDeviceFamily": [1], "CFBundleExecutable": "TestLab"}))
            z.writestr("Payload/TestLab.app/TestLab", b"project-owned executable sentinel")
            z.writestr("Payload/TestLab.app/tone.wav", b"audio sentinel")
        original = source.read_bytes()
        variant = host.ipad_testlab_variant(str(source))
        self.assertNotEqual(Path(variant), source)
        self.assertEqual(source.read_bytes(), original)
        with zipfile.ZipFile(source) as before, zipfile.ZipFile(variant) as after:
            self.assertEqual(before.namelist(), after.namelist())
            for name in before.namelist():
                if not name.endswith("Info.plist"):
                    self.assertEqual(before.read(name), after.read(name))
            info = plistlib.loads(after.read("Payload/TestLab.app/Info.plist"))
            self.assertEqual(info["UIDeviceFamily"], [2])
            self.assertEqual(info["UIInterfaceOrientation"], "UIInterfaceOrientationPortrait")
            self.assertEqual(info["UIInterfaceOrientation~ipad"], "UIInterfaceOrientationLandscapeLeft")
            self.assertEqual(info["CFBundleExecutable"], "TestLab")

    def test_tablet_fixture_requires_the_project_owned_bundle_layout(self):
        source = self.tmp_path / "wrong.ipa"
        with zipfile.ZipFile(source, "w") as z:
            z.writestr("Payload/Other.app/Info.plist", b"other bundle")
        original = source.read_bytes()
        with self.assertRaisesRegex(ValueError, "no bundle Info.plist"):
            host.ipad_testlab_variant(str(source))
        self.assertEqual(source.read_bytes(), original)


if __name__ == "__main__":
    unittest.main()
