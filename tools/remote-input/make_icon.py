#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Draws icon.png (1024x1024), the source of the application icon. Pure Python, no dependencies."""

import math
import struct
import sys
import zlib

N = 1024


def rrect(px, py, cx, cy, hw, hh, r):
    """Signed distance to a rounded rectangle (negative inside)."""
    dx = abs(px - cx) - (hw - r)
    dy = abs(py - cy) - (hh - r)
    return math.hypot(max(dx, 0), max(dy, 0)) + min(max(dx, dy), 0) - r


def cover(d):
    """Anti-aliased coverage from a signed distance, 1 px wide edge."""
    return max(0.0, min(1.0, 0.5 - d))


def mix(a, b, t):
    return tuple(a[i] + (b[i] - a[i]) * t for i in range(3))


def pixel(x, y):
    px, py = x + 0.5, y + 0.5
    # Apple style tile: 824 px rounded square centered in the 1024 canvas
    d = rrect(px, py, N / 2, N / 2, 412, 412, 185)
    a = cover(d)
    if a <= 0:
        return (0, 0, 0, 0)
    t = (py - 100) / 824.0
    col = mix((58, 92, 214), (24, 30, 74), max(0.0, min(1.0, t)))  # blue to deep indigo

    # mouse body (white), with the two button separators drawn in the tile color
    body = rrect(px, py, 512, 540, 175, 255, 150)
    col = mix(col, (245, 247, 252), cover(body))
    if body < 0:
        split = abs(py - 400) < 7 and py < 410 or (abs(px - 512) < 7 and py < 410)
        if split and py < 410:
            col = mix(col, (150, 160, 200), 1.0)
        elif abs(py - 410) < 7:
            col = mix(col, (150, 160, 200), 1.0)
    # capture indicator: green dot with a ring, top right of the mouse
    ring = abs(math.hypot(px - 690, py - 330) - 70) - 22
    col = mix(col, (30, 36, 90), cover(ring - 8) * (1 if math.hypot(px - 690, py - 330) < 110 else 0))
    dot = math.hypot(px - 690, py - 330) - 52
    col = mix(col, (66, 214, 118), cover(dot))
    return (int(col[0]), int(col[1]), int(col[2]), int(255 * a))


def png(path):
    raw = bytearray()
    for y in range(N):
        raw.append(0)
        for x in range(N):
            raw += bytes(pixel(x, y))

    def chunk(tag, data):
        c = struct.pack(">I", len(data)) + tag + data
        return c + struct.pack(">I", zlib.crc32(tag + data) & 0xFFFFFFFF)

    with open(path, "wb") as f:
        f.write(b"\x89PNG\r\n\x1a\n")
        f.write(chunk(b"IHDR", struct.pack(">IIBBBBB", N, N, 8, 6, 0, 0, 0)))
        f.write(chunk(b"IDAT", zlib.compress(bytes(raw), 9)))
        f.write(chunk(b"IEND", b""))


if __name__ == "__main__":
    png(sys.argv[1] if len(sys.argv) > 1 else "icon.png")
