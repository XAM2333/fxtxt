"""Shared SH-4 / Casio G1A helpers for the USB probe.

Byte order: Casio fx-9750GIII / fx-9860G code is BIG-ENDIAN SH-4A.  Verified in
p02: the payload only decodes as big-endian.

Address model: G1A file = 0x20-byte container header + 0x1e0-byte add-in header,
so code starts at file offset 0x200 and is linked at 0x00300000.
"""
import struct
import capstone

CODE_OFF = 0x200
BASE = 0x00300000
GATEWAY = 0x80010070

_md = capstone.Cs(capstone.CS_ARCH_SH, capstone.CS_MODE_BIG_ENDIAN | capstone.CS_MODE_SH4A)
_md.detail = False


class Ins:
    __slots__ = ("a", "raw", "m", "o")

    def __init__(self, a, raw, m, o):
        self.a = a
        self.raw = raw
        self.m = m
        self.o = o

    def __repr__(self):
        return "%08x %s %s %s" % (self.a, self.raw, self.m, self.o)


class Image:
    def __init__(self, path):
        self.path = path
        with open(path, "rb") as f:
            self.data = f.read()
        self.code = self.data[CODE_OFF:]
        self.end = BASE + len(self.code)

    # ---- raw access -----------------------------------------------------
    def u16(self, va):
        off = va - BASE
        if off < 0 or off + 2 > len(self.code):
            return None
        return struct.unpack_from(">H", self.code, off)[0]

    def u32(self, va):
        off = va - BASE
        if off < 0 or off + 4 > len(self.code):
            return None
        return struct.unpack_from(">I", self.code, off)[0]

    def lit(self, va):
        """Operand of a `mov.l <lit>,rN`: capstone prints the resolved ABSOLUTE
        address (no 'pc', no parentheses), so we can read the dword directly."""
        return self.u32(va)

    # ---- disassembly ----------------------------------------------------
    def insns(self):
        """Linear sweep, one instruction at a time, every 2-byte offset.

        SH instructions are fixed 2 bytes, so a linear sweep is sound for code;
        undecodable halfwords are data (icons, fonts, literal pools).
        """
        out = {}
        a = BASE
        n = len(self.code)
        off = 0
        while off + 2 <= n:
            hit = None
            for i in _md.disasm(self.code[off:off + 4], a):
                hit = i
                break
            if hit is not None:
                out[a] = Ins(a, hit.bytes.hex(), hit.mnemonic, hit.op_str)
                step = len(hit.bytes)
            else:
                step = 2
            a += step
            off += step
        return out

    # ---- syscall stubs --------------------------------------------------
    def stubs(self):
        """Find `mov.l <gateway>,r2 ; mov.l <id>,r0 ; jmp @r2 ; nop` 16-byte stubs.

        Returns {stub_va: id}.
        """
        res = {}
        pat = struct.pack(">I", GATEWAY)
        off = 0
        while True:
            k = self.code.find(pat, off)
            if k < 0:
                break
            off = k + 1
            if k < 8:
                continue
            # layout: [k-8 .. k) = d201 d002 422b 0009, literal at k, id at k+4
            pre = self.code[k - 8:k]
            if pre[:4] in (b"\xd2\x01\xd0\x02", b"\xd2\x02\xd0\x02"):
                # stub start = k-8 (literal is at k, so stub base = k-8)
                base = BASE + (k - 8)
                sid = struct.unpack_from(">I", self.code, k + 4)[0]
                res[base] = sid
        return res


# ---------------------------------------------------------------------------
def find_writers(insns, order, idx, reg, back=12):
    """Yield (position, Ins) of instructions that write `reg`, walking back."""
    got = []
    for k in range(idx - 1, max(-1, idx - 1 - back), -1):
        i = insns[order[k]]
        if not i.o:
            continue
        parts = [p.strip() for p in i.o.split(",")]
        dst = parts[-1]
        if dst == reg:
            got.append((k, i))
    return got


def resolve_indirect(insns, order, idx, reg, img, depth=2):
    """Resolve `jsr @reg` / `jmp @reg` to the value the register holds.

    Returns (kind, value, via) where kind is
      'syscall'  -> value is the syscall id (gateway loaded inline)
      'stub'     -> value is the stub VA (look up id in img.stubs())
      'ptr'      -> value is some other pointer
      None       -> unresolved
    """
    if depth <= 0:
        return (None, None, None)
    ws = find_writers(insns, order, idx, reg, back=10)
    if not ws:
        return (None, None, None)
    k, w = ws[0]  # nearest preceding writer
    if w.m == "mov.l":
        # operand form: "0x307928,r2"  (capstone resolved the pc-relative addr)
        try:
            lit = int(w.o.split(",")[0], 16)
        except ValueError:
            return (None, None, w)
        v = img.lit(lit)
        if v is None:
            return (None, None, w)
        if v == GATEWAY:
            # inline trampoline: the id must be in r0 just before
            sid = None
            for k2 in range(k - 1, max(-1, k - 9), -1):
                i2 = insns[order[k2]]
                if i2.m == "mov.l" and i2.o.split(",")[-1].strip() == "r0":
                    try:
                        sid = img.lit(int(i2.o.split(",")[0], 16))
                    except ValueError:
                        sid = None
                    if sid is not None:
                        break
            return ("syscall", sid, w)
        return ("ptr", v, w)
    if w.m in ("mov", "mov.w", "mov.b"):
        src, dst = [p.strip() for p in w.o.split(",")]
        if src.startswith("r"):
            return resolve_indirect(insns, order, k, src, img, depth - 1)
    return (None, None, w)
