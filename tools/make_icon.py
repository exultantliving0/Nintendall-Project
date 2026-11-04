#!/usr/bin/env python3
"""Generate assets/nintendall.ico (and a 256px PNG preview).

The icon is a red rounded square with a white pixel-art "N" taken from
Nintendall's built-in font (src/font.cpp). Pure Python, no image libraries.

usage: python3 tools/make_icon.py [output_dir]
"""
import os
import struct
import sys
import zlib

# The "N" glyph from src/font.cpp (5 x 7 pixels).
GLYPH = ["#...#", "#...#", "##..#", "#.#.#", "#..##", "#...#", "#...#"]
RED = (232, 48, 64)
WHITE = (255, 255, 255)
SIZES = [16, 24, 32, 48, 64, 128, 256]
SS = 4  # supersampling for smooth edges


def render(size):
    """Return size*size RGBA pixels (rows top to bottom)."""
    hi = size * SS
    radius = hi * 0.22
    cell = hi * 0.62 / 7  # glyph height = 62% of the icon
    gx = (hi - cell * 5) / 2
    gy = (hi - cell * 7) / 2

    def sample(x, y):
        # Rounded square mask.
        cx = min(max(x, radius), hi - radius)
        cy = min(max(y, radius), hi - radius)
        if (x - cx) ** 2 + (y - cy) ** 2 > radius ** 2:
            return None
        col = int((x - gx) // cell)
        row = int((y - gy) // cell)
        if 0 <= row < 7 and 0 <= col < 5 and x >= gx and y >= gy and GLYPH[row][col] == "#":
            return WHITE
        return RED

    out = []
    for py in range(size):
        for px in range(size):
            r = g = b = a = 0
            for sy in range(SS):
                for sx in range(SS):
                    c = sample(px * SS + sx + 0.5, py * SS + sy + 0.5)
                    if c:
                        r += c[0]; g += c[1]; b += c[2]; a += 1
            n = SS * SS
            out.append((r // a if a else 0, g // a if a else 0, b // a if a else 0, a * 255 // n))
    return out


def bmp_entry(size, px):
    """Icon image as a 32-bit DIB (BITMAPINFOHEADER + BGRA bottom-up + AND mask)."""
    header = struct.pack("<IiiHHIIiiII", 40, size, size * 2, 1, 32, 0, 0, 0, 0, 0, 0)
    rows = b""
    for y in range(size - 1, -1, -1):
        rows += b"".join(struct.pack("BBBB", p[2], p[1], p[0], p[3]) for p in px[y * size:(y + 1) * size])
    mask_row = ((size + 31) // 32) * 4
    return header + rows + bytes(mask_row * size)


def png(size, px):
    raw = b"".join(b"\0" + b"".join(bytes(p) for p in px[y * size:(y + 1) * size]) for y in range(size))

    def chunk(t, d):
        return struct.pack(">I", len(d)) + t + d + struct.pack(">I", zlib.crc32(t + d) & 0xFFFFFFFF)

    return (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", size, size, 8, 6, 0, 0, 0))
            + chunk(b"IDAT", zlib.compress(raw, 9)) + chunk(b"IEND", b""))


def main():
    out_dir = sys.argv[1] if len(sys.argv) > 1 else os.path.join(os.path.dirname(__file__), "..", "assets")
    os.makedirs(out_dir, exist_ok=True)
    images = {s: render(s) for s in SIZES}
    entries = [bmp_entry(s, images[s]) for s in SIZES]
    ico = struct.pack("<HHH", 0, 1, len(SIZES))
    offset = 6 + 16 * len(SIZES)
    for s, data in zip(SIZES, entries):
        ico += struct.pack("<BBBBHHII", s % 256, s % 256, 0, 0, 1, 32, len(data), offset)
        offset += len(data)
    ico += b"".join(entries)
    with open(os.path.join(out_dir, "nintendall.ico"), "wb") as f:
        f.write(ico)
    with open(os.path.join(out_dir, "nintendall-256.png"), "wb") as f:
        f.write(png(256, images[256]))
    print("wrote", os.path.join(out_dir, "nintendall.ico"))


if __name__ == "__main__":
    main()
