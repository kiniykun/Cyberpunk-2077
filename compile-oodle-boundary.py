import hashlib,json,os,subprocess,sys
from pathlib import Path
root=Path(__file__).resolve().parent
manifest=json.loads((root/'build/modules/network/compile-jobs.json').read_text(encoding='utf-8-sig'))
manifest['CommonArgs']=[a for a in manifest['CommonArgs'] if a!='-DRED_MODULE_network']
os.environ['EMSDK_PYTHON']=sys.executable
units=[]
for name in ('oodle-decompress-web','oodle-sizes-web','oodle-hooks-web','oodle-compress-web'):
 source=root/f'src/{name}.cpp';obj=root/f'build/objects/{name}.o';log=root/f'build/{name}.log'
 with log.open('wb') as f:result=subprocess.run([sys.executable,manifest['Compiler'],*manifest['CommonArgs'],'-c',str(source),'-o',str(obj)],stdout=f,stderr=subprocess.STDOUT)
 units.append({'Compiled':result.returncode==0,'Object':str(obj),'Source':str(source),'SourceSHA256':hashlib.sha256(source.read_bytes()).hexdigest(),'Log':str(log)})
 if result.returncode:print(log.read_text(errors='replace'))
complete=all(u['Compiled'] for u in units)
status={'Compiled':complete,'Object':units[0]['Object'],'Objects':[u['Object'] for u in units],'Units':units,'Args':manifest['CommonArgs'],'EngineCallPathSupported':'Original wrapper non-dictionary/callback-free unthreaded fuzz-safe CRC Kraken decode; Normal/Optimal default-options Kraken encode; size queries and allocator/assertion hooks','FullOodleAPIParity':False,'EngineExecutionVerified':False,'EncoderExecutionVerified':False,'MenusRunning':False}
(root/'build/oodle-boundary-result.json').write_text(json.dumps(status,indent=2))
if complete:print('Compiled engine Oodle decompression boundary and size queries; runtime integration unverified.')
sys.exit(0 if complete else 1)
