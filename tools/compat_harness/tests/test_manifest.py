import hashlib
import json
import os
import sys
import tempfile
import unittest

sys.path.insert(0, os.path.abspath(os.path.join(os.path.dirname(__file__), "..", "..")))

from compat_harness.manifest import (  # noqa: E402
    GRANDFATHERED_LEGACY_IDS,
    ManifestError,
    load_manifest,
    resolve_app,
    sha256_file,
)


ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", "..", ".."))
MANIFEST = os.path.join(ROOT, "tools", "compat_harness", "manifests", "apps.json")


def _app(**kwargs):
    base = {
        "id": "sample-app",
        "sha256": "a" * 64,
        "fixtureRelPath": "ipas/sample.ipa",
        "optional": False,
    }
    base.update(kwargs)
    return base


class ManifestTests(unittest.TestCase):
    def test_checked_in_manifest_loads_and_grandfathers_only_three(self):
        data = load_manifest(MANIFEST)
        ids = [app["id"] for app in data["apps"]]
        self.assertEqual(ids[:3], ["angry-birds", "tiny-tower", "art-of-glow"])
        legacy = {app["id"] for app in data["apps"] if app.get("legacyRepoPath")}
        self.assertEqual(legacy, set(GRANDFATHERED_LEGACY_IDS))
        clash = next(app for app in data["apps"] if app["id"] == "clash-royale-2-1306-1")
        self.assertTrue(clash["optional"])
        self.assertNotIn("legacyRepoPath", clash)

    def test_rejects_new_legacy_repo_path(self):
        path = self._write({"schemaVersion": 1, "apps": [_app(legacyRepoPath="new.ipa")]})
        with self.assertRaises(ManifestError):
            load_manifest(path)

    def test_rejects_path_escape(self):
        path = self._write({"schemaVersion": 1, "apps": [_app(fixtureRelPath="../secret.ipa")]})
        with self.assertRaises(ManifestError):
            load_manifest(path)

    def test_hash_mismatch_is_an_error(self):
        with tempfile.TemporaryDirectory() as tmp:
            ipa = os.path.join(tmp, "ipas")
            os.makedirs(ipa)
            target = os.path.join(ipa, "sample.ipa")
            with open(target, "wb") as f:
                f.write(b"not-the-app")
            resolved = resolve_app(_app(), repo=ROOT, fixture_root=tmp, allow_legacy=True)
        self.assertEqual(resolved["status"], "error")
        self.assertIn("sha256", resolved["reason"])
        self.assertNotEqual(resolved["actualSha256"], "a" * 64)

    def test_fixture_root_does_not_fall_back_to_legacy(self):
        data = load_manifest(MANIFEST)
        angry = next(app for app in data["apps"] if app["id"] == "angry-birds")
        with tempfile.TemporaryDirectory() as tmp:
            resolved = resolve_app(angry, repo=ROOT, fixture_root=tmp, allow_legacy=True)
        self.assertEqual(resolved["status"], "skipped")
        self.assertIsNone(resolved["source"])

    def test_legacy_hash_matches_grandfathered_file(self):
        data = load_manifest(MANIFEST)
        angry = next(app for app in data["apps"] if app["id"] == "angry-birds")
        ipa = os.path.join(ROOT, angry["legacyRepoPath"])
        if not os.path.isfile(ipa):
            self.skipTest("grandfathered IPA is not in this working tree")
        self.assertEqual(sha256_file(ipa), angry["sha256"])
        resolved = resolve_app(angry, repo=ROOT, fixture_root=None, allow_legacy=True)
        self.assertEqual(resolved["status"], "resolved")
        self.assertEqual(resolved["source"], "legacy-repo")

    def test_legacy_can_be_disabled(self):
        data = load_manifest(MANIFEST)
        angry = next(app for app in data["apps"] if app["id"] == "angry-birds")
        resolved = resolve_app(angry, repo=ROOT, fixture_root=None, allow_legacy=False)
        self.assertEqual(resolved["status"], "skipped")

    def test_fixture_hit_uses_external_file(self):
        with tempfile.TemporaryDirectory() as tmp:
            os.makedirs(os.path.join(tmp, "ipas"))
            target = os.path.join(tmp, "ipas", "sample.ipa")
            payload = b"authorized-fixture"
            with open(target, "wb") as f:
                f.write(payload)
            digest = hashlib.sha256(payload).hexdigest()
            resolved = resolve_app(
                _app(sha256=digest), repo=ROOT, fixture_root=tmp, allow_legacy=False
            )
        self.assertEqual(resolved["status"], "resolved")
        self.assertEqual(resolved["source"], "fixture-root")
        self.assertEqual(resolved["displayPath"], "ipas/sample.ipa")

    def _write(self, data) -> str:
        fd, path = tempfile.mkstemp(suffix=".json")
        with os.fdopen(fd, "w", encoding="utf-8") as f:
            json.dump(data, f)
        self.addCleanup(lambda: os.path.exists(path) and os.remove(path))
        return path


if __name__ == "__main__":
    unittest.main()
