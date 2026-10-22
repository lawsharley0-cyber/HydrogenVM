"""PNG framebuffer signals without a third-party image library.

HydrogenVM screenshots are 8-bit non-interlaced RGBA (color type 6), filter
byte 0 per row, from ``Graphics::encodePng``. The decoder also accepts filter
types 1–4 so a corrupt or foreign PNG fails closed instead of being treated
as a varied frame.
"""
from __future__ import annotations

import os
import struct
import zlib

_SIG = b"\x89PNG\r\n\x1a\n"


def summarize_png(path: str) -> dict:
    try:
        with open(path, "rb") as f:
            data = f.read()
    except OSError as e:
        return {"signal": "unreadable", "error": str(e)}
    try:
        w, h, rgba = decode_png_rgba(data)
    except ValueError as e:
        return {"signal": "unreadable", "error": str(e)}
    return summarize_rgba(w, h, rgba)


def decode_png_rgba(data: bytes) -> tuple[int, int, bytes]:
    if len(data) < 8 or data[:8] != _SIG:
        raise ValueError("not a PNG")
    pos = 8
    width = height = None
    color_type = interlace = None
    idat = bytearray()
    while pos + 12 <= len(data):
        length = struct.unpack(">I", data[pos:pos + 4])[0]
        pos += 4
        ctype = data[pos:pos + 4]
        pos += 4
        if length < 0 or pos + length + 4 > len(data):
            raise ValueError("truncated PNG chunk")
        body = data[pos:pos + length]
        pos += length + 4  # skip CRC; we only need the pixels for a signal
        if ctype == b"IHDR":
            if length < 13:
                raise ValueError("short IHDR")
            width, height, depth, color_type, comp, filt, interlace = struct.unpack(">IIBBBBB", body[:13])
            if depth != 8 or color_type != 6 or comp != 0 or filt != 0:
                raise ValueError("only 8-bit RGBA PNGs are read")
            if interlace != 0:
                raise ValueError("interlaced PNG is not read")
        elif ctype == b"IDAT":
            idat.extend(body)
        elif ctype == b"IEND":
            break
    if not width or not height:
        raise ValueError("PNG has no IHDR")
    if width > 8192 or height > 8192:
        raise ValueError("PNG dimensions are unreasonably large")
    try:
        raw = zlib.decompress(bytes(idat))
    except zlib.error as e:
        raise ValueError(f"PNG inflate failed: {e}") from e
    row_bytes = width * 4
    need = (row_bytes + 1) * height
    if len(raw) < need:
        raise ValueError("PNG image data is short")
    out = bytearray(width * height * 4)
    prev = bytearray(row_bytes)
    src = 0
    for y in range(height):
        ft = raw[src]
        src += 1
        row = bytearray(raw[src:src + row_bytes])
        src += row_bytes
        if ft > 4:
            raise ValueError(f"unknown PNG filter {ft}")
        for i in range(row_bytes):
            a = row[i - 4] if i >= 4 else 0
            b = prev[i]
            c = prev[i - 4] if i >= 4 else 0
            if ft == 1:
                row[i] = (row[i] + a) & 0xFF
            elif ft == 2:
                row[i] = (row[i] + b) & 0xFF
            elif ft == 3:
                row[i] = (row[i] + ((a + b) >> 1)) & 0xFF
            elif ft == 4:
                row[i] = (row[i] + _paeth(a, b, c)) & 0xFF
        prev = row
        out[y * row_bytes:(y + 1) * row_bytes] = row
    return width, height, bytes(out)


def summarize_rgba(width: int, height: int, rgba: bytes) -> dict:
    n = width * height
    if n == 0 or len(rgba) < n * 4:
        return {"signal": "unreadable", "error": "empty image", "width": width, "height": height}
    # About an 80-column sample. Stride keeps the signal stable on large frames.
    stride_x = max(1, width // 80)
    stride_y = max(1, height // 120)
    black = white = other = 0
    quant: dict[tuple[int, int, int], int] = {}
    sampled = 0
    for y in range(0, height, stride_y):
        row = y * width * 4
        for x in range(0, width, stride_x):
            i = row + x * 4
            r, g, b = rgba[i], rgba[i + 1], rgba[i + 2]
            sampled += 1
            if r < 16 and g < 16 and b < 16:
                black += 1
            elif r > 240 and g > 240 and b > 240:
                white += 1
            else:
                other += 1
            key = (r >> 4, g >> 4, b >> 4)
            quant[key] = quant.get(key, 0) + 1
    if sampled == 0:
        return {"signal": "unreadable", "error": "no samples", "width": width, "height": height}
    top = max(quant.values())
    dominant = top / sampled
    black_f = black / sampled
    white_f = white / sampled
    other_f = other / sampled
    if black_f >= 0.97:
        signal = "all-black"
    elif white_f >= 0.97:
        signal = "all-white"
    elif dominant >= 0.90:
        signal = "mostly-one-color"
    else:
        signal = "varied"
    return {
        "signal": signal,
        "width": width,
        "height": height,
        "sampled": sampled,
        "blackFraction": round(black_f, 4),
        "whiteFraction": round(white_f, 4),
        "otherFraction": round(other_f, 4),
        "uniqueQuantizedColors": len(quant),
        "dominantShare": round(dominant, 4),
    }


def _paeth(a: int, b: int, c: int) -> int:
    p = a + b - c
    pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
    if pa <= pb and pa <= pc:
        return a
    if pb <= pc:
        return b
    return c


def encode_rgba_png(width: int, height: int, rgba: bytes) -> bytes:
    """Filter-0 RGBA PNG. Used by harness tests and not by the emulator."""
    if len(rgba) != width * height * 4:
        raise ValueError("rgba size does not match dimensions")
    raw = bytearray()
    row = width * 4
    for y in range(height):
        raw.append(0)
        raw.extend(rgba[y * row:(y + 1) * row])
    ihdr = struct.pack(">IIBBBBB", width, height, 8, 6, 0, 0, 0)
    return _SIG + _chunk(b"IHDR", ihdr) + _chunk(b"IDAT", zlib.compress(bytes(raw))) + _chunk(b"IEND", b"")


def _chunk(tag: bytes, body: bytes) -> bytes:
    crc = zlib.crc32(tag + body) & 0xFFFFFFFF
    return struct.pack(">I", len(body)) + tag + body + struct.pack(">I", crc)


def png_exists(path: str) -> bool:
    return bool(path) and os.path.isfile(path) and os.path.getsize(path) > 0
