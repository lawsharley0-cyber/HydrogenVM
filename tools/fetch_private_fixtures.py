#!/usr/bin/env python3
"""Stage the three historical compatibility IPAs from a restricted Drive folder.

The caller supplies an OAuth access token from a trusted identity provider; this
tool does not mint credentials, discover a public link, or print secrets/IDs.
It intentionally has no legacy-repository fallback.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path
import tempfile
from urllib.parse import quote, urlencode
from urllib.request import HTTPRedirectHandler, Request, build_opener

from compat_harness.manifest import load_manifest, default_manifest_path


REQUIRED_IDS = ("angry-birds", "tiny-tower", "art-of-glow")
API = "https://www.googleapis.com/drive/v3/files"
MAX_BYTES = 256 * 1024 * 1024


class FixtureFetchError(Exception):
    """A private integration precondition or integrity check failed."""


class RejectRedirects(HTTPRedirectHandler):
    """Reject all 30x responses, including cross-origin bearer-token leaks."""

    def redirect_request(self, request, fp, code, msg, headers, newurl):
        return None


def _request(url: str, token: str):
    # In urllib's default handler, even cross-origin redirects inherit the
    # Authorization header. A 30x is an error; we never follow it.
    return build_opener(RejectRedirects()).open(
        Request(url, headers={"Authorization": f"Bearer {token}"}), timeout=60
    )


def _new_private_root(root: Path) -> None:
    """Require a fresh 0700 directory under an already existing parent.

    Trusted CI supplies an isolated, single-writer parent outside checkout.
    A new private root prevents a second OS user from racing file publication.
    We also reject symlinks in *every* pre-existing ancestor.
    """
    absolute = root.absolute()
    if root.exists() or root.is_symlink() or not absolute.parent.is_dir():
        raise FixtureFetchError("fixture root must be a new directory")
    for ancestor in (absolute, *absolute.parents):
        if ancestor.is_symlink():
            raise FixtureFetchError("symlinked fixture-root ancestor is forbidden")
    try:
        root.mkdir(mode=0o700)
        (root / "ipas").mkdir(mode=0o700)
    except OSError as exc:
        raise FixtureFetchError("could not create private fixture root") from exc


def stage(root: Path, folder_id: str, token: str, manifest_path: str) -> list[str]:
    if not folder_id or not token:
        raise FixtureFetchError("private fixture identity or folder configuration is absent")
    manifest = load_manifest(manifest_path)
    apps = {app["id"]: app for app in manifest["apps"]}
    if any(ident not in apps for ident in REQUIRED_IDS):
        raise FixtureFetchError("required fixture is missing from the manifest")

    escaped_folder = folder_id.replace("\\", "\\\\").replace("'", "\\'")
    query = urlencode({
        "q": f"'{escaped_folder}' in parents and trashed = false",
        "fields": "nextPageToken,files(id,name,mimeType,size)",
        "page_size": "1000",
    })
    try:
        with _request(f"{API}?{query}", token) as response:
            listing = json.load(response)
    except Exception as exc:
        raise FixtureFetchError("private Drive fixture listing failed") from exc
    if listing.get("nextPageToken"):
        raise FixtureFetchError("fixture folder listing is incomplete")
    files = listing.get("files")
    if not isinstance(files, list):
        raise FixtureFetchError("invalid private fixture listing")

    names = {Path(apps[ident]["fixtureRelPath"]).name for ident in REQUIRED_IDS}
    matched: dict[str, dict] = {}
    for item in files:
        if not isinstance(item, dict) or item.get("name") not in names:
            continue
        name = item["name"]
        if name in matched:
            raise FixtureFetchError("duplicate private fixture name")
        if item.get("mimeType") == "application/vnd.google-apps.folder" or not item.get("id"):
            raise FixtureFetchError("invalid private fixture entry")
        matched[name] = item
    if set(matched) != names:
        raise FixtureFetchError("one or more required private fixtures are unavailable")

    _new_private_root(root)
    staged = []
    published = []
    try:
      for ident in REQUIRED_IDS:
          app = apps[ident]
          relative = Path(app["fixtureRelPath"])
          if relative.parts[0] != "ipas" or len(relative.parts) != 2:
              raise FixtureFetchError("unexpected fixture destination in manifest")
          dest = root / relative
          if dest.parent.is_symlink() or dest.is_symlink() or dest.exists():
              raise FixtureFetchError("fixture destination must be empty")
          dest.parent.mkdir(parents=True, exist_ok=True)
          digest = hashlib.sha256()
          count = 0
          temporary = None
          try:
              with tempfile.NamedTemporaryFile(dir=dest.parent, prefix=".fixture-", delete=False) as out:
                  temporary = Path(out.name)
                  file_id = quote(matched[dest.name]["id"], safe="")
                  with _request(f"{API}/{file_id}?alt=media", token) as response:
                      while chunk := response.read(1024 * 1024):
                          count += len(chunk)
                          if count > MAX_BYTES:
                              raise FixtureFetchError("fixture exceeds allowed size")
                          digest.update(chunk)
                          out.write(chunk)
              if digest.hexdigest() != app["sha256"]:
                  raise FixtureFetchError("fixture SHA-256 mismatch; refusing incompatible bytes")
              # Atomic no-overwrite publication: link() fails if another
              # same-user writer has installed a file after our check.
              os.link(temporary, dest)
              published.append(dest)
              staged.append(ident)
          except Exception as exc:
              if isinstance(exc, FixtureFetchError):
                  raise
              raise FixtureFetchError("private fixture download failed") from exc
          finally:
              if temporary is not None:
                  temporary.unlink(missing_ok=True)
    except Exception:
        # Never leave a partially complete fixture root that another process
        # could mistake for a successfully retrieved integration dataset.
        for dest in published:
            dest.unlink(missing_ok=True)
        raise
    return staged


def main() -> int:
    parser = argparse.ArgumentParser(description="Stage SHA-256-checked private compatibility fixtures")
    parser.add_argument("--root", required=True, type=Path)
    parser.add_argument("--manifest", default=default_manifest_path())
    args = parser.parse_args()
    try:
        staged = stage(args.root, os.environ.get("HVM_DRIVE_FOLDER_ID", ""),
                       os.environ.get("HVM_DRIVE_ACCESS_TOKEN", ""), args.manifest)
    except (FixtureFetchError, ValueError, OSError) as exc:
        # Never print the underlying HTTP exception: it may contain a private ID.
        print(f"ERROR: private fixture staging failed ({type(exc).__name__})")
        return 1
    print(f"Verified {len(staged)} private fixtures; HVM_FIXTURE_ROOT is ready")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
