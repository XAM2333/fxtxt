"""Render glyph-sheet hypotheses for the DLF font, including transposed layouts."""
import sys
sys.path.insert(0, "tools")
from render1bpp import write_png

sys.stdout.reconfigure(encoding="utf-8", errors="replace")


def draw(sheet_w, sheet_h, pix, path, scale):
    write_png(path, sheet_w, sheet_h, pix, scale)


def sheet(blob, off, gw, gh, stride, mode, bits, cols, n, path, scale=4, gap=1):
    """mode: 'row' (bytes run across a row) or 'col' (bytes run down a column).
    bits: 8 or 16 bits per stored unit."""
    unit = bits // 8
    cw, ch = gw + gap, gh + gap
    rows = (n + cols - 1) // cols
    W, H = cols * cw, rows * ch
    pix = [[255] * W for _ in range(H)]
    for gi in range(n):
        base = off + gi * stride
        gx, gy = (gi % cols) * cw, (gi // cols) * ch
        for i in range(gh if mode == "row" else gw):
            for u in range(unit):
                j = i * unit + u
                v = blob[base + j]
                for bit in range(8):
                    on = (v >> (7 - bit)) & 1
                    if mode == "row":
                        x, y = u * 8 + bit, i
                    else:
                        x, y = i, u * 8 + bit
                    if x < gw and y < gh:
                        pix[gy + y][gx + x] = 0 if on else 255
    draw(W, H, pix, path, scale)
    print("wrote %s (mode=%s bits=%d gw=%d gh=%d stride=%d) %dx%d" %
          (path, mode, bits, gw, gh, stride, W, H))


if __name__ == "__main__":
    fn, off = sys.argv[1], int(sys.argv[2], 0)
    blob = open(fn, "rb").read()
    gw, gh, stride = (int(sys.argv[3]), int(sys.argv[4]), int(sys.argv[5]))
    mode, bits = sys.argv[6], int(sys.argv[7])
    cols, n = int(sys.argv[8]), int(sys.argv[9])
    outp = sys.argv[10]
    scale = int(sys.argv[11]) if len(sys.argv) > 11 else 4
    sheet(blob, off, gw, gh, stride, mode, bits, cols, n, outp, scale)
