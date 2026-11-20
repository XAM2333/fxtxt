"""Find direct (bsr/bra/bf/bt) branches into the fxlib stub block.

Complements p06: literal-loaded stub pointers are only one way to reach a stub.
The linker can also place a stub within bsr range (+-4 KB) of its caller and
branch to it relatively -- that leaves no literal pool entry behind.

Usage: p07_bsr.py file.g1a
"""
import sys
import re
import sh4lib

BRANCH = ("bsr", "bra", "bf", "bt", "bf/s", "bt/s")


def main():
    path = sys.argv[1]
    img = sh4lib.Image(path)
    insns = img.insns()
    stubs = img.stubs()
    if not stubs:
        print("%s: no stubs" % path)
        return
    lo = min(stubs)
    hi = lo + 16 * len(stubs)
    hits = {}
    total = 0
    for a in sorted(insns):
        i = insns[a]
        if i.m not in BRANCH:
            continue
        m = re.match(r"^0x([0-9a-f]+)$", i.o.strip())
        if not m:
            continue
        t = int(m.group(1), 16)
        total += 1
        if t in stubs:            # stubs are not necessarily contiguous
            hits.setdefault(stubs[t], []).append(a)
    print("%s: stubs %08x..%08x, %d direct branches total, %d land on a stub"
          % (path, lo, hi, total, sum(len(v) for v in hits.values())))
    for sid in sorted(hits, key=lambda x: -len(hits[x])):
        print("  0x%04x x%-3d %s" % (sid, len(hits[sid]),
                                     " ".join("%08x" % s for s in hits[sid][:10])))


main()
