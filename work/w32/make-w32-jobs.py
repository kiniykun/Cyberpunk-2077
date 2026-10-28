#!/usr/bin/env python3
"""Generate wasm32 compile jobs for every input of the current wasm64 strict link.

Read-only with respect to the live wasm64 pipeline: it reads build/build-result.json, module
compile-jobs.json / result.json manifests, the o2-jobs recipe generator (regen_manifest.py, the
compile-module.ps1 recipe) and the per-object result JSONs, and writes ONLY under D:\\2077\\build-w32.

Per unit the wasm64 args are reconstructed exactly as the live object was built (including the
-O0 -> -O2 [finitemath] transform when the live object is optimized), then:
  * -sMEMORY64=1 is removed (em++ default target is wasm32-unknown-emscripten),
  * a second overlay  -ivfsoverlay build-w32\\clang-vfs-w32.json  is appended (wasm32-only
    redirects; later overlays take precedence, clang-vfs.json itself is never edited),
  * Object/Log paths are redirected from build\\ to build-w32\\.
Everything else is kept identical.

Output: build-w32/jobs/units.json, build-w32/jobs/link-inputs.json, build-w32/jobs/report.json
Usage: python make-w32-jobs.py
"""
import concurrent.futures as cf, hashlib, importlib.util, json, ntpath, os, re, subprocess, sys
from pathlib import Path

ROOT = Path(r'D:\2077'); PORT = r'D:\cp2077 port'
W32 = ROOT / 'build-w32'
SDK = Path(r'D:\Cyber punk\tools\emsdk')
OD = str(SDK / 'upstream/bin/llvm-objdump.exe'); AR = str(SDK / 'upstream/bin/llvm-ar.exe')
SRC = r'D:\cp2077SourceCode\extracted\root\R6.Root\Mainline\dev\src'
VFS32 = str(W32 / 'clang-vfs-w32.json')

spec = importlib.util.spec_from_file_location('regen_manifest', ROOT / 'build/research/regen_manifest.py')
rm = importlib.util.module_from_spec(spec); spec.loader.exec_module(rm)

def load(p): return json.loads(Path(p).read_text(encoding='utf-8-sig'))
def norm(p): return ntpath.normcase(str(p).replace(PORT, str(ROOT)).replace(PORT.lower(), str(ROOT)))

def fix_fm(args):  # identical to opt_o2_module.fix_fm
    args = list(args)
    if '-ffast-math' in args:
        args = [x for x in args if x not in ('-fno-finite-math-only', '-D__FAST_MATH__=1')]
        i = max(k for k, x in enumerate(args) if x == '-ffast-math')
        args[i + 1:i + 1] = ['-fno-finite-math-only', '-D__FAST_MATH__=1']
    return args
def to_o2(args):
    a = [x for x in args if not re.fullmatch(r'-O[0-3sz]?', x)] + ['-O2']
    if '-msimd128' not in a: a.append('-msimd128')
    return fix_fm(a)
def args_hash(args): return hashlib.sha256('|'.join(args).encode()).hexdigest()[:16]

def w32_path(p):
    n = str(p).replace(PORT, str(ROOT))
    b = str(ROOT / 'build') + '\\'
    if not n.lower().startswith(b.lower()): raise RuntimeError('output outside build: ' + n)
    return str(W32) + '\\' + n[len(b):]

def to_w32_args(args):
    out = [x for x in args if not re.fullmatch(r'-sMEMORY64=\d', x)]
    out.append('-ivfsoverlay'); out.append(VFS32)
    return out

_optcache = {}
def optimized(obj):
    """Live object carries DWARF location lists -> it was built optimized (gen_o2_jobs.py heuristic)."""
    if obj in _optcache: return _optcache[obj]
    t = subprocess.run([OD, '-h', obj], capture_output=True, text=True).stdout
    _optcache[obj] = ('debug_loc' in t) or ('debug_loclists' in t)
    return _optcache[obj]

OVERRIDES = {  # same as gen_o2_jobs.py: how the live object was actually built when it differs from the recipe
    ('redMemory', 'src/dynamicLinearAllocator.cpp'): {'Input': str(ROOT / 'src/dynamic-linear-allocator-web.cpp'),
                                                      'Extra': ['-DCPWEB_ALLOCATOR_DIAGNOSTICS=1']},
}

