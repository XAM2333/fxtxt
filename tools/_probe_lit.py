import io
import re

t = io.open("work/fxtxt/src/main.c", encoding="utf-8").read()
i = t.find('ui_title("')
for m in re.finditer(r'"(?:[^"\\]|\\.)*"', t):
    if m.start() <= i + 10 < m.end() or abs(m.start() - (i + 9)) < 40:
        print(m.start(), m.end(), repr(m.group(0))[:80])
print("--- context ---")
print(repr(t[i - 120:i + 90]))
