#!/usr/bin/env python3
"""wasm32 post-link staging on build-w32/game/cp2077.{js,wasm}. Never touches build/.

1. The live post-link stagers (optimize-browser-module.py, stage-asset-response-copy.py, stage-worker-fault-trace.py,
   stage-mailbox-task-delivery.py, stage-main-mailbox-delivery.py, stage-pending-gpu-futures.py) hard-code
   Path(__file__).parent/'build/game'. They are copied into build-w32/stageroot (their own root) and run there on
   copies of the wasm32 outputs; results are copied back. stage-webgpu-memory64.py is skipped (wasm64-only BigInt
   string-view patch).
2. EM_JS pointer normalisation: Emscripten 6.0.12 emits EM_JS bodies verbatim, so on wasm32 with
   MAXIMUM_MEMORY=4GB a pointer/size_t argument >= 2 GiB arrives as a negative i32. For every EM_JS in the compiled
   sources whose parameter is a pointer, uintptr_t or size_t, insert `name>>>=0;` at the top of the generated function.
"""
import glob, json, re, shutil, subprocess, sys
from pathlib import Path
ROOT = Path(r'D:\2077'); W32 = ROOT / 'build-w32'; GAME = W32 / 'game'; SR = W32 / 'stageroot'
PY = r'D:\Cyber punk\tools\emsdk\python\3.13.3_64bit\python.exe'
STAGERS = ['optimize-browser-module.py', 'stage-asset-response-copy.py', 'stage-worker-fault-trace.py',
           'stage-mailbox-task-delivery.py', 'stage-main-mailbox-delivery.py', 'stage-pending-gpu-futures.py',
           'stage-webgpu-memory64.py']  # its BigInt branch is inert on wasm32; it also drops undefined timestamp indices
report = {'Stagers': {}, 'EmJsNormalised': {}}
(SR / 'build/game').mkdir(parents=True, exist_ok=True)
for n in ('cp2077.js', 'cp2077.wasm'): shutil.copyfile(GAME / n, SR / 'build/game' / n)
# wasm64 -> wasm32 spellings of the generated glue that stager anchors contain (applied to the stager COPY only)
ANCHOR_W32 = {'stage-main-mailbox-delivery.py': [('pthread_ptr = bigintToI53Checked(pthread_ptr);', 'pthread_ptr >>>= 0;')]}
for s in STAGERS:
    t = (ROOT / s).read_text(encoding='utf-8')
    for a64, a32 in ANCHOR_W32.get(s, []):
        assert a64 in t, (s, a64); t = t.replace(a64, a32)
    (SR / s).write_text(t, encoding='utf-8')
    r = subprocess.run([PY, str(SR / s)], cwd=str(SR), capture_output=True, text=True)
    report['Stagers'][s] = {'exit': r.returncode, 'tail': (r.stdout + r.stderr)[-600:]}
    print(s, 'exit', r.returncode, (r.stdout + r.stderr).strip().splitlines()[-1:] if (r.stdout + r.stderr).strip() else '')
# EM_JS pointer params (sources = what the wasm32 build compiles: w32 copies win over live files)
srcs = [f for f in glob.glob(str(ROOT / 'src/*.*')) + glob.glob(str(ROOT / 'engine-overlay/**/*.*'), recursive=True)
        if f.endswith(('.cpp', '.h', '.inl', '.hpp'))]
rx = re.compile(r'EM_JS\s*\(\s*[^,]+,\s*(\w+)\s*,\s*\(([^)]*)\)', re.S)
want = {}
for f in srcs:
    for m in rx.finditer(Path(f).read_text(encoding='utf-8', errors='replace')):
        ps = []
        for p in m.group(2).split(','):
            p = p.strip()
            if not p or p == 'void': continue
            if '*' in p or re.search(r'\b(uintptr_t|intptr_t|size_t)\b', p):
                ps.append(re.findall(r'(\w+)\s*$', p)[0])
        if ps: want[m.group(1)] = ps
