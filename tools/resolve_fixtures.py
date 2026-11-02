#!/usr/bin/env python3
"""Print the verified path of private fixtures named in the compat manifest (#204).

    HVM_FIXTURE_ROOT=/path/to/store tools/resolve_fixtures.py angry-birds tiny-tower

One path per line, in the order asked. Exit 0 only when every id is found under the
fixture root AND its SHA-256 matches tools/compat_harness/manifests/apps.json. Otherwise
nothing is printed on stdout, every problem goes to stderr, and the exit status is 2.

It never reads an IPA from the repository (the three grandfathered files included),
never falls back to another directory, and never downloads. Scripts that need a real
game use this instead of a hard-coded path.
"""
from __future__ import annotations

import argparse
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from compat_harness.manifest import default_manifest_path, load_manifest, repo_root, resolve_app  # noqa: E402


def resolve(ids, *, fixture_root, manifest_path=None):
    """Return (paths, problems). Either every id resolves and problems is empty, or paths is empty."""
    if not fixture_root:
        return [], ["HVM_FIXTURE_ROOT is not set (or pass --fixture-root); there is no fallback to repository copies"]
    manifest = load_manifest(manifest_path or default_manifest_path())
    by_id = {app["id"]: app for app in manifest["apps"]}
    paths, problems = [], []
    for ident in ids:
        app = by_id.get(ident)
        if app is None:
            problems.append(f"{ident}: not in the manifest")
            continue
        result = resolve_app(app, repo=repo_root(), fixture_root=fixture_root, allow_legacy=False)
        if result["status"] == "resolved":
            paths.append(result["path"])
        else:
            problems.append(f"{ident}: {result['reason']}")
    return ([], problems) if problems else (paths, [])


def main(argv=None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("ids", nargs="+", help="manifest app ids, e.g. angry-birds")
    parser.add_argument("--fixture-root", default=os.environ.get("HVM_FIXTURE_ROOT"),
                        help="private store directory (default: $HVM_FIXTURE_ROOT)")
    parser.add_argument("--manifest", default=None, help="manifest path (default: the committed apps.json)")
    args = parser.parse_args(argv)
    paths, problems = resolve(args.ids, fixture_root=args.fixture_root, manifest_path=args.manifest)
    if problems:
        for problem in problems:
            print(f"resolve_fixtures: {problem}", file=sys.stderr)
        return 2
    for path in paths:
        print(path)
    return 0


if __name__ == "__main__":
    sys.exit(main())
