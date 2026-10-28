#!/usr/bin/env python3
"""Write build-w32/handpatches/<file>.diff: every wasm32 copy in overlay-map.json against the file the wasm64
build compiles (clang-vfs.json target or original). Makes the hand edits reviewable and re-applicable."""
import json, subprocess
from pathlib import Path
W32 = Path(r'D:\2077\build-w32'); out = W32 / 'handpatches'; out.mkdir(exist_ok=True)
BS = chr(92)
mp = json.loads((W32 / 'overlay-map.json').read_text())
vfs = {r['name'].replace(BS, '/').lower(): r['external-contents']
       for r in json.loads(Path(r'D:\2077\clang-vfs.json').read_text(encoding='utf-8-sig'))['roots']}
for c, dst in sorted(mp.items()):
    src = vfs.get(c) or ('D:/2077/' + c[len('d:/2077/'):] if c.startswith('d:/2077/') else 'D:/Cyber punk/' + c[len('d:/cp2077sourcecode/'):])
    d = subprocess.run(['git', 'diff', '--no-index', '--', src, dst], capture_output=True, text=True, encoding='utf-8', errors='replace').stdout
    (out / (c.split('/')[-1] + '.diff')).write_text(d, encoding='utf-8')
    print(f'{len(d.splitlines()):5} {c.split("/")[-1]}')
