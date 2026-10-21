#!/usr/bin/env python3
"""Compare IMA WAV guest decoding against the independent system libsndfile.

No game assets are changed or included in the report. libsndfile retains block
padding, so compare its output trimmed to the WAV fact sample count.
"""
import argparse
import ctypes as C
import ctypes.util
import json
from pathlib import Path
import struct
import subprocess
import tempfile


class Info(C.Structure):
    _fields_ = [('frames', C.c_longlong), ('rate', C.c_int), ('channels', C.c_int),
                ('format', C.c_int), ('sections', C.c_int), ('seekable', C.c_int)]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('root', type=Path)
    parser.add_argument('--hvm-tests', required=True, type=Path)
    parser.add_argument('--out', required=True, type=Path)
    parser.add_argument('--require-count', type=int)
    args = parser.parse_args()
    library = ctypes.util.find_library('sndfile')
    if not library:
        parser.error('Install libsndfile1 for the independent comparison')
    lib = C.CDLL(library)
    lib.sf_open.argtypes = [C.c_char_p, C.c_int, C.POINTER(Info)]
    lib.sf_open.restype = C.c_void_p
    lib.sf_readf_short.argtypes = [C.c_void_p, C.POINTER(C.c_int16), C.c_longlong]
    lib.sf_readf_short.restype = C.c_longlong
    lib.sf_close.argtypes = [C.c_void_p]
    results = []
    with tempfile.TemporaryDirectory(prefix='hvm-audio-corpus-') as temporary:
        for path in sorted(args.root.rglob('*.wav')):
            raw = path.read_bytes()
            if raw[:4] != b'RIFF' or raw[8:12] != b'WAVE':
                continue
            chunks = {}
            pos = 12
            while pos + 8 <= len(raw):
                size, = struct.unpack_from('<I', raw, pos+4)
                end = pos + 8 + size
                if end > len(raw):
                    raise ValueError(f'Truncated WAV: {path}')
                chunks[raw[pos:pos+4]] = raw[pos+8:end]
                pos = end + (size & 1)
            fmt = chunks.get(b'fmt ', b'')
            if len(fmt) < 2 or struct.unpack_from('<H', fmt)[0] != 17:
                continue
            info = Info()
            handle = lib.sf_open(str(path).encode(), 0x10, C.byref(info))
            if not handle:
                raise RuntimeError(f'Reference cannot open {path}')
            try:
                if info.channels not in (1, 2) or not 0 < info.frames <= 44100 * 60 * 30:
                    raise ValueError(f'Unexpected corpus dimensions: {path}')
                samples = (C.c_int16 * (info.frames * info.channels))()
                count = lib.sf_readf_short(handle, samples, info.frames)
                if count != info.frames:
                    raise RuntimeError(f'Reference short read: {path}')
            finally:
                lib.sf_close(handle)
            expected = bytes(samples) if info.channels == 2 else b''.join(struct.pack('=hh', x, x) for x in samples)
            fact = chunks.get(b'fact')
            frames = struct.unpack_from('<I', fact)[0] if fact else count
            expected = expected[:frames * 4]
            decoded = Path(temporary) / 'decoded.pcm'
            run = subprocess.run([str(args.hvm_tests.resolve()), 'audio-decode', str(path), str(decoded)],
                                 capture_output=True, text=True, check=True, timeout=30)
            actual = decoded.read_bytes()
            if actual != expected:
                raise AssertionError(f'PCM mismatch: {path} ({len(actual)} vs {len(expected)} bytes)')
            if f'rate={info.rate} channels={info.channels} frames={frames} ' not in run.stdout:
                raise AssertionError(f'Audio dimensions mismatch: {path}: {run.stdout}')
            results.append({'file': str(path.relative_to(args.root)), 'rate': info.rate,
                            'channels': info.channels, 'frames': frames, 'byteExact': True})
    if not results or (args.require_count is not None and len(results) != args.require_count):
        raise AssertionError(f'Unexpected corpus size: {len(results)}')
    args.out.write_text(json.dumps(results, indent=2) + '\n')
    print(f'{len(results)} IMA WAV files match libsndfile sample-for-sample')


if __name__ == '__main__':
    main()
