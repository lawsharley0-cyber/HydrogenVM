"""Convert the hash-indexed owner library to the existing fixture manifest."""
import json
from pathlib import Path
from .manifest import load_manifest, default_manifest_path, ManifestError


def load_library(path, fixture_root=None):
    data = json.loads(Path(path).read_text())
    legacy = {app['id']: app for app in load_manifest(default_manifest_path())['apps']}
    if not isinstance(data, dict) or not isinstance(data.get('apps'), list) or not data['apps']:
        raise ManifestError('library apps must be a non-empty list')
    apps = []
    for entry in data['apps']:
        if not isinstance(entry, dict) or not isinstance(entry.get('file'), str):
            raise ManifestError('each library app needs a file basename')
        filename = entry['file']
        if Path(filename).name != filename or '\\' in filename or filename in ('.', '..'):
            raise ManifestError('library file must be a basename')
        app = dict(entry, fixtureRelPath=filename, optional=True, seconds=30)
        if entry.get('id') in legacy and legacy[entry['id']]['sha256'] == entry.get('sha256'):
            app['legacyRepoPath'] = legacy[entry['id']].get('legacyRepoPath')
        apps.append(app)
    manifest = load_manifest(path, data={'schemaVersion': 1, 'apps': apps})
    if fixture_root and Path(fixture_root).is_dir():
        from .manifest import sha256_file
        root = Path(fixture_root).resolve()
        by_hash = {}
        for candidate in sorted(root.rglob('*.ipa')):
            real = candidate.resolve()
            if real.is_file() and real.is_relative_to(root):
                by_hash.setdefault(sha256_file(str(real)), str(real.relative_to(root)))
        for app in apps:
            app['fixtureRelPath'] = by_hash.get(app['sha256'], app['fixtureRelPath'])
    return manifest
