"""Print lines of main.c matching a pattern (helper for exploring the code)."""
import io
import re
import sys

sys.stdout.reconfigure(encoding="utf-8", errors="replace")
pat = sys.argv[1]
path = sys.argv[2] if len(sys.argv) > 2 else "work/fxtxt/src/main.c"
lines = io.open(path, encoding="utf-8").read().split("\n")
rx = re.compile(pat)
for i, l in enumerate(lines, 1):
    if rx.search(l):
        print(i, l)
