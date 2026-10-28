#!/usr/bin/env python3
"""[w32] Cooked PhysX binary collections are serialized for win64 (pointer-bearing in-place layout). On wasm32
createCollectionFromBinary read 64-bit layout as 32-bit and crashed (MaterialLibraryResource::OnPostLoad).

Fix (wasm32 only): before createCollectionFromBinary, a collection whose header platform tag is not this build's
tag is converted with PhysX's own PxBinaryConverter: source metadata = the depot's engine/physics/win64.metaData
(embedded with #embed), destination metadata = PxSerialization::dumpBinaryMetaData of the running wasm32 build.
The converted bytes replace the caller's buffer (PhysX deserializes in place, so lifetime follows the owner):
  * MaterialLibraryResource::m_collectionData, ProxySimpleCollider::m_compiledData (DataBuffer members)
  * DeferredCollection: converted copy kept in its private impl next to the collection.
"""
import subprocess
from pathlib import Path
PY = r'D:\Cyber punk\tools\emsdk\python\3.13.3_64bit\python.exe'; OV = r'D:\2077\work\w32\w32-overlay.py'
P = r'D:\cp2077SourceCode\extracted\root\R6.Root\Mainline\dev\src\common\physics'
def copy(orig):
    subprocess.run([PY, OV, 'add', orig], capture_output=True, check=True)
    return Path(subprocess.run([PY, OV, 'resolve', orig], capture_output=True, text=True, check=True).stdout.strip())
def patch(orig, edits):
    p = copy(orig); raw = p.read_bytes().decode('utf-8', errors='surrogateescape'); crlf = '\r\n' in raw
    s = '\n'.join(l.rstrip() for l in raw.replace('\r\n', '\n').split('\n'))
    if 'CpWebNativeCollection' in s: print('already', p.name); return
    for a, b in edits:
        assert s.count(a) == 1, (p.name, a[:80], s.count(a)); s = s.replace(a, b)
    p.write_bytes((s.replace('\n', '\r\n') if crlf else s).encode('utf-8', errors='surrogateescape')); print('patched', p.name)

DECL = """    REDPHYSICS_API void DestroyActors(red::ArraySpan<physx::PxActor*> actors);
#endif"""  # after the NO_EDITOR block (NO_EDITOR is defined)
patch(P + r'\include\physXNatives.h', [(DECL, DECL + """
#if defined( __wasm32__ )
	// [w32] Converts a cooked (win64) binary collection to this build's layout. Returns true and fills 'converted'
	// (PX_SERIAL_FILE_ALIGN-aligned) when conversion happened; false when the data is already native or invalid.
	REDPHYSICS_API Bool CpWebNativeCollection( const void* data, Uint64 size, DataBuffer& converted );
#endif""")])

