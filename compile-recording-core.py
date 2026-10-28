"""Compile the original recording implementation with a real browser file backend."""
import concurrent.futures,hashlib,json,os,subprocess,sys
from pathlib import Path
root=Path(__file__).resolve().parent
source=Path(r'D:\cp2077SourceCode\extracted\root\R6.Root\Mainline\dev\internal\RecordingToolCore')
out=root/'build/modules/recording-core-browser';out.mkdir(parents=True,exist_ok=True)
manifest=json.loads((root/'build/modules/network/compile-jobs.json').read_text(encoding='utf-8-sig'))
args=['-include',str(source.parents[1]/'src/common/redSystem/include/redSystemPublic.h'), '-I'+str(source/'src/precompiledHeader')]+[a for a in manifest['CommonArgs'] if a!='-DRED_MODULE_network']
os.environ['EMSDK_PYTHON']=sys.executable
# Replace only the Windows clock implementation. Compile the remaining project
# sources unchanged, selecting the new Web file backend rather than console/Win.
sources=[p for p in (source/'src').rglob('*.cpp') if 'platform' not in p.parts and p.name!='scopedProfiler.cpp']
sources.append(root/'src/recording-profiler-web.cpp')
def compile_unit(src):
    relative=src.relative_to(source) if src.is_relative_to(source) else Path(src.name)
    obj=out/('_'.join(relative.with_suffix('').parts)+'.o');log=obj.with_suffix('.log')
    with log.open('wb') as f:result=subprocess.run([sys.executable,manifest['Compiler'],*args,'-c',str(src),'-o',str(obj)],stdout=f,stderr=subprocess.STDOUT)
    print(relative,result.returncode,flush=True)
    if result.returncode:print('\n'.join(line for line in log.read_text(errors='replace').splitlines() if 'error:' in line),flush=True)
    return {'Source':str(src),'SourceSHA256':hashlib.sha256(src.read_bytes()).hexdigest(),'Object':str(obj),'Log':str(log),'Compiled':result.returncode==0}
with concurrent.futures.ThreadPoolExecutor(max_workers=8) as pool:units=list(pool.map(compile_unit,sources))
complete=all(u['Compiled'] for u in units);library=root/'build/lib/librecording-core-browser.a'
if complete:subprocess.run([sys.executable,str(Path(manifest['Compiler']).with_name('emar.py')),'rcs',str(library),*[u['Object'] for u in units]],check=True)
(out/'result.json').write_text(json.dumps({'Complete':complete,'Library':str(library) if complete else None,'Units':units,'Args':args,'RuntimeVerified':False,'Scope':'Original recorder/playback/storage implementation; pthread POSIX file adapter; static linkage replaces Windows DLL loading, not recording functions. Runtime code hot-swapping unavailable.'},indent=2))
sys.exit(0 if complete else 1)
