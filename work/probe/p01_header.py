"""Probe 01 - G1A container / add-in header of PROB.g1a and friends.

Goal: decide whether PROB.g1a is a normal add-in (code at 0x200 -> 0x00300000),
what its internal name is, and dump any header strings.
"""
import hashlib
import struct
import sys

sys.stdout.reconfigure(encoding="utf-8", errors="replace")

FILES = ["E:/fxtxtviewer/PROB.g1a",
         "E:/fxtxtviewer/EREADER.G1A",
         "E:/fxtxtviewer/fxTap.g1a",
         "E:/fxtxtviewer/DL22P.G1A"]


def u32le(b, o):
    return struct.unpack_from("<I", b, o)[0]


def u32be(b, o):
    return struct.unpack_from(">I", b, o)[0]


def hexdump(b, start, n, width=16):
    out = []
    for o in range(start, start + n, width):
        chunk = b[o:o + width]
        hx = " ".join("%02x" % c for c in chunk)
        asc = "".join(chr(c) if 32 <= c < 127 else "." for c in chunk)
        out.append("%08x  %-*s  %s" % (o, width * 3 - 1, hx, asc))
    return "\n".join(out)


def strings(b, minlen=4, lo=0, hi=None):
    hi = len(b) if hi is None else hi
    res = []
    cur = bytearray()
    curstart = 0
    for o in range(lo, hi):
        c = b[o]
        if 32 <= c < 127:
            if not cur:
                curstart = o
            cur.append(c)
        else:
            if len(cur) >= minlen:
                res.append((curstart, cur.decode("ascii")))
            cur = bytearray()
    if len(cur) >= minlen:
        res.append((curstart, cur.decode("ascii")))
    return res


def ustrings(b, minlen=4, lo=0, hi=None):
    """UTF-16LE runs (Casio add-in names are UTF-16LE in the add-in header)."""
    hi = len(b) if hi is None else hi
    res = []
    cur = bytearray()
    curstart = 0
    o = lo
    while o + 1 < hi:
        c, z = b[o], b[o + 1]
        if z == 0 and 32 <= c < 127:
            if not cur:
                curstart = o
            cur.append(c)
            o += 2
        else:
            if len(cur) >= minlen:
                res.append((curstart, cur.decode("ascii")))
            cur = bytearray()
            o += 2
    if len(cur) >= minlen:
        res.append((curstart, cur.decode("ascii")))
    return res


for path in FILES:
    b = open(path, "rb").read()
    print("=" * 78)
    print(path, " size=0x%x (%d)" % (len(b), len(b)))
    print("sha256", hashlib.sha256(b).hexdigest())
    print("first 32 bytes (container header):")
    print(hexdump(b, 0, 32))
    print("u32le @0x00..0x1c:", " ".join("%08x" % u32le(b, o) for o in range(0, 32, 4)))

    ah = 0x20  # add-in header
    print("\nadd-in header dwords (file 0x20..0x40):")
    print("  ", " ".join("%08x" % u32le(b, ah + o) for o in range(0, 32, 4)))
    print("add-in header hexdump:")
    print(hexdump(b, ah, 0x60))

    # Casio add-in header (48 bytes) then a 0x180 bytes "preview" area? Print name fields.
    print("\nstrings (ascii>=4) in 0x00..0x200:")
    for o, s in strings(b, 4, 0, 0x200):
        print("   %06x  %r" % (o, s))
    print("utf16le strings (>=4) in 0x00..0x200:")
    for o, s in ustrings(b, 4, 0, 0x200):
        print("   %06x  %r" % (o, s))
    print("\nfirst 12 ascii strings anywhere:")
    for o, s in strings(b, 8)[:12]:
        print("   %06x  %r" % (o, s))
    # where does the code plausibly begin?  look for 0x80010070 literal
    print("\n0x80010070 occurrences (both endian):",
          [hex(i) for i in range(len(b) - 4)
           if b[i:i + 4] in (b"\x70\x00\x01\x80", b"\x80\x01\x00\x70")])
