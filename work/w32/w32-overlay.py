#!/usr/bin/env python3
"""Manage wasm32-only source copies and build-w32/clang-vfs-w32.json (outer overlay, takes precedence).

  w32-overlay.py add <original path> [...]   copy the file the wasm64 build currently sees (the clang-vfs.json
                                             redirect target if any, else the original) into build-w32/src-w32,
                                             register it, regenerate the overlay. Never edits live files.
  w32-overlay.py gen                          regenerate clang-vfs-w32.json from build-w32/overlay-map.json
  w32-overlay.py resolve <path>               print the file the wasm32 build sees
Port paths get both spellings (D:/2077/... and D:/cp2077 port/...).
"""
import json, os, shutil, sys
from pathlib import Path
ROOT = Path(r'D:\2077'); W32 = ROOT / 'build-w32'; MAP = W32 / 'overlay-map.json'
DEV = 'd:/cp2077sourcecode/extracted/root/r6.root/mainline/dev/'
def key(p): return str(p).replace('\\', '/').lower()
def canon(p):
    k = key(p)
    for a in ('d:/cp2077 port/', 'd:/2077/'):
        if k.startswith(a): return 'd:/2077/' + k[len(a):]
    if k.startswith('d:/cyber punk/'): k = 'd:/cp2077sourcecode/' + k[len('d:/cyber punk/'):]
    return k
vfs = {}
for r in json.loads((ROOT / 'clang-vfs.json').read_text(encoding='utf-8-sig'))['roots']:
    vfs[canon(r['name'])] = r['external-contents']
mp = json.loads(MAP.read_text()) if MAP.exists() else {}
def mirror(c):
    if c.startswith(DEV): return W32 / 'src-w32/dev' / c[len(DEV):]
    if c.startswith('d:/2077/'): return W32 / 'src-w32/port' / c[len('d:/2077/'):]
    raise SystemExit('unsupported path ' + c)
def names(c):
    if c.startswith('d:/2077/'): return [c, 'd:/cp2077 port/' + c[len('d:/2077/'):]]
    return [c]
def gen():
    roots = []
    for c, dst in sorted(mp.items()):
        for n in names(c): roots.append({'type': 'file', 'name': n, 'external-contents': dst.replace('\\', '/')})
    (W32 / 'clang-vfs-w32.json').write_text(json.dumps({'version': 0, 'case-sensitive': False, 'use-external-names': False, 'roots': roots}, indent=1))
    MAP.write_text(json.dumps(mp, indent=1)); print(len(mp), 'overlay entries')
cmd = sys.argv[1]
if cmd == 'add':
    for p in sys.argv[2:]:
        c = canon(Path(p).as_posix())
        if c in mp: print('already', c, '->', mp[c]); continue
        src = vfs.get(c) or p
        dst = mirror(c); dst.parent.mkdir(parents=True, exist_ok=True)
        if not dst.exists(): shutil.copy2(src, dst); os.chmod(dst, 0o666)  # depot files are read-only
        mp[c] = str(dst); print(c, '<-', src, '->', dst)
    gen()
elif cmd == 'gen': gen()
elif cmd == 'resolve':
    c = canon(Path(sys.argv[2]).as_posix()); print(mp.get(c) or vfs.get(c) or sys.argv[2])
