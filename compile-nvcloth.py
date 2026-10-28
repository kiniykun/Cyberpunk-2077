"""Build NVIDIA's CPU NvCloth implementation against the depot's public API.

This is a dependency build, not evidence that the game runs. Keep upstream
notices and record the exact upstream commit and original CMake source list.
"""
import difflib, hashlib, json, re, subprocess, sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent
SDK = Path(r'D:\cp2077SourceCode\tools\emsdk')
EXTERNAL = Path(r'D:\cp2077SourceCode\extracted\root\R6.Root\Mainline\dev\external')
REPO = ROOT / 'tools/NvCloth-source'
VENDOR = REPO / 'NvCloth'
cmake = VENDOR / 'compiler/cmake/common/NvCloth.cmake'
commit = subprocess.check_output(['git', 'rev-parse', 'HEAD'], cwd=REPO, text=True).strip()
if commit != '7bceea80d4a04649f495f4f4331d7e3bdcdb05ca':
    raise RuntimeError('NvCloth dependency commit changed; review API and source selection before building')

def code(text):
    return re.sub(r'\s+', '', re.sub(r'//[^\n]*|/\*.*?\*/', '', text, flags=re.S))

comparison = []
diffs = []
for original in sorted((EXTERNAL / 'NvCloth/include').rglob('*.h')):
    relative = original.relative_to(EXTERNAL / 'NvCloth/include')
    upstream = VENDOR / ('extensions/include' if relative.parts[0] == 'NvClothExt' else 'include') / relative
    identical = upstream.is_file() and code(original.read_text()) == code(upstream.read_text())
    comparison.append({'Header':relative.as_posix(), 'UpstreamExists':upstream.is_file(), 'CodeIdentical':identical})
    if upstream.is_file() and not identical:
        diffs.extend(difflib.unified_diff(upstream.read_text().splitlines(True), original.read_text().splitlines(True), fromfile='upstream/'+relative.as_posix(), tofile='depot/'+relative.as_posix()))
(ROOT/'build/nvcloth-header-comparison.json').write_text(json.dumps(comparison, indent=2))
(ROOT/'build/nvcloth-header-differences.diff').write_text(''.join(diffs))
if any(not row['UpstreamExists'] for row in comparison):
    raise RuntimeError('Missing upstream public headers')
if any(not row['CodeIdentical'] and row['Header'] != 'NvCloth/Callbacks.h' for row in comparison):
    raise RuntimeError('Unexpected public API change; inspect recorded header differences')

# Select only active CPU/extension units from NVIDIA's checked-in source list.
sources = []
for line in cmake.read_text().splitlines():
    match = re.fullmatch(r'\s*\$\{PROJECT_ROOT_DIR\}/(.+\.cpp)\s*', line)
    if match:
        sources.append(VENDOR / match[1])
if not sources or any(not p.is_file() for p in sources):
    raise RuntimeError('Incomplete original NvCloth source list')
subprocess.run([sys.executable, str(ROOT/'stage-port.py')], check=True)
out = ROOT/'build/modules/nvcloth'; out.mkdir(parents=True, exist_ok=True)
px = EXTERNAL/'PhysX342/PxShared'
# Compile against the same public headers and foundation ABI as game physics.
includes = [EXTERNAL/'NvCloth/include', px/'include', px/'src/foundation/include', VENDOR/'src', VENDOR/'extensions/src']
flags = ['-std=c++11', '-O0', '-g', '-fshort-wchar', '-pthread', '-msimd128', '-msse4.1', '-sMEMORY64=1', '-ferror-limit=5', '-DNDEBUG', '-DPX_PHYSX_STATIC_LIB', '-DNV_CLOTH_ENABLE_CUDA=0', '-DNV_CLOTH_ENABLE_DX11=0', '-DNV_CLOTH_DLL=0', '-ivfsoverlay', str(ROOT/'clang-vfs.json')]
# NvSimdTypes tests PX_EMSCRIPTEN before some translation units include
# PxPreprocessor. Initialize the real platform macros consistently in all units.
flags += ['-include', str(px/'include/foundation/PxPreprocessor.h')]
flags += ['-I'+str(p) for p in includes]
units = [{'Source':str(p), 'Input':str(p), 'Object':str(out/f'{i:04d}-{p.stem}.o'), 'Log':str(out/f'{i:04d}-{p.stem}.log')} for i,p in enumerate(sources)]
manifest = {'Module':'nvcloth', 'Compiler':str(SDK/'upstream/emscripten/em++.py'), 'CommonArgs':flags, 'Workers':4, 'Units':units, 'Retained':[], 'StatusPath':str(out/'result.json'), 'ResultsPath':str(out/'compile-results.json')}
(out/'compile-jobs.json').write_text(json.dumps(manifest, indent=2))
subprocess.run([sys.executable, str(ROOT/'compile-units.py'), str(out/'compile-jobs.json')], check=True)
results = json.loads((out/'compile-results.json').read_text())
compiled = sum(u['Compiled'] for u in results); complete = compiled == len(units)
library = ROOT/'build/lib/libnvcloth.a'
if complete:
    temporary = library.with_suffix('.building.a'); temporary.unlink(missing_ok=True)
    subprocess.run([sys.executable, str(SDK/'upstream/emscripten/emar.py'), 'rcs', str(temporary), *[u['Object'] for u in results]], check=True)
    temporary.replace(library)
status = {'Module':'nvcloth', 'Sources':len(units), 'Compiled':compiled, 'Complete':complete, 'Library':str(library) if complete else None, 'Units':results, 'CompilerArgs':flags, 'WcharBytes':2, 'UpstreamRepository':'https://github.com/NVIDIAGameWorks/NvCloth', 'UpstreamCommit':commit, 'OriginalCMake':str(cmake), 'OriginalCMakeSHA256':hashlib.sha256(cmake.read_bytes()).hexdigest(), 'PublicHeaders':comparison, 'RuntimeVerified':False}
(out/'result.json').write_text(json.dumps(status, indent=2))
print(f'NvCloth CPU: {compiled}/{len(units)}; complete={complete}', flush=True)
sys.exit(0 if complete else 1)
