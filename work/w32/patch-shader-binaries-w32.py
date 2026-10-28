#!/usr/bin/env python3
"""[w32] The engine's material shader cache keeps every native DXIL binary in memory (~90 MiB in-world). On the web the
binary is only an identity key (FindConvertedGraphicsShaderBytecode); WebGPU uses the converted WGSL. On load, a
ShaderEntry payload that starts with 'DXBC' is replaced by a 44-byte token: "CPW32SH1" + u32 original length +
SHA-256 of the original bytes. The lookup accepts the token (or full bytes, as before). StaticShaderEntry (static cache,
including the converted compute records) is not touched.
"""
import subprocess
from pathlib import Path
PY = r'D:\Cyber punk\tools\emsdk\python\3.13.3_64bit\python.exe'; OV = r'D:\2077\work\w32\w32-overlay.py'
def copy(orig):
    subprocess.run([PY, OV, 'add', orig], capture_output=True, check=True)
    return Path(subprocess.run([PY, OV, 'resolve', orig], capture_output=True, text=True, check=True).stdout.strip())
def patch(orig, edits, tag):
    p = copy(orig); raw = p.read_bytes().decode('utf-8', errors='surrogateescape'); crlf = '\r\n' in raw
    s = '\n'.join(l.rstrip() for l in raw.replace('\r\n', '\n').split('\n'))
    if tag in s: print('already', p.name); return
    for a, b in edits:
        assert s.count(a) == 1, (p.name, a[:90], s.count(a)); s = s.replace(a, b)
    p.write_bytes((s.replace('\n', '\r\n') if crlf else s).encode('utf-8', errors='surrogateescape')); print('patched', p.name)

patch(r'D:\cp2077SourceCode\extracted\root\R6.Root\Mainline\dev\src\common\shaderCache\include\shaderCacheData.h', [
 ('#include "../../redContainers/include/string/stringBuilder.h"', '''#include "../../redContainers/include/string/stringBuilder.h"
#if defined( __wasm32__ )
#include <cstring>
namespace cp2077 { namespace web { void CpWebSha256Digest( const void* data, std::size_t bytes, unsigned char out[ 32 ] ); } }
// [w32-shaderbin] Native DXIL is only an identity key on the web: keep "CPW32SH1" + u32 length + SHA-256 (44 bytes).
inline void CpWebCompactShaderBinary( DataBuffer& data )
{
	const Uint32 n = data.Size();
	const unsigned char* p = static_cast< const unsigned char* >( data.Data() );
	if ( !p || n < 64 || std::memcmp( p, "DXBC", 4 ) != 0 ) return;
	unsigned char token[ 44 ];
	std::memcpy( token, "CPW32SH1", 8 ); std::memcpy( token + 8, &n, 4 );
	cp2077::web::CpWebSha256Digest( p, n, token + 12 );
	data = DataBuffer::Copy( token, sizeof( token ) );
}
#endif'''),
 ('''		file << m_reflectionParamsHash;
		m_data.Serialize( file );''', '''		file << m_reflectionParamsHash;
		m_data.Serialize( file );
#if defined( __wasm32__ )
		if ( file.IsReader() ) CpWebCompactShaderBinary( m_data ); // [w32-shaderbin]
#endif''')], '[w32-shaderbin]')

patch(r'D:\2077\src\gpu-shader-web.cpp', [
 ('''const ConvertedGraphicsShader* CpWebMaterialize(const ConvertedGraphicsShader* s){''',
  '''void CpWebSha256Digest(const void* data, std::size_t bytes, unsigned char out[32]) { CpWebSha256().Digest(data, bytes, out); } // [w32-shaderbin]
const ConvertedGraphicsShader* CpWebMaterialize(const ConvertedGraphicsShader* s){'''),
 ('''        unsigned char digest[32]; bool hashed=false;
        auto same=[&](const ConvertedGraphicsShader& shader){
            if(bytes!=shader.nativeBytecodeBytes) return false;''', '''        unsigned char digest[32]; bool hashed=false;
        std::size_t length=bytes;
        if(bytes==44 && std::memcmp(data,"CPW32SH1",8)==0){ // [w32-shaderbin] compact token from the shader cache loader
            std::uint32_t original=0; std::memcpy(&original,static_cast<const unsigned char*>(data)+8,4);
            length=original; std::memcpy(digest,static_cast<const unsigned char*>(data)+12,32); hashed=true;
        }
        auto same=[&](const ConvertedGraphicsShader& shader){
            if(length!=shader.nativeBytecodeBytes) return false;''')], '[w32-shaderbin]')