js = (SR / 'build/game/cp2077.js').read_text(encoding='utf-8')
for name, ps in sorted(want.items()):
    m = re.search(r'function ' + re.escape(name) + r'\(([^)]*)\)\s*\{', js)
    if not m: report['EmJsNormalised'][name] = 'not in output'; continue
    params = [x.strip() for x in m.group(1).split(',')]
    fix = [p for p in ps if p in params]
    js = js[:m.end()] + ''.join(f'{p}>>>=0;' for p in fix) + ' /*[w32] unsigned wasm32 pointers*/' + js[m.end():]
    report['EmJsNormalised'][name] = fix
# [w32-lean] mem=ios: prespawn one pthread worker (the proxied main thread); the rest start on demand.
POOL = 'var pthreadPoolSize = 4;'
assert js.count(POOL) == 1, 'pthread pool anchor'
js = js.replace(POOL, 'var pthreadPoolSize = (typeof location == "object" && /[?&]mem=ios(&|$)/.test(location.search)) ? 1 : 4; /*[w32-lean]*/')
report['LeanPool'] = 'mem=ios -> 1'
# [w32-lazytable] JS readers of the (now lazily filled) funcref table fill a null entry first (export from lazy-table-w32.mjs)
TBL = '    /** @suppress {checkTypes} */ wasmTableMirror[funcPtr] = func = wasmTable.get(funcPtr);'
assert js.count(TBL) == 1, 'table mirror anchor'
js = js.replace(TBL, '    /** @suppress {checkTypes} */ wasmTableMirror[funcPtr] = func = wasmTable.get(funcPtr) || (wasmExports["__cp_table_fill"] && wasmExports["__cp_table_fill"](funcPtr), wasmTable.get(funcPtr)); /*[w32-lazytable]*/')
report['LazyTableGlue'] = True
# [w32-pace] mem=ios: on-demand pthread workers load one at a time (each JSC instance comes up alone, ~150 ms apart);
# the thread's run message is posted once its worker has loaded.
GNW = """      var newWorker = PThread.allocateUnusedWorker();
      PThread.loadWasmModuleToWorker(newWorker);"""
assert js.count(GNW) == 1, 'getNewWorker anchor'
js = js.replace(GNW, """      var newWorker = PThread.allocateUnusedWorker();
      if (typeof location == "object" && /[?&]mem=ios(&|$)/.test(location.search)) { /*[w32-pace]*/
        newWorker.cpLoaded = PThread.cpChain = (PThread.cpChain || Promise.resolve()).then(() => PThread.loadWasmModuleToWorker(newWorker)).then(() => new Promise(r => setTimeout(r, 150)));
      } else PThread.loadWasmModuleToWorker(newWorker);""")
RUN = """  worker.postMessage(msg, threadParams.transferList);
  return 0;"""
assert js.count(RUN) == 1, 'spawnThread anchor'
js = js.replace(RUN, """  if (worker.cpLoaded) worker.cpLoaded.then(() => worker.postMessage(msg, threadParams.transferList)); /*[w32-pace]*/
  else worker.postMessage(msg, threadParams.transferList);
  return 0;""")
report['PaceWorkers'] = 'mem=ios'
# [w32-uacheck] emscripten's user-agent Safari version gate rejected real Safari/WebKit in-app browsers that report an
# old "Version/11.1.1" (212 iPhone fail reports 2026-10-26); capcheck.js tests the actual features instead.
SAF = 'if (currentSafariVersion < 15e4) {'
if SAF in js:
    js = js.replace(SAF, 'if (false && currentSafariVersion < 15e4) { /*[w32-uacheck]*/', 1); report['SafariUaCheck'] = 'off'
(SR / 'build/game/cp2077.js').write_text(js, encoding='utf-8')
for n in ('cp2077.js', 'cp2077.wasm'): shutil.copyfile(SR / 'build/game' / n, GAME / n)
(W32 / 'post-link-result.json').write_text(json.dumps(report, indent=1))
print(json.dumps(report['EmJsNormalised'], indent=0))
sys.exit(1 if any(v['exit'] for v in report['Stagers'].values()) else 0)
