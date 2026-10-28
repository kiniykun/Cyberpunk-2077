import json,hashlib,subprocess,os,sys,concurrent.futures
from pathlib import Path
root=Path(r'D:\cp2077 port');source=root/'tools/ooz-wasm/lib';out=root/'build/modules/ooz';out.mkdir(parents=True,exist_ok=True)
compiler=Path(r'D:\cp2077SourceCode\tools\emsdk\upstream\emscripten\em++.py');archiver=compiler.with_name('emar.py')
os.environ['EMSDK_PYTHON']=sys.executable
args=['-std=c++17','-O2','-pthread','-sMEMORY64=1','-msimd128','-msse2','-fno-exceptions','-fno-rtti']
def compile(name):
 p=(root/'src/oodle-checksum-web.cpp') if name=='oodle-checksum-web.cpp' else source/name;obj=out/(p.stem+'.o');log=out/(p.stem+'.log')
 with log.open('wb') as f:r=subprocess.run([sys.executable,str(compiler),*args,'-c',str(p),'-o',str(obj)],stdout=f,stderr=subprocess.STDOUT)
 print(name,r.returncode,flush=True)
 if r.returncode:print('\n'.join(x for x in log.read_text(errors='replace').splitlines() if 'error:' in x),flush=True)
 return {'Source':str(p),'SourceSHA256':hashlib.sha256(p.read_bytes()).hexdigest(),'Object':str(obj),'Log':str(log),'Compiled':r.returncode==0}
with concurrent.futures.ThreadPoolExecutor(max_workers=4) as p:units=list(p.map(compile,['kraken.cpp','lzna.cpp','bitknit.cpp','oodle-checksum-web.cpp']))
complete=all(u['Compiled'] for u in units);library=root/'build/lib/libooz.a'
if complete:subprocess.run([sys.executable,str(archiver),'rcs',str(library),*[u['Object'] for u in units]],check=True)
commit=subprocess.check_output(['git','-C',str(root/'tools/ooz-wasm'),'rev-parse','HEAD'],text=True).strip()
validation_path=out/'checksum-validation/result.json'
validation=json.loads(validation_path.read_text()) if validation_path.exists() else {}
checksum_verified=validation.get('Passed',False) and validation.get('SourceSHA256')==hashlib.sha256((root/'src/oodle-checksum-web.cpp').read_bytes()).hexdigest()
(out/'result.json').write_text(json.dumps({'Complete':complete,'Library':str(library) if complete else None,'Upstream':'https://github.com/SnosMe/ooz-wasm','Commit':commit,'UpstreamModified':True,'Modification':'Kraken_GetCrc forwards to the recovered original quantum checksum implementation, compiled as a fourth unit. Upstream license retained.','ChecksumWasmVerified':checksum_verified,'ChecksumValidation':str(validation_path),'Units':units,'Args':args,'LicenseFile':str(root/'tools/ooz-wasm/LICENSE'),'OodleABIIntegrated':False,'DecoderExecutionVerified':False,'RequiredBeforeIntegration':['Preserve original fuzz-safe contract','Protect input/output padding requirements','Match allocator and decoder callback contracts','Verify on actual engine compression streams'],'GameRunning':False},indent=2))
if not complete:sys.exit(1)
