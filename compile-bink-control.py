import hashlib,json,os,subprocess,sys
from pathlib import Path
root=Path(__file__).resolve().parent
manifest=json.loads((root/'build/modules/network/compile-jobs.json').read_text(encoding='utf-8-sig'))
args=[a for a in manifest['CommonArgs'] if a!='-DRED_MODULE_network']
source=root/'src/bink-control-web.cpp';obj=root/'build/objects/bink-control-web.o';log=root/'build/bink-control-web.log'
os.environ['EMSDK_PYTHON']=sys.executable
with log.open('wb') as f:result=subprocess.run([sys.executable,manifest['Compiler'],*args,'-c',str(source),'-o',str(obj)],stdout=f,stderr=subprocess.STDOUT)
(root/'build/bink-control-result.json').write_text(json.dumps({'Compiled':result.returncode==0,'Object':str(obj),'SourceSHA256':hashlib.sha256(source.read_bytes()).hexdigest(),'Args':args,'Log':str(log),'RuntimeVerified':False,'VideoDecoderImplemented':False,'Scope':'Actual original SDK keyframe lookup algorithm and header layout, not a video decoder or empty Bink startup stub.'},indent=2))
if result.returncode:print(log.read_text(errors='replace'))
else:print('Compiled original-compatible Bink keyframe lookup; frame decoder remains incomplete.')
sys.exit(result.returncode)
