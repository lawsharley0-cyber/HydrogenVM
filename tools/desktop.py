#!/usr/bin/env python3
"""Local desktop viewer for hvm; no server, guest network, or asset patches."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import plistlib
import struct
import subprocess
import tempfile
import time
import uuid
import zipfile

os.environ.setdefault('PYGAME_HIDE_SUPPORT_PROMPT', '1')
import pygame
from desktop_audio import DesktopAudio

PINNED_CLASH_SHA256 = '29269ede437f39ffa62fb4234a1c323303ef417e7eb6ddc5c86f9be76e08fb3f'


def prepare_bundle(hvm, ipa, root, digest, log):
    """Never re-import over a sandbox: importIpa replaces its whole container."""
    with zipfile.ZipFile(ipa) as archive:
        infos = [name for name in archive.namelist() if name.startswith('Payload/') and
                 len(name.split('/')) == 3 and name.endswith('.app/Info.plist')]
        if len(infos) != 1:
            raise ValueError('Expected one top-level app bundle')
        info = plistlib.loads(archive.read(infos[0]))
        bundle_id = info['CFBundleIdentifier']
        if not isinstance(bundle_id, str) or not bundle_id or bundle_id in ('.', '..'):
            raise ValueError('Invalid bundle identifier')
        for ch in '/\\:':
            bundle_id = bundle_id.replace(ch, '_')
        app_name = infos[0].split('/')[1]
        executable = info['CFBundleExecutable']
        if Path(executable).name != executable or executable in ('.', '..'):
            raise ValueError('Unsafe executable name')
        container = root / bundle_id
        bundle = container / app_name
        stamp = container / '.hvm-client-sha256'
        if bundle.is_dir():
            if stamp.exists():
                if stamp.read_text().strip() != digest:
                    raise ValueError('This sandbox belongs to another IPA. Use a different --home; existing saves were preserved.')
            else:
                # Adopt an existing installation only after checking its metadata
                # and executable, preserving all of its Documents/Library files.
                for name in ('Info.plist', executable):
                    if (bundle / name).read_bytes() != archive.read(f'Payload/{app_name}/{name}'):
                        raise ValueError('Existing bundle differs from the IPA. Use a different --home; existing files were preserved.')
        else:
            if container.exists():
                raise ValueError('Sandbox exists without its app bundle. Use a different --home; existing files were preserved.')
            subprocess.run([str(hvm), 'import', str(ipa), str(root)], stdout=log, stderr=subprocess.STDOUT, check=True)
        stamp.write_text(digest + '\n')
        return bundle, container, info.get('CFBundleDisplayName') or info.get('CFBundleName') or app_name[:-4]


def read_frame(path):
    try:
        raw = path.read_bytes()
    except FileNotFoundError:
        return None
    if len(raw) < 16:
        raise ValueError('Truncated live frame header')
    magic, width, height, scale = struct.unpack_from('<4sIIf', raw)
    if magic != b'HVF1' or not 0 < width <= 8192 or not 0 < height <= 8192 or not 0 < scale <= 8:
        raise ValueError('Invalid live frame header')
    if len(raw) != 16 + width * height * 4:
        raise ValueError('Invalid live frame byte count')
    # Guest framebuffer RGB is already premultiplied. Display it over black,
    # without multiplying its alpha a second time.
    surface = pygame.image.frombuffer(raw[16:], (width, height), 'RGBA').copy()
    surface.set_alpha(None)
    return surface, scale


def viewport(window_size, image_size):
    width, height = window_size
    iw, ih = image_size
    factor = min(width / iw, height / ih)
    rect = pygame.Rect(0, 0, max(1, round(iw * factor)), max(1, round(ih * factor)))
    rect.center = (width // 2, height // 2)
    return rect


def point(position, rect, image_size, scale):
    x = min(max((position[0] - rect.x) / rect.width, 0), 1)
    y = min(max((position[1] - rect.y) / rect.height, 0), 1)
    return x * image_size[0] / scale, y * image_size[1] / scale


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--hvm', required=True, type=Path)
    parser.add_argument('--ipa', required=True, type=Path)
    parser.add_argument('--home', required=True, type=Path)
    parser.add_argument('--out', required=True, type=Path)
    parser.add_argument('--pinned-clash', action='store_true')
    parser.add_argument('--versioned-home', action='store_true', help='Keep each IPA hash in its own persistent sandbox')
    parser.add_argument('--seconds', type=float, default=0, help='Automatically stop after this many seconds')
    parser.add_argument('--exit-on-stop', action='store_true')
    parser.add_argument('--tap', help='Schedule one touch through the live input bridge: X,Y@seconds')
    parser.add_argument('--mute', action='store_true', help='Disable the guest mixer output')
    parser.add_argument('--capture-audio', action='store_true', help='Save delivered audio as session/audio.wav')
    args = parser.parse_args()
    with args.ipa.open('rb') as stream:
        digest = hashlib.file_digest(stream, 'sha256').hexdigest()
    if args.pinned_clash and digest != PINNED_CLASH_SHA256:
        parser.error('Pinned Clash client checksum does not match')
    if args.versioned_home:
        args.home = args.home / digest
    args.home.mkdir(parents=True, exist_ok=True)
    session = args.out / (time.strftime('%Y%m%d-%H%M%S') + '-' + uuid.uuid4().hex[:6])
    session.mkdir(parents=True)
    print(f'Logs, screenshots and report: {session}', flush=True)
    print('Mouse: touch/drag | S: screenshot | B: background | F: foreground | Esc/close: save and stop', flush=True)
    pygame.display.init()
    pygame.font.init()
    print(f'Display backend: {pygame.display.get_driver()}', flush=True)
    desktop = pygame.display.get_desktop_sizes()[0]
    height = max(400, min(852, int(desktop[1] * 0.8)))
    window = pygame.display.set_mode((round(height * 320 / 568), height), pygame.RESIZABLE)
    pygame.display.set_caption('HydrogenVM — starting app')
    font = pygame.font.Font(None, 24)
    clock = pygame.time.Clock()
    surface, scale = None, 2.0
    pressed = False
    closing = False
    stop_sent = None
    failed = False
    frame_stamp = None
    frame_count = 0
    scheduled = None
    touch_up = None
    if args.tap:
        xy, at = args.tap.split('@')
        scheduled = (*map(float, xy.split(',')), float(at))
    with tempfile.TemporaryDirectory(prefix='hvm-desktop-') as temporary:
        ipc = Path(temporary)
        controls = ipc / 'input.txt'
        controls.touch()
        frame = ipc / 'frame.rgba'
        with (session / 'runtime.log').open('w') as log, controls.open('a', buffering=1) as control:
            bundle, container, display_name = prepare_bundle(args.hvm.resolve(), args.ipa.resolve(), args.home.resolve(), digest, log)
            repository = Path(__file__).resolve().parent.parent
            commit = subprocess.run(['git', '-C', str(repository), 'rev-parse', 'HEAD'], capture_output=True, text=True)
            dirty = subprocess.run(['git', '-C', str(repository), 'status', '--porcelain'], capture_output=True, text=True)
            with args.hvm.open('rb') as binary:
                engine_digest = hashlib.file_digest(binary, 'sha256').hexdigest()
            (session / 'session.json').write_text(json.dumps({
                'app': display_name, 'ipaSha256': digest, 'engineSha256': engine_digest,
                'sourceCommit': commit.stdout.strip() if commit.returncode == 0 else None,
                'sourceTreeDirty': bool(dirty.stdout.strip()) if dirty.returncode == 0 else None,
                'sandbox': str(container), 'hostOnly': True,
            }, indent=2) + '\n')
            command = [str(args.hvm.resolve()), 'run', str(bundle), '--home', str(container),
                       '--frame-file', str(frame), '--input-file', str(controls),
                       '--screenshot', str(session / 'final.png'), '--report', str(session / 'report.json')]
            audio = DesktopAudio(args.mute, session / 'audio.wav' if args.capture_audio else None)
            inherited = ()
            if audio.writer:
                inherited = (audio.writer.fileno(),)
                command += ['--audio-fd', str(inherited[0])]
            try:
                process = subprocess.Popen(command, stdout=log, stderr=subprocess.STDOUT, pass_fds=inherited)
            except Exception:
                audio.close(session / 'audio.json')
                raise
            audio.start()
            started = time.monotonic()

            def send(command):
                if process.poll() is None:
                    control.write(command + '\n')
                    control.flush()

            def stop():
                nonlocal stop_sent, pressed
                if stop_sent is None:
                    if pressed:
                        send('touch 4 0 0')
                        pressed = False
                    send('foreground')
                    send('stop')
                    stop_sent = time.monotonic()

            def kill_processes():
                nonlocal failed
                if process.poll() is None:
                    stop()
                    try:
                        process.wait(timeout=10)
                    except subprocess.TimeoutExpired:
                        process.kill()
                        process.wait()
                        failed = True

            def save_state():
                if surface:
                    pygame.image.save(surface, session / 'viewer-last.png')

            def close_connections():
                audio.close(session / 'audio.json')
                pygame.quit()

            try:
                running = True
                while running:
                    image_size = surface.get_size() if surface else (640, 1136)
                    rect = viewport(window.get_size(), image_size)
                    for event in pygame.event.get():
                        if event.type == pygame.QUIT or (event.type == pygame.KEYDOWN and event.key == pygame.K_ESCAPE):
                            closing = True
                            stop()
                        elif event.type == pygame.VIDEORESIZE:
                            window = pygame.display.set_mode((max(160, event.w), max(284, event.h)), pygame.RESIZABLE)
                        elif event.type == pygame.KEYDOWN:
                            if event.key == pygame.K_s and surface:
                                path = session / f'screenshot-{time.time_ns()}.png'
                                pygame.image.save(surface, path)
                                print(f'Saved {path}', flush=True)
                            elif event.key == pygame.K_b:
                                send('background')
                            elif event.key == pygame.K_f:
                                send('foreground')
                        elif event.type == pygame.MOUSEBUTTONDOWN and event.button == 1 and surface and rect.collidepoint(event.pos):
                            pressed = True
                            x, y = point(event.pos, rect, image_size, scale)
                            send(f'touch 0 {x} {y}')
                        elif event.type == pygame.MOUSEMOTION and pressed:
                            x, y = point(event.pos, rect, image_size, scale)
                            send(f'touch 1 {x} {y}')
                        elif event.type == pygame.MOUSEBUTTONUP and event.button == 1 and pressed:
                            x, y = point(event.pos, rect, image_size, scale)
                            send(f'touch 3 {x} {y}')
                            pressed = False
                        elif event.type == pygame.WINDOWFOCUSLOST and pressed:
                            send('touch 4 0 0')
                            pressed = False
                    elapsed = time.monotonic() - started
                    if scheduled and elapsed >= scheduled[2]:
                        x, y, _ = scheduled
                        send(f'touch 0 {x} {y}')
                        # Queue touch-up on the next frames, so UIKit can observe both phases.
                        scheduled = None
                        touch_up = (x, y, elapsed + 0.1)
                    if touch_up and elapsed >= touch_up[2]:
                        send(f'touch 3 {touch_up[0]} {touch_up[1]}')
                        touch_up = None
                    if args.seconds > 0 and elapsed >= args.seconds:
                        stop()
                    try:
                        stamp = frame.stat().st_mtime_ns
                        if stamp != frame_stamp:
                            surface, scale = read_frame(frame)
                            frame_stamp = stamp
                            frame_count += 1
                    except FileNotFoundError:
                        pass
                    code = process.poll()
                    if code is not None:
                        pygame.display.set_caption(f'HydrogenVM — stopped ({code}); S saves screenshot, Esc closes')
                        if closing or args.exit_on_stop:
                            running = False
                    elif stop_sent:
                        pygame.display.set_caption('HydrogenVM — saving and stopping…')
                        if time.monotonic() - stop_sent > 10:
                            print('Graceful stop timed out; terminating the host process. Check runtime.log.', flush=True)
                            process.terminate()
                            failed = True
                    else:
                        pygame.display.set_caption(f'HydrogenVM — {display_name} | S screenshot | B/F background/foreground | Esc save/stop')
                    window.fill((18, 20, 28))
                    if surface:
                        rect = viewport(window.get_size(), surface.get_size())
                        window.blit(pygame.transform.scale(surface, rect.size), rect)
                    else:
                        message = 'Stopped — see runtime.log' if code is not None else 'Waiting for the first frame…'
                        text = font.render(message, True, (220, 225, 235))
                        window.blit(text, (12, 24))
                    pygame.display.flip()
                    clock.tick(30)
            finally:
                kill_processes()
                save_state()
                close_connections()
    print(f'Received {frame_count} live frames; engine exit {process.returncode}. Results: {session}', flush=True)
    return 1 if failed else process.returncode


if __name__ == '__main__':
    raise SystemExit(main())
