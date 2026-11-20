"""Probe 02 - find the OS call mechanism in PROB.g1a.

Steps
 1. decide instruction byte order (big vs little) by decoding the entry block.
 2. locate every literal == 0x80010070 / 0x80020070 and the syscall id next to it.
 3. reconstruct the contiguous 16-byte stub block at file 0x7b20.
 4. linear-sweep the whole payload and pair `mov.l @(disp,pc),rN` with the
    following `jmp @rN` / `jsr @rN`, resolving the literal -> gives every
    thunk address and every call site.

vaddr = 0x300000 + (file_offset - 0x200)
"""
import struct
import sys

import capstone

sys.stdout.reconfigure(encoding="utf-8", errors="replace")

PATH = "E:/fxtxtviewer/PROB.g1a"
CODE_OFF = 0x200
BASE = 0x300000
GATEWAYS = (0x80010070, 0x80020070, 0x80030070)

blob = open(PATH, "rb").read()
code = blob[CODE_OFF:]


def va(off):
    return BASE + off - CODE_OFF


def off_of(addr):
    return CODE_OFF + (addr - BASE)


BEMD = capstone.Cs(capstone.CS_ARCH_SH, capstone.CS_MODE_SH4 | capstone.CS_MODE_BIG_ENDIAN)
LEMD = capstone.Cs(capstone.CS_ARCH_SH, capstone.CS_MODE_SH4 | capstone.CS_MODE_LITTLE_ENDIAN)


def show(md, addr, n=12, tag=""):
    print("  %s @%08x" % (tag, addr))
    for insn in md.disasm(bytes(blob[off_of(addr):off_of(addr) + 2 * n]), addr):
        print("    %08x  %-9s %s %s" % (insn.address, insn.bytes.hex(),
                                        insn.mnemonic, insn.op_str))


print("=== 1. byte order check: first 16 bytes of payload (file 0x200) ===")
print("  raw:", blob[0x200:0x210].hex())
show(BEMD, BASE, 8, "big-endian")
show(LEMD, BASE, 8, "little-endian")

print("\n=== 2. literal 0x80010070 / 0x80020070 occurrences ===")
for word, name in ((b"\x80\x01\x00\x70", "BE 0x80010070"),
                   (b"\x70\x00\x01\x80", "LE 0x80010070"),
                   (b"\x80\x02\x00\x70", "BE 0x80020070"),
                   (b"\x70\x00\x02\x80", "LE 0x80020070")):
    hits = []
    i = blob.find(word)
    while i >= 0:
        hits.append(i)
        i = blob.find(word, i + 1)
    print("  %-14s %d hit(s): %s" % (name, len(hits), " ".join("%06x" % h for h in hits[:8])
                                     + (" ..." if len(hits) > 8 else "")))

print("\n=== 3. the contiguous stub block: file 0x7b20 .. 0x8140 ===")
print("   (hex, 16 bytes per line = one candidate stub)")
for o in range(0x7b20, 0x8140, 16):
    b = blob[o:o + 16]
    print("   %06x  %-48s" % (o, " ".join("%02x" % c for c in b)))

print("\n=== 4. all 16-byte stub entries decoded from 0x7b20 ===")
stubs = []
o = 0x7b20
while o + 16 <= len(blob):
    b = blob[o:o + 16]
    if not ((b[0] & 0xF0) == 0xD0):       # mov.l @(d,pc),Rn
        break
    if b[4:8] != b"\x42\x2b\x00\x09":     # jmp @r2 ; nop
        break
    tramp = struct.unpack_from(">I", b, 8)[0]
    sid = struct.unpack_from(">I", b, 12)[0]
    r2 = b[0] & 0xF
    stubs.append((o, va(o), r2, tramp, sid))
    o += 16
print("   %d contiguous stubs, 0x%06x..0x%06x" % (len(stubs), stubs[0][0], stubs[-1][0] + 15))
for o, a, r2, tramp, sid in stubs:
    print("     file %06x  va %08x  r%d=%08x  syscall 0x%04x" % (o, a, r2, tramp, sid))

print("\n=== 5. whole-payload sweep: mov.l @(d,pc),rN ; ... ; jmp/jsr @rN ===")
# decode everything once, remember pc-relative loads and branch register insns
insns = []
for insn in BEMD.disasm(code, BASE):
    insns.append(insn)
print("   decoded %d instructions (%d bytes covered)" % (len(insns), len(code)))

# last write of each register in the 8 instructions before a jmp/jsr @rN
BANK = {}
targets = {}
pending = []          # (idx, reg, literal)
for i, insn in enumerate(insns):
    m = insn.mnemonic
    if m == "mov.l" and "@(" in insn.op_str and "pc" in insn.op_str:
        # op_str like "@(8,pc), r2" / "@(4,pc), r0"
        try:
            disp = int(insn.op_str.split("(")[1].split(",")[0], 0)
            dst = insn.op_str.split(",")[-1].strip()
            lit = insn.address + 4 + disp
            val = struct.unpack_from(">I", blob, off_of(lit) - 0)[0] if 0 <= off_of(lit) <= len(blob) - 4 else None
            pending.append((i, dst, lit, val))
        except Exception as e:
            pass
    elif m in ("jmp", "jsr") and insn.op_str.startswith("@"):
        reg = insn.op_str[1:].strip()
        # find the most recent load into reg among the last 8 instructions
        cand = [p for p in pending if p[1] == reg and i - p[0] <= 8]
        if cand:
            p = cand[-1]
            targets.setdefault((m, reg, p[2], p[3]), []).append(insn.address)
            # also record r0 value at that point (syscall id candidate)
            r0 = [q for q in pending if q[1] == "r0" and i - q[0] <= 8]
            if r0:
                targets[(m, reg, p[2], p[3])][-1] = (insn.address, r0[-1][3])

print("\n   distinct (branch, reg, literal_file_off, literal_value):")
for (m, reg, lit, val), sites in sorted(targets.items(), key=lambda kv: (kv[0][3] or 0)):
    s = []
    for x in sites:
        if isinstance(x, tuple):
            s.append("%08x(r0=%s)" % (x[0], "0x%x" % x[1] if x[1] is not None else "?"))
        else:
            s.append("%08x" % x)
    print("     %-4s @%s  lit@%06x = %s   %d site(s): %s" %
          (m, reg, lit, "0x%08x" % val if val is not None else "?", len(sites),
           " ".join(s[:10]) + (" ..." if len(s) > 10 else "")))
