"""Probe 03 - full linear disassembly of PROB.g1a + call-graph extraction.

SH-4A, big-endian confirmed by probe 02 (the entry thunk decodes only as BE).
Writes prob_dis.txt (full listing) and prints:
  * every distinct branch target reached through a literal pool entry
  * for each fxlib syscall stub: how many call sites, where
  * r4/r5 argument setup immediately before the call (best effort)
  * all literal-pool dwords in OS / peripheral / payload address ranges
"""
import struct
import sys
from collections import Counter, defaultdict

import capstone

sys.stdout.reconfigure(encoding="utf-8", errors="replace")

PATH = "E:/fxtxtviewer/PROB.g1a"
CODE_OFF = 0x200
BASE = 0x300000
OUT = "E:/fxtxtviewer/work/probe/prob_dis.txt"

blob = open(PATH, "rb").read()
code = blob[CODE_OFF:]

MD = capstone.Cs(capstone.CS_ARCH_SH, capstone.CS_MODE_SH4 | capstone.CS_MODE_BIG_ENDIAN)


def va(off):
    return BASE + off - CODE_OFF


def off_of(addr):
    return CODE_OFF + (addr - BASE)


class Insn:
    __slots__ = ("a", "raw", "m", "o", "size")

    def __init__(self, a, raw, m, o):
        self.a = a
        self.raw = raw
        self.m = m
        self.o = o
        self.size = len(raw)

    def text(self):
        return ("%-10s %s" % (self.m, self.o)).rstrip()


insns = {}          # address -> Insn        (only decodable)
raw_at = {}         # address -> 2-byte int  (everything)
p = 0
while p + 1 < len(code):
    addr = BASE + p
    got = None
    for i in MD.disasm(code[p:p + 2], addr):
        got = i
        break
    if got is None:
        raw_at[addr] = int.from_bytes(code[p:p + 2], "big")
        p += 2
        continue
    insns[addr] = Insn(addr, got.bytes, got.mnemonic, got.op_str)
    raw_at[addr] = int.from_bytes(code[p:p + 2], "big")
    p += got.size

print("decoded %d instructions, %d undecodable halfwords, code 0x%x..0x%x"
      % (len(insns), sum(1 for a in raw_at if a not in insns),
         BASE, BASE + len(code)))

order = sorted(insns)
pos = {a: i for i, a in enumerate(order)}


def pcrel_lit(ins):
    """literal address for mov.l @(disp,pc),rn / mov.w / mova"""
    try:
        disp = int(ins.o.split("(")[1].split(",")[0], 0)
    except Exception:
        return None
    if "pc" not in ins.o:
        return None
    return ((ins.a + 4) & ~3) + disp


def rd32(addr):
    o = off_of(addr)
    if 0 <= o <= len(blob) - 4:
        return struct.unpack_from(">I", blob, o)[0]
    return None


# ---------------------------------------------------------------- listing
with open(OUT, "w", encoding="utf-8") as f:
    for a in order:
        i = insns[a]
        f.write("%08x  %-6s %s\n" % (a, i.raw.hex(), i.text()))
        # annotate pc-relative literal loads
        if i.m in ("mov.l", "mov.w", "mova") and "pc" in i.o:
            lit = pcrel_lit(i)
            if lit is not None and lit % 4 == 0 and i.m == "mov.l":
                v = rd32(lit)
                f.write("%24s; lit @%08x = %s\n" % ("", lit, "0x%08x" % v if v is not None else "?"))
print("wrote", OUT)

# ---------------------------------------------------------------- branch targets
# last write of a register within N instructions before a jmp/jsr @rN
def prev_writes(idx, reg, back=10):
    out = []
    for k in range(idx - 1, max(-1, idx - 1 - back), -1):
        i = insns[order[k]]
        dst = i.o.split(",")[-1].strip() if i.o else ""
        if dst == reg and i.m.startswith(("mov", "lds", "add", "xor", "and", "or", "shlr", "shll", "extu", "swap")):
            out.append(i)
    return out


