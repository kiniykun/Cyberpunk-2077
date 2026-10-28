"""Build original Wwise memory-manager units, including its real allocator."""
import hashlib
import json
import os
from pathlib import Path
import re
import subprocess
import sys

ROOT = Path(__file__).resolve().parent
SDK = Path(r'D:\cp2077SourceCode\tools\emsdk')
WWISE = Path(r'D:\cp2077SourceCode\extracted\root\R6.Root\Mainline\dev\external\wwise')
MODULE = WWISE / 'source/SoundEngine/AkMemoryMgr'
OUT = ROOT / 'build/modules/wwise-memory'
OUT.mkdir(parents=True, exist_ok=True)
definition = MODULE / 'Linux/AkMemoryMgrLinux.make'
rules = re.findall(r'^\$\(OBJDIR\)/[^:]+: (\.\./[^\r\n]+\.(?:c|cpp))\s*$', definition.read_text(), re.M)
sources = [(definition.parent / path).resolve() for path in rules]
if len(sources) != 3 or len(set(sources)) != 3 or any(not p.is_file() or not p.is_relative_to(WWISE) for p in sources):
    raise RuntimeError('Original memory-manager build selection differs from the expected three units')
# Original POSIX TLS is part of AkAudiolib's platform implementation and is used
# by the real rpmalloc allocator. Emscripten provides the actual pthread keys.
sources.append(WWISE / 'source/SoundEngine/AkAudiolib/POSIX/AkTls.cpp')
includes = [ROOT / 'src', MODULE / 'Linux', MODULE / 'Common', WWISE / 'include',
            WWISE / 'source/SoundEngine/3rdParty/Common',
            WWISE / 'source/SoundEngine/AkAudiolib/Linux', WWISE / 'source/SoundEngine/AkAudiolib/Common']
flags = ['-std=c++17', '-fshort-wchar', '-pthread', '-msimd128', '-sMEMORY64=1', '-O2', '-g',
         '-DAK_OPTIMIZED', '-DNDEBUG', '-DAUDIOKINETIC', '-DAKSOUNDENGINE_EXPORTS',
         '-ivfsoverlay', str(ROOT / 'clang-vfs.json')]
flags.extend('-I' + str(path) for path in includes)
units = []
for source in sources:
    name = str(source.relative_to(WWISE))
    stem = re.sub(r'[^a-zA-Z0-9_-]', '_', name)
    units.append(dict(Source=name, Input=str(source), Object=str(OUT / (stem + '.o')), Log=str(OUT / (stem + '.log'))))
manifest = dict(Module='wwise-memory', Compiler=str(SDK / 'upstream/emscripten/em++.py'), CommonArgs=flags,
                Workers=8, Units=units, Retained=[], StatusPath=str(OUT / 'result.json'), ResultsPath=str(OUT / 'units.json'))
manifest_path = OUT / 'compile-jobs.json'
manifest_path.write_text(json.dumps(manifest, indent=2))
os.environ['EMSDK_PYTHON'] = sys.executable
subprocess.run([sys.executable, str(ROOT / 'compile-units.py'), str(manifest_path)], check=True)
results = json.loads((OUT / 'units.json').read_text())
status = dict(Module='wwise-memory', Sources=len(units), Compiled=sum(u['Compiled'] for u in results),
              Complete=all(u['Compiled'] for u in results), BuildInProgress=False, Units=results,
              OriginalBuildDefinition=str(definition), OriginalBuildSHA256=hashlib.sha256(definition.read_bytes()).hexdigest(),
              OriginalSourceSHA256={str(p): hashlib.sha256(p.read_bytes()).hexdigest() for p in sources},
              AllocatorRuntimeVerified=False, WwiseStartupImplemented=False)
if status['Complete']:
    library = ROOT / 'build/lib/libwwise-memory.a'
    temporary = library.with_suffix('.building.a')
    temporary.unlink(missing_ok=True)
    subprocess.run([sys.executable, str(SDK / 'upstream/emscripten/emar.py'), 'rcs', str(temporary),
                    *[u['Object'] for u in results]], check=True)
    temporary.replace(library)
    status['Library'] = str(library)
(OUT / 'result.json').write_text(json.dumps(status, indent=2))
print(f"Original Wwise memory manager: {status['Compiled']}/{status['Sources']}; complete={status['Complete']}", flush=True)
sys.exit(0 if status['Complete'] else 1)
