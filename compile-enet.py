"""Compile the bundled ENet sources against Emscripten's actual POSIX socket API."""
import concurrent.futures
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys

root = Path(__file__).resolve().parent
source = Path(r'D:\cp2077SourceCode\extracted\root\R6.Root\Mainline\dev\external\enet')
out = root / 'build/modules/enet-browser'
out.mkdir(parents=True, exist_ok=True)
include = out / 'include/enet'
include.mkdir(parents=True, exist_ok=True)
# The depot flattened the public headers; preserve their bytes while restoring
# the include layout used by the original C sources.
for header in (source / 'include').glob('*.h'):
    shutil.copyfile(header, include / header.name)
compiler = Path(r'D:\cp2077SourceCode\tools\emsdk\upstream\emscripten\emcc.py')
os.environ['EMSDK_PYTHON'] = sys.executable
args = ['-O2', '-pthread', '-sMEMORY64=1', '-I'+str(out/'include')]
args += ['-D'+name+'=1' for name in ['HAS_FCNTL', 'HAS_POLL', 'HAS_INET_PTON', 'HAS_INET_NTOP', 'HAS_MSGHDR_FLAGS', 'HAS_SOCKLEN_T', 'HAS_GETADDRINFO', 'HAS_GETNAMEINFO']]

def compile_unit(name):
    src = source/'src'/name
    obj = out/(src.stem+'.o')
    log = out/(src.stem+'.log')
    with log.open('wb') as f:
        result = subprocess.run([sys.executable, str(compiler), *args, '-c', str(src), '-o', str(obj)], stdout=f, stderr=subprocess.STDOUT)
    print(name, result.returncode, flush=True)
    if result.returncode:
        print(log.read_text(errors='replace'), flush=True)
    return {'Source':str(src), 'SourceSHA256':hashlib.sha256(src.read_bytes()).hexdigest(), 'Object':str(obj), 'Log':str(log), 'Compiled':result.returncode==0}

with concurrent.futures.ThreadPoolExecutor(max_workers=8) as pool:
    units = list(pool.map(compile_unit, ['callbacks.c','compress.c','host.c','list.c','packet.c','peer.c','protocol.c','unix.c']))
complete = all(unit['Compiled'] for unit in units)
library = root/'build/lib/libenet-browser.a'
if complete:
    subprocess.run([sys.executable,str(compiler.with_name('emar.py')),'rcs',str(library),*[unit['Object'] for unit in units]],check=True)
status = {'Complete':complete,'Library':str(library) if complete else None,'Units':units,'Args':args,'Headers':{str(h):hashlib.sha256(h.read_bytes()).hexdigest() for h in (source/'include').glob('*.h')},'RuntimeVerified':False,'Transport':'Original ENet UDP implementation using Emscripten POSIX sockets. Browser connectivity requires a compatible transport bridge; compiling does not establish connectivity.'}
(out/'result.json').write_text(json.dumps(status,indent=2))
sys.exit(0 if complete else 1)
