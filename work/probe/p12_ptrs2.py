#!/usr/bin/env python
"""Consistency check: do literal-pool code pointers agree with the code layout?

Only 4-byte-aligned dwords that are *actually read by a `mov.l <slot>,rN`* are
considered (that removes the misaligned false positives of p11's first version).
For each payload-code pointer we locate the enclosing function start by scanning
back for the nearest `rts` (0x000b) and skipping following `nop`s, then report
the delta.  A healthy binary has delta == 0 for nearly all pointers.

Usage: python p12_ptrs2.py <file.g1a> [...]
"""
import sys
import sh4lib


def func_start(img, va, back=0x300):
    k = va - 2
    while k > max(sh4lib.BASE, va - back):
        if img.u16(k) == 0x000B:            # rts
            s = k + 2
            while img.u16(s) == 0x0009:     # nop padding
                s += 2
            return s
        k -= 2
    return None


def report(path):
    img = sh4lib.Image(path)
    stubs = img.stubs()
    slots = set()
    for a, i in img.insns().items():
        if i.m == "mov.l" and i.o.split(",")[0].startswith("0x"):
            slots.add(int(i.o.split(",")[0], 16))
    ptrs = {}
    for s in slots:
        v = img.u32(s)
        if v is None or not (sh4lib.BASE <= v < img.end):
            continue
        if v in stubs:
            continue
        ptrs.setdefault(v, []).append(s)
    hist = {}
    bad = []
    for v in sorted(ptrs):
        st = func_start(img, v)
        d = (v - st) if st is not None else None
        hist[d] = hist.get(d, 0) + 1
        if d:
            bad.append((v, st, d))
    print("%-13s stubs=%-3d payload_code_ptrs=%-4d delta0=%-3d other=%d" % (
        path.replace("..\\..\\", ""), len(stubs), len(ptrs), hist.get(0, 0), len(bad)))
    print("   delta histogram: %s" % " ".join(
        "%s:%d" % ("?" if k is None else "%+d" % k, v)
        for k, v in sorted(hist.items(), key=lambda kv: -kv[1])[:10]))
    for v, st, d in bad[:16]:
        print("   %08x  start=%s  delta=%+d  slots=%s" % (
            v, "%08x" % st if st else "?", d,
            " ".join("%08x" % s for s in ptrs[v][:3])))


if __name__ == "__main__":
    for p in sys.argv[1:]:
        report(p)
