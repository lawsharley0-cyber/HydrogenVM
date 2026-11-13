import importlib.util
import plistlib
from pathlib import Path
import zipfile

import pytest


spec = importlib.util.spec_from_file_location("tablet_testlab_host", Path(__file__).parents[1] / "hvm_testlab.py")
host = importlib.util.module_from_spec(spec)
spec.loader.exec_module(host)


def test_tablet_fixture_preserves_original_archive_and_all_non_metadata_bytes(tmp_path):
    source = tmp_path / "TestLab.ipa"
    with zipfile.ZipFile(source, "w") as z:
        z.writestr("Payload/TestLab.app/Info.plist", plistlib.dumps({"UIDeviceFamily": [1], "CFBundleExecutable": "TestLab"}))
        z.writestr("Payload/TestLab.app/TestLab", b"project-owned executable sentinel")
        z.writestr("Payload/TestLab.app/tone.wav", b"audio sentinel")
    original = source.read_bytes()
    variant = host.ipad_testlab_variant(str(source))
    assert Path(variant) != source
    assert source.read_bytes() == original
    with zipfile.ZipFile(source) as before, zipfile.ZipFile(variant) as after:
        assert before.namelist() == after.namelist()
        for name in before.namelist():
            if not name.endswith("Info.plist"):
                assert before.read(name) == after.read(name)
        info = plistlib.loads(after.read("Payload/TestLab.app/Info.plist"))
        assert info["UIDeviceFamily"] == [2]
        assert info["UIInterfaceOrientation"] == "UIInterfaceOrientationPortrait"
        assert info["UIInterfaceOrientation~ipad"] == "UIInterfaceOrientationLandscapeLeft"
        assert info["CFBundleExecutable"] == "TestLab"


def test_tablet_fixture_requires_the_project_owned_bundle_layout(tmp_path):
    source = tmp_path / "wrong.ipa"
    with zipfile.ZipFile(source, "w") as z:
        z.writestr("Payload/Other.app/Info.plist", b"other bundle")
    original = source.read_bytes()
    with pytest.raises(ValueError, match="no bundle Info.plist"):
        host.ipad_testlab_variant(str(source))
    assert source.read_bytes() == original