EXT = r'D:\cp2077SourceCode\extracted\root\R6.Root\Mainline\dev\external'
LEX = r'D:\cp2077SourceCode\extracted\root\R6.Root\Mainline\dev\internal\RedLexer\native'
HB = EXT + r'\harfbuzz_new'
HAND = {  # module -> (args, base for relative Source, recipe script) transcribed from the compile scripts
    'redLexer': (['-std=c++17', '-fshort-wchar', '-pthread', '-msimd128', '-sMEMORY64=1', '-O0', '-g', '-DNDEBUG', '-DRELEASE', '-DNO_EDITOR',
                  f'-I{LEX}\\include', f'-I{LEX}\\src', f'-I{PORT}\\generated\\redReflection', '-ivfsoverlay', f'{PORT}\\clang-vfs.json'], LEX, 'compile-lexer.ps1'),
    'freetype': (['-x', 'c', '-std=c11', '-fshort-wchar', '-pthread', '-msimd128', '-sMEMORY64=1', '-O0', '-g', '-DFT2_BUILD_LIBRARY',
                  f'-I{EXT}\\freetype\\include'], EXT + r'\freetype', 'compile-freetype.ps1'),
    'harfbuzz': (['-std=c++17', '-fshort-wchar', '-pthread', '-msimd128', '-sMEMORY64=1', '-O0', '-g', '-DHAVE_FREETYPE=1', '-DHAVE_PTHREAD=1',
                  '-ivfsoverlay', f'{PORT}\\clang-vfs.json', '-DHAVE_UNISTD_H=1', f'-I{HB}\\src', f'-I{HB}\\..\\freetype\\include'], HB, 'compile-harfbuzz.ps1'),
    'icudata': (['-x', 'c', '-std=c23', '-pthread', '-sMEMORY64=1', '-O0'], '', 'compile-icu-data.py'),
}

O2JOBS = {}  # (module, Source) -> arg lists actually given to opt_o2_module.py via --jobs (all variants)
for jf in sorted((ROOT / 'build/research/o2-jobs').glob('*.json')):
    if jf.name.endswith('.report.json'): continue
    try: jd = load(jf)
    except Exception: continue
    if not isinstance(jd, dict) or 'Units' not in jd: continue
    for u in jd['Units']:
        a = u['Args'] if 'Args' in u else to_o2(jd['CommonArgs'])
        if u['Input'].endswith('.c') and 'Args' not in u: a = [x for x in a if x != '-std=c++17'] + ['-x', 'c', '-std=c11']
        O2JOBS.setdefault((jd['Module'], u['Source']), []).append(a)

_op = W32 / 'jobs/opt-overrides.json'
OPT_OVERRIDES = load(_op) if _op.exists() else {}
br = load(ROOT / 'build/build-result.json')
units, report, link_libs, link_objs = [], {'Modules': {}, 'Unresolved': [], 'Notes': []}, [], []

def add(module, source, inp, obj, args, how, o2state=None):
    live = str(obj).replace(PORT, str(ROOT))
    base = list(args)
    final = base
    if o2state is not None and o2state.get('ok'):
        cands = [to_o2(base), fix_fm(base)] + [fix_fm(x) for x in O2JOBS.get((module, source), [])]
        hit = None
        for cand in cands:
            if o2state.get('args') is None or o2state.get('args') == args_hash(cand): hit = cand; break
        if hit is None:
            report['Unresolved'].append({'Module': module, 'Source': source, 'Reason': 'o2 state args hash matches no known recipe'})
            hit = to_o2(base); how += '+o2UNVERIFIED'
        final = hit; how += '+o2state'
    elif '-O0' in base and '-g' in base and Path(live).exists() and optimized(live):
        final = to_o2(base); how += '+o2detected'
    if live.lower() in OPT_OVERRIDES:  # empirically matched by resolve-opt.py against the live object's code
        final = OPT_OVERRIDES[live.lower()]; how += '+optverified'
    o = w32_path(obj)
    units.append({'Module': module, 'Source': source, 'Input': inp, 'Object': o,
                  'Log': o[:-2] + '.log', 'LiveObject': live, 'Recipe': how,
                  'Args64Base': base, 'Args64': final, 'Args': to_w32_args(final)})
    return o

