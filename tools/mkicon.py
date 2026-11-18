"""Menu icon helper for FXTXT.

REQUIREMENT  (what Casio expects for the MAIN MENU icon)
  * canvas 30 x 19 pixels, black & white
  * the packer stores 30 x 17: the first and the last row are dropped, so keep
    the outer 1-pixel rows empty
  * any PNG/BMP works; it is thresholded to 1 bit at half brightness
  * the icon is shown at 1:1 on screen (very small), so use thick strokes and
    no fine detail or grey ramps

Usage
  python tools/mkicon.py --template         empty 30x19 template -> assets/icon.png
  python tools/mkicon.py --default          built-in page-with-text icon
  python tools/mkicon.py my_design.png      convert your image (any size) to the icon
  python tools/mkicon.py --show PATH        print an image as ASCII art
  python tools/mkicon.py --from-g1a F.g1a   print the 30x17 icon inside a .g1a

After changing the icon, rebuild the add-in and check the result with
`--from-g1a out/FXTXT.G1A`.
"""
import os
import sys

sys.path.insert(0, os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "pylib"))
from PIL import Image, ImageDraw  # noqa: E402

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
ICON = os.path.join(ROOT, "work", "fxtxt", "assets", "icon.png")
W, H = 30, 19

sys.stdout.reconfigure(encoding="utf-8", errors="replace")


def art(img, label=""):
    if label:
        print(label)
    im = img.convert("L")
    for y in range(im.height):
        print("  " + "".join("#" if im.getpixel((x, y)) < 128 else "." for x in range(im.width)))


def template():
    im = Image.new("L", (W, H), 255)
    d = ImageDraw.Draw(im)
    for x in range(0, W, 2):          # dashed border = rows/cols to keep free
        im.putpixel((x, 0), 160)
        im.putpixel((x, H - 1), 160)
    d.rectangle([1, 1, W - 2, H - 2], outline=200)
    im.save(ICON)
    print("template written to", ICON, "(30x19; grey = dropped rows, light grey = safe area)")
    art(im, "template:")


def default_icon():
    im = Image.new("L", (W, H), 255)
    d = ImageDraw.Draw(im)
    d.rectangle([5, 2, 24, 16], fill=255, outline=0)          # page
    d.polygon([(19, 2), (24, 2), (24, 7)], fill=255, outline=0)  # folded corner
    d.line([(19, 2), (19, 7)], fill=0)
    d.line([(19, 7), (24, 7)], fill=0)
    for i, y in enumerate((9, 11, 13)):                        # text lines
        d.line([(8, y), (21 - (2 if i == 2 else 0), y)], fill=0)
    d.line([(21, 13), (21, 15)], fill=0)                       # caret
    im.save(ICON)
    print("default icon written to", ICON)
    art(im, "icon (30x19; row 0 and row 18 are dropped when packing):")


