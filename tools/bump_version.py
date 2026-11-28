import io

M = "work/fxtxt/src/main.c"
t = io.open(M, encoding="utf-8", errors="surrogateescape").read()
old = 'draw_ui(2, CONTENT_Y + 1, "FXTXT  1.0", 0);'
new = 'draw_ui(2, CONTENT_Y + 1, "FXTXT  1.2.0", 0);'
assert t.count(old) == 1, t.count(old)
io.open(M, "w", encoding="utf-8", errors="surrogateescape", newline="").write(t.replace(old, new))
print("version bumped to 1.2.0")
