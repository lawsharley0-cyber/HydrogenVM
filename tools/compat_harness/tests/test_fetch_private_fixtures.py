"""Synthetic-only checks: no Google credentials or commercial bytes required."""
import hashlib
import io
import json
import os
from pathlib import Path
import sys
import tempfile
import unittest
from unittest.mock import patch
from urllib.error import HTTPError, URLError

sys.path.insert(0, str(Path(__file__).resolve().parents[2]))

from fetch_private_fixtures import FixtureFetchError, RejectRedirects, stage  # noqa: E402
from urllib.request import Request


NAMES = {
    "angry-birds": "AngryBirds_v1.0_os30.ipa",
    "tiny-tower": "Tiny_Tower_v1.0_os30.ipa",
    "art-of-glow": "Art_Of_Glow_v1.1.2_os313.ipa",
}


class Response(io.BytesIO):
    def __enter__(self):
        return self

    def __exit__(self, *_):
        self.close()


class PrivateFixtureTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.root = Path(self.tmp.name)
        self.contents = {name: (ident + "-bytes").encode() for ident, name in NAMES.items()}
        manifest = {
            "schemaVersion": 1,
            "apps": [{
                "id": ident,
                "sha256": hashlib.sha256(self.contents[name]).hexdigest(),
                "fixtureRelPath": f"ipas/{name}",
            } for ident, name in NAMES.items()],
        }
        self.manifest = self.root / "apps.json"
        self.manifest.write_text(json.dumps(manifest), encoding="utf-8")

    def _serve(self, *, omit=None, duplicate=None, corrupt=None):
        files = [{"id": f"test-{i}", "name": name, "mimeType": "application/octet-stream"}
                 for i, name in enumerate(NAMES.values()) if name != omit]
        if duplicate:
            files.append({"id": "other", "name": duplicate})

        def responder(url, token):
            self.assertEqual(token, "synthetic-token")
            if "?alt=media" not in url:
                return Response(json.dumps({"files": files}).encode())
            file_id = url.split("/")[-1].split("?")[0]
            entry = next(item for item in files if item["id"] == file_id)
            payload = self.contents[entry["name"]]
            return Response(b"tampered" if entry["name"] == corrupt else payload)

        return patch("fetch_private_fixtures._request", side_effect=responder)

    def test_all_three_verified_and_staged_under_fixture_root(self):
        with self._serve():
            result = stage(self.root / "fixtures", "private-folder", "synthetic-token", str(self.manifest))
        self.assertEqual(result, list(NAMES))
        for name in NAMES.values():
            self.assertEqual((self.root / "fixtures" / "ipas" / name).read_bytes(), self.contents[name])

    def test_missing_fixture_is_failure_not_successful_skip(self):
        with self._serve(omit=NAMES["tiny-tower"]):
            with self.assertRaisesRegex(FixtureFetchError, "unavailable"):
                stage(self.root / "fixtures", "private-folder", "synthetic-token", str(self.manifest))

    def test_duplicate_name_is_refused(self):
        with self._serve(duplicate=NAMES["art-of-glow"]):
            with self.assertRaisesRegex(FixtureFetchError, "duplicate"):
                stage(self.root / "fixtures", "private-folder", "synthetic-token", str(self.manifest))

    def test_bad_hash_is_never_published(self):
        name = NAMES["angry-birds"]
        with self._serve(corrupt=name):
            with self.assertRaisesRegex(FixtureFetchError, "SHA-256 mismatch"):
                stage(self.root / "fixtures", "private-folder", "synthetic-token", str(self.manifest))
        self.assertFalse((self.root / "fixtures" / "ipas" / name).exists())
        self.assertEqual(list((self.root / "fixtures" / "ipas").glob(".fixture-*")), [])

    def test_late_integrity_failure_rolls_back_earlier_files(self):
        with self._serve(corrupt=NAMES["art-of-glow"]):
            with self.assertRaisesRegex(FixtureFetchError, "SHA-256 mismatch"):
                stage(self.root / "fixtures", "private-folder", "synthetic-token", str(self.manifest))
        self.assertEqual(list((self.root / "fixtures" / "ipas").iterdir()), [])

    def test_no_token_refuses_before_network(self):
        with patch("fetch_private_fixtures._request") as request:
            with self.assertRaisesRegex(FixtureFetchError, "configuration"):
                stage(self.root / "fixtures", "private-folder", "", str(self.manifest))
        request.assert_not_called()

    def test_http_error_is_generic_and_does_not_expose_private_id(self):
        with patch("fetch_private_fixtures._request", side_effect=HTTPError(
            "https://private.example/secret-id", 403, "Forbidden", {}, None
        )):
            with self.assertRaises(FixtureFetchError) as caught:
                stage(self.root / "fixtures", "private-folder", "synthetic-token", str(self.manifest))
        self.assertNotIn("secret-id", str(caught.exception))

    def test_timeout_is_generic(self):
        with patch("fetch_private_fixtures._request", side_effect=URLError("timed out")):
            with self.assertRaisesRegex(FixtureFetchError, "listing failed"):
                stage(self.root / "fixtures", "private-folder", "synthetic-token", str(self.manifest))

    def test_size_limit_rejects_and_cleans_temporary_file(self):
        with self._serve(), patch("fetch_private_fixtures.MAX_BYTES", 3):
            with self.assertRaisesRegex(FixtureFetchError, "exceeds allowed size"):
                stage(self.root / "fixtures", "private-folder", "synthetic-token", str(self.manifest))
        self.assertEqual(list((self.root / "fixtures" / "ipas").iterdir()), [])

    def test_atomic_publication_does_not_overwrite_racing_file(self):
        import fetch_private_fixtures as downloader
        real_link = os.link

        def race_link(src, dest):
            Path(dest).write_bytes(b"another-writer")
            return real_link(src, dest)

        with self._serve(), patch.object(downloader.os, "link", side_effect=race_link):
            with self.assertRaisesRegex(FixtureFetchError, "download failed"):
                stage(self.root / "fixtures", "private-folder", "synthetic-token", str(self.manifest))
        self.assertEqual((self.root / "fixtures" / "ipas" / NAMES["angry-birds"]).read_bytes(),
                         b"another-writer")

    def test_symlink_destination_refused(self):
        outside = self.root / "outside"
        outside.mkdir()
        fixtures = self.root / "fixtures"
        fixtures.symlink_to(outside, target_is_directory=True)
        with self._serve():
            with self.assertRaises(FixtureFetchError):
                stage(fixtures, "private-folder", "synthetic-token", str(self.manifest))
        self.assertEqual(list(outside.iterdir()), [])

    def test_symlinked_ancestor_is_refused(self):
        outside = self.root / "outside"
        outside.mkdir()
        (self.root / "linked").symlink_to(outside, target_is_directory=True)
        with self._serve():
            with self.assertRaises(FixtureFetchError):
                stage(self.root / "linked" / "fixtures", "private-folder", "synthetic-token", str(self.manifest))
        self.assertEqual(list(outside.iterdir()), [])

    def test_302_to_other_origin_never_transmits_bearer(self):
        # Model urllib's actual 302 dispatch without opening a real socket.
        # The standard handler would return a Request retaining Authorization.
        original = Request("https://www.googleapis.com/drive/v3/files", headers={
            "Authorization": "Bearer synthetic-token"
        })
        forwarded = RejectRedirects().redirect_request(
            original, None, 302, "Found", {}, "https://attacker.example/leak"
        )
        self.assertIsNone(forwarded, "redirect must not create a token-bearing cross-origin request")


if __name__ == "__main__":
    unittest.main()
