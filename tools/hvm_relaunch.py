#!/usr/bin/env python3
"""Record repeated launches and hash-only home diffs without resetting state.

Use a private --home and --out outside the repository for real games. Separate
processes provide a snapshot after each launch. In-process mode records only
the whole batch's before/after state; it never invents intermediate snapshots.
"""
import argparse
from datetime import datetime, timezone
import hashlib
import json
import math
import os
from pathlib import Path
import platform
import re
import subprocess


def file_sha256(path):
    digest = hashlib.sha256()
    with path.open('rb') as stream:
        while block := stream.read(1024 * 1024):
            digest.update(block)
    return digest.hexdigest()


def snapshot_home(home):
    home = Path(home)
    if not home.is_dir():
        raise ValueError("Snapshot requires an existing home directory")
    def fail_walk(error):
        raise error
    result = {}
    for parent, dirs, files in os.walk(home, followlinks=False, onerror=fail_walk):
        for name in sorted(dirs + files):
            path = Path(parent) / name
            relative = path.relative_to(home).as_posix()
            if path.is_symlink():
                result[relative] = {'kind': 'symlink', 'target': os.readlink(path)}
            elif path.is_dir():
                result[relative] = {'kind': 'directory'}
            elif path.is_file():
                result[relative] = {'kind': 'file', 'size': path.stat().st_size, 'sha256': file_sha256(path)}
            else:
                raise ValueError(f'Unsupported file in home: {relative}')
    return dict(sorted(result.items()))


def diff_snapshots(before, after):
    return {'added': sorted(after.keys() - before.keys()),
            'removed': sorted(before.keys() - after.keys()),
            'changed': sorted(k for k in before.keys() & after.keys() if before[k] != after[k])}


def write_json(path, value):
    temporary = path.with_suffix(path.suffix + '.tmp')
    temporary.write_text(json.dumps(value, indent=2, sort_keys=True) + '\n', encoding='utf-8')
    temporary.replace(path)


