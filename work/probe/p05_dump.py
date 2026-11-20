"""Hexdump + disassembly of a VA range of a G1A.  Usage: p05_dump.py file.g1a from_va to_va"""
import sys
import sh4lib


def main():
    path = sys.argv[1]
    lo = int(sys.argv[2], 16)
    hi = int(sys.argv[3], 16)
    img = sh4lib.Image(path)
    insns = img.insns()
    a = lo
    while a < hi:
        off = a - sh4lib.BASE
        raw = img.code[off:off + 2].hex()
        i = insns.get(a)
        dword = img.u32(a) if a + 4 <= hi else None
        tag = ""
        if dword is not None and (dword >> 24) in (0x00, 0x80, 0xA4, 0xA5):
            tag = "  .long 0x%08x" % dword
        if i:
            print("%08x %s  %-8s %-24s%s" % (a, raw, i.m, i.o, tag))
        else:
            print("%08x %s  %-8s %-24s%s" % (a, raw, ".word", "", tag))
        a += 2


main()
