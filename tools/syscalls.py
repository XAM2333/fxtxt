"""Harvest CASIO OS syscall ids from a .g1a by looking at the trampoline literals.

The fx-9860/9750 syscall stub is
    mov.l  id,   r0       (literal pool entry A)
    mov.l  addr, r1       (literal pool entry B == 0x80010070)
    jmp    @r1
so every trampoline leaves the little-endian word 0x80010070 in the code and the
syscall id lands in the literal pool right next to it. We scan for that word and
report the neighbouring dwords.
"""
import io
import struct
import sys
from collections import Counter

sys.stdout.reconfigure(encoding="utf-8", errors="replace")

TRAMP = b"\x70\x00\x01\x80"


def load(path):
    with open(path, "rb") as f:
        return f.read()


def scan(path):
    data = load(path)
    ids = Counter()
    hits = 0
    i = data.find(TRAMP)
    while i >= 0:
        hits += 1
        for off in (-8, -4, 4, 8, 12):
            j = i + off
            if 0 <= j and j + 4 <= len(data):
                v = struct.unpack_from("<I", data, j)[0]
                if 0 < v < 0x2000:
                    ids[v] += 1
        i = data.find(TRAMP, i + 1)
    return len(data), hits, ids


def main():
    for path in sys.argv[1:]:
        size, hits, ids = scan(path)
        print("== %s  %d bytes, %d trampoline words" % (path, size, hits))
        for v, n in sorted(ids.items()):
            print("   id 0x%04x  x%d" % (v, n))


main()
