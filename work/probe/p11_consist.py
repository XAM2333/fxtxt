#!/usr/bin/env python
"""Consistency check: do literal-pool pointers agree with the code layout?

For every 4-byte-aligned dword whose value looks like a payload code address
(0x00300000..end), locate the enclosing "function start" by scanning backwards for
the nearest `rts` (0x000b) and skipping following `nop`s, then report the delta
(pointer - start).  A healthy binary has delta == 0 for nearly all pointers.

Usage: python p11_consist.py <file.g1a> [...]
"""
import sys
import sh4lib


def func_start(img, va, back=0x400):
    for k in range(va - 2, max(sh4lib.BASE, va - back), -2):
        if img.u16(k) == 0x000B:          # rts
            s = k + 2
            while img.u16(s) == 0x0009:   # nop padding
                s += 2
            return s
    return None


def report(path):
    img = sh4lib.Image(path)
    stubs = img.stubs()
    seen = {}
    n = 0x200 and 0
    off = 0
    data = img.code
    for off in range(0, len(data) - 4, 2):
        v = int.from_bytes(data[off:off + 4], "big")
        if not (sh4lib.BASE <= v < img.end):
            continue
        if v in stubs:
            continue
        seen.setdefault(v, 0)
        seen[v] += 1
    hist = {}
    bad = []
    for v in sorted(seen):
        s = func_start(img, v)
        if s is None:
            d = "?"
        else:
            d = v - s
        hist[d] = hist.get(d, 0) + 1
        if d != 0:
            bad.append((v, s, d))
    print("%-14s stubs=%-3d code_ptrs=%-4d delta0=%d other=%d" % (
        path.split("\\")[-1], len(stubs), len(seen), hist.get(0, 0), len(bad)))
    h = " ".join("%s:%d" % (k, v) for k, v in sorted(hist.items(), key=lambda kv: -kv[1])[:8])
    print("   delta histogram: %s" % h)
    for v, s, d in bad[:14]:
        print("   %08x -> start %s delta %s" % (v, "%08x" % s if s else "?", d))
    return len(seen), hist.get(0, 0)


if __name__ == "__main__":
    for p in sys.argv[1:]:
        report(p)
