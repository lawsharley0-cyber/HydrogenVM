"""Resolve authorized app fixtures by hash.

Binaries are never fetched and never written into the repository. A fixture
root (``HVM_FIXTURE_ROOT`` or ``--fixture-root``) is the supported location.
Three IPAs already committed on main are grandfathered: the harness will use
those paths only when legacy mode is on and the bytes match the manifest.
New ids cannot opt into a repository path.
"""
from __future__ import annotations

import hashlib
import os
import re

GRANDFATHERED_LEGACY_IDS = frozenset({"angry-birds", "tiny-tower", "art-of-glow"})
SHA256_RE = re.compile(r"^[0-9a-f]{64}$")


class ManifestError(Exception):
    pass


def package_dir() -> str:
    return os.path.dirname(os.path.abspath(__file__))


def default_manifest_path() -> str:
    return os.path.join(package_dir(), "manifests", "apps.json")


def repo_root() -> str:
    return os.path.abspath(os.path.join(package_dir(), "..", ".."))


def load_manifest(path: str, *, data: dict | None = None) -> dict:
    import json
    if data is None:
        with open(path, encoding="utf-8") as f:
            data = json.load(f)
    if not isinstance(data, dict):
        raise ManifestError("manifest must be a JSON object")
    if data.get("schemaVersion") != 1:
        raise ManifestError("manifest schemaVersion must be 1")
    apps = data.get("apps")
    if not isinstance(apps, list) or not apps:
        raise ManifestError("manifest apps must be a non-empty list")
    seen = set()
    for app in apps:
        if not isinstance(app, dict):
            raise ManifestError("each app must be an object")
        ident = app.get("id")
        if not isinstance(ident, str) or not re.fullmatch(r"[a-z0-9][a-z0-9-]{0,63}", ident):
            raise ManifestError(f"invalid app id: {ident!r}")
        if ident in seen:
            raise ManifestError(f"duplicate app id: {ident}")
        seen.add(ident)
        digest = str(app.get("sha256", "")).lower()
        if not SHA256_RE.fullmatch(digest):
            raise ManifestError(f"{ident}: sha256 must be 64 lowercase hex characters")
        app["sha256"] = digest
        rel = app.get("fixtureRelPath")
        if not _safe_rel(rel):
            raise ManifestError(f"{ident}: fixtureRelPath must be a relative path without '..'")
        legacy = app.get("legacyRepoPath")
        if legacy is not None and not _safe_rel(legacy):
            raise ManifestError(f"{ident}: legacyRepoPath must be a relative path without '..'")
        if legacy and ident not in GRANDFATHERED_LEGACY_IDS:
            raise ManifestError(
                f"{ident}: legacyRepoPath is frozen to the IPAs already on main; "
                "put new fixtures in HVM_FIXTURE_ROOT"
            )
        if "optional" in app and not isinstance(app["optional"], bool):
            raise ManifestError(f"{ident}: optional must be a boolean")
    return data


def legacy_allowed(flag: bool | None) -> bool:
    """None follows HVM_ALLOW_LEGACY_IPAS (default on for the grandfathered IPAs)."""
    if flag is not None:
        return flag
    raw = os.environ.get("HVM_ALLOW_LEGACY_IPAS", "1").strip().lower()
    return raw not in ("0", "false", "no", "off")


def resolve_app(app: dict, *, repo: str, fixture_root: str | None, allow_legacy: bool) -> dict:
    ident = app["id"]
    expected = app["sha256"]
    base = {
        "id": ident,
        "status": "skipped",
        "source": None,
        "path": None,
        "displayPath": None,
        "sha256": expected,
        "actualSha256": None,
        "reason": "",
        "optional": bool(app.get("optional")),
    }
    if fixture_root:
        root = os.path.realpath(fixture_root)
        if not os.path.isdir(root):
            base["status"] = "error"
            base["reason"] = f"fixture root is not a directory: {root}"
            return base
        candidate = os.path.realpath(os.path.join(root, app["fixtureRelPath"]))
        if not _contained(root, candidate):
            base["status"] = "error"
            base["reason"] = "fixture path escapes the fixture root"
            return base
        if not os.path.isfile(candidate):
            base["reason"] = (
                "not in fixture root (legacy repo copies are not used once "
                "HVM_FIXTURE_ROOT or --fixture-root is set)"
            )
            return base
        return _hash_result(base, candidate, expected, "fixture-root", app["fixtureRelPath"])

    legacy = app.get("legacyRepoPath")
    if legacy and allow_legacy:
        if app["id"] not in GRANDFATHERED_LEGACY_IDS:
            base["status"] = "error"
            base["reason"] = "legacy repo path refused for a non-grandfathered id"
            return base
        candidate = os.path.realpath(os.path.join(repo, legacy))
        if not _contained(os.path.realpath(repo), candidate):
            base["status"] = "error"
            base["reason"] = "legacy path escapes the repository"
            return base
        if not os.path.isfile(candidate):
            base["reason"] = "grandfathered IPA is not in the working tree"
            return base
        return _hash_result(base, candidate, expected, "legacy-repo", legacy)

    if legacy and not allow_legacy:
        base["reason"] = "legacy in-repo IPAs are disabled (HVM_ALLOW_LEGACY_IPAS=0)"
        return base
    base["reason"] = "fixture not present; set HVM_FIXTURE_ROOT or --fixture-root"
    return base


def _hash_result(base: dict, path: str, expected: str, source: str, display: str) -> dict:
    actual = sha256_file(path)
    base["actualSha256"] = actual
    base["path"] = path
    base["displayPath"] = display
    if actual != expected:
        base["status"] = "error"
        base["source"] = source
        base["reason"] = "sha256 does not match the manifest; refusing to run this file"
        return base
    base["status"] = "resolved"
    base["source"] = source
    base["reason"] = ""
    return base


def sha256_file(path: str) -> str:
    digest = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            digest.update(chunk)
    return digest.hexdigest()


def _safe_rel(rel: object) -> bool:
    if not isinstance(rel, str) or not rel or os.path.isabs(rel):
        return False
    parts = rel.replace("\\", "/").split("/")
    return ".." not in parts and not any(p == "" for p in parts)


def _contained(root: str, path: str) -> bool:
    try:
        return os.path.commonpath([root, path]) == root
    except ValueError:
        return False
