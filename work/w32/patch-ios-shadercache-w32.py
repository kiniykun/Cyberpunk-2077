#!/usr/bin/env python3
"""[w32-shcache] mem=ios: the material shader cache's shader section (~90 MiB, mostly native DXIL that the web build
replaces with 44-byte tokens on load) is no longer read into one buffer. Entries are parsed from 4 MiB chunks read
synchronously through the resource IO layer (CpResRead), so the 90 MiB transient - which stays resident in the
never-shrinking wasm heap - is gone. Other sections and the desktop path are unchanged."""
import subprocess
from pathlib import Path
PY = r'D:\Cyber punk\tools\emsdk\python\3.13.3_64bit\python.exe'; OV = r'D:\2077\work\w32\w32-overlay.py'
def copy(orig):
    subprocess.run([PY, OV, 'add', orig], capture_output=True, check=True)
    p = Path(subprocess.run([PY, OV, 'resolve', orig], capture_output=True, text=True, check=True).stdout.strip())
    assert 'build-w32' in str(p), p
    return p
def patch(orig, edits, tag):
    p = copy(orig); raw = p.read_bytes().decode('utf-8', errors='surrogateescape'); crlf = '\r\n' in raw
    s = raw.replace('\r\n', '\n')
    if tag in s: print('already', p.name); return
    for a, b in edits:
        assert s.count(a) == 1, (p.name, a[:90], s.count(a)); s = s.replace(a, b)
    p.write_bytes((s.replace('\n', '\r\n') if crlf else s).encode('utf-8', errors='surrogateescape')); print('patched', p.name)
SRC = r'D:\cp2077SourceCode\extracted\root\R6.Root\Mainline\dev\src\common\shaderCache'
patch(SRC + r'\include\shaderCacheReadonly.h', [
 ('''	void SerializeShaderEntries( Uint32 entryCount, IFile& file );''', '''	void SerializeShaderEntries( Uint32 entryCount, IFile& file );
	Bool CpWebStreamShaderEntries( Uint32 entryCount, Uint64 dataSize, const AnsiChar* path, Uint32 version ); // [w32-shcache]'''),
], '[w32-shcache]')
patch(SRC + r'\src\shaderCacheReadonly.cpp', [
 ('''#include "../../redIO/include/redIOAsyncIO.h"
''', '''#include "../../redIO/include/redIOAsyncIO.h"
#include <vector>   // [w32-shcache]
#include <cstring>
#include <cstdio>
#include <algorithm>
#include <limits.h>
#include <stdlib.h>
extern "C" int CpWebMemIos();
static AnsiChar s_cpShaderCachePath[ 512 ] = {};
'''),
 ('''void CShaderCacheReadonly::SerializeShaderEntries( Uint32 entryCount, IFile & file )
{''', '''extern "C" int CpResLookup( const char* path );                                  // [w32-shcache] resource IO (EM_JS)
extern "C" int CpResRead( int index, double offset, void* dest, int length );
Bool CShaderCacheReadonly::CpWebStreamShaderEntries( Uint32 entryCount, Uint64 dataSize, const AnsiChar* path, Uint32 version )
{
	char resolved[ PATH_MAX ];
	const int file = ::realpath( path, resolved ) ? CpResLookup( resolved ) : -1;
	if ( file < 0 ) { std::fprintf( stderr, "CP2077 w32 shader cache: streaming unavailable (path=%s), full read%c", path, 10 ); return false; }
	m_shaderEntries.Reserve( entryCount );
	m_shaders.Resize( entryCount );
	const size_t chunk = 4u << 20;
	std::vector< Uint8 > buf( chunk );
	size_t have = 0, pos = 0;
	Uint64 fileOffset = 0;
	auto fill = [&]( size_t need ) -> bool
	{
		if ( have - pos >= need ) return true;
		std::memmove( buf.data(), buf.data() + pos, have - pos ); have -= pos; pos = 0;
		if ( buf.size() < need ) buf.resize( need );
		while ( have < need && fileOffset < dataSize )
		{
			const int n = int( std::min< Uint64 >( buf.size() - have, dataSize - fileOffset ) );
			const int r = CpResRead( file, double( fileOffset ), buf.data() + have, n );
			if ( r <= 0 ) return false;
			have += size_t( r ); fileOffset += Uint64( r );
		}
		return have - pos >= need;
	};
	for ( Uint32 index = 0; index != entryCount; ++index )
	{
		if ( !fill( 20 ) ) return false;
		Uint32 size = 0; std::memcpy( &size, buf.data() + pos + 16, 4 );
		if ( !fill( 20 + size_t( size ) ) ) return false;
		CMemoryFileReader reader( buf.data() + pos, 20 + size, 0 );
		reader.SetInternalMemoryPool( rend::PoolShaderCacheData_ShaderBinaries::GetInstance() );
		reader.m_version = version;
		ShaderEntry& shader = m_shaders[ index ];
		shader.Serialize( reader );
		m_shaderEntries.Insert( shader.m_hash, index );
		pos += 20 + size_t( size );
	}
	return true;
}

void CShaderCacheReadonly::SerializeShaderEntries( Uint32 entryCount, IFile & file )
{'''),
 ('''		RED_FREE( rend::PoolShaderCacheData, asyncReadToken.m_buffer );

		// allocate buffer for shaders''', '''		RED_FREE( rend::PoolShaderCacheData, asyncReadToken.m_buffer );

		if ( ::CpWebMemIos() && loader->m_cache->CpWebStreamShaderEntries( loader->m_info->m_numShaderEntries, loader->m_info->m_shadersDataSize, s_cpShaderCachePath, loader->m_info->m_version ) )
		{
			// [w32-shcache] shader entries parsed in chunks; continue with the materials exactly as OnShadersLoaded does
			std::fprintf( stderr, "CP2077 w32 shader cache: %u shader entries streamed (%llu bytes, no full-section buffer)\\n",
				unsigned( loader->m_info->m_numShaderEntries ), static_cast< unsigned long long >( loader->m_info->m_shadersDataSize ) );
			loader->m_materialsBuffer = (Uint8*)RED_ALLOCATE( rend::PoolShaderCacheData, loader->m_info->m_materialsDataSize );
			auto nextAsyncReadToken = asyncReadToken;
			nextAsyncReadToken.m_buffer = loader->m_materialsBuffer;
			nextAsyncReadToken.m_offset = loader->m_info->m_materialsOffset;
			nextAsyncReadToken.m_numberOfBytesToRead = (Uint32)loader->m_info->m_materialsDataSize;
			nextAsyncReadToken.m_callback = &OnMaterialsLoaded;
			io::GAsyncIO.BeginRead( loader->m_fileHandle, nextAsyncReadToken, io::eAsyncPriority_High );
			return;
		}

		// allocate buffer for shaders'''),
 ('''	const AnsiChar* absoluteFilePathBuffer = absoluteFilePath.AsChar();
''', '''	const AnsiChar* absoluteFilePathBuffer = absoluteFilePath.AsChar();
	red::Strcpy( s_cpShaderCachePath, absoluteFilePathBuffer, sizeof( s_cpShaderCachePath ) ); // [w32-shcache]
'''),
], '[w32-shcache]')
