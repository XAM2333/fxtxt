#!/usr/bin/env python3
"""Convert the FXTXT word list between the plain text form and the binary
form the calculator stores.

    python tools/ime_conv.py to-bin  words.txt FXTXT.IME_1
    python tools/ime_conv.py to-text FXTXT.IME_1 words.txt

There is a GUI for the same job: tools/ime_conv_gui.py

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
ALLOC = 512


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
        raise ValueError("cannot encode byte 0x%02X in %r" % (b, text))
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


def parse_text(raw):
    """Text form bytes -> (chars, words)."""
    chars, words, sect = [], [], None
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
                raise ValueError("[chars] entry must be one character: %r" % line)
            chars.append(cells[0])
        elif sect == "words":
            cells = encode_text(line)
            if not 0 < len(cells) <= MAX_WORD_LEN:
                raise ValueError("word too long (%d cells): %r" % (len(cells), line))
            words.append(cells)
        else:
            raise ValueError("entry outside [chars]/[words]: %r" % line)
    return chars, words


def render_text(chars, words):
    lines = ["[chars]"]
    lines += [decode_cells([c]) for c in chars]
    lines.append("[words]")
    lines += [decode_cells(w) for w in words]
    return ("\n".join(lines) + "\n").encode("cp936")


def build_bin(chars, words):
    chars = list(chars)[:MAX_CHARS]
    words = list(words)[:MAX_WORDS]
    blob = bytearray(MAGIC)
    blob.append(len(chars))
    for c in chars:
        blob += struct.pack("<H", c)
    blob.append(len(words))
    for w in words:
        w = list(w)[:MAX_WORD_LEN]
        blob.append(len(w))
        for c in w:
            blob += struct.pack("<H", c)
    return blob


def parse_bin(blob):
    if not blob.startswith(MAGIC):
        raise ValueError("not a FIM1 word list")
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
    return chars, words


def read_file(path):
    with open(path, "rb") as f:
        return f.read()


def load_any(path):
    """Load either form: FIM1 header means binary, anything else text."""
    raw = read_file(path)
    if raw.startswith(MAGIC):
        return parse_bin(raw)
    return parse_text(raw)


def save_bin(path, chars, words):
    blob = build_bin(chars, words)
    with open(path, "wb") as f:
        f.write(blob)
        f.write(b"\0" * (ALLOC - len(blob)))
    return len(blob)


def save_text(path, chars, words):
    raw = render_text(chars, words)
    with open(path, "wb") as f:
        f.write(raw)
    return len(raw)


def main(argv):
    if len(argv) != 4 or argv[1] not in ("to-bin", "to-text"):
        print(__doc__)
        return 2
    src, dst = argv[2], argv[3]
    try:
        chars, words = load_any(src)
        if len(chars) > MAX_CHARS:
            print("warning: %d characters, the calculator keeps %d"
                  % (len(chars), MAX_CHARS))
        if len(words) > MAX_WORDS:
            print("warning: %d words, the calculator keeps %d"
                  % (len(words), MAX_WORDS))
        if argv[1] == "to-bin":
            n = save_bin(dst, chars, words)
            print("%s -> %s : %d chars, %d words, %d bytes of data"
                  % (src, dst, len(chars), len(words), n))
        else:
            n = save_text(dst, chars, words)
            print("%s -> %s : %d chars, %d words, %d bytes of text"
                  % (src, dst, len(chars), len(words), n))
    except (ValueError, OSError) as e:
        raise SystemExit("%s: %s" % (src, e))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
