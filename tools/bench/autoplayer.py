#!/usr/bin/env python3
"""Drive hvm's live framebuffer and retain the native recording of every touch.

python3 tools/bench/autoplayer.py game.ipa --hvm build/hvm --out build/game-01
An output directory is private and must be new. Start each guest with a fresh home.
Exit 0 means the bench finished, 70 means a guest crash was reproduced, 1 means
a harness error or unreproduced crash. Inspect step thumbnails for game progress.
"""
import argparse
from dataclasses import dataclass
import hashlib
import json
import math
from pathlib import Path
import random
import struct
import subprocess
import sys
import time


@dataclass
class Frame:
    width: int
    height: int
    scale: float
    thumb: bytes  # RGB, 96 by 64

    @classmethod
    def read(cls, path):
        with path.open('rb') as src:
            header = src.read(16)
            if len(header) != 16:
                raise ValueError('truncated frame')
            magic, w, h, scale = struct.unpack('<4sIIf', header)
            if magic != b'HVF1' or not (0 < w <= 8192 and 0 < h <= 8192 and 0 < scale <= 8):
                raise ValueError('invalid live frame')
            rgba = src.read(w * h * 4 + 1)
            if len(rgba) != w * h * 4:
                raise ValueError('invalid live frame size')
            thumb = bytearray()
            for y in range(64):
                for x in range(96):
                    i = ((y * h // 64) * w + x * w // 96) * 4
                    thumb.extend(rgba[i:i + 3])
            return cls(w, h, scale, bytes(thumb))

    def label_points(self):
        # Count short horizontal intensity transitions within each tile. Words
        # and button outlines score above flat margins and backgrounds.
        gray = [sum(self.thumb[i:i+3]) // 3 for i in range(0, len(self.thumb), 3)]
        scores = []
        for row in range(4):
            for col in range(6):
                edges = sum(abs(gray[y * 96 + x] - gray[y * 96 + x - 1]) > 50
                            for y in range(row * 16, (row + 1) * 16, 2)
                            for x in range(col * 16 + 1, (col + 1) * 16))
                if edges >= 6:
                    scores.append((edges, (col + 0.5) / 6, (row + 0.5) / 4))
        return [(x, y) for _, x, y in sorted(scores, reverse=True)[:4]]


def choose(frame, step, rng):
    buttons = frame.label_points()
    if buttons and step % 3 == 0:
        return [rng.choice(buttons)]
    if step % 5 == 4:
        return [(0.22, 0.72), (0.72, 0.40)]
    if step % 5 == 3:
        return [(rng.uniform(0.05, 0.95), rng.uniform(0.05, 0.95))]
    return [[(0.5, 0.5)], [(0.85, 0.85)], [(0.5, 0.76)]][step % 5]


def gesture(stream, frame, points):
    def send(phase, p):
        x, y = p
        stream.write(f'touch {phase} {x * frame.width / frame.scale:.3f} {y * frame.height / frame.scale:.3f}\n')
        stream.flush()
    start, end = points[0], points[-1]
    send(0, start)
    if len(points) == 2:
        for step in range(1, 6):
            time.sleep(0.035)
            send(1, tuple(a + (b - a) * step / 5 for a, b in zip(start, end)))
    time.sleep(0.08)
    send(3, end)


def load_report(path):
    if path.exists():
        return json.loads(path.read_text())
    return {}


def run(args):
    out = args.out.resolve()
    out.mkdir(mode=0o700, parents=True, exist_ok=False)
    command = [str(args.hvm.resolve()), 'run', str(args.game.resolve()), '--phone', '--gpu',
               '--run-for', str(args.seconds)]
    controls = out / 'controls'
    controls.touch()
    recording = out / 'session.hvmrec'
    report = out / 'record.json'
    frame_path = out / 'live.hvf'
    rng = random.Random(args.seed)
    steps = 0
    deadline = time.monotonic() + args.seconds + 30
    timed_out = False
    with (out / 'guest.log').open('w') as log, controls.open('a') as inputs, (out / 'steps.jsonl').open('w') as journal:
        proc = subprocess.Popen(command + ['--home', str(out / 'home'), '--frame-file', str(frame_path),
                                '--input-file', str(controls), '--record', str(recording),
                                '--report', str(report)], stdout=log, stderr=subprocess.STDOUT)
        try:
            next_input = 0
            while proc.poll() is None:
                now = time.monotonic()
                if now >= deadline:
                    timed_out = True
                    break
                if steps < args.steps and now >= next_input and frame_path.exists():
                    frame = Frame.read(frame_path)
                    points = choose(frame, steps, rng)
                    thumb = out / f'step-{steps:03d}.ppm'
                    thumb.write_bytes(b'P6\n96 64\n255\n' + frame.thumb)
                    journal.write(json.dumps({'step': steps, 'points': points, 'image': thumb.name,
                                              'frameHash': hashlib.sha256(frame.thumb).hexdigest()}) + '\n')
                    journal.flush()
                    gesture(inputs, frame, points)
                    steps += 1
                    next_input = time.monotonic() + args.interval
                time.sleep(0.1)
        finally:
            if proc.poll() is None:
                proc.kill()
            proc.wait()

    recorded = load_report(report)
    complete = recording.is_file() and recording.stat().st_size > 0
    summary = {'seed': args.seed, 'steps': steps, 'exitCode': proc.returncode, 'timeout': timed_out,
               'recorded': complete, 'crashed': recorded.get('crashed'), 'signature': recorded.get('signature'),
               'crashReproduced': None}
    rc = 0 if proc.returncode == 0 and complete and steps and recorded and not timed_out else 1
    if recorded.get('crashed') and complete:
        retry_report = out / 'replay.json'
        with (out / 'replay.log').open('w') as log:
            try:
                result = subprocess.run(command + ['--home', str(out / 'replay-home'), '--replay', str(recording),
                                        '--report', str(retry_report)], stdout=log,
                                        stderr=subprocess.STDOUT, timeout=args.seconds + 30)
                retry = load_report(retry_report)
                summary['crashReproduced'] = bool(result.returncode == 70 and retry.get('crashed')
                        and recorded.get('signature') and retry.get('signature') == recorded['signature']
                        and not retry.get('replayDiverged', False))
            except subprocess.TimeoutExpired:
                summary['crashReproduced'] = False
        rc = 70 if summary['crashReproduced'] else 1
    (out / 'result.json').write_text(json.dumps(summary, indent=2) + '\n')
    print(json.dumps(summary))
    return rc


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('game', type=Path)
    parser.add_argument('--hvm', type=Path, default=Path('build/hvm'))
    parser.add_argument('--out', type=Path, required=True)
    parser.add_argument('--steps', type=int, default=60)
    parser.add_argument('--seconds', type=int, default=90)
    parser.add_argument('--interval', type=float, default=1.0)
    parser.add_argument('--seed', type=int, default=1)
    args = parser.parse_args()
    if not (0 < args.steps <= 1000 and 0 < args.seconds <= 3600 and math.isfinite(args.interval) and args.interval > 0):
        parser.error('steps must be 1..1000, seconds 1..3600, interval positive and finite')
    try:
        return run(args)
    except (OSError, ValueError) as exc:
        parser.exit(1, f'autoplayer: {exc}\n')


if __name__ == '__main__':
    sys.exit(main())