def convert(path):
    """Accept any image; keep 30x19 / 30x17 designs pixel-exact, fit the rest."""
    raw = Image.open(path)
    # composite over white so transparent pixels are not treated as black
    if raw.mode in ("RGBA", "LA", "P"):
        raw = raw.convert("RGBA")
        bg = Image.new("RGBA", raw.size, (255, 255, 255, 255))
        g = Image.alpha_composite(bg, raw).convert("L")
    else:
        g = raw.convert("L")

    canvas = Image.new("L", (W, H), 255)
    if g.size == (W, H):                 # already 30x19
        canvas.paste(g, (0, 0))
    elif g.size == (W, H - 2):           # already the stored 30x17: pad 1 row each side
        canvas.paste(g, (0, 1))
    else:
        g.thumbnail((W - 2, H - 2), Image.LANCZOS)
        canvas.paste(g, ((W - g.width) // 2, (H - g.height) // 2))

    bw = canvas.point(lambda v: 0 if v < 128 else 255)
    bw.save(ICON)
    print("icon written to", ICON, "(from %s %dx%d)" % (os.path.basename(path), raw.width, raw.height))
    art(bw, "full 30x19 canvas (row 0 and row 18 are dropped when packing):")
    print("\nwhat the menu will show (30x17):")
    for y in range(1, H - 1):
        print("  " + "".join("#" if bw.getpixel((x, y)) < 128 else "." for x in range(W)))


def show(path):
    im = Image.open(path).convert("L")
    art(im, "%s  %dx%d" % (path, im.width, im.height))


def from_g1a(path):
    b = open(path, "rb").read()
    data = b[0x4C:0x4C + 68]
    print("%s: %d bytes at 0x4C (30x17 stored)" % (path, len(data)))
    for y in range(17):
        row = data[y * 4:y * 4 + 4]
        print("  " + "".join("#" if (row[x >> 3] >> (7 - (x & 7))) & 1 else "."
                             for x in range(30)))


# 3x5 bitmap font for the launch-key badge (A-Z, 0-9)
GLYPHS = {
    "A": ".#. #.# ### #.# #.#", "B": "##. #.# ##. #.# ##.",
    "C": ".## #.. #.. #.. .##", "D": "##. #.# #.# #.# ##.",
    "E": "### #.. ##. #.. ###", "F": "### #.. ##. #.. #..",
    "G": ".## #.. #.# #.# .##", "H": "#.# #.# ### #.# #.#",
    "I": "### .#. .#. .#. ###", "J": "..# ..# ..# #.# .#.",
    "K": "#.# #.# ##. #.# #.#", "L": "#.. #.. #.. #.. ###",
    "M": "#.# ### ### #.# #.#", "N": "##. #.# #.# #.# #.#",
    "O": ".#. #.# #.# #.# .#.", "P": "##. #.# ##. #.. #..",
    "Q": ".#. #.# #.# ##. .##", "R": "##. #.# ##. #.# #.#",
    "S": ".## #.. .#. ..# ##.", "T": "### .#. .#. .#. .#.",
    "U": "#.# #.# #.# #.# ###", "V": "#.# #.# #.# #.# .#.",
    "W": "#.# #.# ### ### #.#", "X": "#.# #.# .#. #.# #.#",
    "Y": "#.# #.# .#. .#. .#.", "Z": "### ..# .#. #.. ###",
    "0": "### #.# #.# #.# ###", "1": ".#. ##. .#. .#. ###",
    "2": "##. ..# .#. #.. ###", "3": "##. ..# .#. ..# ##.",
    "4": "#.# #.# ### ..# ..#", "5": "### #.. ##. ..# ##.",
    "6": ".## #.. ### #.# ###", "7": "### ..# .#. .#. .#.",
    "8": "### #.# ### #.# ###", "9": "### #.# ### ..# ##.",
}

BADGE_BOX = (23, 10, 29, 16)      # x0, y0, x1, y1 on the 30x17 stored area


def draw_badge(im, ch):
    """Black box in the bottom-right corner with the launch key in white."""
    x0, y0, x1, y1 = BADGE_BOX
    d = ImageDraw.Draw(im)
    d.rectangle([x0, y0, x1, y1], fill=0)
    pat = GLYPHS.get(ch.upper())
    if not pat:
        return
    rows = pat.split()
    ox = x0 + (x1 - x0 + 1 - 3) // 2
    oy = y0 + (y1 - y0 + 1 - 5) // 2
    for ry, row in enumerate(rows):
        for rx, c in enumerate(row):
            if c == "#":
                im.putpixel((ox + rx, oy + ry), 255)


def to_bits(path=None):
    """Dump the current icon as editable text art (30x19, '#' = black)."""
    src = path or ICON
    im = Image.open(src).convert("L")
    canvas = Image.new("L", (W, H), 255)
    canvas.paste(im, (0, 0))
    txt = "\n".join("".join("#" if canvas.getpixel((x, y)) < 128 else "."
                            for x in range(W)) for y in range(H)) + "\n"
    out = os.path.join(ROOT, "work", "fxtxt", "assets", "icon.txt")
    with open(out, "w", encoding="utf-8") as f:
        f.write(txt)
    print("text art written to", out)
    print("edit the # / . grid, then: python tools/mkicon.py --bits %s" % out)
    print()
    print(txt)


def parse_bits(path):
    """Read a # / . grid (17..19 rows, 30 cols) into a 30x19 image."""
    rows = []
    for line in open(path, encoding="utf-8"):
        line = line.rstrip("\r\n")
        if not line or line.lstrip().startswith(";"):
            continue
        bits = "".join("#" if c in "#oO1" else "." for c in line
                       if c in "#.*oO10_ ")
        if bits:
            rows.append(bits[:W].ljust(W, "."))
    if not rows:
        raise SystemExit("no pixel rows found in " + path)
    if len(rows) == 17:                  # stored area: re-add the dropped rows
        rows = ["." * W] + rows + ["." * W]
    rows = rows[:H]
    while len(rows) < H:
        rows.append("." * W)
    im = Image.new("L", (W, H), 255)
    for y, row in enumerate(rows):
        for x, c in enumerate(row):
            if c == "#":
                im.putpixel((x, y), 0)
    return im


def patch_g1a(path, im):
    """Write the 30x17 menu icon straight into an assembled .g1a (no rebuild)."""
    b = bytearray(open(path, "rb").read())
    if len(b) < 0x90:
        raise SystemExit("not a .g1a")
    data = bytearray(68)
    for y in range(17):
        src_y = y + 1                    # canvas row 0 is dropped by the packer
        for x in range(30):
            if im.getpixel((x, src_y)) < 128:
                data[y * 4 + (x >> 3)] |= 0x80 >> (x & 7)
    b[0x4C:0x4C + 68] = data
    open(path, "wb").write(b)
    print("patched icon directly into", path)
    from_g1a(path)


def bits_to_icon(path, g1a=None):
    im = parse_bits(path)
    im.save(ICON)
    print("icon written to", ICON)
    art(im, "icon (30x19):")
    if g1a:
        patch_g1a(g1a, im)


def main():
    a = sys.argv[1:]
    if not a or a[0] in ("--help", "-h"):
        print(__doc__)
    elif a[0] == "--template":
        template()
    elif a[0] == "--default":
        default_icon()
    elif a[0] == "--show":
        show(a[1])
    elif a[0] == "--from-g1a":
        from_g1a(a[1])
    elif a[0] == "--to-bits":
        to_bits(a[1] if len(a) > 1 else None)
    elif a[0] == "--bits":
        g1a = a[a.index("--g1a") + 1] if "--g1a" in a else None
        bits_to_icon(a[1], g1a)
    elif a[0] == "--badge":
        src = a[2] if len(a) > 2 else ICON
        convert(src) if src != ICON else None
        im = Image.open(ICON).convert("L")
        draw_badge(im, a[1])
        im.save(ICON)
        art(im, "badge '%s' drawn:" % a[1])
    else:
        convert(a[0])


if __name__ == "__main__":
    main()
