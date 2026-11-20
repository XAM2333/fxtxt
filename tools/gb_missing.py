"""List chrome characters that GB2312 cannot encode."""
import io
import re
import subprocess
import sys

sys.stdout.reconfigure(encoding="utf-8", errors="replace")
src = subprocess.run(["git", "show", "8cb0efc:work/fxtxt/src/main.c"],
                     capture_output=True).stdout.decode("utf-8")
io.open("work/main_pre_gb.c", "w", encoding="utf-8", newline="").write(src)

bad = {}
for m in re.finditer(r'"(?:[^"\\]|\\.)*"', src):
    for ch in m.group(0)[1:-1]:
        if ord(ch) < 0x80:
            continue
        try:
            ch.encode("gb2312")
        except UnicodeEncodeError:
            bad[ch] = bad.get(ch, 0) + 1
print("count:", len(bad))
for ch, n in sorted(bad.items(), key=lambda kv: -kv[1]):
    print("U+%04X %s  x%d" % (ord(ch), ch, n))
