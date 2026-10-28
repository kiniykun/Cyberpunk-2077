#!/usr/bin/env python3
"""Summarise build-w32/failures.json: counts by first error (path stripped), and units whose errors are not
plain `sizeof(X) == N` asserts (those are handled by auto-relax-asserts.py). Optional arg: --units"""
import collections, json, re, sys
from pathlib import Path
f = json.loads(Path(r'D:\2077\build-w32\failures.json').read_text())
plain = re.compile(r"sizeof\(.*\) == \d+'")
c = collections.Counter(); other = []
for obj, rec in f.items():
    errs = [e for e in rec.get('errors', []) if 'error:' in e] or rec.get('errors', [])
    first = re.sub(r':\d+:\d+:', ':', re.sub(r'^.*[\\/]', '', errs[0])) if errs else '?'
    c[first[:220]] += 1
    if not any(plain.search(e) for e in errs): other.append((rec.get('module'), Path(obj).name, first[:220]))
print(len(f), 'failed units')
for e, n in c.most_common(30): print(f'{n:4} {e}')
if '--units' in sys.argv or len(other) < 40:
    print('--- not plain size asserts ---')
    for o in sorted(other): print(*o, sep=' | ')
