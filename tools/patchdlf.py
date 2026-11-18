"""Write repaired copies of the DLF fonts: the bracket glyphs are redrawn because
the originals are flush against the cell edges (openers right, closers left), which
makes them look detached with the font's own advance.

Usage: python tools/patchdlf.py [outdir]
"""
import os
import sys

sys.stdout.reconfigure(encoding="utf-8", errors="replace")
ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OUT = os.path.join(ROOT, sys.argv[1] if len(sys.argv) > 1 else "out", "fonts")

BASE = 0x24
ASCII_GLYPH0 = 188
FIX8 = {
    "(": [0x10, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x10],
    ")": [0x20, 0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x20],
    "[": [0x30, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x30],
    "]": [0x30, 0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x30],
    "{": [0x30, 0x20, 0x20, 0x40, 0x20, 0x20, 0x30, 0x00],
    "}": [0x30, 0x10, 0x10, 0x08, 0x10, 0x10, 0x30, 0x00],
    "<": [0x00, 0x10, 0x20, 0x40, 0x20, 0x10, 0x00, 0x00],
    ">": [0x00, 0x10, 0x08, 0x04, 0x08, 0x10, 0x00, 0x00],
}


def scale_to12(rows8):
    out = []
    for y in range(12):
        src = rows8[min(7, y * 8 // 12)]
        v = 0
        for x in range(12):
            sx = min(7, x * 8 // 12)
            if (src >> (7 - sx)) & 1:
                v |= 1 << (11 - x)
        out.append((v << 4) >> 8 & 0xFF)
        out.append((v << 4) & 0xFF)
    return out


def main():
    os.makedirs(OUT, exist_ok=True)
    for fn, stride in (("FONT8.dlf", 8), ("FONT12.dlf", 24)):
        data = bytearray(open(os.path.join(ROOT, fn), "rb").read())
        patched = 0
        for ch, g8 in FIX8.items():
            gi = ASCII_GLYPH0 + (ord(ch) - 0x21)
            off = BASE + gi * stride
            blob = g8 if stride == 8 else scale_to12(g8)
            data[off:off + stride] = bytes(blob)
            patched += 1
        dest = os.path.join(OUT, fn)
        open(dest, "wb").write(data)
        print("%-11s -> %s (%d glyphs repaired, %d bytes)" %
              (fn, dest, patched, len(data)))


if __name__ == "__main__":
    main()
