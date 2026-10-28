#!/usr/bin/env python3
import struct
import tempfile
from pathlib import Path
import unittest
from unittest.mock import patch

import sys
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parent / 'clash'))

from clash_protocol_probe import Frames, MAX_PAYLOAD, RC4, Reader, load_profile, login, login_ok, protocol_cipher, string, vint


class ProtocolTests(unittest.TestCase):
    def test_rc4_known_vector_and_stream_chunks(self):
        self.assertEqual(RC4(b'Key').apply(b'Plaintext').hex(), 'bbf316e8d940af0ad3')
        data = bytes(range(256))
        cipher = protocol_cipher()
        self.assertEqual(cipher.apply(data[:73]) + cipher.apply(data[73:]), protocol_cipher().apply(data))

    def test_signed_vint_boundaries(self):
        for value in (0, 1, 63, 64, 127, 128, 1306, -1, -64, -65, -(1 << 31), (1 << 31) - 1):
            reader = Reader(vint(value))
            self.assertEqual(reader.vint(), value)
            self.assertEqual(reader.offset, len(reader.data))
        self.assertEqual(vint(1306).hex(), '9a14')
        for invalid in (b'\x80', b'\x80' * 6, b'\xbf\xff\xff\xff\xff\x00'):
            with self.assertRaises(ValueError):
                Reader(invalid).vint()

    def test_partial_and_coalesced_frames(self):
        frame = bytes.fromhex('27750000030003') + b'abc'
        empty = bytes.fromhex('277c0000000000')
        parser = Frames()
        self.assertEqual(parser.feed(frame[:4]), [])
        self.assertEqual(parser.feed(frame[4:8]), [])
        self.assertEqual(parser.feed(frame[8:] + empty), [(10101, 3, b'abc'), (10108, 0, b'')])
        self.assertEqual(parser.pending, b'')
        with self.assertRaises(ValueError):
            parser.feed(b'\x27\x75' + (MAX_PAYLOAD + 1).to_bytes(3, 'big') + b'\x00\x03')

    def test_login_uses_vints_after_account_and_token(self):
        payload = struct.pack('>ii', 0, 1) + string('local') + vint(2) + vint(0) + vint(1306)
        self.assertEqual(login(payload), ((0, 1), 'local', (2, 0, 1306)))
        with self.assertRaises(ValueError):
            login(payload[:-1])
        with self.assertRaises(ValueError):
            Reader(struct.pack('>i', -2)).string()

    def test_login_reply_matches_pinned_decoder_shape(self):
        reader = Reader(login_ok({'account': [0, 1], 'token': 'local', 'created': 123}))
        self.assertEqual([reader.integer() for _ in range(4)], [0, 1, 0, 1])
        self.assertEqual([reader.string() for _ in range(3)], ['local', '', ''])
        self.assertEqual([reader.vint() for _ in range(4)], [2, 0, 1306, 0])
        self.assertEqual(reader.string(), 'dev')
        self.assertEqual([reader.vint() for _ in range(3)], [1, 0, 0])
        self.assertEqual(reader.string(), '')
        self.assertTrue(reader.string().isdigit())
        self.assertEqual(reader.string(), '123')
        self.assertEqual(reader.vint(), 0)
        self.assertEqual(reader.offset, len(reader.data))

    def test_local_identity_is_reused_without_overwrite(self):
        with tempfile.TemporaryDirectory() as folder:
            path = Path(folder) / 'identity.json'
            first = load_profile(path)
            original = path.read_bytes()
            self.assertEqual(load_profile(path), first)
            self.assertEqual(path.read_bytes(), original)
            self.assertEqual(first['account'], [0, 1])
            self.assertEqual(len(first['token']), 48)



    @unittest.mock.patch('clash_home_state.load_state')
    @unittest.mock.patch('clash_home_state.encode_home')
    @unittest.mock.patch('clash_protocol_probe.load_profile')
    @unittest.mock.patch('clash_sector_state.load_template')
    def test_probe_unsupported_turn_does_not_disconnect(self, mock_load_template, mock_load_profile, mock_encode_home, mock_load_state):
        import argparse
        import json
        import socket
        import threading
        import time
        from clash_protocol_probe import serve

        with tempfile.TemporaryDirectory() as temp_dir:
            temp_path = Path(temp_dir)
            args = argparse.Namespace(
                out=temp_path,
                port=0,
                seconds=1.5,
                login_ok=True,
                state=temp_path / 'identity.json',
                home_client=Path('dummy.ipa'),
                game_state=temp_path / 'home-state.json',
                npc_fixture=True
            )
            mock_load_profile.return_value = {'format': 1, 'account': [0, 1], 'token': 'token1234', 'created': 1234}
            mock_load_state.return_value = {'home': {}, 'avatar': {}}
            mock_encode_home.return_value = b'home'
            mock_load_template.return_value = {'seed': 1234, 'npcDeck': [], 'towers': [], 'damageTypes': 5}

            s = socket.socket()
            s.bind(('127.0.0.1', 0))
            args.port = s.getsockname()[1]
            s.close()

            t = threading.Thread(target=serve, args=(args,), daemon=True)
            t.start()

            time.sleep(0.5)
            client = socket.create_connection(('127.0.0.1', args.port))

            import struct
            from clash_wire import string, vint, Writer
            payload = struct.pack('>ii', 0, 1) + string('token1234') + vint(2) + vint(0) + vint(1306)

            outgoing = protocol_cipher()
            encrypted = outgoing.apply(payload)
            packet = struct.pack('>H', 10101) + len(encrypted).to_bytes(3, 'big') + struct.pack('>H', 1) + encrypted
            client.sendall(packet)

            time.sleep(0.2)

            w = Writer()
            w.vint(100)
            w.vint(1234)
            w.vint(1)
            w.vint(1000)
            w.vint(0)
            w.vint(1)
            w.vint(0)
            w.vint(1)
            w.raw(struct.pack('>i', -1))

            payload = bytes(w.data)
            encrypted = outgoing.apply(payload)
            packet = struct.pack('>H', 14102) + len(encrypted).to_bytes(3, 'big') + struct.pack('>H', 1) + encrypted
            client.sendall(packet)

            time.sleep(1.0)
            client.close()
            t.join()

            events_file = temp_path / 'protocol-events.json'
            self.assertTrue(events_file.exists())
            with open(events_file, 'r') as f:
                data = json.load(f)
            event_types = [e['event'] for e in data['events']]
            self.assertIn('unsupported_turn', event_types)
            self.assertNotIn('connection_error', event_types)

    @unittest.mock.patch('clash_home_state.load_state')
    @unittest.mock.patch('clash_home_state.encode_home')
    @unittest.mock.patch('clash_protocol_probe.load_profile')
    @unittest.mock.patch('clash_sector_state.load_template')
    def test_probe_supported_turn_does_not_disconnect(self, mock_load_template, mock_load_profile, mock_encode_home, mock_load_state):
        import argparse
        import json
        import socket
        import threading
        import time
        from clash_protocol_probe import serve

        with tempfile.TemporaryDirectory() as temp_dir:
            temp_path = Path(temp_dir)
            args = argparse.Namespace(
                out=temp_path,
                port=0,
                seconds=1.5,
                login_ok=True,
                state=temp_path / 'identity.json',
                home_client=Path('dummy.ipa'),
                game_state=temp_path / 'home-state.json',
                npc_fixture=True
            )
            mock_load_profile.return_value = {'format': 1, 'account': [0, 1], 'token': 'token1234', 'created': 1234}
            mock_load_state.return_value = {'home': {}, 'avatar': {}}
            mock_encode_home.return_value = b'home'
            mock_load_template.return_value = {'seed': 1234, 'npcDeck': [], 'towers': [], 'damageTypes': 5}

            s = socket.socket()
            s.bind(('127.0.0.1', 0))
            args.port = s.getsockname()[1]
            s.close()

            t = threading.Thread(target=serve, args=(args,), daemon=True)
            t.start()

            time.sleep(0.5)

            client = socket.create_connection(('127.0.0.1', args.port))

            import struct
            from clash_wire import string, vint, Writer
            payload = struct.pack('>ii', 0, 1) + string('token1234') + vint(2) + vint(0) + vint(1306)

            outgoing = protocol_cipher()
            encrypted = outgoing.apply(payload)
            packet = struct.pack('>H', 10101) + len(encrypted).to_bytes(3, 'big') + struct.pack('>H', 1) + encrypted
            client.sendall(packet)

            time.sleep(0.2)

            w = Writer()
            w.vint(100)
            w.vint(1234)
            w.vint(1)
            w.vint(1)
            w.vint(0)
            w.vint(1)
            w.vint(0)
            w.vint(1)
            w.vint(0)
            w.boolean(False)
            w.vint(100)
            w.vint(100)
            w.raw(struct.pack('>i', -1))

            payload = bytes(w.data)
            encrypted = outgoing.apply(payload)
            packet = struct.pack('>H', 14102) + len(encrypted).to_bytes(3, 'big') + struct.pack('>H', 1) + encrypted
            client.sendall(packet)

            time.sleep(1.0)
            client.close()
            t.join()

            events_file = temp_path / 'protocol-events.json'
            self.assertTrue(events_file.exists())
            with open(events_file, 'r') as f:
                data = json.load(f)
            event_types = [e['event'] for e in data['events']]
            self.assertIn('client_turn', event_types)
            self.assertNotIn('connection_error', event_types)

if __name__ == '__main__':
    unittest.main()
