import concurrent.futures,hashlib,json,os,subprocess,sys
from pathlib import Path
root=Path(__file__).resolve().parent
source=Path(r'D:\cp2077SourceCode\extracted\root\R6.Root\Mainline\dev\internal\Audio\Otic Early Reflections Plugins\cdp_voice_broadcast\SoundEnginePlugin')
out=root/'build/modules/voice-broadcast';out.mkdir(parents=True,exist_ok=True)
manifest=json.loads((root/'build/modules/wwise-sound-core/compile-jobs.json').read_text())
args=manifest['CommonArgs']+['-I'+str(source)]
os.environ['EMSDK_PYTHON']=sys.executable
def compile_unit(src):
 obj=out/(src.stem+'.o');log=obj.with_suffix('.log')
 with log.open('wb') as f:result=subprocess.run([sys.executable,manifest['Compiler'],*args,'-c',str(src),'-o',str(obj)],stdout=f,stderr=subprocess.STDOUT)
 print(src.name,result.returncode,flush=True)
 if result.returncode:print('\n'.join(line for line in log.read_text(errors='replace').splitlines() if 'error:' in line),flush=True)
 return {'Source':str(src),'SourceSHA256':hashlib.sha256(src.read_bytes()).hexdigest(),'Object':str(obj),'Log':str(log),'Compiled':result.returncode==0}
sources=sorted(source.glob('*.cpp'))+[root/'src/wwise-plugin-registry-web.cpp']
with concurrent.futures.ThreadPoolExecutor(max_workers=6) as pool:units=list(pool.map(compile_unit,sources))
complete=all(u['Compiled'] for u in units);library=root/'build/lib/libvoice-broadcast.a'
if complete:subprocess.run([sys.executable,str(Path(manifest['Compiler']).with_name('emar.py')),'rcs',str(library),*[u['Object'] for u in units]],check=True)
(out/'result.json').write_text(json.dumps({'Complete':complete,'Library':str(library) if complete else None,'Units':units,'Args':args,'RuntimeVerified':False,'Scope':'Original CDP voice send/receive plugin and original double-buffer callback; source compilation only.'},indent=2))
sys.exit(0 if complete else 1)
