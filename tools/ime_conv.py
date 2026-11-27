#!/usr/bin/env python3
"""Convert the FXTXT word list between the plain text form and the binary
form the calculator stores.

    python tools/ime_conv.py to-bin words.txt FXTXT.IME_1
    python tools/ime_conv.py to-text FXTXT.IME_1 words.txt

The binary file is what the add-in writes; the text file is for editing on a
PC.  Any file matching ``*.IME_*`` in ``\\fls0`` is picked up, so the suffix
after the underscore is free (``FXTXT.IME_1``, ``FXTXT.IME_mine`` ...).

Text form

    # comment
    [chars]
    的
    一
    [words]
    您好
    谢谢

Binary form (little endian)

    "FIM1"            4 bytes
    nchars            1 byte
    cells[nchars]     2 bytes each
    nwords            1 byte
    per word: len (1 byte) + cells[len] (2 bytes each)

A cell is the calculator's display code: printable ASCII maps to
``byte - 0x20``, a GB2312 character to ``0x100 + (hi - 0xA1) * 94 + lo - 0xA1``.
"""

import struct
import sys

CELL_CJK = 0x100
MAX_CHARS = 24
MAX_WORDS = 8
MAX_WORD_LEN = 12
MAGIC = b"FIM1"


def encode_text(text):
    """GB2312 text -> list of display cells."""
    cells = []
    data = text.encode("cp936")
    i = 0
    while i < len(data):
        b = data[i]
        if 0xA1 <= b <= 0xF7 and i + 1 < len(data):
            lo = data[i + 1]
            if 0xA1 <= lo <= 0xFE:
                cells.append(CELL_CJK + (b - 0xA1) * 94 + (lo - 0xA1))
                i += 2
                continue
        if 0x20 < b < 0x7F:
            cells.append(b - 0x20)
            i += 1
            continue
        raise SystemExit("cannot encode byte 0x%02X in %r" % (b, text))
    return cells


def decode_cells(cells):
    out = bytearray()
    for c in cells:
        if c < CELL_CJK:
            out.append(c + 0x20)
        else:
            g = c - CELL_CJK
            out.append(0xA1 + g // 94)
            out.append(0xA1 + g % 94)
    return out.decode("cp936")


def read_text(path):
    chars, words, sect = [], [], None
    with open(path, "rb") as f:
        raw = f.read()
    for line in raw.decode("cp936").splitlines():
        line = line.strip()
        if not line or line.startswith("#"):
            continue
        low = line.lower()
        if low == "[chars]":
            sect = "chars"
            continue
        if low == "[words]":
            sect = "words"
            continue
        if sect == "chars":
            cells = encode_text(line)
            if len(cells) != 1:
                raise SystemExit("[chars] entry must be one character: %r" % line)
            chars.append(cells[0])
        elif sect == "words":
            cells = encode_text(line)
            if not 0 < len(cells) <= MAX_WORD_LEN:
                raise SystemExit("word too long (%d cells): %r" % (len(cells), line))
            words.append(cells)
        else:
            raise SystemExit("entry outside [chars]/[words]: %r" % line)
    return chars, words


def to_bin(text_path, bin_path):
    chars, words = read_text(text_path)
    if len(chars) > MAX_CHARS:
        print("warning: %d characters, the calculator keeps %d" % (len(chars), MAX_CHARS))
    if len(words) > MAX_WORDS:
        print("warning: %d words, the calculator keeps %d" % (len(words), MAX_WORDS))
    chars = chars[:MAX_CHARS]
    words = words[:MAX_WORDS]
    blob = bytearray(MAGIC)
    blob.append(len(chars))
    for c in chars:
        blob += struct.pack("<H", c)
    blob.append(len(words))
    for w in words:
        blob.append(len(w))
        for c in w:
            blob += struct.pack("<H", c)
    with open(bin_path, "wb") as f:
        f.write(blob)
        f.write(b"\0" * (512 - len(blob)))
    print("%s -> %s : %d chars, %d words, %d bytes of data"
          % (text_path, bin_path, len(chars), len(words), len(blob)))


def to_text(bin_path, text_path):
    with open(bin_path, "rb") as f:
        blob = f.read()
    if not blob.startswith(MAGIC):
        raise SystemExit("%s: not a FIM1 word list" % bin_path)
    p = 4
    nchars = blob[p]
    p += 1
    chars = list(struct.unpack_from("<%dH" % nchars, blob, p))
    p += 2 * nchars
    nwords = blob[p]
    p += 1
    words = []
    for _ in range(nwords):
        n = blob[p]
        p += 1
        words.append(list(struct.unpack_from("<%dH" % n, blob, p)))
        p += 2 * n
    lines = ["[chars]"]
    lines += [decode_cells([c]) for c in chars]
    lines.append("[words]")
    lines += [decode_cells(w) for w in words]
    with open(text_path, "wb") as f:
        f.write(("\n".join(lines) + "\n").encode("cp936"))
    print("%s -> %s : %d chars, %d words" % (bin_path, text_path, len(chars), len(words)))


def main(argv):
    if len(argv) != 4 or argv[1] not in ("to-bin", "to-text"):
        print(__doc__)
        return 2
    if argv[1] == "to-bin":
        to_bin(argv[2], argv[3])
    else:
        to_text(argv[2], argv[3])
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
