#!/usr/bin/env python3
import struct
import tempfile
from pathlib import Path
import unittest
import argparse
import json
import queue
import socket
import threading
from unittest.mock import patch

from clash_protocol_probe import Frames, MAX_PAYLOAD, RC4, Reader, load_profile, login, login_ok, protocol_cipher, string, vint
from clash_protocol_probe import serve


class ProtocolTests(unittest.TestCase):
    def test_unknown_turn_preserves_encrypted_connection_and_heartbeat(self):
        # No client assets: only the template/home encoders are mocked. Real
        # framing, RC4 streams, login, NPC handling and TCP stay under test.
        events = queue.Queue()
        failures = []
        def capture(value, **kwargs):
            events.put(json.loads(value))
        def run(args):
            try:
                serve(args)
            except Exception as error:
                failures.append(error)
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            args = argparse.Namespace(out=root/'events', state=root/'identity.json',
                login_ok=True, home_client=root/'not-an-ipa', game_state=root/'home.json',
                npc_fixture=True, port=0, seconds=1)
            with patch('clash_home_state.load_state', return_value={}), \
                 patch('clash_home_state.encode_home', return_value=b'home'), \
                 patch('clash.clash_sector_state.load_template', return_value={'seed':2}), \
                 patch('clash.clash_sector_state.encode_sector', return_value=b'sector'), \
                 patch('clash_protocol_probe.print', side_effect=capture):
                worker = threading.Thread(target=run, args=(args,))
                worker.start()
                try:
                    ready = events.get(timeout=2)
                    self.assertEqual(ready['event'], 'listening')
                    with socket.create_connection(('127.0.0.1',ready['port']),timeout=2) as client:
                        outgoing, incoming, frames = protocol_cipher(), protocol_cipher(), Frames()
                        def send(kind, payload=b''):
                            data=outgoing.apply(payload)
                            client.sendall(struct.pack('>H',kind)+len(data).to_bytes(3,'big')+b'\0\0'+data)
                        def receive(count):
                            result=[]
                            while len(result)<count:
                                data=client.recv(65536)
                                self.assertTrue(data, 'Peer disconnected during metadata observation')
                                result.extend((kind,incoming.apply(body)) for kind,_,body in frames.feed(data))
                            return result
                        send(10101,struct.pack('>ii',0,0)+string(None)+vint(2)+vint(0)+vint(1306))
                        self.assertEqual([kind for kind,_ in receive(2)],[20104,24101])
                        send(14104,b'\x12\x00')
                        self.assertEqual(receive(1)[0][0],21903)
                        # Unknown command 99, then a heartbeat on the SAME RC4 stream.
                        send(14102,vint(1)+vint(0)+vint(1)+vint(99))
                        send(10108)
                        self.assertEqual(receive(1),[(20108,b'')])
                finally:
                    worker.join(timeout=3)
                self.assertFalse(worker.is_alive())
                self.assertEqual(failures,[])
            recorded=json.loads((args.out/'protocol-events.json').read_text())['events']
            self.assertEqual(sum(e['event']=='unsupported_client_turn' for e in recorded),1)
            self.assertFalse(any(e['event']=='connection_error' for e in recorded))

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
        # Literal pinned ByteStream::writeVInt (0x193748) encodings, rather
        # than relying only on our writer/reader agreeing with each other.
        for value,encoded in ((-64,'c07f'),(-8192,'c0807f'),(-(1<<31),'c080808000'),
                              ((1<<31)-1,'bfffffff0f'),(-134217728,'c08080800f')):
            self.assertEqual(vint(value).hex(),encoded)
            self.assertEqual(Reader(bytes.fromhex(encoded)).vint(),value)
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



    def test_unsupported_and_supported_turns(self):
        from unittest.mock import patch
        import threading
        import socket
        import json
        import time
        from clash_wire import Writer
        from argparse import Namespace
        import sys
        sys.path.insert(0, str(Path(__file__).resolve().parent / 'clash'))
        import clash_sector_state
        from clash_protocol_probe import serve

        with tempfile.TemporaryDirectory() as folder:
            out = Path(folder) / 'out'
            state = Path(folder) / 'state.json'
            state.write_text(json.dumps({'format': 1, 'account': [0, 1], 'token': 'abc', 'created': 123}))

            args = Namespace(
                out=out,
                port=19339,
                seconds=2.0,
                login_ok=True,
                state=state,
                home_client=Path('fake.ipa'),
                game_state=Path(folder) / 'game_state.json',
                npc_fixture=True
            )

            # mock load_template and the home encoding stuff
            with patch('clash_home_state.load_state', return_value={'home': {'decks': [[]]}, 'avatar': {'name': 'test'}}),                  patch('clash_home_state.encode_home', return_value=b'home'),                  patch('clash_sector_state.load_template', return_value={'seed': 123}), patch('clash_sector_state.encode_sector', return_value=b'sector'):

                thread = threading.Thread(target=serve, args=(args,))
                thread.start()

                time.sleep(0.1) # wait for server to listen

                with socket.socket() as client:
                    client.connect(('127.0.0.1', 19339))

                    # We need to apply RC4 encryption.
                    cipher = protocol_cipher()

                    # 14102 payload with command type 1000
                    w = Writer()
                    w.vint(1) # tick
                    w.vint(0) # checksum
                    w.vint(1) # count
                    w.vint(1000) # kind

                    payload1 = bytes(w.data)
                    encrypted1 = cipher.apply(payload1)
                    packet1 = struct.pack('>H', 14102) + len(encrypted1).to_bytes(3, 'big') + struct.pack('>H', 0) + encrypted1
                    client.sendall(packet1)

                    # 14102 payload with command type 1 (spell)
                    w2 = Writer()
                    w2.vint(1) # tick
                    w2.vint(0) # checksum
                    w2.vint(1) # count
                    w2.vint(1) # kind
                    w2.vint(1) # given
                    w2.vint(1) # sequence
                    w2.vint(0) # high
                    w2.vint(1) # low
                    w2.vint(1) # spellIndex
                    w2.boolean(False) # custom spell
                    w2.vint(1000) # x
                    w2.vint(1000) # y
                    w2.raw(struct.pack('>i', -1)) # trailer

                    payload2 = bytes(w2.data)
                    encrypted2 = cipher.apply(payload2)
                    packet2 = struct.pack('>H', 14102) + len(encrypted2).to_bytes(3, 'big') + struct.pack('>H', 0) + encrypted2
                    client.sendall(packet2)

                    # Sleep to let events process
                    time.sleep(0.5)

                thread.join()

            events_json = json.loads((out / 'protocol-events.json').read_text())
            events = [e['event'] for e in events_json['events']]

            self.assertIn('unsupported_turn', events)
            self.assertIn('client_turn', events)
            self.assertNotIn('connection_error', events)

if __name__ == '__main__':
    unittest.main()
