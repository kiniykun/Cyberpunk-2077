"""Build the discovered actual Kraken encoder for the isolated codec module."""
import concurrent.futures,hashlib,json,os,subprocess,sys
from pathlib import Path
root=Path(__file__).resolve().parent;source=root/'tools/ooz-encoder';out=root/'build/modules/ooz-encoder';out.mkdir(parents=True,exist_ok=True)
compiler=Path(r'D:\cp2077SourceCode\tools\emsdk\upstream\emscripten\em++.py');os.environ['EMSDK_PYTHON']=sys.executable
args=['-std=c++17','-O2','-msimd128','-msse4.1','-include','immintrin.h','-fno-exceptions','-fno-rtti']
names=['compress.cpp','compr_entropy.cpp','compr_kraken.cpp','compr_leviathan.cpp','compr_match_finder.cpp','compr_mermaid.cpp','compr_multiarray.cpp','compr_tans.cpp']
def compile_unit(name):
 src=source/name;obj=out/(src.stem+'.o');log=obj.with_suffix('.log')
 with log.open('wb') as f:result=subprocess.run([sys.executable,str(compiler),*args,'-c',str(src),'-o',str(obj)],stdout=f,stderr=subprocess.STDOUT)
 print(name,result.returncode,flush=True)
 if result.returncode:print('\n'.join(line for line in log.read_text(errors='replace').splitlines() if 'error:' in line),flush=True)
 return {'Source':str(src),'SourceSHA256':hashlib.sha256(src.read_bytes()).hexdigest(),'Object':str(obj),'Log':str(log),'Compiled':result.returncode==0}
with concurrent.futures.ThreadPoolExecutor(max_workers=8) as pool:units=list(pool.map(compile_unit,names))
complete=all(u['Compiled'] for u in units);library=root/'build/lib/libooz-encoder-private.a'
if complete:subprocess.run([sys.executable,str(compiler.with_name('emar.py')),'rcs',str(library),*[u['Object'] for u in units]],check=True)
commit=subprocess.check_output(['git','-C',str(source),'rev-parse','HEAD'],text=True).strip()
(out/'result.json').write_text(json.dumps({'Complete':complete,'Library':str(library) if complete else None,'Upstream':'https://github.com/rarten/ooz','Commit':commit,'Units':units,'Args':args,'Target':'wasm32 for separate private codec worker, engine remains wasm64','EngineABIIntegrated':False,'EncoderExecutionVerified':False,'LicenseStatus':'No separate license file identified yet; preserve upstream source/provenance, no publication performed.'},indent=2))
sys.exit(0 if complete else 1)