# ---------- libraries ----------
for lib in br['LibraryInputs']:
    m = Path(lib).stem[3:]
    mdir = ROOT / 'build/modules' / m
    res = load(mdir / 'result.json') if (mdir / 'result.json').exists() else {}
    o2s = load(mdir / 'o2/state.json') if (mdir / 'o2/state.json').exists() else {}
    jobs = load(mdir / 'compile-jobs.json') if (mdir / 'compile-jobs.json').exists() else None
    objs = []
    if jobs and jobs.get('Units'):
        how = 'compile-jobs'
        for u in jobs['Units']:
            ov = OVERRIDES.get((m, u['Source']), {})
            a = list(u.get('Args', jobs['CommonArgs'])) + ov.get('Extra', [])
            if u['Input'].endswith('.c') and 'Args' not in u: a = [x for x in a if x != '-std=c++17'] + ['-x', 'c', '-std=c11']
            objs.append(add(m, u['Source'], ov.get('Input', u['Input']), u['Object'], a, how, o2s.get(u['Source'])))
    else:
        manifest = None
        try:
            manifest, _ex = rm.generate(m)
            rm.validate_against_result(m, manifest['Units'], _ex)
        except BaseException as e:
            manifest = None; report['Notes'].append(f'{m}: regen_manifest unavailable ({type(e).__name__}: {str(e)[:120]})')
        if manifest:
            how = 'regen_manifest'
            for u in manifest['Units']:
                ov = OVERRIDES.get((m, u['Source']), {})
                a = list(manifest['CommonArgs']) + ov.get('Extra', [])
                if u['Input'].endswith('.c'): a = [x for x in a if x != '-std=c++17'] + ['-x', 'c', '-std=c11']
                objs.append(add(m, u['Source'], ov.get('Input', u['Input']), u['Object'], a, how, o2s.get(u['Source'])))
        else:
            how = 'result.json'
            common = res.get('Args') or res.get('CompileArguments') or res.get('CompilerArgs')
            hand = HAND.get(m)
            if hand: common, how = hand[0], 'hand:' + hand[2]
            for u in res.get('Units', []):
                a = u.get('Args') or common
                src = u.get('Source')
                if hand and src and not os.path.isabs(src): src = hand[1] + '\\' + src.replace('/', '\\')
                if a is None or not src or not os.path.isabs(src):
                    report['Unresolved'].append({'Module': m, 'Source': src, 'Object': u.get('Object')})
                    objs.append(w32_path(u['Object'])); continue
                objs.append(add(m, src, src, u['Object'], a, how + ('-unitargs' if u.get('Args') else '')))
    report['Modules'][m] = {'Recipe': how, 'Units': len(objs), 'ResultUnits': len(res.get('Units', []))}
    link_libs.append({'Module': m, 'Library': str(W32 / 'lib' / Path(lib).name), 'Objects': objs})

# ---------- directly linked objects ----------
def jr(name): return load(ROOT / 'build' / name)
boot = ['-std=c++17', '-fshort-wchar', f'-I{PORT}\\src', f'-I{SRC}', '-O0', '-g', '-ferror-limit=5', '-ffast-math',
        '-fno-finite-math-only', '-D__FAST_MATH__=1', '-msimd128', '-msse4.1', '-pthread', '-DRED_COMPILER_CLANG',
        '-DRED_CONFIGURATION_FINAL', '-DNDEBUG', '-DRELEASE', '-DNO_EDITOR', '-DUNICODE', '-D_UNICODE', '-sMEMORY64=1',
        '-ivfsoverlay', f'{PORT}\\clang-vfs.json']
boot_inc = [f'-I{SRC}\\games\\cpLauncher\\src', f'-I{SRC}\\games\\cpLauncher', f'-I{SRC}\\common\\engine\\src',
            f'-I{SRC}\\common\\engine\\include', f'-I{PORT}\\generated\\commProtocol']
direct = {
    'mainBrowser': (f'{PORT}\\src\\mainBrowser.cpp', boot + boot_inc),
    'mainRed': (f'{SRC}\\games\\cpLauncher\\src\\mainRed.cpp', boot + boot_inc),
    'gameApp': (f'{SRC}\\common\\engine\\src\\gameApp.cpp', boot + boot_inc),
    'moduleDependencies': (f'{PORT}\\src\\module-dependencies.cpp', boot + boot_inc),
}
for name, mod, rel in (('engineTime', 'redReflection', r'common\redReflection\src\engineTime.cpp'),
                       ('memoryBudgets', 'core', r'common\core\src\memoryBudgets.cpp')):
    mr = f'{SRC}\\common\\{mod}'
    direct[name] = (f'{SRC}\\{rel}', boot + [f'-I{mr}\\src', f'-I{mr}\\include', f'-I{PORT}\\generated\\commProtocol', f'-DRED_MODULE_{mod}'])
direct['utf16-runtime'] = (f'{PORT}\\src\\utf16-runtime.cpp', ['-std=c++17', '-fshort-wchar', '-pthread', '-sMEMORY64=1'])
for fn in ('bink-allocator-result.json', 'bink-engine-result.json', 'bink-control-result.json'):
    d = jr(fn); n = Path(d['Object']).stem
    direct[n] = (f'{PORT}\\src\\{n}.cpp', d['Args'])
for fn in ('galaxy-boundary-result.json', 'oodle-boundary-result.json'):
    d = jr(fn)
    for u in d['Units']:
        n = Path(u['Object']).stem; direct[n] = (u.get('Source') or f'{PORT}\\src\\{n}.cpp', u.get('Args') or d['Args'])
