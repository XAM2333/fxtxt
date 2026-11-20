#!/usr/bin/env python
"""Function-boundary sanity check for code pointers found in literal pools.

For every literal-pool dword that is used as a jsr/jmp target and points into the
payload, report:
  * whether some bsr/bra in the image targets it,
  * the preceding halfword  (000b rts / 0009 nop+aligned / else)
  * the nearest preceding `rts`, and how far back that is.

A real function entry is normally a bsr/bra target and sits right after an `rts`
(possibly with nop padding).  Anything else is either a jump table entry or a
data value that merely looks like a code pointer.

Usage: python p10_funcs.py <file.g1a>            (summary + list of suspicious ptrs)
       python p10_funcs.py <file.g1a> <va> ...   (detail for given addresses)
"""
import sys
import sh4lib


def main():
    path = sys.argv[1]
    want = [int(x, 16) for x in sys.argv[2:]]
    img = sh4lib.Image(path)
    insns = img.insns()
    stubs = img.stubs()

    # 1. collect branch targets
    btargets = {}
    for a, i in insns.items():
        if i.m in ("bsr", "bra", "bf", "bt", "bf/s", "bt/s") and i.o.startswith("0x"):
            t = int(i.o, 16)
            btargets.setdefault(t, []).append((a, i.m))

    # 2. collect code pointers used as jsr/jmp targets (same tracking as p09)
    ptr_use = {}
    regs = {}
    for a in sorted(insns):
        i = insns[a]
        m, op = i.m, i.o
        if m in ("mov.l",) and op.split(",")[0].startswith("0x"):
            parts = [p.strip() for p in op.split(",")]
            regs[parts[-1]] = (int(parts[0], 16), img.u32(int(parts[0], 16)))
        elif m == "mov" and op.startswith("#"):
            parts = [p.strip() for p in op.split(",")]
            regs[parts[-1]] = (None, int(parts[0][1:], 10))
        elif m in ("jsr", "jmp") and op.startswith("@r"):
            src = regs.get(op[1:].strip())
            if src and src[1] is not None and sh4lib.BASE <= src[1] < img.end:
                ptr_use.setdefault(src[1], []).append((a, m, src[0]))
        elif m in ("mov", "mov.b", "mov.w", "mov.l", "add", "sub", "xor", "and", "or",
                   "shll", "shlr", "extu.b", "extu.w", "exts.b", "exts.w", "swap.b",
                   "swap.w", "neg", "not", "lds", "sts", "ldc", "movt", "mulu", "muls",
                   "shll2", "shlr2", "shll8", "shlr8", "shll16", "shlr16") and op:
            dst = op.split(",")[-1].strip()
            if not dst.startswith("@") and not dst.startswith("#"):
                if not (m == "mov.l" and op.split(",")[0].startswith("0x")):
                    if not (m == "mov" and op.startswith("#")):
                        regs.pop(dst, None)

    print("%s: %d code pointers used as jsr/jmp targets" % (path, len(ptr_use)))
    bad = []
    for t in sorted(ptr_use):
        prev = img.u16(t - 2)
        near = None
        for k in range(t - 2, max(sh4lib.BASE, t - 0x400), -2):
            if img.u16(k) == 0x000B:      # rts
                near = k
                break
        ok = t in btargets
        tag = "OK  " if (ok or prev in (0x000B, 0x0009, 0x0000)) else "SUSP"
        if not ok and prev not in (0x000B, 0x0009, 0x0000):
            bad.append(t)
        print("  %s %08x  bsr/bra=%s prev=%04x nearest_rts=%s  used_by=%s" % (
            tag, t,
            ",".join("%08x/%s" % (a, m) for a, m in btargets.get(t, [])) or "none",
            prev if prev is not None else 0, "%08x" % near if near else "-",
            ",".join("%08x/%s" % (a, m) for a, m, _ in ptr_use[t][:6])))
    print("\n  %d suspicious (no direct branch, odd predecessor): %s" % (
        len(bad), " ".join("%08x" % b for b in bad)))

    for w in want:
        print("\n--- %08x" % w)
        for k in range(w - 16, w + 16, 2):
            i = insns.get(k)
            print("   %08x %s" % (k, ("%s %s" % (i.m, i.o)) if i else ".word %04x" % img.u16(k)))


if __name__ == "__main__":
    main()
