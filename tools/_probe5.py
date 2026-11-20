import io
import re

s = io.open("work/fxtxt/src/main.c", encoding="utf-8").read().split("\n")
for i, l in enumerate(s, 1):
    if re.search(r"static void draw_name|MODE_NAME\)|case MODE_NAME", l):
        print("---- line", i)
        for j in range(i - 1, min(len(s), i + 22)):
            print(j + 1, s[j][:100])
