import hashlib,json,os,subprocess,sys
from pathlib import Path
root=Path(__file__).resolve().parent
manifest=json.loads((root/'build/modules/network/compile-jobs.json').read_text(encoding='utf-8-sig'))
args=[arg for arg in manifest['CommonArgs'] if arg!='-DRED_MODULE_network']
source=root/'src/bink-allocator-web.cpp';obj=root/'build/objects/bink-allocator-web.o';log=root/'build/bink-allocator-web.log'
os.environ['EMSDK_PYTHON']=sys.executable
with log.open('wb') as stream:
    result=subprocess.run([sys.executable,manifest['Compiler'],*args,'-c',str(source),'-o',str(obj)],stdout=stream,stderr=subprocess.STDOUT)
(root/'build/bink-allocator-result.json').write_text(json.dumps({'Compiled':result.returncode==0,'Object':str(obj),
    'SourceSHA256':hashlib.sha256(source.read_bytes()).hexdigest(),'Args':args,'Log':str(log),
    'RuntimeVerified':False,'Scope':'Original allocator callbacks own real SDK backing bytes, synchronized with private guest CPU cache; matching frees and no-fallback failure sentinel.'},indent=2))
if result.returncode:print(log.read_text(errors='replace'))
else:print('Compiled Bink allocator service; cross-worker execution remains unverified.')
sys.exit(result.returncode)
