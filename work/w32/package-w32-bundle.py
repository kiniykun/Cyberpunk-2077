#!/usr/bin/env python3
"""Package build-w32/game/cp2077.{js,wasm,data} as an immutable engine-versions bundle (same layout and BuildID rule
as work/publish-versioned-engine-bundle.py: sha256 of the sorted per-file sha256 map), plus .gz siblings for
cloudflare/scripts/upload-bundle.mjs. Creates a NEW build/game/engine-versions/<sha> folder only: it does not edit
native-browser.html, build/active-engine-bundle.json or any existing bundle. Not promoted anywhere."""
import gzip, hashlib, json, shutil, sys
from pathlib import Path
SRC = Path(r'D:\2077\build-w32\game'); DST = Path(r'D:\2077\build\game\engine-versions')
files = ['cp2077.js', 'cp2077.wasm', 'cp2077.data']
def sha(p): return hashlib.sha256(p.read_bytes()).hexdigest()
hashes = {n: sha(SRC / n) for n in files}
identity = hashlib.sha256(json.dumps(hashes, sort_keys=True).encode()).hexdigest()
folder = DST / identity; folder.mkdir(parents=True, exist_ok=True)
for n in files:
    t = folder / n
    if t.exists(): assert sha(t) == hashes[n], 'immutable bundle modified: ' + str(t)
    else: shutil.copyfile(SRC / n, t); assert sha(t) == hashes[n]
    g = folder / (n + '.gz')
    if not g.exists():
        with open(t, 'rb') as fi, open(g, 'wb') as raw, gzip.GzipFile(fileobj=raw, mode='wb', compresslevel=9, mtime=0) as fo: shutil.copyfileobj(fi, fo, 1 << 20)
link = json.loads((Path(r'D:\2077\build-w32') / 'link-result.json').read_text())
manifest = {'BuildID': identity, 'Files': hashes,
            'Scope': 'wasm32 (no MEMORY64) phase-1 build of the engine for iPhone Safari: same sources/flags as the wasm64 '
                     'build except -sMEMORY64 removed, guarded wasm32 overlay fixes (build-w32/overlay-map.json), '
                     'commit-on-demand slab chunks, wasm32 JS glue copies, MAXIMUM_MEMORY=4GB. Not promoted. '
                     'capcheck.js blocks non-memory64 browsers; use ?force=1 on desktop.',
            'Architecture': 'wasm32', 'MaximumLinearMemoryBytes': link.get('MaximumLinearMemoryBytes'),
            'Unresolved': link.get('Unresolved')}
(folder / 'bundle.json').write_text(json.dumps(manifest, indent=2))
print(identity)
