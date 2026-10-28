"""Compile original ICU source lists; vendor wchar_t stays at its libc ABI.

The engine uses ICU's char16_t/UChar API. Integration must audit that it does
not call ICU's wchar_t interfaces across the engine's short-wchar boundary.
Locale/data packaging is a separate required input, never an empty substitute.
"""
from pathlib import Path
import concurrent.futures
import hashlib
import json
import re
import subprocess
import sys

ROOT = Path(__file__).resolve().parent
SDK = Path(r'D:\cp2077SourceCode\tools\emsdk')
VENDOR = Path(r'D:\cp2077SourceCode\extracted\root\R6.Root\Mainline\dev\external\icu\source')
PYTHON = SDK / 'python/3.13.3_64bit/python.exe'
COMPILER = SDK / 'upstream/emscripten/em++.py'
ARCHIVER = SDK / 'upstream/emscripten/emar.py'

def build(component):
    definition = VENDOR / component / 'Makefile.in'
    text = definition.read_text(encoding='utf-8-sig')
    match = re.search(r'^OBJECTS\s*=\s*(.*?)(?:\n\s*\n)', text, re.M | re.S)
    if not match:
        raise RuntimeError(f'Original {component} object list not found')
    names = re.findall(r'([\w-]+)\.o\b', match.group(1))
    if len(names) < 100 or len(names) != len(set(names)):
        raise RuntimeError('Unexpected original ICU source list')
    source_units = []
    for name in names:
        matches = list((VENDOR / component).rglob(name + '.cpp'))
        if len(matches) != 1:
            raise RuntimeError(f'{name}: expected one original implementation, found {len(matches)}')
        source_units.append(matches[0])
    module = 'icu' + component
    out = ROOT / 'build/modules' / module
    out.mkdir(parents=True, exist_ok=True)
    status_path = out / 'result.json'
    status = dict(Module=module, Sources=len(source_units), Compiled=0,
                  Complete=False, BuildInProgress=True, WcharBytes=4,
                  OriginalBuildDefinition=str(definition),
                  OriginalBuildSHA256=hashlib.sha256(definition.read_bytes()).hexdigest())
    status_path.write_text(json.dumps(status, indent=2))
    flags = ['-std=c++17', '-pthread', '-msimd128', '-sMEMORY64=1', '-O0', '-g',
             '-DU_STATIC_IMPLEMENTATION', '-DU_ENABLE_DYLOAD=0',
             '-DU_COMMON_IMPLEMENTATION' if component == 'common' else '-DU_I18N_IMPLEMENTATION',
             '-I' + str(VENDOR / 'common'), '-I' + str(VENDOR / 'i18n')]
    def compile_unit(source):
        obj = out / (source.stem + '.o')
        log = out / (source.stem + '.log')
        with log.open('w', encoding='utf-8') as stream:
            result = subprocess.run([str(PYTHON), str(COMPILER), *flags,
                                     '-c', str(source), '-o', str(obj)],
                                    stdout=stream, stderr=subprocess.STDOUT)
        return dict(Source=str(source), Compiled=result.returncode == 0,
                    Object=str(obj), Log=str(log))
    units = []
    with concurrent.futures.ThreadPoolExecutor(max_workers=4) as pool:
        futures = [pool.submit(compile_unit, source) for source in source_units]
        for future in concurrent.futures.as_completed(futures):
            unit = future.result()
            units.append(unit)
            if not unit['Compiled']:
                errors = [s for s in Path(unit['Log']).read_text().splitlines() if 'error:' in s]
                print('\n'.join(errors[:3]), flush=True)
    status.update(Compiled=sum(u['Compiled'] for u in units), BuildInProgress=False,
                  Units=sorted(units, key=lambda u: u['Source']))
    status['Complete'] = status['Compiled'] == status['Sources']
    library = ROOT / 'build/lib' / ('lib' + module + '.a')
    if status['Complete']:
        temporary = library.with_suffix('.building.a')
        temporary.unlink(missing_ok=True)
        subprocess.run([str(PYTHON), str(ARCHIVER), 'rcs', str(temporary),
                        *[u['Object'] for u in status['Units']]], check=True)
        temporary.replace(library)
        status['Library'] = str(library)
    status['CompileArguments'] = flags
    status['RuntimeDataIncluded'] = False
    status['EngineWcharBoundaryAudited'] = False
    status_path.write_text(json.dumps(status, indent=2))
    print(f"{module}: {status['Compiled']}/{status['Sources']}; complete={status['Complete']}", flush=True)
    return status['Complete']

if __name__ == '__main__':
    components = sys.argv[1:] or ['common', 'i18n']
    if any(c not in ['common', 'i18n'] for c in components):
        raise ValueError('Unknown ICU component')
    results = [build(component) for component in components]
    sys.exit(0 if all(results) else 1)
