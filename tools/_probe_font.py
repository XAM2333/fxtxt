import io
import re

s = io.open("work/fxtxt/src/font_ascii.c", encoding="utf-8").read()
for m in re.finditer(r"/\* 0x7[CDE] [^\n]*\n", s):
    print(s[m.start():m.start() + 120].rstrip())
i = s.find("font_adv8")
print(s[i:i + 260])
