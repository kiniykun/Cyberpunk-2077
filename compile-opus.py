"""Build the bundled original Ogg/Opus decoder sources for the browser ABI.

Source selection comes from the supplied original decoder makefile. Platform
plugin wrappers remain separate; this archive does not fake Wwise startup.
"""
import hashlib
import json
import os
from pathlib import Path
import re
import subprocess
import sys

ROOT = Path(__file__).resolve().parent
SDK = Path(r'D:\cp2077SourceCode\tools\emsdk')
CODEC = Path(r'D:\cp2077SourceCode\extracted\root\R6.Root\Mainline\dev\external\wwise\source\SoundEngine\Codecs\AkOpusDecoder')
WWISE = CODEC.parents[3]
COMPILER = SDK / 'upstream/emscripten/em++.py'
ARCHIVER = SDK / 'upstream/emscripten/emar.py'
OUT = ROOT / 'build/modules/opus-decoder'
OUT.mkdir(parents=True, exist_ok=True)
definition = CODEC / 'Linux/AkOpusDecoderLinux.make'
rules = re.findall(r'^\$\(OBJDIR\)/[^:]+: (\.\./[^\r\n]+\.c)\s*$', definition.read_text(), re.M)
sources = [(definition.parent / name).resolve() for name in rules]
if not sources or len(sources) != len(set(sources)):
    raise RuntimeError('Original decoder source selection is empty or duplicated')
if any(not source.is_file() or not source.is_relative_to(CODEC) for source in sources):
    raise RuntimeError('Original decoder source selection is invalid')

# Keep the original allocator bodies and processing-pool identity. The C codec
# expects C linkage, while the original memory manager retains its C++ ABI.
allocation_source = CODEC / 'AkOpusLib.cpp'
allocation_body = allocation_source.read_text().split('void *AK_malloc(size_t in_size)', 1)[1]
hook_source = OUT / 'original-opus-allocation.cpp'
hook_source.write_text('#include <AK/Tools/Common/AkObject.h>\nextern "C" {\nvoid *AK_malloc(size_t in_size)' + allocation_body + '\n}\n')
sources.append(hook_source)
includes = [CODEC / p for p in ['opus', 'opus/include', 'opus/celt', 'opus/silk', 'opus/silk/float', 'opusfile/include', 'libogg/include']]
includes.extend([ROOT / 'src', WWISE / 'include'])
flags = ['-std=c++17', '-fshort-wchar', '-pthread', '-msimd128', '-sMEMORY64=1', '-O2', '-g',
         '-DAK_OPTIMIZED', '-DNDEBUG', '-DAUDIOKINETIC', '-DAKSOUNDENGINE_EXPORTS', '-DFLOAT_APPROX', '-DHAVE_CONFIG_H', '-DHAVE_ALLOCA_H=1',
         '-ivfsoverlay', str(ROOT / 'clang-vfs.json')]
flags.extend('-I' + str(path) for path in includes)
units = []
for source in sources:
    name = str(source.relative_to(CODEC)) if source.is_relative_to(CODEC) else source.name
    stem = re.sub(r'[^a-zA-Z0-9_-]', '_', name)
    units.append(dict(Source=name, Input=str(source), Object=str(OUT / (stem + '.o')), Log=str(OUT / (stem + '.log'))))
manifest = dict(Module='opus-decoder', Compiler=str(COMPILER), CommonArgs=flags, Workers=8,
                Units=units, Retained=[], StatusPath=str(OUT / 'result.json'), ResultsPath=str(OUT / 'units.json'))
manifest_path = OUT / 'compile-jobs.json'
manifest_path.write_text(json.dumps(manifest, indent=2))
os.environ['EMSDK_PYTHON'] = sys.executable
subprocess.run([sys.executable, str(ROOT / 'compile-units.py'), str(manifest_path)], check=True)
results = json.loads((OUT / 'units.json').read_text())
status = dict(Module='opus-decoder', Sources=len(units), Compiled=sum(u['Compiled'] for u in results),
              Complete=all(u['Compiled'] for u in results), BuildInProgress=False, Units=results,
              OriginalBuildDefinition=str(definition), OriginalBuildSHA256=hashlib.sha256(definition.read_bytes()).hexdigest(),
              OriginalAllocatorSource=str(allocation_source), OriginalAllocatorSHA256=hashlib.sha256(allocation_source.read_bytes()).hexdigest(),
              CodecRuntimeVerified=False, WwiseStartupImplemented=False)
if status['Complete']:
    library = ROOT / 'build/lib/libopus-decoder.a'
    temporary = library.with_suffix('.building.a')
    temporary.unlink(missing_ok=True)
    subprocess.run([sys.executable, str(ARCHIVER), 'rcs', str(temporary), *[u['Object'] for u in results]], check=True)
    temporary.replace(library)
    status['Library'] = str(library)
(OUT / 'result.json').write_text(json.dumps(status, indent=2))
print(f"Original Opus/Ogg codec: {status['Compiled']}/{status['Sources']} units; complete={status['Complete']}", flush=True)
sys.exit(0 if status['Complete'] else 1)
