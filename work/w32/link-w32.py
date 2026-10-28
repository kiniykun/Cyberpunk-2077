#!/usr/bin/env python3
"""wasm32 strict link into build-w32/game (never touches build/, build/game, build-result.json or outputs/).

Mirrors work/relink-prepared-engine.py's link line with: wasm32 objects/archives from build-w32, the wasm32 glue
copies from build-w32/glue, no -sMEMORY64, -sMAXIMUM_MEMORY=4GB. Archives are rebuilt from build-w32 objects
(old archive deleted first, @rsp response file).
Usage: python link-w32.py [--archives-only] [--no-archive]
"""
import hashlib, json, os, re, subprocess, sys, time
from pathlib import Path
ROOT = Path(r'D:\2077'); W32 = ROOT / 'build-w32'
SDK = Path(r'D:\Cyber punk\tools\emsdk'); PY = str(SDK / 'python/3.13.3_64bit/python.exe')
EMAR = str(SDK / 'upstream/emscripten/emar.py'); EMXX = str(SDK / 'upstream/emscripten/em++.py')
ENV = dict(os.environ, EMSDK_PYTHON=PY)
li = json.loads((W32 / 'jobs/link-inputs.json').read_text())
missing = [o for o in li['Objects'] if not Path(o).exists()]
for L in li['Libraries']: missing += [o for o in L['Objects'] if not Path(o).exists()]
if missing:
    print(f'{len(missing)} wasm32 objects missing, e.g. {missing[:5]}'); sys.exit(2)
(W32 / 'lib').mkdir(exist_ok=True); (W32 / 'rsp').mkdir(exist_ok=True)
if '--no-archive' not in sys.argv:
    for L in li['Libraries']:
        lib = Path(L['Library'])
        newest = max(os.stat(o).st_mtime for o in L['Objects'])
        if lib.exists() and os.stat(lib).st_mtime > newest: continue
        rsp = W32 / 'rsp' / (lib.stem + '.rsp')
        rsp.write_text('\n'.join('"' + o.replace('\\', '/') + '"' for o in L['Objects']), encoding='utf-8')
        lib.unlink(missing_ok=True)
        subprocess.run([PY, EMAR, 'rcs', str(lib), f'@{rsp}'], check=True, env=ENV)
        print('archived', lib.name, len(L['Objects']), flush=True)
if '--archives-only' in sys.argv: sys.exit(0)
G = W32 / 'glue'; out = W32 / 'game'; out.mkdir(exist_ok=True)
inputs = li['Objects'] + [L['Library'] for L in li['Libraries']]
args = [PY, EMXX, *inputs, '--use-port=emdawnwebgpu']
args += ['--js-library', str(SDK / 'upstream/emscripten/src/lib/libidbfs.js'), '-sEXPORTED_RUNTIME_METHODS=IDBFS']
args += ['--js-library', str(G / 'webgpu-lowalloc-web.js'), '-sDEFAULT_LIBRARY_FUNCS_TO_INCLUDE=$cp2077Glue']
for name in ('oodle-decoder-broker-web.js', 'bink-decoder-broker-web.js', 'filesystem-web.js', 'resource-filesystem-web.js', 'stdio-async-web.js'):
    args += ['--pre-js', str(G / name)]
args += ['--preload-file', str(ROOT / 'runtime/engine') + '@/game/engine', '-lidbfs.js', '-sFORCE_FILESYSTEM=1', '-sFETCH=1', '-fshort-wchar',
         '-pthread', '-msimd128', '-sPROXY_TO_PTHREAD=1', '-sPTHREAD_POOL_SIZE=4', '-sSTACK_SIZE=8388608',
         '-sDEFAULT_PTHREAD_STACK_SIZE=8388608', '-g2', '-sENVIRONMENT=web', '-sEXIT_RUNTIME=0', '-sALLOW_MEMORY_GROWTH=1',
         '-sINITIAL_MEMORY=201326592', '-sMAXIMUM_MEMORY=4294967296', '-sMODULARIZE=1', '-sEXPORT_NAME=createCP2077',
         '-Wl,--error-limit=0', '-o', str(out / 'cp2077.js')]
args += ['--preload-file', str(ROOT / 'runtime/low-config/r6/config') + '@/game/r6/config',
         '--preload-file', str(ROOT / 'runtime/low-config/engine/config') + '@/game/engine/config']
if os.environ.get('CPWEB_MALLOC'): args += ['-sMALLOC=' + os.environ['CPWEB_MALLOC']]
if os.environ.get('CPWEB_MALLOC_TRACE') == '1':  # diagnostic build only: large-malloc attribution (work/w32/malloc-trace-w32.cpp)
    tobj = W32 / 'diag' / 'malloc-trace.o'; tobj.parent.mkdir(exist_ok=True)
    subprocess.run([PY, EMXX, '-O2', '-pthread', '-std=c++17', '-c', str(ROOT / 'work/w32/malloc-trace-w32.cpp'), '-o', str(tobj)], check=True, env=ENV)
    args.insert(2, str(tobj))
    args += ['-Wl,--wrap=malloc,--wrap=free,--wrap=calloc,--wrap=realloc,--wrap=memalign,--wrap=aligned_alloc,--wrap=posix_memalign']
# [w32-threads] always: creator names of every pthread (work/w32/thread-trace-w32.cpp)
_tt = W32 / 'diag' / 'thread-trace.o'; _tt.parent.mkdir(exist_ok=True)
subprocess.run([PY, EMXX, '-O2', '-pthread', '-std=c++17', '-c', str(ROOT / 'work/w32/thread-trace-w32.cpp'), '-o', str(_tt)], check=True, env=ENV)
args.insert(2, str(_tt)); args += ['-Wl,--wrap=pthread_create']
if os.environ.get('CPWEB_EMIT_RELOCS', '1') == '1': args += ['-Wl,--emit-relocs']  # [w32-tablecompact] relocations for the table remap
log = W32 / 'link.log'; t0 = time.time()
with log.open('wb') as f: rc = subprocess.run(args, stdout=f, stderr=subprocess.STDOUT, env=ENV, cwd=str(W32), creationflags=0x4000).returncode
content = log.read_text(errors='replace')
missing = sorted(set(re.findall(r'undefined symbol: ([^\r\n]+)', content)))
(W32 / 'unresolved-symbols.json').write_text(json.dumps(missing, indent=1))
res = {'LinkExitCode': rc, 'Seconds': round(time.time() - t0), 'Unresolved': len(missing), 'Log': str(log),
       'MaximumLinearMemoryBytes': 4294967296, 'Wasm': str(out / 'cp2077.wasm')}
if rc == 0:
    res['WasmBytes'] = os.path.getsize(out / 'cp2077.wasm')
    res['WasmSHA256'] = hashlib.sha256((out / 'cp2077.wasm').read_bytes()).hexdigest()
(W32 / 'link-result.json').write_text(json.dumps(res, indent=1))
print(json.dumps(res, indent=1))
if rc: print(content[-6000:])
sys.exit(rc)
