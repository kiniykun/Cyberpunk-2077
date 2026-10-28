#!/usr/bin/env python3
"""Diagnose compile-w32.py staleness: for every compiled unit, the first dependency newer than its object."""
import collections, json, os, re, sys
from pathlib import Path
W32 = Path(r'D:\2077\build-w32')
src = Path(r'D:\2077\work\w32\compile-w32.py').read_text(encoding='utf-8')
ns = {'json': json, 'os': os, 're': re, 'Path': Path, 'W32': W32}
exec(src[src.index('# Dependency staleness'):src.index('def todo(u):')], ns)
units = json.loads((W32 / 'jobs/units.json').read_text())
c = collections.Counter()
for u in units:
    if not os.path.exists(u['Object']): c['<no object>'] += 1; continue
    ds = ns['deps'](u['Object'])
    if ds is None: c['<no depfile>'] += 1; continue
    om = os.stat(u['Object']).st_mtime
    newer = [d for d in ds + [u['Input']] if ns['mtime'](d) > om]
    if newer:
        d = newer[0]; c[('MISSING ' if ns['mtime'](d) == float('inf') else '') + d] += 1
for k, v in c.most_common(int(sys.argv[1]) if len(sys.argv) > 1 else 20): print(v, k)
