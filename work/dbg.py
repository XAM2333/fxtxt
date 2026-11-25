"""Revertible debug tweaks for the emulator verification.

  python work/dbg.py on        show chunk count / text_max in the title bar
  python work/dbg.py small     force a 1024 byte text window (many windows)
  python work/dbg.py off       restore work/main_patched.c
"""
import shutil
import sys

P = r'work\fxtxt\src\main.c'
BASE = r'work\main_patched.c'

INFO_OLD = (b'\t\t\t\tsnprintf(info, sizeof info, "%d/%d",\n'
            b'\t\t\t\t         chunk_rows + view_top + 1,\n'
            b'\t\t\t\t         total_rows > 0 ? total_rows : rows_total);')
INFO_NEW = (b'\t\t\t\tsnprintf(info, sizeof info, "%d/%d W%d M%d",\n'
            b'\t\t\t\t         chunk_rows + view_top + 1,\n'
            b'\t\t\t\t         total_rows > 0 ? total_rows : rows_total,\n'
            b'\t\t\t\t         chunk_n, text_max);')

CAP_OLD = b'for (int cap = 32768; cap >= TEXT_MIN; cap /= 2) {'
CAP_NEW = b'for (int cap = 1024; cap >= TEXT_MIN; cap /= 2) {'

MIN_OLD = b'#define TEXT_MIN    4096'
MIN_NEW = b'#define TEXT_MIN 1024'


def apply(mode):
    b = open(BASE, 'rb').read()
    if mode == 'on':
        assert b.count(INFO_OLD) == 1
        b = b.replace(INFO_OLD, INFO_NEW)
    elif mode == 'small':
        assert b.count(CAP_OLD) == 1
        b = b.replace(CAP_OLD, CAP_NEW)
        assert b.count(MIN_OLD) == 1
        b = b.replace(MIN_OLD, MIN_NEW)
    elif mode == 'both':
        assert b.count(INFO_OLD) == 1 and b.count(CAP_OLD) == 1
        b = b.replace(INFO_OLD, INFO_NEW).replace(CAP_OLD, CAP_NEW).replace(MIN_OLD, MIN_NEW)
    open(P, 'wb').write(b)
    print('mode', mode)


if __name__ == '__main__':
    if sys.argv[1] == 'off':
        shutil.copyfile(BASE, P)
        print('restored main_patched.c')
    else:
        apply(sys.argv[1])
