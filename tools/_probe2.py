import io
import re

s = io.open("work/fxtxt/src/main.c", encoding="utf-8").read().split("\n")


def show(pat, after=18):
    for i, l in enumerate(s, 1):
        if re.search(pat, l):
            print("---- line", i)
            for j in range(i - 1, min(len(s), i + after)):
                print(j + 1, s[j][:100])
            return


show(r"static int cell_adv", 10)
show(r"case KEY_RIGHT:", 14)
show(r"static void handle_name_key", 26)
