#!/usr/bin/env python3
"""[w32-texmip] mem=ios: microblend textures also drop mips (min 2; they were exempt from the SafeMode drop), and the
first 64 render textures >= 1024 px are logged (path, group, drop) so the big boot-time bc7 set can be identified."""
import subprocess
from pathlib import Path
PY = r'D:\Cyber punk\tools\emsdk\python\3.13.3_64bit\python.exe'; OV = r'D:\2077\work\w32\w32-overlay.py'
def copy(orig):
    subprocess.run([PY, OV, 'add', orig], capture_output=True, check=True)
    return Path(subprocess.run([PY, OV, 'resolve', orig], capture_output=True, text=True, check=True).stdout.strip())
def patch(orig, edits, tag):
    p = copy(orig); assert 'build-w32' in str(p), p
    raw = p.read_bytes().decode('utf-8', errors='surrogateescape'); crlf = '\r\n' in raw
    s = raw.replace('\r\n', '\n')
    if tag in s: print('already', p.name); return
    for a, b in edits:
        assert s.count(a) == 1, (p.name, a[:90], s.count(a)); s = s.replace(a, b)
    p.write_bytes((s.replace('\n', '\r\n') if crlf else s).encode('utf-8', errors='surrogateescape')); print('patched', p)

patch(r'D:\cp2077SourceCode\extracted\root\R6.Root\Mainline\dev\src\common\renderer\src\renderTexture.cxx', [
 ('''TRenderPtr<CRenderTexture> CRenderTexture::Create( const SRenderTextureCreateInfo& info )
{''', '''#include <atomic>
#include <cstdio>
extern "C" int CpWebMemIos(); // [w32-texmip]
TRenderPtr<CRenderTexture> CRenderTexture::Create( const SRenderTextureCreateInfo& info )
{'''),
 ('''	if ( info.setup->m_group == GpuWrapApi::TEXG_Multilayer_Microblend )
	{
		numMipsToDrop = 0;
	}
''', '''	if ( info.setup->m_group == GpuWrapApi::TEXG_Multilayer_Microblend )
	{
		numMipsToDrop = ::CpWebMemIos() ? 2 : 0; // [w32-texmip] iPhone: microblends drop 2 mips too
	}
	if ( ::CpWebMemIos() && sizeInfo.m_width >= 1024 ) // [w32-texmip] identify the big textures (first 64)
	{
		static std::atomic< unsigned > cpBigLogged{ 0 };
		if ( cpBigLogged.fetch_add( 1 ) < 64 )
			std::fprintf( stderr, "CP2077 w32 big texture: %ux%u mips=%u group=%d drop=%u path=%s\\n", Uint32( sizeInfo.m_width ), Uint32( sizeInfo.m_height ),
				Uint32( texInfo.m_mipCount ), Int32( info.setup->m_group ), numMipsToDrop, info.m_resourcePath.ToDebugString() );
	}
'''),
], '[w32-texmip]')
