"""Drive the FxStudio emulator and read program state with GDB expressions.

Usage:
  python work/verify.py --tag v1 --open "SHIFT,F3,DOWN,DOWN,EXE" \
      --eval "chunk_rows+view_top+1" --steps "DOWN*60"

  python work/verify.py --tag v2 --open "..." --steps "F2,7,7,7,F1"

Each step is a key name (or NAME*COUNT).  After every step the emulator is
paused and the --eval expression (comma separated list allowed) is read with
debug.evaluate; the values are printed one per line as
  STEP <n> <key> <value>[,<value>...]
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
os.makedirs(STATE, exist_ok=True)
_TMP = os.path.join(ROOT, ".tmp")
os.makedirs(_TMP, exist_ok=True)
os.environ["TMP"] = _TMP
os.environ["TEMP"] = _TMP


def run(*args, check=True):
    cmd = [PY, HELPER, "--package", PKG, "--state", STATE] + [str(a) for a in args]
    r = subprocess.run(cmd, capture_output=True, text=True)
    if r.returncode != 0 and check:
        print("CMD FAILED:", " ".join(cmd))
        print(r.stdout[-1200:])
        print(r.stderr[-1200:])
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
    tmp = os.path.join(STATE, "args-%d-%d.json" % (os.getpid(), _op[0]))
    open(tmp, "w", encoding="utf-8").write(json.dumps(args))
    extra = ["--screen", screen] if screen else []
    reply = run("call", command, "--args-file", tmp, "--request-key", key, *extra,
                check=not tolerate)
    op = reply.get("structuredContent", {}).get("operation") or reply.get("operation")
    if not op:
        print("no operation for", command, json.dumps(reply)[:300])
        return None
    while op.get("status") in ("queued", "running"):
        time.sleep(0.2)
        reply = run("operation", op["operationId"], *extra, check=not tolerate)
        newop = reply.get("structuredContent", {}).get("operation") or reply.get("operation")
        if not newop:
            break
        op = newop
    if op.get("status") != "succeeded":
        print(command, op.get("status"), json.dumps(op.get("error"), ensure_ascii=False)[:300])
    return op


def result(op):
    if not op:
        return None
    return op.get("result")


def evaluate(sid, expr):
    op = call("debug.evaluate", {"sessionId": sid, "expression": expr}, tolerate=True)
    r = result(op)
    if not r:
        return None
    for k in ("value", "result", "text"):
        if isinstance(r, dict) and k in r:
            return r[k]
    return r


def snapshot(sid, name):
    path = os.path.join(SHOTS, "%s%s.png" % (TAG, name))
    call("debug.snapshot", {"sessionId": sid, "include": ["screen"]}, screen=path, tolerate=True)


def steps_of(spec):
    out = []
    if not spec:
        return out
    for part in spec.split(","):
        part = part.strip()
        if not part:
            continue
        m = re.match(r"^([A-Za-z0-9+]+)\*(\d+)$", part)
        if m:
            out += [m.group(1)] * int(m.group(2))
        else:
            out.append(part)
    return out


def press(sid, name, delay=0.16):
    if name == "RELAUNCH":
        call("debug.stop", {"sessionId": sid}, tolerate=True)
        call("debug.launch", {"sessionId": sid})
        call("debug.resume", {"sessionId": sid})
        time.sleep(0.8)
        return
    call("debug.key", {"sessionId": sid, "key": name, "action": "press"}, tolerate=True)
    time.sleep(delay)


def kill_stale():
    """Kill FxStudio.Agent and its python parent from a previous run.

    A surviving host holds the 1024-operation budget and the state directory,
    which makes the next `start` fail with a 'Permission denied' bridge error.
    """
    ps = subprocess.run(
        ["powershell", "-NoProfile", "-Command",
         "Get-CimInstance Win32_Process -Filter \"Name='FxStudio.Agent'\" | "
         "ForEach-Object { \"$($_.ProcessId) $($_.ParentProcessId)\" }"],
        capture_output=True, text=True)
    pids = []
    for line in ps.stdout.split():
        if line.isdigit():
            pids.append(int(line))
    for pid in pids:
        subprocess.run(["taskkill", "/F", "/PID", str(pid)],
                       capture_output=True, text=True)
    if pids:
        print("killed stale host pids", pids)
        time.sleep(1.5)


def install_source(mode):
    """(Re)install the working source before a session build.

    The tree has been reset to the original source by an outside actor, so
    every verification run re-installs work/main_patched.c (mode 'real') or
    the small-window debug variant of it (mode 'small').
    """
    base = os.path.join(ROOT, "work", "main_patched.c")
    dst = os.path.join(ROOT, "work", "fxtxt", "src", "main.c")
    b = open(base, "rb").read()
    if mode == "small":
        cap = b"for (int cap = 32768; cap >= TEXT_MIN; cap /= 2) {"
        tmin = b"#define TEXT_MIN    4096"
        assert b.count(cap) == 1 and b.count(tmin) == 1
        b = b.replace(cap, b"for (int cap = 1024; cap >= TEXT_MIN; cap /= 2) {")
        b = b.replace(tmin, b"#define TEXT_MIN 1024")
    open(dst, "wb").write(b)
    print("installed source mode", mode, len(b), "bytes")


def main():
    global TAG
    argv = sys.argv[1:]
    args = {}
    i = 0
    while i < len(argv):
        if argv[i].startswith("--"):
            args[argv[i][2:]] = argv[i + 1]
            i += 2
        else:
            i += 1
    TAG = (args.get("tag", "v") + "_")
    expr = args.get("eval", "")
    exprs = [e.strip() for e in expr.split(";") if e.strip()]
    sess = args.get("session") or ""
    if sess:
        sid = sess
        print("attached", sid)
    else:
        kill_stale()
        install_source(args.get("src", "small"))
        run("start")
        op = call("session.open", {"projectPath": PROJECT})
        sid = result(op).get("sessionId")
        print("session", sid)
        call("build.run", {"sessionId": sid,
                           "configuration": os.environ.get("FXCONFIG", "Release")}, tolerate=True)
        call("debug.launch", {"sessionId": sid})
        call("debug.resume", {"sessionId": sid})
        time.sleep(1.0)
        call("debug.pause", {"sessionId": sid})
        snapshot(sid, "000_start")
        if exprs:
            print("START", ", ".join("%s=%s" % (e, evaluate(sid, e)) for e in exprs))
    n = 0
    for name in steps_of(args.get("open", "")):
        call("debug.resume", {"sessionId": sid})
        press(sid, name)
        call("debug.pause", {"sessionId": sid})
        snapshot(sid, "%03d_%s" % (n, name.replace("+", "_")))
        n += 1
    if exprs:
        print("OPEN", ", ".join("%s=%s" % (e, evaluate(sid, e)) for e in exprs))
    snaps = args.get("snaps", "1") == "1"
    batch = max(1, int(args.get("batch", "1")))
    names = steps_of(args.get("steps", ""))
    for k, name in enumerate(names, 1):
        if (k - 1) % batch == 0:
            call("debug.resume", {"sessionId": sid})
        press(sid, name, 0.14)
        last = k == len(names)
        if k % batch and not last:
            continue
        call("debug.pause", {"sessionId": sid})
        if snaps and (k % int(args.get("every", "1")) == 0):
            snapshot(sid, "%03d_%s" % (k, name.replace("+", "_")))
        vals = [evaluate(sid, e) for e in exprs]
        print("STEP %d %s %s" % (k, name, ",".join(str(v) for v in vals)), flush=True)
    if args.get("hold"):
        print("HOLD", evaluate(sid, args["hold"]))
    if args.get("keep") == "1":
        print("kept session", sid)
        return
    call("debug.stop", {"sessionId": sid}, tolerate=True)
    call("session.close", {"sessionId": sid}, tolerate=True)
    run("stop", check=False)


if __name__ == "__main__":
    main()
