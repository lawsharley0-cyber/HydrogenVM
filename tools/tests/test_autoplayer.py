import io
from pathlib import Path
import random
import struct
import sys
from tempfile import TemporaryDirectory
import unittest
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'bench'))
import autoplayer as bot


class FrameTests(unittest.TestCase):
    def test_point_scale_and_gesture_phases(self):
        frame = bot.Frame(640, 960, 2, b'')
        controls = io.StringIO()
        with patch.object(bot.time, 'sleep'):
            bot.gesture(controls, frame, [(0.25, 0.75), (0.5, 0.25)])
        events = controls.getvalue().splitlines()
        self.assertEqual(events[0], 'touch 0 80.000 360.000')
        self.assertEqual(events[-1], 'touch 3 160.000 120.000')
        self.assertEqual([line.split()[1] for line in events], ['0'] + ['1'] * 5 + ['3'])

    def test_frame_and_text_heuristic(self):
        with TemporaryDirectory() as tmp:
            path = Path(tmp) / 'frame'
            rgba = bytearray([0, 0, 0, 255]) * (96 * 64)
            for y in range(16, 32):
                for x in range(32, 48, 2):
                    index = 4 * (y * 96 + x)
                    rgba[index:index + 3] = b'\xff\xff\xff'
            path.write_bytes(struct.pack('<4sIIf', b'HVF1', 96, 64, 1) + rgba)
            frame = bot.Frame.read(path)
            self.assertEqual(bot.choose(frame, 0, random.Random(0)), [(2.5 / 6, 1.5 / 4)])
            for bad in [b'HVF1', struct.pack('<4sIIf', b'HVF1', 1, 1, float('nan')),
                        struct.pack('<4sIIf', b'HVF1', 96, 64, 1) + b'x']:
                path.write_bytes(bad)
                with self.assertRaises(ValueError):
                    bot.Frame.read(path)

    def test_repeatable_exploration(self):
        frame = bot.Frame(1, 1, 1, bytes(96 * 64 * 3))
        def sequence():
            rng = random.Random(22)
            return [bot.choose(frame, i, rng) for i in range(16)]
        self.assertEqual(sequence(), sequence())
        self.assertTrue(any(len(action) == 2 for action in sequence()))
