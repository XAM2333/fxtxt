"""Report every literal-pool pointer into OS / peripheral address space.

For each `mov.l <lit>,rN` the dword at <lit> is read; if it points at the OS
(0x8000_0000..) or the SH7305 peripheral window (0xA4xx_xxxx / 0xA5xx_xxxx),
the load site is printed.  Also prints USB-related strings.

Usage: p08_ptrs.py file.g1a [...]
"""
import sys
import sh4lib

# SH7305 (fx-9860G) peripheral windows used by the project's own USB attempts:
USB_BASE = 0xA4D80000      # USB module
INTC_BASE = 0xA4D00000     # interrupt controller
CPG_BASE = 0xA4150000      # clock pulse generator (USBCLKCR etc.)
RAM_BASE = 0x88000000      # on-chip RAM window used by Casio

REGIONS = [
    ("OS 0x8000xxxx", 0x80000000, 0x80100000),
    ("periph 0xA4xxxxxx", 0xA4000000, 0xA5000000),
    ("RAM 0x88xxxxxx", 0x88000000, 0x88100000),
]

WORDS = ("usb", "conn", "cable", "link", "mode", "serial")


def scan(path):
    img = sh4lib.Image(path)
    insns = img.insns()
    print("=" * 78)
    print(path)
    hits = {name: [] for name, _, _ in REGIONS}
    for a in sorted(insns):
        i = insns[a]
        if i.m not in ("mov.l", "mova"):
            continue
        head = i.o.split(",")[0].strip()
        if not head.startswith("0x"):
            continue
        try:
            lit = int(head, 16)
        except ValueError:
            continue
        v = img.u32(lit)
        if v is None:
            continue
        for name, lo, hi in REGIONS:
            if lo <= v < hi:
                hits[name].append((a, v, lit))
    for name, _, _ in REGIONS:
        lst = hits[name]
        uniq = {}
        for (a, v, lit) in lst:
            uniq.setdefault(v, []).append(a)
        print("  %s: %d loads, %d distinct values" % (name, len(lst), len(uniq)))
        for v in sorted(uniq):
            if v == sh4lib.GATEWAY:
                continue
            print("     0x%08x  x%-3d  %s" % (v, len(uniq[v]),
                                             " ".join("%08x" % x for x in uniq[v][:8])))
    # strings
    data = img.data
    found = []
    for w in WORDS:
        for enc, name in ((w.encode(), "ascii"), (w.encode("utf-16-be"), "utf16be")):
            off = 0
            while True:
                k = data.lower().find(enc, off)
                if k < 0:
                    break
                off = k + 1
                ctx = data[max(0, k - 24):k + 48]
                txt = "".join(chr(c) if 32 <= c < 127 else "." for c in ctx)
                found.append((name, w, k, txt))
    if found:
        print("  string hits:")
        for (enc, w, off, txt) in found[:24]:
            print("     %-8s %-7s @file 0x%05x  |%s|" % (enc, w, off, txt))
    else:
        print("  no usb/conn/cable/link strings")


for p in sys.argv[1:]:
    scan(p)
