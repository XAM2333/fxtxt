import io
import re

s = io.open("work/fxtxt/src/main.c", encoding="utf-8").read().split("\n")
for i, l in enumerate(s, 1):
    if re.search(r"cur_col\+\+|cur_col--|cur_col \+=|cur_col -=|move_cursor_row|move_cursor_line|blink", l):
        print(i, l[:100])
