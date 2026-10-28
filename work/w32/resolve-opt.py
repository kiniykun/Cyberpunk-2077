#!/usr/bin/env python3
"""For units whose optimization level is guessed (Recipe has o2detected / optimize), compile wasm64 variants into
build-w32/verify64 and keep the one whose llvm-objdump -d code equals the live object.
Writes build-w32/jobs/opt-overrides.json {LiveObject(lower): Args64}; make-w32-jobs.py applies it."""
import concurrent.futures as cf, hashlib, json, os, re, subprocess
from pathlib import Path
W32 = Path(r'D:\2077\build-w32')
PY = r'D:\Cyber punk\tools\emsdk\python\3.13.3_64bit\python.exe'
EM = r'D:\Cyber punk\tools\emsdk\upstream\emscripten\em++.py'
OD = r'D:\Cyber punk\tools\emsdk\upstream\bin\llvm-objdump.exe'
ENV = dict(os.environ, EMSDK_PYTHON=PY)
def fix_fm(args):
    args = list(args)
    if '-ffast-math' in args:
        args = [x for x in args if x not in ('-fno-finite-math-only', '-D__FAST_MATH__=1')]
        i = max(k for k, x in enumerate(args) if x == '-ffast-math')
        args[i + 1:i + 1] = ['-fno-finite-math-only', '-D__FAST_MATH__=1']
    return args
def strip(a): return [x for x in a if not re.fullmatch(r'-O[0-3sz]?', x)]
def variants(a):
    s = strip(a); out = []
    for v in (s + ['-O0'] if '-O0' in a or not any(re.fullmatch(r'-O[0-3sz]', x) for x in a) else a,
              a, s, fix_fm(s + ['-O2', '-msimd128']), fix_fm(s) + ['-O2', '-msimd128'], fix_fm(s) + ['-O2'],
              s + ['-O2'], fix_fm(s) + ['-O3', '-msimd128'], fix_fm(s) + ['-O1']):
        if v not in out: out.append(v)
    return out
def code(p): return hashlib.sha256('\n'.join(subprocess.run([OD, '-d', str(p)], capture_output=True, text=True).stdout.splitlines()[2:]).encode()).hexdigest()
units = [u for u in json.loads((W32 / 'jobs/units.json').read_text()) if 'o2detected' in u['Recipe'] or 'optimize' in u['Recipe']]
op = W32 / 'jobs/opt-overrides.json'
over = json.loads(op.read_text()) if op.exists() else {}
def run(u):
    live = code(u['LiveObject']); tmp = W32 / 'verify64' / ('opt__' + Path(u['Object']).name)
    base = u.get('Args64Base', u['Args64'])
    for v in variants(base):
        r = subprocess.run([PY, EM, *v, '-c', u['Input'], '-o', str(tmp)], capture_output=True, env=ENV, creationflags=0x4000)
        if r.returncode == 0 and code(tmp) == live: tmp.unlink(); return u, v
    return u, None
todo = [u for u in units if u['LiveObject'].lower() not in over]
with cf.ThreadPoolExecutor(2) as pool:
    for u, v in pool.map(run, todo):
        print(('match ' + ' '.join(x for x in v if x.startswith('-O'))) if v else 'NO MATCH', u['Module'], Path(u['Object']).name, flush=True)
        if v: over[u['LiveObject'].lower()] = v
        op.write_text(json.dumps(over, indent=1))
