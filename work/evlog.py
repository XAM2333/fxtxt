import json
import os
import subprocess
import sys

ROOT = r'E:\fxtxtviewer'
PKG = os.path.join(ROOT, 'ref', 'pkg3', 'FxStudio-0.2.0-win-x64')
PY = os.path.join(PKG, 'sdk', 'native', 'host', 'python', 'python.exe')
HELPER = os.path.join(ROOT, 'ref', 'skill', 'fxstudio', 'scripts', 'fxstudio.py')
STATE = os.path.join(ROOT, 'work', '.fxstudio', 'state')
os.environ['TMP'] = os.path.join(ROOT, '.tmp')
os.environ['TEMP'] = os.environ['TMP']

r = subprocess.run([PY, HELPER, '--package', PKG, '--state', STATE, 'events', '--after', '0'],
                   capture_output=True, text=True)
d = json.loads(r.stdout)
evs = d['structuredContent']['events']
print('events', len(evs))
n = int(sys.argv[1]) if len(sys.argv) > 1 else 1
for e in evs[-n:]:
    print('==', e.get('kind'), e.get('operationId'), e.get('level'))
    print((e.get('text') or '')[-4000:])
