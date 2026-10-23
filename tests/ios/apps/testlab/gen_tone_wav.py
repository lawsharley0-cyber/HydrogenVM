#!/usr/bin/env python3
"""Generate an original 440 Hz sine-wave WAV for Test Lab Case D.

Output: tone.wav  — 1 second, 44100 Hz, mono, 16-bit PCM, no copyrighted content.

The file is committed so builds do not need Python at packaging time.
Re-generate with:
    python3 tests/ios/apps/testlab/gen_tone_wav.py tests/ios/apps/testlab/tone.wav
"""
import math
import struct
import sys
from pathlib import Path


def generate(path: str, freq: float = 440.0, rate: int = 44100, duration: float = 1.0) -> None:
    n_samples = int(rate * duration)
    amplitude = 28000  # well below int16 max (32767), avoids clipping

    samples = []
    for i in range(n_samples):
        v = int(amplitude * math.sin(2.0 * math.pi * freq * i / rate))
        samples.append(v)

    pcm = struct.pack(f"<{n_samples}h", *samples)

    # RIFF/WAVE/fmt /data header
    data_size  = len(pcm)
    fmt_size   = 16
    num_channels   = 1
    bits_per_sample = 16
    byte_rate  = rate * num_channels * bits_per_sample // 8
    block_align = num_channels * bits_per_sample // 8
    riff_size  = 4 + (8 + fmt_size) + (8 + data_size)

    hdr = b"RIFF"
    hdr += struct.pack("<I", riff_size)
    hdr += b"WAVE"
    hdr += b"fmt "
    hdr += struct.pack("<I", fmt_size)
    hdr += struct.pack("<H", 1)            # PCM
    hdr += struct.pack("<H", num_channels)
    hdr += struct.pack("<I", rate)
    hdr += struct.pack("<I", byte_rate)
    hdr += struct.pack("<H", block_align)
    hdr += struct.pack("<H", bits_per_sample)
    hdr += b"data"
    hdr += struct.pack("<I", data_size)

    Path(path).write_bytes(hdr + pcm)
    print(f"wrote {path}  ({n_samples} samples, {len(hdr)+len(pcm)} bytes)")


if __name__ == "__main__":
    out = sys.argv[1] if len(sys.argv) > 1 else "tone.wav"
    generate(out)
