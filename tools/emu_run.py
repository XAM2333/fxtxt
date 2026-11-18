"""Drive the Fx Studio agent: build a project, run it in the emulator, take
screenshots and send key presses. Usage: emu_run.py MODE [args]

MODE=shot  : open session, build, launch, screenshot to work/shots/NN.png, stop
MODE=keys  : open session, build, launch, screenshot, press a key sequence
"""
import json
import os
import subprocess
import sys
import time

PKG = os.path.abspath(os.path.join("ref", "pkg3", "FxStudio-0.2.0-win-x64"))
PY = os.path.join(PKG, "sdk", "native", "host", "python", "python.exe")
HELPER = os.path.abspath(os.path.join("ref", "skill", "fxstudio", "scripts", "fxstudio.py"))
STATE = os.path.abspath(os.path.join("work", ".fxstudio", "state"))
SHOTS = os.path.abspath(os.path.join("work", "shots"))
PROJECT = os.environ.get("FXPROJECT") or os.path.abspath(os.path.join("work", "fxtxt"))
os.makedirs(SHOTS, exist_ok=True)


def run(*args, check=True):
    cmd = [PY, HELPER, "--package", PKG, "--state", STATE] + [str(a) for a in args]
    r = subprocess.run(cmd, capture_output=True, text=True)
    if r.returncode != 0 and check:
        print("CMD FAILED:", " ".join(cmd))
        print(r.stdout[-2000:])
        print(r.stderr[-2000:])
        raise SystemExit(1)
    out = r.stdout.strip()
    try:
        return json.loads(out) if out else {}
    except json.JSONDecodeError:
        return {"raw": out}


_op = [0]


def call(command, args, screen=None):
    _op[0] += 1
    key = "k%d-%d-%d" % (int(time.time()), _op[0], os.getpid())
    tmp = os.path.join(STATE, "args.json")
    os.makedirs(STATE, exist_ok=True)
    open(tmp, "w", encoding="utf-8").write(json.dumps(args))
    extra = ["--screen", screen] if screen else []
    reply = run("call", command, "--args-file", tmp, "--request-key", key, *extra)
    op = reply.get("structuredContent", {}).get("operation") or reply.get("operation")
    if not op:
        print("no operation in reply:", json.dumps(reply)[:800])
        return None
    while op.get("status") in ("queued", "running"):
        time.sleep(0.4)
        reply = run("operation", op["operationId"], "--screen", screen) if screen else \
            run("operation", op["operationId"])
        op = reply.get("structuredContent", {}).get("operation") or reply.get("operation")
    print("%-14s %s" % (command, op.get("status")))
    if op.get("status") != "succeeded":
        print(json.dumps(op.get("error"), ensure_ascii=False)[:800])
    return op


def main():
    mode = sys.argv[1] if len(sys.argv) > 1 else "shot"
    keys = sys.argv[2].split(",") if len(sys.argv) > 2 else []
    run("start")
    op = call("session.open", {"projectPath": PROJECT})
    sid = None
    try:
        sid = op["result"]["sessionId"]
    except Exception:
        print("no sessionId:", json.dumps(op)[:500])
        run("stop", check=False)
        return
    print("session", sid)
    call("build.run", {"sessionId": sid, "configuration": os.environ.get("FXCONFIG","Debug")})
    call("debug.launch", {"sessionId": sid})
    # the add-in is stopped at main; let it run so it can draw
    call("debug.resume", {"sessionId": sid})
    time.sleep(1.2)
    call("debug.pause", {"sessionId": sid})
    shot = os.path.join(SHOTS, "00_start.png")
    call("debug.snapshot", {"sessionId": sid, "include": ["screen"]}, screen=shot)
    n = 1
    for k in keys:
        call("debug.resume", {"sessionId": sid})
        time.sleep(0.25)
        if k == "RELAUNCH":
            # stop the add-in and start it again in the same emulator session
            call("debug.stop", {"sessionId": sid})
            call("debug.launch", {"sessionId": sid})
            call("debug.resume", {"sessionId": sid})
        else:
            call("debug.key", {"sessionId": sid, "key": k, "action": "press"})
        time.sleep(0.45)
        call("debug.pause", {"sessionId": sid})
        shot = os.path.join(SHOTS, "%02d_%s.png" % (n, k.replace("+", "_")))
        call("debug.snapshot", {"sessionId": sid, "include": ["screen"]}, screen=shot)
        n += 1
    call("debug.stop", {"sessionId": sid})
    call("session.close", {"sessionId": sid})
    run("stop", check=False)
    print("screenshots in", SHOTS)


if __name__ == "__main__":
    main()


