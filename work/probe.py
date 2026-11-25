import sys
p = r'E:\fxtxtviewer\work\fxtxt\src\main.c'
b = open(p, 'rb').read()
print('size', len(b), 'nonascii', sum(1 for c in b if c > 127), 'lines', b.count(b'\n'))
bad = [i + 1 for i, ln in enumerate(b.split(b'\n')) if any(c > 127 for c in ln)]
print('lines with nonascii:', bad[:40], '...' if len(bad) > 40 else '')
print('has utf8 replacement', b'\xef\xbf\xbd' in b)
