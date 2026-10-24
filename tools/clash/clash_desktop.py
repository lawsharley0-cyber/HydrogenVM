#!/usr/bin/env python3
"""Interactive pinned first-NPC experiment, with a local peer and actual audio.

The peer initializes the original client tutorial and NPC simulation. Battle
completion, rewards and progression persistence remain under investigation.
Identity and the installed app are retained; this is not a finished offline game.

The peer and pygame viewer stay in ``tools/``. Use ``--gpu`` to retain the
shared accelerated renderer. This remains a first-mission experiment until
battle results and earned progression are verified.
"""
import argparse
import fcntl
from pathlib import Path
import signal
import subprocess
import sys
import time
import uuid


def tool_dir():
    here = Path(__file__).resolve().parent
    if (here / 'clash_protocol_probe.py').is_file():
        return here
    return here.parent


def main():
    p = argparse.ArgumentParser(description=__doc__)
    for name in ('hvm', 'ipa', 'home', 'out'):
        p.add_argument('--' + name, type=Path, required=True)
    p.add_argument('--seconds', type=float, default=0)
    p.add_argument('--gpu', action='store_true', help='Use the shared host GLES backend')
    p.add_argument('--capture-audio', action='store_true')
    p.add_argument('--mute', action='store_true')
    p.add_argument('--exit-on-stop', action='store_true')
    args = p.parse_args()
    args.home = args.home.resolve()
    args.out = args.out.resolve()
    args.home.mkdir(parents=True, exist_ok=True)
    args.out.mkdir(parents=True, exist_ok=True)
    session = args.out / ('npc-' + time.strftime('%Y%m%d-%H%M%S') + '-' + uuid.uuid4().hex[:6])
    session.mkdir()
    scripts = tool_dir()
    with (args.home / 'battle-session.lock').open('a') as lock:
        try:
            fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
        except BlockingIOError:
            p.error('An NPC session is already using this sandbox')
        with (session / 'peer.log').open('w') as log:
            peer = subprocess.Popen([sys.executable, str(scripts / 'clash_protocol_probe.py'),
                '--out', str(session), '--state', str(args.home / 'identity.json'), '--login-ok',
                '--home-client', str(args.ipa.resolve()), '--game-state', str(args.home / 'home-state.json'),
                '--npc-fixture', '--seconds', '0'], stdout=log, stderr=subprocess.STDOUT)
            try:
                for _ in range(200):
                    if peer.poll() is not None:
                        raise RuntimeError((session / 'peer.log').read_text())
                    if '"event": "listening"' in (session / 'peer.log').read_text():
                        break
                    time.sleep(.05)
                else:
                    raise RuntimeError('Local peer did not initialize')
                command = [sys.executable, str(scripts / 'desktop.py'), '--hvm', str(args.hvm.resolve()),
                    '--ipa', str(args.ipa.resolve()), '--home', str(args.home / 'game'), '--out', str(session / 'viewer'),
                    '--pinned-clash', '--loopback-network', '--loopback-alias', 'game.clashroyaleapp.com',
                    '--seconds', str(args.seconds)]
                for flag in ('capture_audio', 'mute', 'exit_on_stop', 'gpu'):
                    if getattr(args, flag):
                        command.append('--' + flag.replace('_', '-'))
                print('Experimental NPC session: ' + str(session), flush=True)
                return subprocess.run(command).returncode
            finally:
                if peer.poll() is None:
                    peer.send_signal(signal.SIGINT)
                try:
                    peer.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    peer.terminate()
                    peer.wait(timeout=5)


if __name__ == '__main__':
    raise SystemExit(main())
