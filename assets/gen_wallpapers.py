#!/usr/bin/env python3
"""Generate Windows 11 style wallpapers for ElevenDE (pure stdlib, no PIL).

Outputs 1920x1080 PNGs:
  wallpaper-bloom.png   Win11-like deep blue "bloom" with soft radial light
  wallpaper-warm.png    warm sunrise gradient with a soft sun glow
  wallpaper-dark.png    minimal dark gradient with a faint accent glow
"""

import math
import os
import struct
import zlib

W, H = 1920, 1080
OUT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "wallpapers")


def write_png(path, rows):
    raw = bytearray()
    for row in rows:
        raw.append(0)  # filter type 0
        raw.extend(row)

    def chunk(tag, data):
        c = struct.pack(">I", len(data)) + tag + data
        c += struct.pack(">I", zlib.crc32(tag + data) & 0xFFFFFFFF)
        return c

    png = b"\x89PNG\r\n\x1a\n"
    png += chunk(b"IHDR", struct.pack(">IIBBBBB", W, H, 8, 2, 0, 0, 0))
    png += chunk(b"IDAT", zlib.compress(bytes(raw), 6))
    png += chunk(b"IEND", b"")
    with open(path, "wb") as f:
        f.write(png)


def clamp(v):
    return 0 if v < 0 else (255 if v > 255 else int(v))


def mix(a, b, t):
    return (a[0] + (b[0] - a[0]) * t,
            a[1] + (b[1] - a[1]) * t,
            a[2] + (b[2] - a[2]) * t)


def vgrad(top, bottom, y):
    return mix(top, bottom, y / (H - 1))


def bloom():
    """Deep blue Win11 bloom: vertical gradient + two soft radial glows."""
    top = (16, 32, 72)
    bottom = (6, 12, 34)
    # glow centers (normalized), color, radius
    glows = [
        (0.62, 0.42, (60, 130, 240), 0.62, 0.85),
        (0.30, 0.78, (90, 70, 200), 0.55, 0.55),
        (0.85, 0.85, (30, 90, 190), 0.50, 0.60),
    ]
    rows = []
    for y in range(H):
        row = bytearray(W * 3)
        gy = y / H
        base = vgrad(top, bottom, y)
        for x in range(W):
            gx = x / W
            r, g, b = base
            for cx, cy, col, rad, amp in glows:
                d = math.sqrt((gx - cx) ** 2 + ((gy - cy) * (H / W)) ** 2)
                t = 1.0 - d / rad
                if t > 0:
                    t = t * t * (3 - 2 * t)  # smoothstep
                    r += col[0] * t * amp
                    g += col[1] * t * amp
                    b += col[2] * t * amp
            row[x * 3] = clamp(r)
            row[x * 3 + 1] = clamp(g)
            row[x * 3 + 2] = clamp(b)
        rows.append(row)
    write_png(os.path.join(OUT, "wallpaper-bloom.png"), rows)
    print("wallpaper-bloom.png done")


def warm():
    """Sunrise gradient with a soft sun glow near the horizon."""
    top = (46, 16, 64)
    mid = (196, 84, 60)
    bottom = (255, 176, 92)
    sun = (0.68, 0.62, (255, 224, 150), 0.30, 0.9)
    rows = []
    for y in range(H):
        row = bytearray(W * 3)
        gy = y / H
        if gy < 0.55:
            base = mix(top, mid, gy / 0.55)
        else:
            base = mix(mid, bottom, (gy - 0.55) / 0.45)
        for x in range(W):
            gx = x / W
            r, g, b = base
            cx, cy, col, rad, amp = sun
            d = math.sqrt((gx - cx) ** 2 + ((gy - cy) * (H / W)) ** 2)
            t = 1.0 - d / rad
            if t > 0:
                t = t * t * (3 - 2 * t)
                r += col[0] * t * amp
                g += col[1] * t * amp
                b += col[2] * t * amp
            row[x * 3] = clamp(r)
            row[x * 3 + 1] = clamp(g)
            row[x * 3 + 2] = clamp(b)
        rows.append(row)
    write_png(os.path.join(OUT, "wallpaper-warm.png"), rows)
    print("wallpaper-warm.png done")


def dark():
    """Minimal dark wallpaper with a faint accent glow bottom-right."""
    top = (32, 32, 36)
    bottom = (16, 16, 20)
    glow = (0.80, 0.85, (0, 90, 160), 0.75, 0.35)
    rows = []
    for y in range(H):
        row = bytearray(W * 3)
        gy = y / H
        base = vgrad(top, bottom, y)
        for x in range(W):
            gx = x / W
            r, g, b = base
            cx, cy, col, rad, amp = glow
            d = math.sqrt((gx - cx) ** 2 + ((gy - cy) * (H / W)) ** 2)
            t = 1.0 - d / rad
            if t > 0:
                t = t * t * (3 - 2 * t)
                r += col[0] * t * amp
                g += col[1] * t * amp
                b += col[2] * t * amp
            row[x * 3] = clamp(r)
            row[x * 3 + 1] = clamp(g)
            row[x * 3 + 2] = clamp(b)
        rows.append(row)
    write_png(os.path.join(OUT, "wallpaper-dark.png"), rows)
    print("wallpaper-dark.png done")


if __name__ == "__main__":
    os.makedirs(OUT, exist_ok=True)
    bloom()
    warm()
    dark()
    print("all wallpapers generated in", OUT)
