import io
import re

s = io.open("work/fxtxt/src/main.c", encoding="utf-8").read().split("\n")
for i, l in enumerate(s, 1):
    if re.search(r"caret_x|rep_n|sleep_ms|next_event\(", l):
        print(i, l[:100])
