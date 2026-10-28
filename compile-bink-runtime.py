"""Compile repaired CPU units and stage the original-SDK browser worker.

Requires the configured pinned Unicorn.js archive. This does not link the game.
"""
import hashlib,importlib.util,json,os,shutil,subprocess,sys
from pathlib import Path
root=Path(__file__).resolve().parent
cpu=Path(r'D:\cpweb-unicorn-source')
directory=cpu/'unicorn/build-debug'
archive=directory/'libunicorn.a'
ninja=root/'tools/build-tools/ninja.exe'
if not archive.is_file() or not ninja.is_file():
    raise SystemExit('Configure and compile the pinned CPU library before staging Bink')
os.environ['EMSDK_PYTHON']=sys.executable
emroot=Path(r'D:\cp2077SourceCode\tools\emsdk\upstream\emscripten')
os.environ['PATH']=str(emroot)+os.pathsep+os.environ['PATH']
source=cpu/'unicorn/qemu/target/i386/translate.c'
text=source.read_text()
lines=text.splitlines(keepends=True)
text=''.join(line for line in lines if 'CPWEB_TRANSLATOR_TRACE' not in line and 'fprintf(stderr, "CPWEB translate ' not in line)
source.write_text(text)
commands=subprocess.check_output([str(ninja),'-C',str(directory),'-t','commands'],text=True).splitlines()
log=root/'build/bink-runtime-build.log'
out=root/'build/game/codec/bink';out.mkdir(parents=True,exist_ok=True)
with log.open('wb') as stream:
    for unit in ['target/i386/translate.c','tcg/tcg.c']:
        command=next(c for c in commands if ' -c ' in c and c.endswith('/'+unit))
        subprocess.run(command,shell=True,cwd=directory,stdout=stream,stderr=subprocess.STDOUT,check=True)
        subprocess.run([r'D:\cp2077SourceCode\tools\emsdk\upstream\bin\llvm-ar.exe','r',str(archive),
            'CMakeFiles/x86_64-softmmu.dir/qemu/'+unit+'.o'],cwd=directory,check=True)
    spec=importlib.util.spec_from_file_location('ucbuild',cpu/'build.py')
    build=importlib.util.module_from_spec(spec);spec.loader.exec_module(build)
    args=['-Os',str(archive),'-s',f'EXPORTED_FUNCTIONS={build.EXPORTED_FUNCTIONS}',
        '-s',"EXPORTED_RUNTIME_METHODS=['ccall','getValue','setValue','addFunction','removeFunction','writeArrayToMemory']",
        '-s','ALLOW_TABLE_GROWTH=1','-s','ALLOW_MEMORY_GROWTH=1','-s','MODULARIZE=1','-s','SINGLE_FILE=1',
        '-s','ASSERTIONS=1','-s',"EXPORT_NAME='MUnicorn'",'--post-js','src/constants_x86.js',
        '--post-js','src/unicorn-wrapper.js','-o',str(out/'unicorn_x86.js')]
    subprocess.run([sys.executable,str(emroot/'emcc.py'),*args],cwd=cpu,stdout=stream,stderr=subprocess.STDOUT,check=True)
for name in ['bink-decoder-worker.js','bink-sdk-vm.mjs','bink-platform-web.mjs','bink-framebuffers-web.mjs','bink-memory-client-web.mjs','cached-bink-movie-web.mjs','predecoded-bink-frames-web.mjs']:
    shutil.copyfile(root/'src'/name,out/name)
shutil.copyfile(cpu/'LICENSE',out/'UNICORN_JS_LICENSE')
shutil.copyfile(cpu/'unicorn/COPYING',out/'UNICORN_COPYING')
result={'Compiled':True,'Output':str(out),'CPUCommit':subprocess.check_output(['git','rev-parse','HEAD'],cwd=cpu,text=True).strip(),
    'CPUFix':'Free zero-extension helper arguments after emitting each TCG call',
    'Artifacts':{path.name:hashlib.sha256(path.read_bytes()).hexdigest() for path in out.iterdir() if path.is_file()},
    'EngineIntegrated':False,'BrowserWorkerVerified':False,
    'Scope':'Original SDK Wasm CPU worker and real caller-owned decoded planes; no replacement game UI.'}
(root/'build/bink-runtime-result.json').write_text(json.dumps(result,indent=2))
print('Staged repaired CPU runtime and Bink worker; engine linkage remains pending.')