patch(P + r'\src\physXNatives.cpp', [("""#ifdef PHYSICS_DEBUG
#pragma optimize("", on)
#endif""", """#if defined( __wasm32__ )
#include <mutex>
#include <cstdio>
namespace physics
{
	namespace
	{
		alignas( 16 ) const unsigned char cpwebWin64MetaData[] = {
#embed "D:/Cyber punk/extracted/root/R6.Root/Mainline/engine/physics/win64.metaData"
		};
		physx::PxBinaryConverter* CpWebWin64Converter()
		{
			static physx::PxBinaryConverter* converter = []() -> physx::PxBinaryConverter*
			{
				physx::PxBinaryConverter* c = physx::PxSerialization::createBinaryConverter();
				if( !c ) return nullptr;
				c->setReportMode( physx::PxConverterReportMode::eNORMAL );
				physx::PxDefaultMemoryOutputStream native;
				physx::PxSerialization::dumpBinaryMetaData( native, GetPxSerializationRegistry() );
				physx::PxDefaultMemoryInputData src( const_cast< physx::PxU8* >( cpwebWin64MetaData ), sizeof( cpwebWin64MetaData ) );
				physx::PxDefaultMemoryInputData dst( native.getData(), native.getSize() );
				if( !c->setMetaData( src, dst ) )
				{
					std::fprintf( stderr, "CP2077 w32 PhysX: binary converter metadata rejected\\n" );
					c->release(); return nullptr;
				}
				std::fprintf( stderr, "CP2077 w32 PhysX: win64 -> wasm32 binary converter ready (dst metadata %u bytes)\\n", native.getSize() );
				return c;
			}();
			return converter;
		}
	}
	Bool CpWebNativeCollection( const void* data, Uint64 size, DataBuffer& converted )
	{
		if( !data || size < 24 ) return false;
		const physx::PxU32* header = static_cast< const physx::PxU32* >( data );
		if( header[ 0 ] != PX_MAKE_FOURCC( 'S', 'E', 'B', 'D' ) ) return false;
		if( header[ 4 ] == PX_MAKE_FOURCC( 'L', '_', '3', '2' ) ) return false; // already this build's layout
		static std::mutex lock;
		std::lock_guard< std::mutex > guard( lock );
		physx::PxBinaryConverter* c = CpWebWin64Converter();
		if( !c ) return false;
		physx::PxDefaultMemoryInputData src( static_cast< physx::PxU8* >( const_cast< void* >( data ) ), physx::PxU32( size ) );
		physx::PxDefaultMemoryOutputStream dst;
		if( !c->convert( src, physx::PxU32( size ), dst ) )
		{
			std::fprintf( stderr, "CP2077 w32 PhysX: binary collection conversion failed (%llu bytes)\\n", (unsigned long long)size );
			return false;
		}
		converted = DataBuffer::Copy( dst.getData(), dst.getSize(), PX_SERIAL_FILE_ALIGN, PoolPhysics() );
		return true;
	}
}
#endif

#ifdef PHYSICS_DEBUG
#pragma optimize("", on)
#endif""")])

patch(P + r'\src\physicsMaterialLibraryResource.cpp', [(
"""	RED_ASSERT( m_collection == nullptr );
	m_collection = physx::PxSerialization::createCollectionFromBinary( m_collectionData.Data(), GetPxSerializationRegistry() );""",
"""	RED_ASSERT( m_collection == nullptr );
#if defined( __wasm32__ )
	{ DataBuffer converted; if( physics::CpWebNativeCollection( m_collectionData.Data(), m_collectionData.Size(), converted ) ) m_collectionData = std::move( converted ); } // [w32]
#endif
	m_collection = physx::PxSerialization::createCollectionFromBinary( m_collectionData.Data(), GetPxSerializationRegistry() );""")])

patch(P + r'\src\physXProxySimpleCollider.cpp', [(
"""			m_compiledData = std::move( desc.m_compiledData );
""", """			m_compiledData = std::move( desc.m_compiledData );
#if defined( __wasm32__ )
			{ DataBuffer converted; if( physics::CpWebNativeCollection( m_compiledData.Data(), m_compiledData.Size(), converted ) ) m_compiledData = std::move( converted ); } // [w32]
#endif
""")])

patch(P + r'\src\physicsCollection.cpp', [(
"""		physx::PxCollection* m_collection = nullptr;""",
"""		physx::PxCollection* m_collection = nullptr;
#if defined( __wasm32__ )
		DataBuffer cpwebConverted; // [w32] win64 -> wasm32 converted bytes, deserialized in place by m_collection
#endif"""), (
"""			auto* data = m_buffer.Data().GetSpan().Data();
			pimpl->m_collection = physx::PxSerialization::createCollectionFromBinary(data, GetPxSerializationRegistry(), GetMaterialCollection());""",
"""			auto* data = m_buffer.Data().GetSpan().Data();
#if defined( __wasm32__ )
			if( physics::CpWebNativeCollection( data, m_buffer.Data().GetSpan().Size(), pimpl->cpwebConverted ) ) data = static_cast< decltype( data ) >( pimpl->cpwebConverted.Data() ); // [w32]
#endif
			pimpl->m_collection = physx::PxSerialization::createCollectionFromBinary(data, GetPxSerializationRegistry(), GetMaterialCollection());""")])
