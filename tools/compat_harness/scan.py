"""Legacy scan command adapter; all app execution belongs to runner.py."""
import argparse
import hashlib
import json
from pathlib import Path
import re
import tempfile
from .cli import main as harness_main


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('command', choices=['scan'])
    parser.add_argument('inputs', nargs='+', type=Path)
    parser.add_argument('--out', default='compat')
    parser.add_argument('--seconds', type=float, default=30)
    parser.add_argument('--hvm', default='build/hvm')
    parser.add_argument('--only')
    parser.add_argument('--jobs', type=int, default=1, help='accepted for compatibility; runs sequentially')
    args = parser.parse_args(argv)
    files = set()
    for path in args.inputs:
        if path.is_dir():
            files.update(path.rglob('*.ipa'))
        elif path.suffix.lower() == '.ipa':
            files.add(path)
        else:
            parser.error('scan accepts IPA files or directories of IPAs; use a manifest for other fixtures')
    if not files:
        parser.error('no IPA fixtures found')
    # Resolve existing files against their common parent; nothing is copied or unpacked here.
    import os
    root = Path(os.path.commonpath([str(path.resolve().parent) for path in files]))
    apps = []
    for path in sorted(files):
        digest = hashlib.sha256(path.read_bytes()).hexdigest()
        ident = re.sub('[^a-z0-9-]', '-', path.stem.lower())[:50].strip('-') or 'app'
        apps.append(dict(id=ident + '-' + digest[:8], sha256=digest,
                         fixtureRelPath=str(path.resolve().relative_to(root)), seconds=args.seconds))
    with tempfile.TemporaryDirectory() as directory:
        manifest = Path(directory) / 'manifest.json'
        manifest.write_text(json.dumps(dict(schemaVersion=1, apps=apps)))
        command = ['apps', '--manifest', str(manifest), '--fixture-root', str(root), '--out', args.out,
                   '--seconds', str(args.seconds), '--hvm', args.hvm, '--keep-going']
        if args.only:
            selected = [app['id'] for app in apps if app['id'] in args.only.split(',') or Path(app['fixtureRelPath']).stem in args.only.split(',')]
            if not selected: parser.error('--only did not match any input IPA')
            command += ['--only', ','.join(selected)]
        return harness_main(command)