edges = defaultdict(list)      # (kind, target, via_reg, litaddr) -> [callsite]
iter_reg = {}                  # register -> most recent Insn that set it
for idx, a in enumerate(order):
    i = insns[a]
    if i.m in ("jmp", "jsr", "braf", "bsrf") and i.o.startswith("@"):
        reg = i.o[1:].strip()
        ws = prev_writes(idx, reg)
        if ws:
            w = ws[0]
            tgt = None
            lit = None
            if w.m == "mov.l" and "pc" in w.o:
                lit = pcrel_lit(w)
                tgt = rd32(lit) if lit else None
            elif w.m.startswith("mov") and w.o.startswith("#"):
                try:
                    tgt = int(w.o.split("#")[1], 0)
                except Exception:
                    tgt = None
            elif w.m == "mova":
                lit = pcrel_lit(w)
                tgt = lit
            edges[(i.m, tgt, reg, lit)].append(a)

print("\n=== branch-register edges (target resolved from literal pool) ===")
for (kind, tgt, reg, lit), sites in sorted(edges.items(),
                                           key=lambda kv: (kv[0][1] is None, kv[0][1] or 0)):
    print("  %-5s @%-4s -> %-10s lit@%s  %3d site(s)  %s"
          % (kind, reg, "0x%08x" % tgt if tgt is not None else "UNRESOLVED",
             "%08x" % lit if lit else "-", len(sites),
             " ".join("%08x" % s for s in sites[:8]) + (" ..." if len(sites) > 8 else "")))

# ---------------------------------------------------------------- syscall stubs
stubs = {}     # stub addr -> (file_off, syscall id)
o = 0x7b20
while o + 16 <= len(blob):
    b = blob[o:o + 16]
    if (b[0] & 0xF0) != 0xD0 or b[4:8] != b"\x42\x2b\x00\x09":
        break
    stubs[va(o)] = (o, struct.unpack_from(">I", b, 12)[0])
    o += 16
print("\n%d syscall stubs at 0x%08x..0x%08x" % (len(stubs), min(stubs), max(stubs)))

# who calls each stub: literal == stub address anywhere in code, followed by jsr
lit_to_stub = {}
for a in order:
    i = insns[a]
    if i.m == "mov.l" and "pc" in i.o:
        lit = pcrel_lit(i)
        if lit:
            v = rd32(lit)
            if v in stubs:
                lit_to_stub.setdefault(lit, v)

calls = defaultdict(list)      # stub addr -> [(callsite, r0_if_id)]
for a in order:
    i = insns[a]
    if i.m in ("jsr", "jmp") and i.o.startswith("@"):
        reg = i.o[1:].strip()
        idx = pos[a]
        for k in range(idx - 1, max(-1, idx - 6), -1):
            w = insns[order[k]]
            if (w.o.split(",")[-1].strip() if w.o else "") != reg:
                continue
            if w.m == "mov.l" and "pc" in w.o:
                lit = pcrel_lit(w)
                if lit in lit_to_stub:
                    calls[lit_to_stub[lit]].append(a)
            break

print("\n=== per-syscall call counts ===")
by_id = {}
for s, (fo, sid) in stubs.items():
    by_id.setdefault(sid, []).append(s)
for sid in sorted(by_id):
    tot = 0
    detail = []
    for s in by_id[sid]:
        n = len(calls.get(s, []))
        tot += n
        if n:
            detail.append("%08x:%d%s" % (s, n, "," + " ".join("%08x" % c for c in calls[s][:4]) if n else ""))
    print("  syscall 0x%04x  stub(s) %-30s  %2d call site(s)  %s"
          % (sid, " ".join("%08x" % s for s in by_id[sid]), tot, " ".join(detail)))

never = [sid for sid in by_id if sum(len(calls.get(s, [])) for s in by_id[sid]) == 0]
print("\n  stubs never called via literal->jsr (maybe indirect/other form): %s"
      % " ".join("0x%04x" % x for x in sorted(never)))
