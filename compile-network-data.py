"""Compile unchanged transport-independent network data units, not a substitute network module."""
import concurrent.futures,hashlib,json,os,subprocess,sys
from pathlib import Path
root=Path(__file__).resolve().parent
original=Path(r'D:\cp2077SourceCode\extracted\root\R6.Root\Mainline\dev\src\common\network')
m=json.loads((root/'build/modules/network/compile-jobs.json').read_text(encoding='utf-8-sig'))
out=root/'build/modules/network-data';out.mkdir(parents=True,exist_ok=True)
header=original/'src/build.h'
text=header.read_text()
assert text.count('#include "networkConfig.h"')==1
adapted=out/'build-network-data.h'
adapted.write_text('#ifndef CPWEB_NETWORK_DATA_ONLY\n#error "Data-only PCH cannot configure a transport implementation"\n#endif\n'+text.replace('#include "networkConfig.h"','// No transport selection is used by properties.cpp/networkConditions.cpp.'))
overlay=out/'network-data-vfs.json'
overlay.write_text(json.dumps({'version':0,'use-external-names':False,'roots':[{'type':'file','name':str(header),'external-contents':str(adapted)}]}))
args=m['CommonArgs']+['-DCPWEB_NETWORK_DATA_ONLY=1','-ivfsoverlay',str(overlay)]
selected=[u for u in m['Units'] if u['Source'] in ('src/properties.cpp','src/networkConditions.cpp')]
assert len(selected)==2
os.environ['EMSDK_PYTHON']=sys.executable
def compile(u):
 obj=out/(Path(u['Input']).stem+'.o');log=out/(Path(u['Input']).stem+'.log')
 with log.open('wb') as f:p=subprocess.run([sys.executable,m['Compiler'],*args,'-c',u['Input'],'-o',str(obj)],stdout=f,stderr=subprocess.STDOUT)
 print(u['Source'],p.returncode,flush=True)
 if p.returncode:print('\n'.join(x for x in log.read_text(errors='replace').splitlines() if 'error:' in x),flush=True)
 return {'Source':u['Input'],'SourceSHA256':hashlib.sha256(Path(u['Input']).read_bytes()).hexdigest(),'Object':str(obj),'Log':str(log),'Compiled':p.returncode==0}
with concurrent.futures.ThreadPoolExecutor(max_workers=2) as p:units=list(p.map(compile,selected))
library=root/'build/lib/libnetwork-data.a'
complete=all(u['Compiled'] for u in units)
if complete:subprocess.run([sys.executable,r'D:\cp2077SourceCode\tools\emsdk\upstream\emscripten\emar.py','rcs',str(library),*[u['Object'] for u in units]],check=True)
status={'Module':'network-data','Complete':complete,'Library':str(library) if complete else None,'Compiled':sum(u['Compiled'] for u in units),'Sources':len(units),'OriginalFullNetworkModuleComplete':False,'Units':units,'Args':args,'OriginalBuildHeaderSHA256':hashlib.sha256(header.read_bytes()).hexdigest(),'Scope':'Unchanged original data semantics only; transport implementations/configuration remain required.'}
(out/'result.json').write_text(json.dumps(status,indent=2))
if not complete:sys.exit(1)
