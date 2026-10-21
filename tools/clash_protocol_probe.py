#!/usr/bin/env python3
"""Loopback-only diagnostic peer for the pinned Clash client; no home or battle server.

The default captures message metadata only. --login-ok exercises the pinned
native LoginOk decoder with a local diagnostic identity. This is not playable
offline mode, a real account service, or verified game-state persistence.
"""
import argparse
import json
import os
from pathlib import Path
import secrets
import socket
import struct
import time

MAX_PAYLOAD = 1 << 20
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


class Reader:
    def __init__(self, data):
        self.data, self.offset = data, 0

    def read(self, count):
        if count < 0 or self.offset + count > len(self.data):
            raise ValueError('Truncated message')
        result = self.data[self.offset:self.offset + count]
        self.offset += count
        return result

    def integer(self):
        return struct.unpack('>i', self.read(4))[0]

    def string(self):
        size = self.integer()
        if size == -1:
            return None
        if size < 0 or size > MAX_PAYLOAD:
            raise ValueError('Invalid string length')
        return self.read(size).decode('utf-8')

    def vint(self):
        first = byte = self.read(1)[0]
        value, shift = first & 63, 6
        while byte & 128:
            if shift >= 34:
                raise ValueError('Overlong VInt')
            byte = self.read(1)[0]
            value |= (byte & 127) << shift
            shift += 7
        if first & 64:
            value |= -(1 << shift)
        if not -(1 << 31) <= value < (1 << 31):
            raise ValueError('VInt exceeds signed 32-bit range')
        return value


def vint(value):
    if not -(1 << 31) <= value < (1 << 31):
        raise ValueError('VInt exceeds signed 32-bit range')
    negative = value < 0
    first = (value & 63) | (64 if negative else 0)
    value >>= 6
    out = bytearray()
    done = -1 if negative else 0
    out.append(first | (128 if value != done else 0))
    while value != done:
        byte = value & 127
        value >>= 7
        out.append(byte | (128 if value != done else 0))
    return bytes(out)


def string(value):
    if value is None:
        return struct.pack('>i', -1)
    data = value.encode('utf-8')
    return struct.pack('>i', len(data)) + data


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
        event('listening', host='127.0.0.1', port=server.getsockname()[1], loginReply=args.login_ok)
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
                                elif message == 10108 and not payload and args.login_ok:
                                    send(20108, 0)
                    except (ValueError, OSError) as error:
                        event('connection_error', reason=str(error))
                    event('disconnected', incompleteBytes=len(frames.pending))
        except KeyboardInterrupt:
            pass
        finally:
            (args.out / 'protocol-events.json').write_text(json.dumps({
                'scope': 'Diagnostic login peer only. No home state, battles or game progression.',
                'events': events,
            }, indent=2) + '\n')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--out', required=True, type=Path)
    parser.add_argument('--port', type=int, default=9339)
    parser.add_argument('--seconds', type=float, default=60)
    parser.add_argument('--login-ok', action='store_true')
    parser.add_argument('--state', type=Path, help='Local diagnostic identity JSON; required with --login-ok')
    args = parser.parse_args()
    if args.login_ok and args.state is None:
        parser.error('--login-ok requires --state')
    if not 0 <= args.port <= 65535 or not 0 < args.seconds <= 3600:
        parser.error('Invalid port or duration')
    serve(args)


if __name__ == '__main__':
    main()
