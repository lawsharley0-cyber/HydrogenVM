#!/usr/bin/env python3
"""Run the pinned client against its local initial-home peer, preserving relaunch state.

Success means native home acceptance and an NPC request, not a playable battle.
Audio is pulled continuously and can be recorded; this headless probe does not
open a speaker device. Use desktop.py for device playback.
"""
import argparse
import hashlib
import json
from pathlib import Path
import signal
import socket
import subprocess
import sys
import threading
import time
import uuid
import wave
import zipfile

from clash_home_state import PINNED_SHA256


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--hvm', required=True, type=Path)
    parser.add_argument('--ipa', required=True, type=Path)
    parser.add_argument('--out', required=True, type=Path)
    parser.add_argument('--results', type=Path, help='Optional separate directory for public run diagnostics')
    parser.add_argument('--seconds', type=float, default=90)
    parser.add_argument('--launches', type=int, choices=(1,2), default=2)
    parser.add_argument('--capture-audio', action='store_true')
    args = parser.parse_args()
    if not 30 <= args.seconds <= 300:
        parser.error('Use a startup observation interval between 30 and 300 seconds')
    with args.ipa.open('rb') as source:
        if hashlib.file_digest(source,'sha256').hexdigest() != PINNED_SHA256:
            parser.error('Pinned client checksum mismatch')
    args.out.mkdir(parents=True,exist_ok=True)
    results = args.results or args.out
    results.mkdir(parents=True,exist_ok=True)
    session = results / ('run-' + time.strftime('%Y%m%d-%H%M%S') + '-' + uuid.uuid4().hex[:6])
    session.mkdir()
    container = args.out / 'game/com.supercell.scroll'
    bundle = container / 'Clash Royale.app'
    binary = str(args.hvm.resolve())
    if not container.exists():
        subprocess.run([binary,'import',str(args.ipa.resolve()),str(args.out/'game')],check=True,stdout=subprocess.DEVNULL)
    else:
        with zipfile.ZipFile(args.ipa) as archive:
            for name in ('Info.plist','Clash Royale'):
                if (bundle/name).read_bytes() != archive.read('Payload/Clash Royale.app/'+name):
                    parser.error('Existing installation differs; use another --out to preserve it')
    command = [sys.executable,str(Path(__file__).with_name('clash_protocol_probe.py')),
               '--out',str(session),'--login-ok','--state',str(args.out/'identity.json'),
               '--home-client',str(args.ipa.resolve()),'--game-state',str(args.out/'home-state.json'),
               '--seconds',str(min(3600,args.seconds*args.launches+120))]
    evidence = []
    with (session/'peer.log').open('w') as log:
        peer = subprocess.Popen(command,stdout=log,stderr=subprocess.STDOUT)
        try:
            for _ in range(100):
                if peer.poll() is not None:
                    raise RuntimeError('Peer failed: '+(session/'peer.log').read_text())
                if '"event": "listening"' in (session/'peer.log').read_text():
                    break
                time.sleep(.05)
            else:
                raise RuntimeError('Peer did not initialize')
            for launch in range(1,args.launches+1):
                stem = session/f'launch-{launch}'
                reader,writer = socket.socketpair(socket.AF_UNIX,socket.SOCK_DGRAM)
                reader.settimeout(.1)
                done = threading.Event()
                audio_errors = []
                def capture():
                    recording = None
                    try:
                        if args.capture_audio:
                            recording = wave.open(str(stem.with_suffix('.wav')),'wb')
                            recording.setparams((2,2,44100,0,'NONE','not compressed'))
                        while not done.is_set():
                            try:
                                data = reader.recv(8193)
                            except socket.timeout:
                                continue
                            if len(data) != 8192:
                                raise ValueError('Invalid mixer block')
                            if recording:
                                recording.writeframesraw(data)
                    except Exception as error:
                        audio_errors.append(str(error))
                    finally:
                        if recording:
                            recording.close()
                thread = threading.Thread(target=capture,name='probe-audio');thread.start()
                began = time.monotonic()
                try:
                    with stem.with_suffix('.log').open('w') as client_log:
                        run = subprocess.run([binary,'run',str(bundle.resolve()),'--home',str(container.resolve()),
                            '--loopback-network','--loopback-alias','game.clashroyaleapp.com',
                            '--run-for',str(args.seconds),'--graceful','--audio-fd',str(writer.fileno()),
                            '--screenshot',str(stem.with_suffix('.png')),'--report',str(stem.with_suffix('.json'))],
                            pass_fds=(writer.fileno(),),stdout=client_log,stderr=client_log,timeout=args.seconds+20)
                finally:
                    done.set();thread.join(timeout=2);reader.close();writer.close()
                report = json.loads(stem.with_suffix('.json').read_text()) if stem.with_suffix('.json').exists() else {}
                text = stem.with_suffix('.log').read_text(errors='replace')
                item = {'launch':launch,'exitCode':run.returncode,'seconds':round(time.monotonic()-began,2),
                        'nativeHomeAccepted':'Loaded own home from server' in text,
                        'crashed':report.get('crashed',True),'errors':report.get('errors',[]),
                        'audio':report.get('audio'),'audioCaptureErrors':audio_errors}
                evidence.append(item);print(json.dumps(item),flush=True)
                if run.returncode or not item['nativeHomeAccepted'] or item['crashed'] or item['errors'] or audio_errors:
                    break
        finally:
            if peer.poll() is None:
                peer.send_signal(signal.SIGINT)
            peer.wait(timeout=5)
            events = json.loads((session/'protocol-events.json').read_text())['events'] if (session/'protocol-events.json').exists() else []
            summary = {'scope':'Initial-home/NPC-request diagnostics only; no playable battle or progression.',
                       'ipaSha256':PINNED_SHA256,'launches':evidence,
                       'loginKinds':[e['fresh'] for e in events if e['event']=='login'],
                       'npcRequests':[e['npc'] for e in events if e['event']=='npc_requested']}
            with args.hvm.open('rb') as source:
                summary['engineSha256'] = hashlib.file_digest(source,'sha256').hexdigest()
            saved_home = args.out/'home-state.json'
            summary['savedHomeSha256'] = hashlib.sha256(saved_home.read_bytes()).hexdigest() if saved_home.exists() else None
            (session/'verification.json').write_text(json.dumps(summary,indent=2)+'\n')
    print(f'Diagnostics: {session}',flush=True)
    valid = len(evidence) == args.launches and len(summary['npcRequests']) >= args.launches
    valid &= len(summary['loginKinds']) == args.launches
    if args.launches == 2:
        valid &= summary['loginKinds'][-1:] == [False]
    for result in evidence:
        audio = result['audio'] or {}
        valid &= (result['nativeHomeAccepted'] and result['exitCode'] == 0 and
                  not result['crashed'] and not result['errors'] and not result['audioCaptureErrors'] and
                  audio.get('enabled', False) and audio.get('nonSilentFrames', 0) > 0 and
                  audio.get('transportError', -1) == 0)
    return 0 if valid else 1


if __name__ == '__main__':
    raise SystemExit(main())