def run_relaunch(hvm, app, home, out, launches=3, seconds=20, mode='separate', timeout=None, extra_args=()):
    hvm, app, home, out = (Path(p).resolve() for p in (hvm, app, home, out))
    if home == out or home in out.parents or out in home.parents:
        raise ValueError('Home and evidence directories must not overlap')
    if mode not in ('separate', 'in-process') or not 1 <= launches <= 20:
        raise ValueError('Use separate/in-process mode and 1..20 launches')
    if not math.isfinite(seconds) or seconds <= 0:
        raise ValueError('seconds must be finite and positive')
    if not home.is_dir():
        raise ValueError('Preserved capture requires an existing home directory')
    if os.name == 'posix' and out.exists() and out.stat().st_mode & 0o077:
        raise ValueError('Evidence directory must not be accessible by group or other users')
    if out.exists() and any(out.iterdir()):
        raise ValueError('Evidence directory must be empty; previous evidence is preserved')
    if not app.exists():
        raise ValueError('The app or IPA does not exist')
    out.mkdir(parents=True, exist_ok=True, mode=0o700)
    if app.is_file():
        fingerprint = {'kind': 'file', 'sha256': file_sha256(app)}
    else:
        files = snapshot_home(app)
        fingerprint = {'kind': 'directory', 'treeSha256': hashlib.sha256(
            json.dumps(files, sort_keys=True, separators=(',', ':')).encode()).hexdigest()}
    result = {'schemaVersion': 1, 'kind': 'hvm-relaunch-evidence',
              'startedAtUtc': datetime.now(timezone.utc).isoformat(),
              'host': {'system': platform.system(), 'pageSize': os.sysconf('SC_PAGESIZE') if hasattr(os, 'sysconf') else None},
              'hvm': str(hvm), 'hvmSha256': file_sha256(hvm),
              'app': str(app), 'appFingerprint': fingerprint,
              'home': str(home), 'mode': mode, 'requestedLaunches': launches,
              'intermediateSnapshotsAvailable': mode == 'separate', 'runs': []}
    def capture_home(stage):
        try:
            return snapshot_home(home)
        except (OSError, ValueError) as error:
            result['captureComplete'] = False
            result['captureError'] = {'stage': stage, 'message': str(error)}
            write_json(out / 'relaunch.json', result)
            raise
    before = capture_home('before first launch')
    write_json(out / 'home-0.json', before)
    count = launches if mode == 'separate' else 1
    for index in range(1, count + 1):
        report_path = out / f'launch-{index}.json'
        command = [str(hvm), 'run', str(app), '--home', str(home), '--run-for', str(seconds),
                   '--graceful', '--report', str(report_path), '--screenshot', str(out / f'launch-{index}.png')]
        batch = launches if mode == 'in-process' else 1
        if mode == 'in-process':
            command += ['--launches', str(launches)]
        command += list(extra_args)
        row = {'index': index, 'command': command, 'returnCode': None, 'timedOut': False}
        with (out / f'launch-{index}.log').open('wb') as log:
            try:
                completed = subprocess.run(command, stdout=log, stderr=subprocess.STDOUT,
                                           timeout=timeout if timeout is not None else seconds * batch + 30)
                row['returnCode'] = completed.returncode
            except subprocess.TimeoutExpired:
                row['timedOut'] = True
            except OSError as error:
                row['toolError'] = str(error)
        try:
            after = capture_home(f'after launch {index}')
        except (OSError, ValueError):
            result['runs'].append(row)
            write_json(out / 'relaunch.json', result)
            raise
        write_json(out / f'home-{index}.json', after)
        row['homeDiff'] = diff_snapshots(before, after)
        try:
            if report_path.stat().st_size > 8 * 1024 * 1024:
                raise ValueError('Report exceeds 8 MB')
            report = json.loads(report_path.read_text(encoding='utf-8'))
            if not isinstance(report, dict) or type(report.get('exitStatus')) is not int or type(report.get('crashed')) is not bool:
                raise ValueError('Missing structured exitStatus/crashed fields')
            row['report'] = report
        except (OSError, ValueError) as error:
            row['reportError'] = str(error)
        if mode == 'in-process':
            text = (out / f'launch-{index}.log').read_text(encoding='utf-8', errors='replace')
            banners = re.findall(r'\[hvm\] ---- launch (\d+) of (\d+) ----', text)
            row['attemptedLaunches'] = max((int(n) for n, total in banners if int(total) == launches), default=0)
            row['allRequestedLaunchesAttempted'] = row['attemptedLaunches'] == launches
        result['runs'].append(row)
        before = after
        # Preserve partial results even when a later launch crashes or times out.
        write_json(out / 'relaunch.json', result)
    result['captureComplete'] = True
    result['allProcessesExitedZero'] = all(r['returnCode'] == 0 for r in result['runs'])
    result['reportsComplete'] = all('report' in r for r in result['runs'])
    result['allRunsExitedWithoutCrash'] = all(r['returnCode'] == 0 and
        r.get('report', {}).get('exitStatus') == 0 and r.get('report', {}).get('crashed') is False and
        r.get('allRequestedLaunchesAttempted', True) for r in result['runs'])
    write_json(out / 'relaunch.json', result)
    return result


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--hvm', type=Path, required=True)
    parser.add_argument('--app', type=Path, required=True)
    parser.add_argument('--home', type=Path, required=True)
    parser.add_argument('--out', type=Path, required=True)
    parser.add_argument('--launches', type=int, default=3)
    parser.add_argument('--seconds', type=float, default=20)
    parser.add_argument('--mode', choices=('separate', 'in-process'), default='separate')
    parser.add_argument('--gpu', action='store_true')
    args = parser.parse_args(argv)
    try:
        result = run_relaunch(args.hvm, args.app, args.home, args.out, args.launches, args.seconds,
                              args.mode, extra_args=['--gpu'] if args.gpu else [])
    except (OSError, ValueError) as error:
        parser.error(str(error))
    print(json.dumps({'evidence': str(args.out / 'relaunch.json'),
                      'returnCodes': [r['returnCode'] for r in result['runs']],
                      'reportsComplete': result['reportsComplete']}))
    if not result['reportsComplete']:
        return 2
    return 0 if result['allRunsExitedWithoutCrash'] else 1


if __name__ == '__main__':
    raise SystemExit(main())
