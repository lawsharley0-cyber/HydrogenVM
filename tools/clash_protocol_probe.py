#!/usr/bin/env python3
"""Loopback-only diagnostic peer for the pinned Clash client.

The default captures message metadata only. --login-ok exercises the pinned
native LoginOk decoder with a local diagnostic identity. --home-client also
sends a pinned, persisted initial home/avatar fixture. --npc-fixture sends
an initial sector fixture. Turn authority and progression saves are absent.
"""
import argparse
import json
import os
from pathlib import Path
import secrets
import socket
import struct
import sys
import time
import zlib
from clash_wire import MAX_PAYLOAD, Reader, vint, string

# Public protocol constant also present verbatim in the unmodified pinned IPA.
# No private account keys are used. Each direction has a distinct stream state.
PROTOCOL_KEY = b'fhsd6f86f67rt8fw78fw789we78r9789wer6re' + b'nonce'


class RC4:
    def __init__(self, key):
        if not key:
            raise ValueError('Empty RC4 key')
        self.state, self.i, self.j = list(range(256)), 0, 0
        j = 0
        for i in range(256):
            j = (j + self.state[i] + key[i % len(key)]) & 255
            self.state[i], self.state[j] = self.state[j], self.state[i]

    def apply(self, data):
        out = bytearray()
        for byte in data:
            self.i = (self.i + 1) & 255
            self.j = (self.j + self.state[self.i]) & 255
            self.state[self.i], self.state[self.j] = self.state[self.j], self.state[self.i]
            out.append(byte ^ self.state[(self.state[self.i] + self.state[self.j]) & 255])
        return bytes(out)


def protocol_cipher():
    cipher = RC4(PROTOCOL_KEY)
    cipher.apply(PROTOCOL_KEY)  # Native init drops the key+nonce length.
    return cipher


class Frames:
    def __init__(self):
        self.pending = bytearray()

    def feed(self, chunk):
        self.pending.extend(chunk)
        messages = []
        while len(self.pending) >= 7:
            size = int.from_bytes(self.pending[2:5], 'big')
            if size > MAX_PAYLOAD:
                raise ValueError('Message payload exceeds diagnostic limit')
            if len(self.pending) < 7 + size:
                break
            messages.append((int.from_bytes(self.pending[:2], 'big'),
                             int.from_bytes(self.pending[5:7], 'big'),
                             bytes(self.pending[7:7 + size])))
            del self.pending[:7 + size]
        return messages


def login(data):
    reader = Reader(data)
    account = reader.integer(), reader.integer()
    token = reader.string()
    version = tuple(reader.vint() for _ in range(3))
    return account, token, version


def login_ok(profile):
    # Derived from this client's LoginOkMessage::decode at 0x702e4.
    # The nearby 2.1507 reference's fixed-width ints and extra URL fields do
    # not match this decoder, so its payload is deliberately not reused.
    account = struct.pack('>ii', *profile['account'])
    timestamp = str(profile['created'])
    return (account + account + string(profile['token']) + string('') + string('') +
            b''.join(vint(n) for n in (2, 0, 1306, 0)) + string('dev') +
            b''.join(vint(n) for n in (1, 0, 0)) + string('') +
            string(str(int(time.time()))) + string(timestamp) + vint(0))


def load_profile(path):
    if path.exists():
        profile = json.loads(path.read_text())
        if profile.get('format') != 1 or profile.get('account') != [0, 1] or not isinstance(profile.get('token'), str):
            raise ValueError('Unsupported diagnostic profile')
        return profile
    profile = {'format': 1, 'account': [0, 1], 'token': secrets.token_hex(24), 'created': int(time.time())}
    path.parent.mkdir(parents=True, exist_ok=True)
    # Exclusive creation keeps an existing profile intact.
    with path.open('x') as target:
        os.chmod(path, 0o600)
        json.dump(profile, target, indent=2)
    return profile


