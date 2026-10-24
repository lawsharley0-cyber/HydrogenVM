#!/usr/bin/env python3
"""Compare host M4A decoding to ffmpeg; never stores input audio in its report."""
import argparse
import array
import hashlib
import json
from pathlib import Path
import re
import subprocess
import tempfile


def command(args):
    result = subprocess.run(args, capture_output=True, text=True)
    if result.returncode:
        raise RuntimeError(result.stderr.strip() or f'command failed: {args[0]}')
    return result.stdout


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--hvm-tests', default='build/hvm_tests')
    parser.add_argument('--assets', type=Path)
    parser.add_argument('--require-assets', type=int, default=0)
    parser.add_argument('--out', type=Path, required=True)
    args = parser.parse_args()
    if args.require_assets and not args.assets:
        parser.error('--require-assets needs --assets')
    report = {'cases': [], 'malformedRejected': 0, 'assetsChecked': 0}
    with tempfile.TemporaryDirectory() as directory:
        root = Path(directory)
        synthetic = []
        # AAC in MP4 (.m4a) and in CAF (Jetpack Joyride's music archive).
        for container in ['m4a', 'caf']:
            for channels, rate in [(1, 22050), (2, 44100)]:
                path = root / f'synthetic-{channels}ch-{rate}.{container}'
                command(['ffmpeg', '-v', 'error', '-f', 'lavfi', '-i', f'sine=frequency=440:sample_rate={rate}:duration=0.15',
                         '-ac', str(channels), '-c:a', 'aac', '-y', str(path)])
                synthetic.append(path)
        assets = sorted(args.assets.rglob('*.m4a')) if args.assets else []
        for index, path in enumerate(synthetic + assets):
            row = {'name': path.name, 'sha256': hashlib.sha256(path.read_bytes()).hexdigest()}
            try:
                probe = json.loads(command(['ffprobe', '-v', 'error', '-select_streams', 'a:0', '-show_entries', 'stream=sample_rate,channels', '-of', 'json', str(path)]))['streams'][0]
                channels, rate = int(probe['channels']), int(probe['sample_rate'])
                actual, reference = root / f'{index}.pcm', root / f'{index}.ref.pcm'
                output = command([args.hvm_tests, 'audio-decode', str(path), str(actual)])
                metadata = re.search(r'rate=(\d+) channels=(\d+) frames=(\d+)', output)
                if not metadata: raise RuntimeError('missing audio-decode frame metadata')
                pan = 'pan=stereo|c0=c0|c1=' + ('c0' if channels == 1 else 'c1')
                command(['ffmpeg', '-v', 'error', '-i', str(path), '-map', '0:a:0', '-af', pan,
                         '-f', 's16le', '-c:a', 'pcm_s16le', '-y', str(reference)])
                a, b = array.array('h'), array.array('h')
                a.frombytes(actual.read_bytes()); b.frombytes(reference.read_bytes())
                if tuple(map(int, metadata.groups()[:2])) != (rate, channels): raise RuntimeError('rate/channel metadata differs from input')
                if len(a) != len(b) or len(a) // 2 != int(metadata.group(3)): raise RuntimeError(f'frame counts differ: hvm={len(a)//2} ffmpeg={len(b)//2}')
                maximum = max((abs(x - y) for x, y in zip(a, b)), default=0)
                if maximum > 1: raise RuntimeError(f'max sample difference {maximum} exceeds 1')
                if channels == 1 and a[::2] != a[1::2]: raise RuntimeError('mono channels differ')
                if not a or not any(a): raise RuntimeError('decoded silence')
                row.update(ok=True, sampleRate=rate, channels=channels, frames=len(a)//2, maxSampleDifference=maximum)
                print('ok', path.name)
            except (RuntimeError, OSError, ValueError, KeyError, IndexError) as error:
                row.update(ok=False, error=str(error))
                print('FAIL', path.name, str(error))
            report['cases'].append(row)
        report['assetsChecked'] = len(assets)
        for index, data in enumerate([b'', b'\x00\x00\x00\x0cftypM4A ', synthetic[0].read_bytes()[:32]]):
            path = root / f'bad-{index}.m4a'
            path.write_bytes(data)
            result = subprocess.run([args.hvm_tests, 'audio-decode', str(path), str(root / 'bad.pcm')], capture_output=True)
            report['malformedRejected'] += int(result.returncode != 0)
    report['ok'] = all(row['ok'] for row in report['cases']) and report['malformedRejected'] == 3 and report['assetsChecked'] >= args.require_assets
    args.out.parent.mkdir(parents=True, exist_ok=True)
    args.out.write_text(json.dumps(report, indent=2) + '\n')
    print(args.out, f"malformedRejected={report['malformedRejected']}")
    return 0 if report['ok'] else 1


if __name__ == '__main__':
    raise SystemExit(main())
