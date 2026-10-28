#!/usr/bin/env python3
"""Resumable wasm32 compile driver for build-w32/jobs/units.json.

State: build-w32/state.json  {Object: {ok, args, sec, errors}}  (atomic replace after every unit)
Failures: build-w32/failures.json (first error lines per failed unit)
A unit is skipped when state says ok, the object exists and the args hash is unchanged.
Compilers run at BELOW_NORMAL priority; default 2 workers (shared PC).

Usage: python compile-w32.py [--workers 2] [--module M[,M]] [--match REGEX] [--sample] [--retry-failed]
                            [--invalidate REGEX]  (forget state of matching objects, e.g. after an overlay header change)
"""
import argparse, concurrent.futures as cf, hashlib, json, os, re, subprocess, sys, threading, time
from pathlib import Path

W32 = Path(r'D:\2077\build-w32')
PY = r'D:\Cyber punk\tools\emsdk\python\3.13.3_64bit\python.exe'
EM = r'D:\Cyber punk\tools\emsdk\upstream\emscripten\em++.py'
ENV = dict(os.environ, EMSDK_PYTHON=PY)
LOCK = threading.Lock()
ap = argparse.ArgumentParser()
ap.add_argument('--workers', type=int, default=2); ap.add_argument('--module'); ap.add_argument('--match')
ap.add_argument('--sample', action='store_true'); ap.add_argument('--retry-failed', action='store_true')
ap.add_argument('--invalidate'); ap.add_argument('--limit', type=int)
a = ap.parse_args()

units = json.loads((W32 / 'jobs/units.json').read_text())
sp = W32 / 'state.json'
state = json.loads(sp.read_text()) if sp.exists() else {}
def h(args): return hashlib.sha256('|'.join(args).encode()).hexdigest()[:16]
def save():
    t = sp.with_suffix('.tmp'); t.write_text(json.dumps(state, indent=0)); os.replace(t, sp)
    fails = {k: v for k, v in state.items() if not v.get('ok')}
    t = W32 / 'failures.tmp'; t.write_text(json.dumps(fails, indent=1)); os.replace(t, W32 / 'failures.json')

if a.invalidate:
    n = 0
    for k in list(state):
        if re.search(a.invalidate, k, re.I): del state[k]; n += 1
    save(); print('invalidated', n)
    if not (a.module or a.match or a.sample): sys.exit(0)

sel = units
if a.module: mods = set(a.module.split(',')); sel = [u for u in sel if u['Module'] in mods]
if a.match: sel = [u for u in sel if re.search(a.match, u['Object'] + ' ' + u['Input'], re.I)]
if a.sample:
    seen, s = set(), []
    for u in sel:
        if u['Module'] not in seen or u['Module'] in ('objects', 'webgpu', 'engine-startup'): seen.add(u['Module']); s.append(u)
    sel = s

# Dependency staleness: every unit is compiled with -MD -MF <obj>.d (not part of the args hash). A dependency is
# mapped through clang-vfs-w32.json (outer) then clang-vfs.json to the file clang really read; newer -> recompile.
BS = chr(92)
def vkey(p): return os.path.normpath(p).replace(BS, '/').lower()
def vfsmap(path):
    m = {}
    try:
        for r in json.loads(Path(path).read_text(encoding='utf-8-sig'))['roots']:
            m[vkey(r['name'])] = r['external-contents']
    except FileNotFoundError: pass
    return m
VFS64 = vfsmap(Path('D:/2077/clang-vfs.json')); VFS32 = vfsmap(W32 / 'clang-vfs-w32.json')
_mt = {}
def mtime(dep):
    real = os.path.normpath(VFS32.get(vkey(dep)) or VFS64.get(vkey(dep)) or dep)  # normpath: '..' chains exceed MAX_PATH
    if real not in _mt:
        try: _mt[real] = os.stat(real).st_mtime
        except OSError: _mt[real] = float('inf')
    return _mt[real]
TOKEN = re.compile(r'(?:' + re.escape(BS) + r' |\S)+')
def deps(obj):
    d = Path(str(obj) + '.d')
    if not d.exists(): return None
    t = d.read_text(encoding='utf-8', errors='replace').replace(BS + '\r\n', ' ').replace(BS + '\n', ' ')
    i = t.find('.tmp: ')
    body = t[i + 6:] if i >= 0 else t.split(': ', 1)[-1]
    return [x.replace(BS + ' ', ' ') for x in TOKEN.findall(body)]
def stale(u):
    ds = deps(u['Object'])
    if ds is None: return True
    om = os.stat(u['Object']).st_mtime
    return any(mtime(x) > om for x in ds) or mtime(u['Input']) > om

def todo(u):
    r = state.get(u['Object'])
    if r and r.get('ok') and r.get('args') == h(u['Args']) and Path(u['Object']).exists() and not stale(u): return False
    if r and not r.get('ok') and r.get('args') == h(u['Args']) and not a.retry_failed: return False
    return True
pending = [u for u in sel if todo(u)]
if a.limit: pending = pending[:a.limit]
print(f'selected {len(sel)}, pending {len(pending)}', flush=True)

def run(u):
    obj = Path(u['Object']); obj.parent.mkdir(parents=True, exist_ok=True)
    tmp = obj.with_name(obj.name + '.tmp'); t0 = time.time()
    with open(u['Log'], 'wb') as log:
        rc = subprocess.run([PY, EM, *u['Args'], '-MD', '-MF', str(obj) + '.d', '-c', u['Input'], '-o', str(tmp)], stdout=log, stderr=subprocess.STDOUT,
                            env=ENV, cwd=str(W32), creationflags=0x00004000).returncode
    rec = {'ok': rc == 0, 'args': h(u['Args']), 'sec': round(time.time() - t0, 1), 'module': u['Module']}
    if rc == 0: os.replace(tmp, obj)
    else:
        tmp.unlink(missing_ok=True)
        lines = Path(u['Log']).read_text(encoding='utf-8', errors='replace').splitlines()
        rec['errors'] = [l[:400] for l in lines if 'error' in l][:6] or lines[-6:]
    with LOCK: state[u['Object']] = rec; save()
    return u, rec

done = 0
with cf.ThreadPoolExecutor(a.workers) as pool:
    for fut in cf.as_completed([pool.submit(run, u) for u in pending]):
        u, rec = fut.result(); done += 1
        print(f"[{done}/{len(pending)}] {'ok  ' if rec['ok'] else 'FAIL'} {rec['sec']:6.1f}s {u['Module']}/{Path(u['Object']).name}", flush=True)
        if not rec['ok']:
            for l in rec['errors'][:3]: print('     ', l, flush=True)
ok = sum(1 for u in units if state.get(u['Object'], {}).get('ok'))
fail = sum(1 for u in units if u['Object'] in state and not state[u['Object']].get('ok'))
print(f'TOTAL ok {ok} / {len(units)}, failed {fail}', flush=True)
