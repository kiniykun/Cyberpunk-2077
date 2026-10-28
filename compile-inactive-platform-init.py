import json,subprocess,os,sys,hashlib,concurrent.futures,re
from pathlib import Path
root=Path(r'D:\cp2077 port')
base=Path(r'D:\cp2077SourceCode\extracted\root\R6.Root\Mainline\dev\src')
m=json.loads((root/'build/modules/gameServices/compile-jobs.json').read_text(encoding='utf-8-sig'))
out=root/'build/modules/platform-init';out.mkdir(parents=True,exist_ok=True)
selection=[('gameServicesWin','win32','RED_PLATFORM_WINPC'),('gameServicesDurango','durango','RED_PLATFORM_DURANGO'),('gameServicesOrbis','orbis','RED_PLATFORM_ORBIS')]
os.environ['EMSDK_PYTHON']=sys.executable
units=[]
for module,platform,guard in selection:
 directory=base/platform/module
 source=directory/'src'/f'{module}Init.cpp'
 text=source.read_text()
 assert re.search(r'#ifdef\s+'+guard+r'\s+Register'+module[0].upper()+module[1:]+r'RTTI\(\);',text)
 include=re.search(r'#include "([^"]*typeRegistryFuncImpl.h)"',text).group(1)
 original=(source.parent/include).resolve()
 header=out/f'{module}-inactive-rtti.h'
 header.write_text(f'#pragma once\n#if defined({guard})\n#error "Browser inactive-platform init partition cannot provide native-platform RTTI"\n#endif\n')
 overlay=out/f'{module}-vfs.json';overlay.write_text(json.dumps({'version':0,'use-external-names':False,'roots':[{'type':'file','name':str(original),'external-contents':str(header)}]}))
 args=[x for x in m['CommonArgs'] if not x.startswith('-DRED_MODULE_')]+[f'-DRED_MODULE_{module}','-ivfsoverlay',str(overlay),f'-I{directory}/src',f'-I{directory}/include']
 units.append({'Module':module,'Source':str(source),'SourceSHA256':hashlib.sha256(source.read_bytes()).hexdigest(),'Object':str(out/f'{module}.o'),'Log':str(out/f'{module}.log'),'Args':args,'InactivePlatformGuard':guard})
def compile(u):
 with open(u['Log'],'wb') as f:p=subprocess.run([sys.executable,m['Compiler'],*u['Args'],'-c',u['Source'],'-o',u['Object']],stdout=f,stderr=subprocess.STDOUT)
 u['Compiled']=p.returncode==0;print(u['Module'],p.returncode,flush=True)
 if p.returncode:print('\n'.join(x for x in Path(u['Log']).read_text(errors='replace').splitlines() if 'error:' in x),flush=True)
 return u
with concurrent.futures.ThreadPoolExecutor(max_workers=3) as p:units=list(p.map(compile,units))
complete=all(u['Compiled'] for u in units)
library=root/'build/lib/libplatform-init.a'
if complete:subprocess.run([sys.executable,r'D:\cp2077SourceCode\tools\emsdk\upstream\emscripten\emar.py','rcs',str(library),*[u['Object'] for u in units]],check=True)
(out/'result.json').write_text(json.dumps({'Module':'platform-init','Complete':complete,'Library':str(library) if complete else None,'Units':units,'Scope':'Exact original RED_MODULE initialization functions with their original inactive-platform guards. Does not implement Windows/Xbox/PlayStation services or claim their RTTI is registered.'},indent=2))
if not complete:sys.exit(1)
