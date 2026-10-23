"""Tests for the read-only turn decoder. No IPA or network."""
import sys
from pathlib import Path

_HERE = Path(__file__).resolve().parent
_TOOLS = _HERE.parent
for _path in (str(_HERE), str(_TOOLS)):
    if _path not in sys.path:
        sys.path.insert(0, _path)

import struct
import unittest
from clash_turns import decode_turn
from clash_wire import Writer


class Turns(unittest.TestCase):
    def test_native_empty_turn(self):
        raw = bytes.fromhex('8803b4a6c7f20500ffffffff')
        turn = decode_turn(raw, [0, 1])
        self.assertEqual(turn, {'tick': 200, 'checksum': 791210420, 'commands': []})

    def test_truncation_and_trailing_bytes(self):
        raw = bytes.fromhex('8803b4a6c7f20500ffffffff')
        for data in (raw[:-1], raw + b'\0', raw[:-4] + b'\0' * 4):
            with self.assertRaises(ValueError):
                decode_turn(data, [0, 1])

    def test_scope_rejects_unknown_commands_and_executors(self):
        for kind, account in ((1000, (0, 1)), (1, (0, 2))):
            w = Writer()
            for n in (200, 0, 1, kind, 50, 1, *account):
                w.vint(n)
            w.raw(struct.pack('>i', -1))
            with self.assertRaises(ValueError):
                decode_turn(bytes(w.data), [0, 1])

    def test_custom_spell_is_not_forwarded(self):
        w = Writer()
        for n in (200, 0, 1, 1, 50, 1, 0, 1, 0):
            w.vint(n)
        w.boolean(True)
        with self.assertRaises(ValueError):
            decode_turn(bytes(w.data), [0, 1])

    def test_turn_limits(self):
        for tick, count in ((-1, 0), (0, 65), (0, -1)):
            w = Writer()
            for n in (tick, 0, count):
                w.vint(n)
            with self.assertRaises(ValueError):
                decode_turn(bytes(w.data), [0, 1])


if __name__ == '__main__':
    unittest.main()
