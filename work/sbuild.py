"""Start the FxStudio host, open a session, run build.run and dump the log."""
import json
import os
import subprocess
import sys

ROOT = r'E:\fxtxtviewer'
PKG = os.path.join(ROOT, 'ref', 'pkg3', 'FxStudio-0.2.0-win-x64')
PY = os.path.join(PKG, 'sdk', 'native', 'host', 'python', 'python.exe')
HELPER = os.path.join(ROOT, 'ref', 'skill', 'fxstudio', 'scripts', 'fxstudio.py')
STATE = os.path.join(ROOT, 'work', '.fxstudio', 'state')
PROJECT = os.path.join(ROOT, 'work', 'fxtxt')
os.environ['TMP'] = os.path.join(ROOT, '.tmp')
os.environ['TEMP'] = os.environ['TMP']
print('TMP=', os.environ['TMP'])


def run(*a):
    r = subprocess.run([PY, HELPER, '--package', PKG, '--state', STATE] + [str(x) for x in a],
                       capture_output=True, text=True)
    return r.returncode, r.stdout, r.stderr


def call(command, args):
    tmp = os.path.join(STATE, 'args.json')
    open(tmp, 'w', encoding='utf-8').write(json.dumps(args))
    rc, out, err = run('call', command, '--args-file', tmp, '--request-key', 'k' + command)
    try:
        d = json.loads(out)
    except Exception:
        print('call failed', command, out[:400], err[:400])
        return None
    op = (d.get('structuredContent') or {}).get('operation') or d.get('operation')
    return op


def wait(op):
    import time
    while op and op.get('status') in ('queued', 'running'):
        time.sleep(0.3)
        rc, out, err = run('operation', op['operationId'])
        try:
            d = json.loads(out)
        except Exception:
            break
        op = (d.get('structuredContent') or {}).get('operation') or d.get('operation')
    return op


print(run('start')[1][:200])
op = wait(call('session.open', {'projectPath': PROJECT}))
print('session', json.dumps(op.get('result'))[:200] if op else None)
sid = (op.get('result') or {}).get('sessionId')
op = wait(call('build.run', {'sessionId': sid, 'configuration': os.environ.get('FXCONFIG', 'Release')}))
print('build', op.get('status'), json.dumps(op.get('error'))[:300])
rc, out, err = run('events', '--after', '0')
try:
    d = json.loads(out)
    evs = d['structuredContent']['events']
    open(os.path.join(ROOT, 'work', 'events.json'), 'w', encoding='utf-8').write(
        json.dumps(evs, indent=1, ensure_ascii=False))
    print('events', len(evs), 'keys', sorted(set(k for e in evs for k in e)))
    for e in evs:
        if e.get('kind') == 'build.log':
            print('--- build.log ---')
            print(json.dumps(e, ensure_ascii=False)[:6000])
except Exception as ex:
    print('events failed', ex, out[:300], err[:300])
print(run('stop')[1][:200])
