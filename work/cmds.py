import json
import re

p = r'ref\pkg3\FxStudio-0.2.0-win-x64\package-manifest.json'
t = open(p, encoding='utf-8').read()
names = sorted(set(re.findall(r'"([a-z][a-zA-Z0-9]*\.[a-zA-Z0-9]+)"', t)))
print('\n'.join(names))
