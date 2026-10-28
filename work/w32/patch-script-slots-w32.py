#!/usr/bin/env python3
"""[w32] Script bytecode keeps the x64 in-memory layout on wasm32.

Root cause (first wasm32 boot crash): CScriptedDataFunction::Load sizes the in-memory code from the blob's stored
size, which is the x64 in-memory size (pointer / intptr operands take 8 bytes). On wasm32 those operands were written
as 4 bytes, so CodeDiskToMemory never reached EndOfStream at the real end, read into the next object's data and
crashed in MapObject. Jump/skip/switch offsets inside the bytecode are x64 byte offsets as well.

Fix (wasm32 only, wasm64 text kept in #else): every pointer-width operand (any T*, intptr_t, uintptr_t) occupies an
8-byte slot in the bytecode -- value in the low 4 bytes, upper 4 bytes zero -- in all three places that touch it:
  * CScriptStackFrame::Read<T>              (VM, scriptStackFrame.h)
  * CodeMemoryToRuntime Read/Write          (binder, scriptDataBinder.h; also used by CodeRuntimeToRuntime)
  * CodeDiskToMemory::Write, CodeMemoryToDisk::Read (scriptDataObject.cpp; disk side unchanged: indices / Int64)
"""
import subprocess, sys
from pathlib import Path
PY = r'D:\Cyber punk\tools\emsdk\python\3.13.3_64bit\python.exe'; OV = r'D:\2077\work\w32\w32-overlay.py'
R = r'D:\cp2077SourceCode\extracted\root\R6.Root\Mainline\dev\src\common\redReflection'
TRAIT = '''#if defined( __wasm32__ ) && !defined( CPWEB_CODE_SLOT )
#define CPWEB_CODE_SLOT
#include <type_traits>
#include <cstring>
// [w32] Pointer-width bytecode operands keep their 8-byte x64 slot (see work/w32/patch-script-slots-w32.py).
template< typename T > struct CpWebCodeSlot
{
	static constexpr bool wide = std::is_pointer< T >::value || std::is_same< T, intptr_t >::value || std::is_same< T, uintptr_t >::value;
};
#endif
'''
def copy(orig):
    subprocess.run([PY, OV, 'add', orig], capture_output=True, check=True)
    return Path(subprocess.run([PY, OV, 'resolve', orig], capture_output=True, text=True, check=True).stdout.strip())
def patch(orig, edits, trait_after):
    p = copy(orig); raw = p.read_bytes().decode('utf-8', errors='surrogateescape'); crlf = '\r\n' in raw
    s = '\n'.join(l.rstrip() for l in raw.replace('\r\n', '\n').split('\n'))
    if 'CpWebCodeSlot' in s: print('already', p.name); return
    assert s.count(trait_after) == 1, (p.name, trait_after); s = s.replace(trait_after, trait_after + '\n' + TRAIT, 1)
    for a, b, n in edits:
        assert s.count(a) == n, (p.name, a[:80], s.count(a)); s = s.replace(a, b)
    p.write_bytes((s.replace('\n', '\r\n') if crlf else s).encode('utf-8', errors='surrogateescape')); print('patched', p.name)

patch(R + r'\include\scriptStackFrame.h', [(
"""	template < typename T >
	RED_INLINE T Read()
	{
		const T *data = reinterpret_cast< const T* >( m_code );""",
"""	template < typename T >
	RED_INLINE T Read()
	{
#if defined( __wasm32__ )
		if constexpr ( CpWebCodeSlot< T >::wide )
		{
			T value; std::memcpy( &value, m_code, sizeof( T ) ); m_code += 8; // [w32] 8-byte x64 slot
			return value;
		}
#endif
		const T *data = reinterpret_cast< const T* >( m_code );""", 1)], '#include "scriptableThreadSafetyMonitor.h"')

patch(R + r'\include\scriptDataBinder.h', [(
"""		template< typename T >
		RED_INLINE T Read()
		{
			RED_FATAL_ASSERT( m_readPos + sizeof(T) <= m_size, "Reading past stream end" );""",
"""		template< typename T >
		RED_INLINE T Read()
		{
#if defined( __wasm32__ )
			if constexpr ( CpWebCodeSlot< T >::wide )
			{
				RED_FATAL_ASSERT( m_readPos + 8 <= m_size, "Reading past stream end" );
				T ret; std::memcpy( &ret, m_data + m_readPos, sizeof( T ) ); m_readPos += 8; // [w32] 8-byte x64 slot
				return ret;
			}
#endif
			RED_FATAL_ASSERT( m_readPos + sizeof(T) <= m_size, "Reading past stream end" );""", 1), (
"""		template< typename T >
		RED_INLINE void Write( const T& data )
		{
			RED_FATAL_ASSERT( m_writePos + sizeof(T) <= m_size, "Reading past stream end" );""",
"""		template< typename T >
		RED_INLINE void Write( const T& data )
		{
#if defined( __wasm32__ )
			if constexpr ( CpWebCodeSlot< T >::wide )
			{
				RED_FATAL_ASSERT( m_writePos + 8 <= m_size, "Reading past stream end" );
				std::memset( (void*)( m_data + m_writePos ), 0, 8 ); std::memcpy( (void*)( m_data + m_writePos ), &data, sizeof( T ) ); m_writePos += 8; // [w32]
				return;
			}
#endif
			RED_FATAL_ASSERT( m_writePos + sizeof(T) <= m_size, "Reading past stream end" );""", 1)], '#include "resourceReferenceScriptToken.h"')

patch(R + r'\src\scriptDataObject.cpp', [(
# CodeMemoryToDisk::Read (memory side)
"""		template< typename T >
		RED_INLINE T Read()
		{
			RED_FATAL_ASSERT( m_pos + sizeof(T) <= m_size, "Reading past stream end" );
			const T ret = *(const T*)( m_data + m_pos );""",
"""		template< typename T >
		RED_INLINE T Read()
		{
#if defined( __wasm32__ )
			if constexpr ( CpWebCodeSlot< T >::wide )
			{
				RED_FATAL_ASSERT( m_pos + 8 <= m_size, "Reading past stream end" );
				T ret; std::memcpy( &ret, m_data + m_pos, sizeof( T ) ); m_pos += 8; // [w32] 8-byte x64 slot
				return ret;
			}
#endif
			RED_FATAL_ASSERT( m_pos + sizeof(T) <= m_size, "Reading past stream end" );
			const T ret = *(const T*)( m_data + m_pos );""", 1), (
# CodeDiskToMemory::Write (memory side)
"""		template< typename T >
		RED_INLINE void Write( const T& data )
		{
			RED_FATAL_ASSERT( m_pos + sizeof(T) <= m_size, "Reading past stream end" );
			*(T*)( m_data + m_pos ) = data;""",
"""		template< typename T >
		RED_INLINE void Write( const T& data )
		{
#if defined( __wasm32__ )
			if constexpr ( CpWebCodeSlot< T >::wide )
			{
				RED_FATAL_ASSERT( m_pos + 8 <= m_size, "Reading past stream end" );
				std::memset( m_data + m_pos, 0, 8 ); std::memcpy( m_data + m_pos, &data, sizeof( T ) ); m_pos += 8; // [w32]
				return;
			}
#endif
			RED_FATAL_ASSERT( m_pos + sizeof(T) <= m_size, "Reading past stream end" );
			*(T*)( m_data + m_pos ) = data;""", 1)], '#include "scriptOpcodeTransformer.h"')
