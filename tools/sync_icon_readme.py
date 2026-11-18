"""Keep the icon ASCII block in README.md in sync with out/FXTXT.G1A."""
import io
import sys

sys.stdout.reconfigure(encoding="utf-8", errors="replace")

b = open("out/FXTXT.G1A", "rb").read()
d = b[0x4C:0x4C + 68]
rows = []
for y in range(17):
    row = d[y * 4:y * 4 + 4]
    rows.append("  " + "".join("#" if (row[x >> 3] >> (7 - (x & 7))) & 1 else "."
                               for x in range(30)))
art = "\n".join(rows)

t = io.open("README.md", encoding="utf-8").read()
start = t.find("现在 FXTXT 的图标按你最新的一版")
assert start > 0
open_fence = t.find("```", start)
close_fence = t.find("```", open_fence + 3)
assert open_fence > 0 and close_fence > open_fence
t = t[:open_fence] + "```\n" + art + "\n" + t[close_fence:]

io.open("README.md", "w", encoding="utf-8").write(t)
print("README icon block synced\n" + art)
