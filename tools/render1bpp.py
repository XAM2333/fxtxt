"""Render a binary blob as a 1bpp glyph contact sheet into a PNG (no deps)."""
import struct
import sys
import zlib


def write_png(path, w, h, pix, scale=1):
    """pix: list of rows, each row list of 0/255 ints (grayscale)."""
    W, H = w * scale, h * scale
    raw = bytearray()
    for y in range(h):
        row = pix[y]
        if scale == 1:
            raw.append(0)
            raw.extend(row)
        else:
            for _ in range(scale):
                raw.append(0)
                line = bytearray()
                for v in row:
                    line.extend([v] * scale)
                raw.extend(line)

    def chunk(tag, data):
        c = struct.pack(">I", len(data)) + tag + data
        return c + struct.pack(">I", zlib.crc32(tag + data) & 0xFFFFFFFF)

    png = b"\x89PNG\r\n\x1a\n"
    png += chunk(b"IHDR", struct.pack(">IIBBBBB", W, H, 8, 0, 0, 0, 0))
    png += chunk(b"IDAT", zlib.compress(bytes(raw), 9))
    png += chunk(b"IEND", b"")
    open(path, "wb").write(png)


def sheet(blob, off, gw, gh, bpr, cols, maxglyphs, path, scale=4, gap=1):
    """gw,gh in pixels; bpr = bytes per glyph row; glyph stride = gh*bpr."""
    stride = gh * bpr
    n = min(maxglyphs, (len(blob) - off) // stride)
    rows = (n + cols - 1) // cols
    cw, ch = gw + gap, gh + gap
    W, H = cols * cw, rows * ch
    pix = [[255] * W for _ in range(H)]
    for gi in range(n):
        base = off + gi * stride
        gx = (gi % cols) * cw
        gy = (gi // cols) * ch
        for r in range(gh):
            rowbytes = blob[base + r * bpr: base + r * bpr + bpr]
            for byteidx, b in enumerate(rowbytes):
                for bit in range(8):
                    x = byteidx * 8 + bit
                    if x >= gw:
                        break
                    on = (b >> (7 - bit)) & 1
                    pix[gy + r][gx + x] = 0 if on else 255
    write_png(path, W, H, pix, scale)
    print("wrote %s  glyphs=%d  %dx%d" % (path, n, W, H))


if __name__ == "__main__":
    blob = open(sys.argv[1], "rb").read()
    off = int(sys.argv[2], 0)
    gw, gh, bpr, cols, n = (int(x) for x in sys.argv[3:8])
    scale = int(sys.argv[8]) if len(sys.argv) > 8 else 4
    sheet(blob, off, gw, gh, bpr, cols, n, sys.argv[9] if len(sys.argv) > 9 else "out.png", scale)
