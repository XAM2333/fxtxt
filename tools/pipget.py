"""Download a wheel with urllib (bypasses pip temp-dir sandbox issues), then unzip to pylib."""
import json, os, sys, urllib.request, zipfile

UA = {"User-Agent": "fxtxt-probe"}
DEST = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "pylib")


def api(u):
    return json.load(urllib.request.urlopen(urllib.request.Request(u, headers=UA), timeout=30))


def main():
    pkg = sys.argv[1]
    ver = sys.argv[2] if len(sys.argv) > 2 else None
    os.makedirs(DEST, exist_ok=True)
    meta = api("https://pypi.org/pypi/%s/json" % pkg)
    ver = ver or meta["info"]["version"]
    files = meta["releases"][ver]
    want = None
    for f in files:
        n = f["filename"]
        if n.endswith(".whl") and ("py3-none-any" in n or "win_amd64" in n):
            want = f
            if "win_amd64" in n:
                break
    if not want:
        print("no suitable wheel for", pkg, ver, [f["filename"] for f in files])
        return 1
    print("downloading", want["filename"], want["size"])
    data = urllib.request.urlopen(urllib.request.Request(want["url"], headers=UA), timeout=120).read()
    p = os.path.join(DEST, want["filename"])
    with open(p, "wb") as fp:
        fp.write(data)
    with zipfile.ZipFile(p) as z:
        z.extractall(DEST)
    print("extracted to", os.path.abspath(DEST))
    return 0


if __name__ == "__main__":
    sys.exit(main())
