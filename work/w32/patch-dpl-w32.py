#!/usr/bin/env python3
"""[w32-dpl] mem=ios: particle lighting (DPL) texture at 1/4 resolution (2048 -> 512, 16 -> 1 MiB R11G11B10); the
engine's own console path (base-1 on Xbox One/PS4) already compensates the mip bias."""
import subprocess
from pathlib import Path
PY = r'D:\Cyber punk\tools\emsdk\python\3.13.3_64bit\python.exe'; OV = r'D:\2077\work\w32\w32-overlay.py'
orig = r'D:\cp2077SourceCode\extracted\root\R6.Root\Mainline\dev\src\common\renderer\src\particleDPLTexture.cpp'
subprocess.run([PY, OV, 'add', orig], capture_output=True, check=True)
p = Path(subprocess.run([PY, OV, 'resolve', orig], capture_output=True, text=True, check=True).stdout.strip()); assert 'build-w32' in str(p)
raw = p.read_bytes().decode('utf-8', errors='surrogateescape'); crlf = '\r\n' in raw; s = raw.replace('\r\n', '\n')
if '[w32-dpl]' in s: raise SystemExit('already')
a = '''Uint32 CParticleDPLTexture::GetDPLTextureResLog2()
{
	if ( '''
b = '''extern "C" int CpWebMemIos(); // [w32-dpl]
Uint32 CParticleDPLTexture::GetDPLTextureResLog2()
{
	if ( ::CpWebMemIos() ) return c_baseResLog2 - 2; // [w32-dpl] iPhone: 1/4 resolution
	if ( '''
assert s.count(a) == 1; s = s.replace(a, b)
p.write_bytes((s.replace('\n', '\r\n') if crlf else s).encode('utf-8', errors='surrogateescape')); print('patched', p)
