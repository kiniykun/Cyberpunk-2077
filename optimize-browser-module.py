"""Reduce duplicate native functions without replacing any engine implementation."""
import hashlib,json,subprocess,shutil
from pathlib import Path
root=Path(__file__).resolve().parent
binary=root/'build/game/cp2077.wasm'
def function_count(path):
    data=path.read_bytes()
    def integer(offset):
        value=shift=0
        while True:
            byte=data[offset];offset+=1;value|=(byte&127)<<shift
            if not byte&128:return value,offset
            shift+=7
    position=8
    while position<len(data):
        section=data[position];position+=1
        size,start=integer(position)
        if section==3:return integer(start)[0]
        position=start+size
    return 0
def main():
    before=function_count(binary)
    result={'BeforeFunctions':before,'AfterFunctions':before,'EngineRuntimeVerified':False}
    tool=Path(r'D:\Cyber punk\tools\emsdk\upstream\bin\wasm-opt.exe')
    digest=lambda path:hashlib.sha256(path.read_bytes()).hexdigest()
    cache=root/'build/browser-function-reduction-cache.wasm'
    metadata=root/'build/browser-function-reduction-cache.json'
    raw_hash=digest(binary)
    tool_hash=digest(tool)
    reused=False
    if before>=1000000 and cache.is_file() and metadata.is_file():
        cached=json.loads(metadata.read_text())
        if cached['RawSHA256']==raw_hash and cached['ToolSHA256']==tool_hash and cached['OptimizedSHA256']==digest(cache):
            after=function_count(cache)
            if after>=1000000:raise RuntimeError('Invalid cached browser function count')
            shutil.copyfile(binary,root/'build/cp2077-before-function-reduction.wasm')
            shutil.copyfile(cache,binary)
            result.update(AfterFunctions=after,ExactBinaryCacheReused=True)
            reused=True
    if before>=1000000 and not reused:
        output=root/'build/game/cp2077.optimized.wasm'
        log=root/'build/browser-function-reduction.log'
        with log.open('wb') as target:
            subprocess.run([str(tool),str(binary),'--all-features','--disable-compact-imports','-g','--duplicate-function-elimination','--remove-unused-module-elements','-o',str(output)],stdout=target,stderr=subprocess.STDOUT,check=True)
        after=function_count(output)
        if after>=1000000:raise RuntimeError(f'Function reduction still exceeds browser limit: {after}')
        backup=root/'build/cp2077-before-function-reduction.wasm'
        backup.write_bytes(binary.read_bytes())
        binary.write_bytes(output.read_bytes())
        result.update(AfterFunctions=after,OriginalBackup=str(backup))
        shutil.copyfile(binary,cache)
        metadata.write_text(json.dumps({'RawSHA256':raw_hash,'OptimizedSHA256':digest(cache),'ToolSHA256':tool_hash},indent=2))
    result.update(WasmBytes=binary.stat().st_size,WasmSHA256=hashlib.sha256(binary.read_bytes()).hexdigest(),Scope='Duplicate-function elimination and unreachable module element removal; ordinary import encoding retained.')
    (root/'build/browser-function-reduction-result.json').write_text(json.dumps(result,indent=2))
    print(json.dumps(result,indent=2))
if __name__=='__main__':main()
