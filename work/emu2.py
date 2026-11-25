"""Tolerant FxStudio emulator driver (works when build.run is broken).

Usage: emu2.py keys [--no-build] [--tag T] SHIFT,F3,DOWN*12,...

The .g1a must already be in work/fxtxt/.fxstudio/build/Release/artifacts/
(produced by: python <pkg>/sdk/native/service/build-native.py <pkg>/sdk/native
 work/fxtxt work/fxtxt/.fxstudio/build/Release Release).

Keys: NAME or NAME*COUNT (repeat). RELAUNCH restarts the add-in.
"""
import json
import os
import re
import subprocess
import sys
import time

ROOT = os.path.abspath(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
PKG = os.path.join(ROOT, "ref", "pkg3", "FxStudio-0.2.0-win-x64")
PY = os.path.join(PKG, "sdk", "native", "host", "python", "python.exe")
HELPER = os.path.join(ROOT, "ref", "skill", "fxstudio", "scripts", "fxstudio.py")
STATE = os.path.join(ROOT, "work", ".fxstudio", "state")
SHOTS = os.path.join(ROOT, "work", "shots")
PROJECT = os.environ.get("FXPROJECT") or os.path.join(ROOT, "work", "fxtxt")
TAG = ""
os.makedirs(SHOTS, exist_ok=True)
# The FxStudio agent inherits our environment; its default TEMP is not writable
# in this box (sh-elf-gcc then fails with "Cannot create temporary file").
_TMP = os.path.join(ROOT, ".tmp")
os.makedirs(_TMP, exist_ok=True)
os.environ["TMP"] = _TMP
os.environ["TEMP"] = _TMP


def run(*args, check=True):
    cmd = [PY, HELPER, "--package", PKG, "--state", STATE] + [str(a) for a in args]
    r = subprocess.run(cmd, capture_output=True, text=True)
    if r.returncode != 0 and check:
        print("CMD FAILED:", " ".join(cmd))
        print(r.stdout[-1500:])
        print(r.stderr[-1500:])
        raise SystemExit(1)
    out = (r.stdout or "").strip()
    try:
        return json.loads(out) if out else {}
    except json.JSONDecodeError:
        return {"raw": out}


_op = [0]


def call(command, args, screen=None, tolerate=False):
    _op[0] += 1
    key = "k%d-%d-%d" % (int(time.time()), _op[0], os.getpid())
    tmp = os.path.join(STATE, "args.json")
    os.makedirs(STATE, exist_ok=True)
    open(tmp, "w", encoding="utf-8").write(json.dumps(args))
    extra = ["--screen", screen] if screen else []
    reply = run("call", command, "--args-file", tmp, "--request-key", key, *extra,
                check=tolerate)
    op = reply.get("structuredContent", {}).get("operation") or reply.get("operation")
    if not op:
        print("no operation for", command, json.dumps(reply)[:400])
        return None
    while op.get("status") in ("queued", "running"):
        time.sleep(0.25)
        reply = run("operation", op["operationId"], *extra, check=tolerate)
        newop = reply.get("structuredContent", {}).get("operation") or reply.get("operation")
        if not newop:
            break
        op = newop
    print("%-14s %s" % (command, op.get("status")))
    if op.get("status") != "succeeded":
        print("   ", json.dumps(op.get("error"), ensure_ascii=False)[:400])
        if tolerate:
            return op
    return op


def snapshot(sid, name):
    path = os.path.join(SHOTS, "%s%s.png" % (TAG, name))
    call("debug.snapshot", {"sessionId": sid, "include": ["screen"]}, screen=path)
    return path


def main():
    global TAG
    argv = sys.argv[1:]
    mode = argv[0] if argv else "keys"
    no_build = "--no-build" in argv
    if "--tag" in argv:
        TAG = argv[argv.index("--tag") + 1] + "_"
    keys = []
    for a in argv[1:]:
        if a.startswith("--") or a == TAG.strip("_"):
            continue
        if mode == "keys" and re.match(r"^[A-Z0-9_,*+]+$", a) and ("," in a or a.count("*") or a.isalpha()):
            keys = a.split(",")
            break
    run("start")
    op = call("session.open", {"projectPath": PROJECT})
    sid = None
    try:
        sid = op["result"]["sessionId"]
    except Exception:
        print("no sessionId:", json.dumps(op)[:400])
        run("stop", check=False)
        return
    print("session", sid)
    if not no_build:
        call("build.run", {"sessionId": sid,
                           "configuration": os.environ.get("FXCONFIG", "Release")},
             tolerate=True)
    call("debug.launch", {"sessionId": sid})
    call("debug.resume", {"sessionId": sid})
    time.sleep(1.0)
    call("debug.pause", {"sessionId": sid})
    snapshot(sid, "00_start")
    n = 1
    for spec in keys:
        if not spec:
            continue
        m = re.match(r"^([A-Za-z0-9+]+)\*(\d+)$", spec)
        name, cnt = (m.group(1), int(m.group(2))) if m else (spec, 1)
        for _ in range(cnt):
            call("debug.resume", {"sessionId": sid})
            time.sleep(0.12)
            if name == "RELAUNCH":
                call("debug.stop", {"sessionId": sid})
                call("debug.launch", {"sessionId": sid})
                call("debug.resume", {"sessionId": sid})
            else:
                call("debug.key", {"sessionId": sid, "key": name, "action": "press"})
            time.sleep(cnt > 1 and 0.14 or 0.30)
            call("debug.pause", {"sessionId": sid})
            snapshot(sid, "%03d_%s" % (n, name.replace("+", "_")))
            n += 1
    call("debug.stop", {"sessionId": sid}, tolerate=True)
    call("session.close", {"sessionId": sid}, tolerate=True)
    run("stop", check=False)
    print("screenshots in", SHOTS)


if __name__ == "__main__":
    main()
