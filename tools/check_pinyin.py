"""Offline check of the pinyin lookup used by the add-in."""
import io
import re
import sys

sys.stdout.reconfigure(encoding="utf-8", errors="replace")
src = io.open("work/fxtxt/src/pinyin.c", encoding="utf-8").read()


def table(name):
    m = re.search(r"const [^=]*\b%s\[[^=]*=\s*\{(.*?)\n\};" % name, src, re.S)
    body = m.group(1)
    return [int(x, 0) for x in re.findall(r"\d+", body)]


def strings(name):
    m = re.search(r'const char %s\[\]\s*=\s*\n(.*?);\n' % name, src, re.S)
    body = m.group(0)
    out = bytearray()
    for lit in re.findall(r'"((?:[^"\\]|\\.)*)"', body):
        i = 0
        while i < len(lit):
            c = lit[i]
            if c == "\\" and i + 1 < len(lit):
                n = lit[i + 1]
                if n == "0":
                    out.append(0)
                elif n == "\\":
                    out.append(ord("\\"))
                elif n == '"':
                    out.append(ord('"'))
                else:
                    out.append(ord(n))
                i += 2
                continue
            out.append(ord(c))
            i += 1
    return bytes(out)


py_off = table("py_off")
py_start = table("py_start")
py_glyph = table("py_glyph")
names = strings("py_names")
COUNT = len(py_off) - 1


def name(i):
    s = py_off[i]
    e = py_off[i + 1] - 1
    return names[s:e].decode("ascii")


def glyph_char(g):
    return bytes([g // 94 + 1 + 0xA0, g % 94 + 1 + 0xA0]).decode("gb2312")


def match(buf):
    lo, hi = 0, COUNT
    while lo < hi:
        mid = (lo + hi) // 2
        nm = name(mid)
        if nm < buf:
            lo = mid + 1
        else:
            hi = mid
    if lo < COUNT and name(lo).startswith(buf):
        return lo
    return -1


for probe in ("ji", "jie", "zhong", "no", "mo", "a"):
    idx = match(probe)
    if idx < 0:
        print("%-6s -> no match" % probe)
        continue
    gl = py_glyph[py_start[idx]:py_start[idx + 1]]
    cands = "".join(glyph_char(g) for g in gl[:12])
    print("%-6s -> syllable %-6s (%d candidates)  %s" % (probe, name(idx), len(gl), cands))

# all syllables must still be reachable by their own spelling
bad = [name(i) for i in range(COUNT) if match(name(i)) != i]
print("syllables whose own spelling does not resolve to itself:", len(bad), bad[:6])
