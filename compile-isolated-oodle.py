"""Build the actual codec into a private worker module, separate from engine memory."""
import hashlib,json,os,shutil,subprocess,sys
from pathlib import Path
root=Path(__file__).resolve().parent;out=root/'build/game/codec';out.mkdir(parents=True,exist_ok=True)
status=root/'build/modules/ooz/isolated-worker-result.json';source=root/'tools/ooz-wasm/lib'
compiler=Path(r'D:\cp2077SourceCode\tools\emsdk\upstream\emscripten\em++.py');os.environ['EMSDK_PYTHON']=sys.executable
inputs=[source/name for name in ('kraken.cpp','lzna.cpp','bitknit.cpp')]+[root/'src/oodle-checksum-web.cpp']
encoder_status=json.loads((root/'build/modules/ooz-encoder/result.json').read_text())
if not encoder_status['Complete']:raise SystemExit('Compile actual encoder before linking private codec')
inputs.append(root/'src/oodle-encoder-private.cpp')
encoder_library=Path(encoder_status['Library'])
args=['-DCPWEB_OOZ_KRAKEN_ONLY=1','-O2','-std=c++17','-msimd128','-msse2','-fno-exceptions','-fno-rtti','-sENVIRONMENT=worker','-sMODULARIZE=1','-sEXPORT_ES6=1','-sALLOW_MEMORY_GROWTH=1','-sMAXIMUM_MEMORY=536870912','-sSTACK_SIZE=1048576','-sEXPORTED_RUNTIME_METHODS=["HEAPU8"]','-sEXPORTED_FUNCTIONS=["_Kraken_Decompress","_malloc"]','--no-entry']
log=status.with_suffix('.log')
args=[arg.replace('["_Kraken_Decompress","_malloc"]','["_Kraken_Decompress","_CpWebKrakenEncode","_malloc"]') for arg in args]
with log.open('wb') as f:result=subprocess.run([sys.executable,str(compiler),*[str(p) for p in inputs],str(encoder_library),*args,'-o',str(out/'oodle-decoder.mjs')],stdout=f,stderr=subprocess.STDOUT)
if result.returncode:print(log.read_text(errors='replace'));sys.exit(result.returncode)
shutil.copyfile(root/'src/oodle-decoder-worker.mjs',out/'oodle-decoder-worker.mjs')
status.write_text(json.dumps({'Compiled':True,'Inputs':{str(p):hashlib.sha256(p.read_bytes()).hexdigest() for p in inputs},'EncoderLibrary':str(encoder_library),'EncoderLibrarySHA256':hashlib.sha256(encoder_library.read_bytes()).hexdigest(),'EncoderCommit':encoder_status['Commit'],'Args':args,'Output':str(out/'oodle-decoder.mjs'),'Worker':str(out/'oodle-decoder-worker.mjs'),'WorkerSourceSHA256':hashlib.sha256((root/'src/oodle-decoder-worker.mjs').read_bytes()).hexdigest(),'BrokerSourceSHA256':hashlib.sha256((root/'src/oodle-decoder-broker-web.js').read_bytes()).hexdigest(),'EngineABIIntegrated':False,'WorkerExecutionVerified':False,'EncoderExecutionVerified':False,'Scope':'Real ooz decoder, Kraken encoder and original-compatible checksum in private non-shared wasm32 memory. Engine remains wasm64. Browser worker copies input/output, checks padding guards, discards codec instance and enforces deadlines via broker. Does not claim full native Oodle API parity or game execution.'},indent=2))
print('Built and linked isolated Kraken encoder/decoder worker; engine execution remains unverified.')

