"""Minimal .a (ar) member lister/extractor - no binutils needed."""
import sys

sys.stdout.reconfigure(encoding="utf-8", errors="replace")

MAGIC = b"!<arch>\n"


def members(path):
    with open(path, "rb") as f:
        data = f.read()
    if not data.startswith(MAGIC):
        raise SystemExit("not an ar archive")
    i = len(MAGIC)
    while i + 60 <= len(data):
        hdr = data[i:i + 60]
        name = hdr[0:16].decode("latin1").strip()
        size = int(hdr[48:58].decode("latin1").strip() or 0)
        body_at = i + 60
        body = data[body_at:body_at + size]
        yield name, body
        i = body_at + size + (size & 1)


def main():
    path = sys.argv[1]
    want = sys.argv[2] if len(sys.argv) > 2 else None
    out = sys.argv[3] if len(sys.argv) > 3 else None
    for name, body in members(path):
        if want and want not in name:
            continue
        if out:
            fn = out.replace("*", name.replace("/", "_"))
            with open(fn, "wb") as f:
                f.write(body)
            print("wrote", fn, len(body))
        else:
            print("%-40s %7d" % (name, len(body)))


main()
