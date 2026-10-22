import os
import sys
import tempfile
import unittest

sys.path.insert(0, os.path.abspath(os.path.join(os.path.dirname(__file__), "..", "..")))

from compat_harness.frame import decode_png_rgba, encode_rgba_png, summarize_png, summarize_rgba  # noqa: E402


class FrameTests(unittest.TestCase):
    def test_all_black_and_varied(self):
        black = summarize_rgba(4, 4, bytes([0, 0, 0, 255]) * 16)
        self.assertEqual(black["signal"], "all-black")
        pixels = bytearray()
        for i in range(32 * 32):
            pixels.extend((i % 255, (i * 3) % 255, (i * 7) % 255, 255))
        varied = summarize_rgba(32, 32, bytes(pixels))
        self.assertEqual(varied["signal"], "varied")
        self.assertGreater(varied["uniqueQuantizedColors"], 8)

    def test_all_white_png_roundtrip(self):
        white = bytes([255, 255, 255, 255]) * 9
        blob = encode_rgba_png(3, 3, white)
        width, height, rgba = decode_png_rgba(blob)
        self.assertEqual((width, height), (3, 3))
        self.assertEqual(rgba, white)
        with tempfile.TemporaryDirectory() as tmp:
            path = os.path.join(tmp, "frame.png")
            with open(path, "wb") as f:
                f.write(blob)
            summary = summarize_png(path)
        self.assertEqual(summary["signal"], "all-white")
        self.assertEqual(summary["whiteFraction"], 1.0)

    def test_filter_sub_roundtrip(self):
        # Encode with filter 0, then the decoder must also accept a Sub row.
        raw_pixels = bytes([10, 20, 30, 255, 11, 21, 31, 255])
        # Build a one-row PNG manually using filter type 1 (Sub) via the public decoder.
        import struct
        import zlib
        width, height = 2, 1
        row = bytearray([1])  # Sub
        prev = [0, 0, 0, 0]
        for i, value in enumerate(raw_pixels):
            row.append((value - prev[i % 4]) & 0xFF)
            prev[i % 4] = value
        sig = b"\x89PNG\r\n\x1a\n"
        ihdr = struct.pack(">IIBBBBB", width, height, 8, 6, 0, 0, 0)

        def chunk(tag, body):
            crc = zlib.crc32(tag + body) & 0xFFFFFFFF
            return struct.pack(">I", len(body)) + tag + body + struct.pack(">I", crc)

        blob = sig + chunk(b"IHDR", ihdr) + chunk(b"IDAT", zlib.compress(bytes(row))) + chunk(b"IEND", b"")
        self.assertEqual(decode_png_rgba(blob)[2], raw_pixels)

    def test_rejects_non_png(self):
        with tempfile.TemporaryDirectory() as tmp:
            path = os.path.join(tmp, "frame.png")
            with open(path, "wb") as f:
                f.write(b"not a png")
            summary = summarize_png(path)
        self.assertEqual(summary["signal"], "unreadable")


if __name__ == "__main__":
    unittest.main()
