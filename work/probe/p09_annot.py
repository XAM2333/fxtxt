#!/usr/bin/env python
"""Annotated listing: resolve jsr/jmp @rN to a concrete target (stub id or code VA).

Usage: python p09_annot.py <file.g1a> <from_va> <to_va>

Register tracking is a simple forward scan over the whole image; a register's value
is set by mov.l/mov.w/mova <lit>,rN and mov #imm,rN, and is dropped on any other
write it cannot model.  Good enough to read a function's call sequence by eye.
"""
import sys
import sh4lib

# Syscall ids that are documented for fx-9860G (gint src/kernel/syscalls.S) or
# strongly inferred from this binary.  Everything else prints as "?".
NAMES = {
    0x0acd: "malloc", 0x0acc: "free", 0x0e6d: "realloc",
    0x0439: "BFile_Remove", 0x0434: "BFile_Create", 0x042c: "BFile_Open",
    0x042d: "BFile_Close", 0x042f: "BFile_Size", 0x0431: "BFile_Seek",
    0x0435: "BFile_Write", 0x0432: "BFile_Read", 0x043b: "BFile_FindFirst",
    0x043c: "BFile_FindNext", 0x043d: "BFile_FindClose",
    0x0118: "Timer_Install", 0x011a: "Timer_Start", 0x011b: "Timer_Stop",
    0x0119: "Timer_Deinstall", 0x0248: "PutKeyCode", 0x0247: "GetKeyWait",
    0x0241: "ClearKeyBuffer", 0x0135: "GetVRAMAddress",
    0x0494: "SetQuitHandler", 0x0236: "Reset",
}


def main():
    path, lo, hi = sys.argv[1], int(sys.argv[2], 16), int(sys.argv[3], 16)
    img = sh4lib.Image(path)
    insns = img.insns()
    stubs = img.stubs()

    regs = {}
    a = sh4lib.BASE
    while a < img.end:
        ins = insns.get(a)
        if ins is None:
            a += 2
            continue
        m, op = ins.m, ins.o
        note = ""

        if m in ("mov.l", "mov.w", "mova") and op.split(",")[0].startswith("0x"):
            parts = [p.strip() for p in op.split(",")]
            lit = int(parts[0], 16)
            dst = parts[-1]
            v = img.u32(lit) if m == "mov.l" else img.u16(lit)
            regs[dst] = (m, lit, v)
        elif m == "mov" and op.startswith("#"):
            parts = [p.strip() for p in op.split(",")]
            regs[parts[-1]] = ("imm", None, int(parts[0][1:], 10))
        elif m in ("jsr", "jmp") and op.startswith("@r"):
            r = op[1:].strip()
            src = regs.get(r)
            if src is not None and src[0] in ("mov.l", "mov.w") and src[2] is not None:
                t = src[2]
                if t in stubs:
                    sid = stubs[t]
                    note = "=> stub %08x  SYSCALL 0x%04x %s" % (t, sid, NAMES.get(sid, "?"))
                elif sh4lib.BASE <= t < img.end:
                    note = "=> 0x%08x  (payload fn, literal @ %08x)" % (t, src[1])
                else:
                    note = "=> 0x%08x  (literal @ %08x)" % (t, src[1])
            elif src is not None and src[0] == "mova":
                note = "=> mova 0x%08x" % src[1]
            else:
                note = "=> %s untracked" % r
        elif m in ("mov", "mov.b", "mov.w", "mov.l", "mova", "add", "sub", "xor", "and", "or",
                   "shll", "shlr", "shll2", "shlr2", "shll8", "shlr8", "shll16", "shlr16",
                   "extu.b", "extu.w", "exts.b", "exts.w", "swap.b", "swap.w", "neg", "not",
                   "lds", "sts", "ldc", "tas", "dt", "div1", "movt", "mulu", "muls", "swap") and op:
            dst = op.split(",")[-1].strip()
            if not dst.startswith("@") and not dst.startswith("#"):
                if not (m in ("mov.l", "mov.w", "mova") and op.split(",")[0].startswith("0x")):
                    if not (m == "mov" and op.startswith("#")):
                        regs.pop(dst, None)

        if lo <= a <= hi:
            print("%08x %-4s %-8s %-26s %s" % (a, ins.raw, m, op, note))
        a += 2


if __name__ == "__main__":
    main()
