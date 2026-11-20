"""p13_delta.py -- test whether payload pointer literals are systematically
offset relative to the code/data layout (i.e. the file was shifted/padded).

Method: collect every 4-byte-aligned literal-pool dword whose value looks like
a payload pointer (0x300000..0x30ffff).  For each candidate delta d in
-64..+64 count how many of those pointers land on an ASCII-printable run of
length >= 6 when read at (ptr + d).  A working image should show a huge spike at
d = 0 (string pointers, function starts on code); a shifted image shows the
spike at d != 0.

Usage: python p13_delta.py <g1a> [...]
"""
import sys, array
import sh4lib

BASE = sh4lib.BASE
CODE_OFF = sh4lib.CODE_OFF


def printable_run(data, off, need=6):
    if off < 0 or off + need > len(data):
        return False
    for i in range(need):
        c = data[off + i]
        if not (0x20 <= c <= 0x7e):
            return False
    return True


def starts_text(data, off, need=6):
    """printable run that is preceded by a non-printable byte (string start)."""
    if not printable_run(data, off, need):
        return False
    if off > 0 and 0x20 <= data[off - 1] <= 0x7e:
        return False
    return True


def main(paths):
    for p in paths:
        img = sh4lib.Image(p)
        data = img.data
        ptrs = []
        for va in range(BASE, BASE + len(data) - CODE_OFF, 4):
            v = img.u32(va)
            if BASE <= v < BASE + len(data) - CODE_OFF and (v - BASE) % 2 == 0:
                ptrs.append((va, v))
        hist = {}
        for va, v in ptrs:
            for d in range(-64, 65):
                off = CODE_OFF + (v + d - BASE)
                if starts_text(data, off):
                    hist[d] = hist.get(d, 0) + 1
        top = sorted(hist.items(), key=lambda kv: -kv[1])[:6]
        print("%-40s ptrs=%4d  best deltas: %s" % (
            p.split("\\")[-1], len(ptrs),
            ", ".join("d=%+d:%d" % kv for kv in top)))
        # also: how many pointers point at a long printable run at all (d=0)
        print("    d=0 text-start hits: %d / %d" % (hist.get(0, 0), len(ptrs)))


if __name__ == "__main__":
    main(sys.argv[1:])
