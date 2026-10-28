#!/usr/bin/env python3
"""[w32-meshlod] mem=ios: static multi-LOD meshes drop their LOD0-only chunks at upload (LOD1 chunks also serve LOD0),
and the GPU vertex/index buffer is repacked with only the kept chunks (~40 % of mesh GPU memory). Skinned (bones),
topology (garment/skin compute) and custom-data meshes are untouched. ?meshLod0=1 keeps full meshes; ?meshLod0=0
forces the drop on any profile."""
import subprocess
from pathlib import Path
PY = r'D:\Cyber punk\tools\emsdk\python\3.13.3_64bit\python.exe'; OV = r'D:\2077\work\w32\w32-overlay.py'
orig = r'D:\cp2077SourceCode\extracted\root\R6.Root\Mainline\dev\src\common\renderer\src\renderMesh.cxx'
subprocess.run([PY, OV, 'add', orig], capture_output=True, check=True)
p = Path(subprocess.run([PY, OV, 'resolve', orig], capture_output=True, text=True, check=True).stdout.strip()); assert 'build-w32' in str(p), p
raw = p.read_bytes().decode('utf-8', errors='surrogateescape'); crlf = '\r\n' in raw; s = raw.replace('\r\n', '\n')
if '[w32-meshlod]' in s: raise SystemExit('already')
a = '''	// Upload buffers
	renderMesh->SetupRenderData( header, meshBlob.m_renderBuffer.Data(), vertexBufferSize, indexBufferSize, indexBufferOffset, info.m_resourcePath.ToStringView() );
'''
b = '''	// Upload buffers
#ifdef __EMSCRIPTEN__
	// [w32-meshlod] iPhone memory: drop LOD0-only chunks of static multi-LOD meshes and repack the device buffer
	std::vector< Uint8 > cpPacked;
	{
		static const int cpDrop = EM_ASM_INT( { try { const q = new URLSearchParams( location.search ); const v = q.get( 'meshLod0' ); if ( v === '1' ) return 0; if ( v === '0' ) return 1; return q.get( 'mem' ) === 'ios' ? 1 : 0; } catch ( e ) { return 0; } } );
		auto& chunks = renderMesh->m_chunksData;
		Bool eligible = cpDrop && header.m_renderLODs.Size() >= 2 && header.m_bonePositions.Empty() && std::all_of( header.m_topology.Begin(), header.m_topology.End(), []( const rend::TopologyData& t ) { return t.m_data.Empty() && t.m_metadata.Empty(); } ) && header.m_customData.Empty() && meshBlob.m_renderBuffer.Data() != nullptr;
		Bool anyLod1 = false, anyLod0Only = false;
		for ( const auto& c : chunks ) { if ( c.m_lodMask & 2 ) anyLod1 = true; if ( c.m_lodMask == 1 ) anyLod0Only = true; }
		if ( eligible && anyLod1 && anyLod0Only )
		{
			red::DynArray< rend::Chunk > work = chunks;
			const Uint8* src = static_cast< const Uint8* >( meshBlob.m_renderBuffer.Data() );
			cpPacked.reserve( totalDataSize );
			auto align = [ & ]( size_t al ) { while ( cpPacked.size() % al ) cpPacked.push_back( 0 ); };
			Bool ok = true;
			for ( auto& c : work )
			{
				if ( c.m_lodMask == 1 ) continue;
				const GpuWrapApi::VertexLayoutDesc* ld = GpuApi::GetVertexLayoutDesc( GpuApi::GetVertexLayout( c.m_chunkVertices.m_vertexLayout ) );
				if ( !ld ) { ok = false; break; }
				for ( Uint32 st = 0; st < RENDER_MESH_NUM_STREAMS && ok; ++st )
				{
					const Uint32 stride = ld->GetSlotStride( st );
					if ( !stride ) continue;
					const Uint32 off = c.m_chunkVertices.m_byteOffsets[ st ], n = Uint32( c.m_numVertices ) * stride;
					if ( Uint64( off ) + n > vertexBufferSize ) { ok = false; break; }
					align( 16 ); c.m_chunkVertices.m_byteOffsets[ st ] = Uint32( cpPacked.size() );
					cpPacked.insert( cpPacked.end(), src + off, src + off + n );
				}
				if ( !ok ) break;
			}
			align( 16 );
			const Uint32 newIndexOffset = Uint32( cpPacked.size() );
			for ( auto& c : work )
			{
				if ( !ok ) break;
				if ( c.m_lodMask == 1 ) { c.m_lodMask = 0; c.m_numIndices = 0; c.m_numVertices = 0; c.m_chunkIndices.byteOffset = 0; continue; }
				const Uint32 isz = c.m_chunkIndices.type == GpuWrapApi::IBCT_IndexUInt ? 4 : 2;
				const Uint32 off = indexBufferOffset + c.m_chunkIndices.byteOffset, n = c.m_numIndices * isz;
				if ( Uint64( c.m_chunkIndices.byteOffset ) + n > indexBufferSize ) { ok = false; break; }
				align( 4 ); c.m_chunkIndices.byteOffset = Uint32( cpPacked.size() ) - newIndexOffset;
				cpPacked.insert( cpPacked.end(), src + off, src + off + n );
				if ( c.m_lodMask & 2 ) c.m_lodMask |= 1;
			}
			if ( ok && cpPacked.size() < totalDataSize )
			{
				align( 4 );
				chunks = std::move( work );
				for ( Uint32 i = 0; i < chunks.Size(); ++i ) renderMesh->m_chunksBatchDataIds[ i ].numIndices = chunks[ i ].m_numIndices;
				static std::atomic< Uint64 > cpSaved{ 0 }; static std::atomic< Uint32 > cpMeshes{ 0 };
				const Uint64 saved = cpSaved.fetch_add( totalDataSize - cpPacked.size() ) + ( totalDataSize - cpPacked.size() );
				const Uint32 nm = cpMeshes.fetch_add( 1 ) + 1;
				if ( ( nm & ( nm - 1 ) ) == 0 ) std::fprintf( stderr, "CP2077 w32 meshlod: meshes=%u savedMiB=%.1f
", nm, saved / 1048576.0 );
				renderMesh->SetupRenderData( header, cpPacked.data(), newIndexOffset, Uint32( cpPacked.size() ) - newIndexOffset, newIndexOffset, info.m_resourcePath.ToStringView() );
			}
			else cpPacked.clear();
		}
	}
	if ( cpPacked.empty() )
#endif
	renderMesh->SetupRenderData( header, meshBlob.m_renderBuffer.Data(), vertexBufferSize, indexBufferSize, indexBufferOffset, info.m_resourcePath.ToStringView() );
'''
assert s.count(a) == 1, s.count(a); s = s.replace(a, b)
s = '#ifdef __EMSCRIPTEN__\n#include <emscripten.h>\n#include <vector>\n#include <atomic>\n#include <cstdio>\n#endif\n' + s
p.write_bytes((s.replace('\n', '\r\n') if crlf else s).encode('utf-8', errors='surrogateescape')); print('patched', p)