def serve(args):
    args.out.mkdir(parents=True, exist_ok=True)
    profile = load_profile(args.state) if args.login_ok else None
    home = None
    template = None
    if getattr(args, 'home_client', None):
        from clash_home_state import load_state, encode_home
        home_state = load_state(args.home_client, args.game_state, profile['account'])
        home = encode_home(home_state, profile['account'])
    if getattr(args, 'npc_fixture', False):
        sys.path.insert(0, str(Path(__file__).resolve().parent / 'clash'))
        from clash_sector_state import load_template, encode_sector
        from clash_turns import decode_turn
        template = load_template(args.home_client)
    events = []
    def event(kind, **details):
        item = {'event': kind, **details}
        events.append(item)
        print(json.dumps(item), flush=True)
    deadline = time.monotonic() + args.seconds
    with socket.socket() as server:
        server.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        server.bind(('127.0.0.1', args.port))
        server.listen(2)
        server.settimeout(0.2)
        event('listening', host='127.0.0.1', port=server.getsockname()[1], loginReply=args.login_ok, homeReply=home is not None)
        try:
            while time.monotonic() < deadline:
                try:
                    connection, _ = server.accept()
                except socket.timeout:
                    continue
                with connection:
                    connection.settimeout(0.2)
                    frames, incoming, outgoing = Frames(), protocol_cipher(), protocol_cipher()
                    event('connected')
                    def send(message, version, payload=b''):
                        encrypted = outgoing.apply(payload)
                        packet = struct.pack('>H', message) + len(encrypted).to_bytes(3, 'big') + struct.pack('>H', version) + encrypted
                        connection.sendall(packet)
                        event('sent', message=message, version=version, size=len(payload))
                    try:
                        while time.monotonic() < deadline:
                            try:
                                chunk = connection.recv(65536)
                            except socket.timeout:
                                continue
                            if not chunk:
                                break
                            for message, version, encrypted in frames.feed(chunk):
                                payload = incoming.apply(encrypted)
                                event('received', message=message, version=version, size=len(payload))
                                if message == 10101:
                                    account, token, build = login(payload)
                                    event('login', version=list(build), fresh=account == (0, 0))
                                    if build != (2, 0, 1306):
                                        raise ValueError('Unsupported client wire version')
                                    if args.login_ok:
                                        if account != (0, 0) and (list(account) != profile['account'] or token != profile['token']):
                                            raise ValueError('Identity does not match this local diagnostic profile')
                                        send(20104, 1, login_ok(profile))
                                        if home is not None:
                                            send(24101, 0, home)
                                elif message == 10108 and not payload and args.login_ok:
                                    send(20108, 0)
                                elif message == 14104 and home is not None:
                                    reader = Reader(payload)
                                    npc = reader.reference()
                                    if reader.offset != len(payload) or npc // 1000000 != 18:
                                        raise ValueError('Invalid start-mission request')
                                    if npc == 18000000 and template is not None:
                                        sector = encode_sector(template, home_state, profile['account'])
                                        compressed = zlib.compress(sector)
                                        reply = b'\x01' + struct.pack('<I', len(sector)) + compressed
                                        send(21903, 1, reply)
                                        event('npc_requested', npc=npc, supported=True)
                                        event('npc_fixture_sent', size=len(sector), seed=template['seed'])
                                    else:
                                        event('npc_requested', npc=npc, supported=False)
                                elif message == 14102 and template is not None:
                                    try:
                                        decode_turn(payload, profile['account'])
                                        event('client_turn')
                                    except ValueError as error:
                                        event('unsupported_turn', reason=str(error))
                    except (ValueError, OSError) as error:
                        event('connection_error', reason=str(error))
                    event('disconnected', incompleteBytes=len(frames.pending))
        except KeyboardInterrupt:
            pass
        finally:
            (args.out / 'protocol-events.json').write_text(json.dumps({
                'scope': 'Diagnostic login, optional initial home fixture, and optional initial sector fixture. Turn authority and progression saves are absent.',
                'events': events,
            }, indent=2) + '\n')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--out', required=True, type=Path)
    parser.add_argument('--port', type=int, default=9339)
    parser.add_argument('--seconds', type=float, default=60)
    parser.add_argument('--login-ok', action='store_true')
    parser.add_argument('--state', type=Path, help='Local diagnostic identity JSON; required with --login-ok')
    parser.add_argument('--home-client', type=Path, help='Pinned unchanged IPA: send its initial home/avatar fixture')
    parser.add_argument('--game-state', type=Path, help='Persistent initial home/avatar JSON; required with --home-client')
    parser.add_argument('--npc-fixture', action='store_true')
    args = parser.parse_args()
    if args.login_ok and args.state is None:
        parser.error('--login-ok requires --state')
    if args.npc_fixture and not args.home_client:
        parser.error('--npc-fixture requires --home-client')
    if args.home_client and (not args.login_ok or not args.game_state):
        parser.error('--home-client requires --login-ok, --state and --game-state')
    if args.game_state and not args.home_client:
        parser.error('--game-state requires --home-client')
    if args.game_state and args.state and args.game_state.resolve() == args.state.resolve():
        parser.error('Identity and game state must use separate paths')
    if not 0 <= args.port <= 65535 or not 0 < args.seconds <= 3600:
        parser.error('Invalid port or duration')
    serve(args)


if __name__ == '__main__':
    main()