d = jr('browser-audio-result.json'); direct['wwise-audio-web'] = (d['Source'], d.get('Args') or d['CompileArguments'])
vb = load(ROOT / 'build/modules/voice-broadcast/result.json')
vbmap = {Path(u['Object']).stem: u for u in vb['Units']}
gj = load(ROOT / 'build/webgpu/gpu-compile-jobs.json'); gmap = {u['Source']: u for u in gj['Units']}
es = load(ROOT / 'build/engine-startup/result.json')
es_args = ['-std=c++17', '-fshort-wchar', '-O0', '-g', '-ferror-limit=5', '-ffast-math', '-fno-finite-math-only', '-D__FAST_MATH__=1',
           '-msimd128', '-msse4.1', '-pthread', '-DRED_COMPILER_CLANG', '-DRED_CONFIGURATION_FINAL', '-DNDEBUG', '-DRELEASE',
           '-DNO_EDITOR', '-DUNICODE', '-D_UNICODE', '-DRED_MODULE_engine', '-sMEMORY64=1', '-ivfsoverlay', f'{PORT}\\clang-vfs.json',
           f'-I{PORT}\\src', f'-I{SRC}', f'-I{SRC}\\common\\engine\\src', f'-I{SRC}\\common\\engine\\include', f'-I{PORT}\\generated\\commProtocol']
for obj in br['ObjectInputs']:
    stem = Path(obj).stem; parent = Path(obj).parent.name
    if parent == 'webgpu':
        u = gmap[stem]; a = fix_fm(u['Args']) + ['-O2', '-msimd128']  # compile-selected-gpu.py --optimize
        link_objs.append(add('webgpu', stem, u['Input'], obj, a, 'gpu-compile-jobs+optimize'))
    elif parent == 'engine-startup':
        u = next(x for x in es if x['Unit'] == stem)
        link_objs.append(add('engine-startup', stem, u['Source'], obj, es_args, 'compile-engine-startup.ps1'))
    elif parent == 'voice-broadcast':
        u = vbmap[stem]; a = u.get('Args') or vb['Args']
        link_objs.append(add('voice-broadcast', stem, u['Source'], obj, a, 'voice-broadcast result.json'))
    elif stem in direct:
        inp, a = direct[stem]
        link_objs.append(add('objects', stem, inp, obj, a, 'direct-recipe'))
    else:
        report['Unresolved'].append({'Module': 'objects', 'Object': obj}); link_objs.append(w32_path(obj))

# relink-prepared-engine.py prepends the local WebGPU event-manager override object. Its recipe is not recorded;
# use the GPU bridge recipe (verify-recipes.py / resolve-opt.py check it against the live object).
_ev = gmap['gpu-device-web']
link_objs.insert(0, add('webgpu', 'emdawn-event-manager-web', f'{PORT}\\src\\emdawn-event-manager-web.cpp',
                        f'{PORT}\\build\\webgpu\\emdawn-event-manager-web.o',
                        ['--use-port=emdawnwebgpu', '-std=c++20', '-fno-exceptions', '-pthread', '-sMEMORY64=1', '-O2', '-DNDEBUG', '-g'],
                        'emdawn-port-flags(UNVERIFIED: no tried variant reproduces the live object)'))

# Validate inputs exist (through the junction spelling too).
missing = [u for u in units if not Path(u['Input'].replace(PORT, str(ROOT))).exists()]
for u in missing: report['Unresolved'].append({'Module': u['Module'], 'Source': u['Source'], 'MissingInput': u['Input']})
(W32 / 'jobs').mkdir(parents=True, exist_ok=True)
if not (W32 / 'clang-vfs-w32.json').exists():
    (W32 / 'clang-vfs-w32.json').write_text(json.dumps({'version': 0, 'case-sensitive': False, 'use-external-names': False, 'roots': []}, indent=1))
(W32 / 'jobs/units.json').write_text(json.dumps(units, indent=1))
(W32 / 'jobs/link-inputs.json').write_text(json.dumps({'Objects': link_objs, 'Libraries': link_libs}, indent=1))
report['Totals'] = {'Units': len(units), 'Unresolved': len(report['Unresolved']), 'Libraries': len(link_libs), 'Objects': len(link_objs),
                    'Optimized': sum('o2' in u['Recipe'] or 'optimize' in u['Recipe'] for u in units)}
(W32 / 'jobs/report.json').write_text(json.dumps(report, indent=1))
print(json.dumps(report['Totals'])); print('\n'.join(report['Notes']))
for x in report['Unresolved'][:60]: print('UNRESOLVED', x)
