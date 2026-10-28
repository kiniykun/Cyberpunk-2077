"""Compile complete original Wwise component source tables for the browser ABI."""
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
COMPONENTS = {'stream': ('source/StreamManager/Linux/AkStreamMgr.make', 'wwise-stream'),
              'music': ('source/SoundEngine/AkMusicEngine/Linux/AkMusicEngineLinux.make', 'wwise-music'),
              'sound': ('source/SoundEngine/AkAudiolib/Linux/AkSoundEngineLinux.make', 'wwise-sound'),
              'sound-core': ('source/SoundEngine/AkAudiolib/Linux/AkSoundEngineLinux.make', 'wwise-sound-core')}
if len(sys.argv) not in (2, 3) or sys.argv[1] not in COMPONENTS or (len(sys.argv) == 3 and sys.argv[2] != '--retry-failed'):
    raise SystemExit('Select stream, music, sound or sound-core')
definition_name, module = COMPONENTS[sys.argv[1]]
definition = WWISE / definition_name
text = definition.read_text()
release_block = re.search(r'ifeq \(\$\(config\),release\)(.*?)\nendif', text, re.S)
if not release_block:
    raise RuntimeError('Original release configuration was not identified')
original_defines = re.findall(r'-D\S+', re.search(r'^\s*DEFINES \+= (.+)$', release_block[1], re.M)[1])
sources = [(definition.parent / name).resolve() for name in
           re.findall(r'^\$\(OBJDIR\)/[^:]+: ([^\r\n]+\.(?:c|cpp))\s*$', text, re.M)]
if not sources or len(sources) != len(set(sources)) or any(not p.is_file() or not p.is_relative_to(WWISE) for p in sources):
    raise RuntimeError('Original component source selection is invalid')
original_source_count = len(sources)
excluded_platform = []
if sys.argv[1] == 'sound-core':
    # Partition only the OS output implementation. All common engine/mixer/
    # software-pipeline sources remain unchanged. The missing platform calls
    # stay undefined at link time until a real browser backend supplies them.
    excluded_platform = [p for p in sources if p.parent == definition.parent]
    if {p.name for p in excluded_platform} != {'AkLEngine.cpp', 'AkSink.cpp', 'AkSinkALSA.cpp', 'AkSinkPulseAudio.cpp', 'PulseAudioAPI.cpp'}:
        raise RuntimeError('Original Linux sound-output partition differs from the expected five units')
    sources = [p for p in sources if p not in excluded_platform]
include_line = re.findall(r'^\s*INCLUDES \+= (.+)$', text, re.M)[0]
includes = [ROOT / 'src', *[(definition.parent / name).resolve() for name in re.findall(r'-I(\S+)', include_line)]]
out = ROOT / 'build/modules' / module
out.mkdir(parents=True, exist_ok=True)
flags = ['-std=c++17', '-fshort-wchar', '-fno-builtin-wcslen', '-pthread', '-msimd128', '-msse4.1', '-sMEMORY64=1', '-O2', '-g',
         '-DAK_OPTIMIZED', '-DNDEBUG', '-DAUDIOKINETIC', '-DAKSOUNDENGINE_EXPORTS',
         '-ivfsoverlay', str(ROOT / 'clang-vfs.json')]
flags.extend(flag for flag in original_defines if flag not in flags)
flags.extend('-I' + str(path) for path in includes)
units = []
for source in sources:
    name = str(source.relative_to(WWISE))
    stem = re.sub(r'[^a-zA-Z0-9_-]', '_', name)
    units.append(dict(Source=name, Input=str(source), Object=str(out / (stem + '.o')), Log=str(out / (stem + '.log'))))
manifest_path = out / 'compile-jobs.json'
retained = []
if len(sys.argv) == 3:
    previous_manifest = json.loads(manifest_path.read_text())
    previous_results = json.loads((out / 'units.json').read_text())
    if previous_manifest['CommonArgs'] != flags or previous_manifest['Units'] != units:
        raise RuntimeError('Retry requires unchanged flags and source selection')
    retained = [unit for unit in previous_results if unit['Compiled'] and Path(unit['Object']).is_file()]
manifest = dict(Module=module, Compiler=str(SDK / 'upstream/emscripten/em++.py'), CommonArgs=flags, Workers=8,
                Units=units, Retained=retained, StatusPath=str(out / 'result.json'), ResultsPath=str(out / 'units.json'))
manifest_path.write_text(json.dumps(manifest, indent=2))
os.environ['EMSDK_PYTHON'] = sys.executable
subprocess.run([sys.executable, str(ROOT / 'compile-units.py'), str(manifest_path)], check=True)
results = json.loads((out / 'units.json').read_text())
status = dict(Module=module, Sources=len(units), Compiled=sum(u['Compiled'] for u in results),
              Complete=all(u['Compiled'] for u in results), BuildInProgress=False, Units=results,
              OriginalBuildDefinition=str(definition), OriginalBuildSHA256=hashlib.sha256(definition.read_bytes()).hexdigest(),
              OriginalReleaseDefines=original_defines,
              OriginalSourceCount=original_source_count, ExcludedPlatformSources=[str(p) for p in excluded_platform],
              BrowserOutputBackendImplemented=False,
              OriginalSourceSHA256={str(p): hashlib.sha256(p.read_bytes()).hexdigest() for p in sources},
              RuntimeVerified=False, WwiseStartupVerified=False)
if status['Complete']:
    library = ROOT / 'build/lib' / ('lib' + module + '.a')
    temporary = library.with_suffix('.building.a')
    temporary.unlink(missing_ok=True)
    subprocess.run([sys.executable, str(SDK / 'upstream/emscripten/emar.py'), 'rcs', str(temporary),
                    *[u['Object'] for u in results]], check=True)
    temporary.replace(library)
    status['Library'] = str(library)
(out / 'result.json').write_text(json.dumps(status, indent=2))
print(f"{module}: {status['Compiled']}/{status['Sources']}; complete={status['Complete']}", flush=True)
sys.exit(0 if status['Complete'] else 1)
