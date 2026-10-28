#!/usr/bin/env python3
"""Prove the generator's reconstructed wasm64 args (Args64) reproduce the live objects.
Compiles Args64 into build-w32/verify64/ (scratch) and compares llvm-objdump -d code hashes with the live object.
Selection: all direct objects (objects/webgpu/engine-startup/voice-broadcast), all redMemory, units owned by the
optimized-frame / segmented-tlsf / sparse-big manifests, and one unit per module.
Result: build-w32/verify64/result.json. Never writes outside build-w32.
"""
import concurrent.futures as cf, hashlib, json, os, subprocess, sys
from pathlib import Path
W32 = Path(r'D:\2077\build-w32'); ROOT = Path(r'D:\2077')
PY = r'D:\Cyber punk\tools\emsdk\python\3.13.3_64bit\python.exe'
EM = r'D:\Cyber punk\tools\emsdk\upstream\emscripten\em++.py'
OD = r'D:\Cyber punk\tools\emsdk\upstream\bin\llvm-objdump.exe'
ENV = dict(os.environ, EMSDK_PYTHON=PY)
units = json.loads((W32 / 'jobs/units.json').read_text())
owned = set()
p = ROOT / 'build/optimized-frame-units-result.json'
if p.exists(): owned |= {Path(u['object']).name.lower() for u in json.loads(p.read_text())['units']}
for seg in ('segmented-tlsf-result.json', 'sparse-big-allocator-result.json'):
    owned.add(Path(json.loads((ROOT / 'build' / seg).read_text())['object']['path']).name.lower())
sel, seen = [], set()
for u in units:
    n = Path(u['Object']).name.lower()
    if u['Module'] in ('objects', 'webgpu', 'engine-startup', 'voice-broadcast', 'redMemory') or n in owned or u['Module'] not in seen:
        sel.append(u); seen.add(u['Module'])
if len(sys.argv) > 1: sel = [u for u in sel if any(x in u['Object'] for x in sys.argv[1:])]
out = W32 / 'verify64'; out.mkdir(exist_ok=True)
def code(path):
    t = subprocess.run([OD, '-d', str(path)], capture_output=True, text=True).stdout.splitlines()[2:]
    return hashlib.sha256('\n'.join(t).encode()).hexdigest()
def run(u):
    tmp = out / (u['Module'] + '__' + Path(u['Object']).name)
    r = subprocess.run([PY, EM, *u['Args64'], '-c', u['Input'], '-o', str(tmp)], capture_output=True, text=True, env=ENV,
                       cwd=str(W32), creationflags=0x00004000)
    if r.returncode: return u, 'compile-failed', (r.stdout + r.stderr)[-600:]
    same = code(tmp) == code(u['LiveObject'])
    if same: tmp.unlink()
    return u, ('same' if same else 'DIFFERS'), ''
res = []
with cf.ThreadPoolExecutor(2) as pool:
    for u, st, msg in pool.map(run, sel):
        res.append({'Module': u['Module'], 'Object': Path(u['Object']).name, 'Recipe': u['Recipe'], 'Status': st, 'Msg': msg})
        print(st, u['Module'], Path(u['Object']).name, u['Recipe'], flush=True)
(out / 'result.json').write_text(json.dumps(res, indent=1))
print({s: sum(r['Status'] == s for r in res) for s in ('same', 'DIFFERS', 'compile-failed')})
