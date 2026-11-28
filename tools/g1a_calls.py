import re
import subprocess
from collections import Counter

OD = r"ref\pkg3\FxStudio-0.2.0-win-x64\sdk\native\bin\sh-elf-objdump.exe"
START = 0x60

LINE = re.compile(r"^\s*([0-9a-f]+):\s+([0-9a-f]{2})\s+([0-9a-f]{2})\s+(\S.*)")
MOVL = re.compile(r"mov\.l\s+0x[0-9a-f]+,r(\d+)\s+!\s+([0-9a-f]+)")
CALL = re.compile(r"(?:jsr|jmp)\s+@r(\d+)")


def scan(path):
    data = open(path, "rb").read()
    open(".tmp/dd.bin", "wb").write(data[START:])
    out = subprocess.run([OD, "-D", "-b", "binary", "-m", "sh", ".tmp/dd.bin"],
                         capture_output=True, text=True, errors="replace").stdout
    lit = {}
    hits = Counter()
    nj = 0
    for ln in out.splitlines():
        m = LINE.match(ln)
        if not m:
            continue
        ins = m.group(4)
        ml = MOVL.match(ins)
        if ml:
            lit[int(ml.group(1))] = int(ml.group(2), 16)
            continue
        mc = CALL.match(ins)
        if mc:
            nj += 1
            t = lit.get(int(mc.group(1)))
            if t is not None:
                hits[t] += 1
    return hits, nj


res = {}
for f in ["GEOMETRY.g1a", "EREADER.G1A", "PROB.g1a", "DL22P.G1A", "PHYSIUM.g1a"]:
    h, n = scan(f)
    res[f] = h
    os = {v: c for v, c in h.items() if 0x80000000 <= v <= 0x8FFFFFFF}
    print("%-14s jsr/jmp %5d  targets %4d  os-range %3d" % (f, n, len(h), len(os)))

for f in ["EREADER.G1A", "PROB.g1a", "GEOMETRY.g1a"]:
    print("\n%s os targets:" % f)
    for v, c in sorted(((v, c) for v, c in res[f].items() if 0x80000000 <= v <= 0x8FFFFFFF),
                       key=lambda kv: -kv[1])[:10]:
        print("   %08X  x%d" % (v, c))
