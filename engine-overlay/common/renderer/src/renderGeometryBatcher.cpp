/**
* Copyright (c) 2018 CD Projekt Red. All Rights Reserved.
*/

#include "build.h"
#include "renderGeometryBatcher.h"
#include "renderMesh.h"
#include "renderParticleData.h"
#include "renderParticleEmitter.h"
#include "renderDecalMesh.h"
#include "renderSkinManager.h"
#include "renderSkinningData.h"
#include "renderDebugMesh.h"
#include "renderMaterialModifiers.h"
#include "renderScene.h"
#include "fontCache.h"
#include "../../renderData/include/meshEnum.h"
#include "../../renderData/include/renderDrawBuffer.h"
#include "../../../external/vergesort/vergesort.h"

// #TODO: The dynamic scope strings caused problems with the profilers, because if a material gets unloaded or
// something, then we'll try to access a deleted string. If we want these back, we need to dump the names to some
// global string table, so they can be kept alive and never freed.
//#define USE_DYNAMIC_PC_SCOPES
#if defined( USE_DYNAMIC_PC_SCOPES )
#include "../../redCore/include/profilerManager.h"
#endif

static const Uint32 c_decalDataBufferSize = 8192;
static const Uint32 c_dynamicInstanceBufferSize = 1 << 17; // 128K

// TEMP! TODO: Remove once occlusion works correctly while GI baking  
#ifndef RED_CONFIGURATION_FINAL
#include "../../redCore/include/commandline.h"
static const Uint32 c_cmdletDecalDataBufferSize = 8192 * 16;
static const Uint32 c_cmdletDynamicInstanceBufferSize = 1 << 20;
#endif // RED_CONFIGURATION_FINAL
#include "mapped-write-track-web.h" // [skperf] round 23 mapTrack (port src/)
// [skperf] round 33 reSplit: a split GBuffer command list k of n renders only the k-th contiguous slice of the
// sorted chunk array (renderRenderFrame.cpp builds n lists with the same targets, submitted in order k = 0..n-1, so
// the overall draw order is unchanged). The slice is armed by the RenderElements condition of that list and only
// applies while recording into the SAME command list (id check), so nothing else on the thread is affected.
namespace { struct CpReSplitState { Int32 k = 0, n = 1; Uint32 listId = 0; }; thread_local CpReSplitState tl_cpReSplit; }
void CpReSplitArm( Int32 k, Int32 n, Uint32 listId ) { tl_cpReSplit.k = k; tl_cpReSplit.n = n; tl_cpReSplit.listId = listId; }
// [skperf] round 37 geometry probe (?perfprobe=1, probeMask bit 2; heap-free): for every RenderGeometry call with
// >= 300 chunks: extract, filter/split/sort, slice, the draw loop, and inside the loop the port's draw recording
// (time + count + its stage split) vs the engine-side remainder (material/constant setup, batching). Every ~2 s one
// line with per-call averages for these big lists, plus the single slowest call.
#include <cstdio>
#include <atomic>
#include <mutex>
#include <emscripten.h>
namespace cp2077 { namespace web { extern std::atomic<int> skperfEncodeProbe; } }
extern "C" void cp2077_skperf_tl_draw_take( double* out );
extern "C" double cp2077_skperf_tl_port_peek(); // [skperf] round 57
namespace cpGeoProbe
{
	thread_local double tl_sortMs = 0; thread_local Uint32 tl_sorted = 0, tl_sliced = 0;
	// [skperf] round 57 engine split (probeMask bit 2): per segment inclusive ms and the port+api ms inside it.
	// 0 psoBind 1 geoBind 2 matMod 3 flushBatch 4 addInstances 5 drawSingle 6 instWrite (nested in 3/4/5)
	enum { SegCount = 7 };
	thread_local Bool tl_segOn = false; thread_local double tl_seg[ SegCount ] = {}, tl_segPort[ SegCount ] = {}, tl_segN[ SegCount ] = {};
	struct Seg
	{
		int i; double t0 = 0, p0 = 0;
		explicit Seg( int k ) : i( k ) { if ( tl_segOn ) { p0 = cp2077_skperf_tl_port_peek(); t0 = emscripten_get_now(); } }
		~Seg() { if ( tl_segOn && t0 > 0 ) { const double t1 = emscripten_get_now(); tl_seg[ i ] += t1 - t0; tl_segPort[ i ] += cp2077_skperf_tl_port_peek() - p0; tl_segN[ i ] += 1; } }
	};
	double s_seg[ SegCount ] = {}, s_segPort[ SegCount ] = {}, s_segN[ SegCount ] = {}, s_segDraws = 0;
	std::mutex s_lock;
	double s_calls = 0, s_chunks = 0, s_sliced = 0, s_draws = 0, s_extract = 0, s_sort = 0, s_loop = 0, s_port = 0, s_stage[ 6 ] = {}, s_windowStart = 0;
	double s_maxTotal = 0, s_maxExtract = 0, s_maxSort = 0, s_maxLoop = 0, s_maxPort = 0, s_maxDraws = 0, s_maxChunks = 0;
	double s_apiMs[ 8 ] = {}, s_apiCalls[ 8 ] = {}; // [skperf] round 39
	inline Bool On() { return ( cp2077::web::skperfEncodeProbe.load( std::memory_order_relaxed ) & 2 ) != 0; }
	// [skperf] round 39: a call is 'concurrent' if another RenderGeometry was active at its entry or entered during it.
	std::atomic< Uint32 > g_active{ 0 }, g_entries{ 0 };
	double s_b[ 2 ][ 8 ] = {}; // [alone/concurrent]: calls, draws, loop, port, resources, flush, bindGroup, api
	void Note( double extract, double sort, double loop, const double* port, Uint32 chunks, Uint32 sliced, Bool conc )
	{
		std::lock_guard< std::mutex > guard( s_lock );
		for ( int i = 0; i < SegCount; ++i ) { s_seg[ i ] += tl_seg[ i ]; s_segPort[ i ] += tl_segPort[ i ]; s_segN[ i ] += tl_segN[ i ]; } s_segDraws += port[ 1 ]; // [skperf] round 57
		{
			double* b = s_b[ conc ? 1 : 0 ]; double api = 0; for ( int i = 0; i < 7; ++i ) api += port[ 8 + i ];
			b[ 0 ] += 1; b[ 1 ] += port[ 1 ]; b[ 2 ] += loop; b[ 3 ] += port[ 0 ]; b[ 4 ] += port[ 4 ]; b[ 5 ] += port[ 6 ]; b[ 6 ] += port[ 5 ]; b[ 7 ] += api;
		}
		s_calls += 1; s_chunks += chunks; s_sliced += sliced; s_draws += port[ 1 ]; s_extract += extract; s_sort += sort; s_loop += loop; s_port += port[ 0 ];
		for ( int i = 0; i < 6; ++i ) s_stage[ i ] += port[ 2 + i ];
		for ( int i = 0; i < 8; ++i ) { s_apiMs[ i ] += port[ 8 + i ]; s_apiCalls[ i ] += port[ 16 + i ]; }
		const double total = extract + sort + loop;
		if ( total > s_maxTotal ) { s_maxTotal = total; s_maxExtract = extract; s_maxSort = sort; s_maxLoop = loop; s_maxPort = port[ 0 ]; s_maxDraws = port[ 1 ]; s_maxChunks = chunks; }
		const double now = emscripten_get_now();
		if ( s_windowStart == 0 ) { s_windowStart = now; return; }
		if ( now - s_windowStart < 2000 ) return;
		const double c = s_calls ? s_calls : 1;
		std::fprintf( stderr, "CP2077 skperf geometry probe: bigCalls=%.0f per call: chunks=%.0f afterSlice=%.0f draws=%.0f extractMs=%.2f filterSplitSortMs=%.2f loopMs=%.2f portDrawMs=%.2f engineSideMs=%.2f portUsPerDraw=%.1f (port stages ms: state=%.2f pipeline=%.2f resources=%.2f bindGroup=%.2f flush=%.2f encode=%.2f) | slowest: totalMs=%.2f extract=%.2f sort=%.2f loop=%.2f port=%.2f draws=%.0f chunks=%.0f\n",
			s_calls, s_chunks / c, s_sliced / c, s_draws / c, s_extract / c, s_sort / c, s_loop / c, s_port / c, ( s_loop - s_port ) / c, s_draws ? s_port * 1000.0 / s_draws : 0.0,
			s_stage[ 0 ] / c, s_stage[ 1 ] / c, s_stage[ 2 ] / c, s_stage[ 3 ] / c, s_stage[ 4 ] / c, s_stage[ 5 ] / c,
			s_maxTotal, s_maxExtract, s_maxSort, s_maxLoop, s_maxPort, s_maxDraws, s_maxChunks );
		std::fprintf( stderr, "CP2077 skperf geometry api probe: per big call (ms / calls): lockUnlockBuffer=%.2f/%.0f bindCB=%.2f/%.0f vbIb=%.2f/%.0f vscPsc=%.2f/%.0f shader=%.2f/%.0f textures=%.2f/%.0f samplers=%.2f/%.0f\n",
			s_apiMs[ 0 ] / c, s_apiCalls[ 0 ] / c, s_apiMs[ 1 ] / c, s_apiCalls[ 1 ] / c, s_apiMs[ 2 ] / c, s_apiCalls[ 2 ] / c, s_apiMs[ 3 ] / c, s_apiCalls[ 3 ] / c,
			s_apiMs[ 4 ] / c, s_apiCalls[ 4 ] / c, s_apiMs[ 5 ] / c, s_apiCalls[ 5 ] / c, s_apiMs[ 6 ] / c, s_apiCalls[ 6 ] / c );
		for ( int i = 0; i < 8; ++i ) { s_apiMs[ i ] = 0; s_apiCalls[ i ] = 0; }
		for ( int k = 0; k < 2; ++k )
		{
			const double* b = s_b[ k ]; const double d = b[ 1 ] ? b[ 1 ] / 1000.0 : 1; // per-draw values in us
			std::fprintf( stderr, "CP2077 skperf geometry conc probe: %s calls=%.0f draws/call=%.0f usPerDraw: loop=%.1f port=%.1f (resources=%.1f flush=%.1f bindGroup=%.1f) api=%.1f engineOnly=%.1f\n",
				k ? "concurrent" : "alone", b[ 0 ], b[ 0 ] ? b[ 1 ] / b[ 0 ] : 0.0, b[ 2 ] / d, b[ 3 ] / d, b[ 4 ] / d, b[ 5 ] / d, b[ 6 ] / d, b[ 7 ] / d, ( b[ 2 ] - b[ 3 ] - b[ 7 ] ) / d );
		}
		{ // [skperf] round 57 engine split: per draw, engine-only us (inclusive minus port+api inside) / calls per draw
			const double d = s_segDraws ? s_segDraws / 1000.0 : 1; const double dn = s_segDraws ? s_segDraws : 1;
			double nowUs = 0; { const double a = emscripten_get_now(); double x = 0; for ( int i = 0; i < 200; ++i ) x += emscripten_get_now(); nowUs = ( emscripten_get_now() - a ) * 1000.0 / 201.0; if ( x < 0 ) nowUs = -1; }
			std::fprintf( stderr, "CP2077 skperf geometry engine probe: usPerDraw engineOnly(calls/draw): psoBind=%.1f(%.2f) geoBind=%.1f(%.2f) matMod=%.1f(%.2f) flushBatch=%.1f(%.2f) addInst=%.1f(%.2f) drawSingle=%.1f(%.2f) instWrite=%.1f(%.2f) | port+api inside: flush=%.1f single=%.1f matMod=%.1f pso=%.1f geo=%.1f | nowUs=%.2f\n",
				( s_seg[ 0 ] - s_segPort[ 0 ] ) / d, s_segN[ 0 ] / dn, ( s_seg[ 1 ] - s_segPort[ 1 ] ) / d, s_segN[ 1 ] / dn, ( s_seg[ 2 ] - s_segPort[ 2 ] ) / d, s_segN[ 2 ] / dn,
				( s_seg[ 3 ] - s_segPort[ 3 ] ) / d, s_segN[ 3 ] / dn, ( s_seg[ 4 ] - s_segPort[ 4 ] ) / d, s_segN[ 4 ] / dn, ( s_seg[ 5 ] - s_segPort[ 5 ] ) / d, s_segN[ 5 ] / dn,
				( s_seg[ 6 ] - s_segPort[ 6 ] ) / d, s_segN[ 6 ] / dn, s_segPort[ 3 ] / d, s_segPort[ 5 ] / d, s_segPort[ 2 ] / d, s_segPort[ 0 ] / d, s_segPort[ 1 ] / d, nowUs );
			for ( int i = 0; i < SegCount; ++i ) { s_seg[ i ] = 0; s_segPort[ i ] = 0; s_segN[ i ] = 0; } s_segDraws = 0;
		}
		for ( auto& row : s_b ) for ( auto& v : row ) v = 0;
		s_calls = s_chunks = s_sliced = s_draws = s_extract = s_sort = s_loop = s_port = 0; for ( auto& v : s_stage ) v = 0;
		s_maxTotal = s_maxExtract = s_maxSort = s_maxLoop = s_maxPort = s_maxDraws = s_maxChunks = 0; s_windowStart = now;
	}
}
#include <atomic>

// [skperf] round 23 mapTrack: the dynamic instance ring buffer is persistently mapped and was diffed in full (several
// MB, ~18 ms/frame in the bar) before every submit batch that referenced it. Both Write() overloads are the only
// writers of m_bufferPtr (private member), so they report the bytes they store; the port then diffs only those pages.
// The tracker lives in the buffer's registry entry (port); this table maps the ring object to it without changing
// the class layout (the header is shared with every renderer unit).
namespace
{
	struct CpTrackSlot { std::atomic< const void* > owner{ nullptr }; std::atomic< const cp2077::web::MappedWriteTracker* > tracker{ nullptr }; };
	CpTrackSlot s_cpTrack[ 8 ];
	const cp2077::web::MappedWriteTracker* CpTrackerFor( const void* owner )
	{
		for ( auto& slot : s_cpTrack ) if ( slot.owner.load( std::memory_order_acquire ) == owner ) return slot.tracker.load( std::memory_order_acquire );
		return nullptr;
	}
	void CpTrackerSet( const void* owner, const cp2077::web::MappedWriteTracker* tracker )
	{
		for ( auto& slot : s_cpTrack ) if ( slot.owner.load( std::memory_order_acquire ) == owner ) { slot.tracker.store( tracker, std::memory_order_release ); if ( !tracker ) slot.owner.store( nullptr, std::memory_order_release ); return; }
		if ( !tracker ) return;
		for ( auto& slot : s_cpTrack )
		{
			const void* expected = nullptr;
			if ( slot.owner.compare_exchange_strong( expected, owner ) ) { slot.tracker.store( tracker, std::memory_order_release ); return; }
		}
	}
}
static const Uint32 c_dynamicSkinnedInstanceBufferSize = 32 * 1024;
static const Uint32 c_maxStaticInstances = 128 * 1024; // 6MB

static_assert( ( 1 << BATCHMASK_DATA_INSTANCE_OFFSET_NUMBITS ) >= c_maxStaticInstances, "Static instance buffer offset won't fit in render batch data structure" );

static_assert( CInstanceDataAllocator::MatrixNumElements * sizeof( Float ) == sizeof( CRenderGeometryBatcher::MeshInstanceDescriptor ),
	"Instance descriptor probably got some extra data inside, so we need to use per instance custom copy instead of simple memcpy" );

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - 

CRenderGeometryBatcher::DynamicInstanceBuffer::DynamicInstanceBuffer()
{
	
}

CRenderGeometryBatcher::DynamicInstanceBuffer::~DynamicInstanceBuffer()
{
	CpTrackerSet( this, nullptr ); // [skperf] mapTrack
	GpuApi::SafeRelease( m_buffer );
}

void CRenderGeometryBatcher::DynamicInstanceBuffer::Init( Uint32 sizeInElements, Uint32 elementSize, GpuWrapApi::eBufferChunkCategory bufferCategory )
{
	RED_FATAL_ASSERT( ( elementSize > 0 ) && ( elementSize % 16 == 0 ), "Element size must be multiple of 16" );
	RED_FATAL_ASSERT( red::IsPowerOf2( sizeInElements ), "Buffer capacity must be power of two" );
	m_capacity = sizeInElements;
	m_elementSize = elementSize;

	GpuApi::BufferDesc desc;
	desc.size = sizeInElements * elementSize;
	desc.structureStride = elementSize;
	desc.category = bufferCategory;
	desc.usage = GpuApi::BUT_Mapped;
	desc.group = GpuWrapApi::BufferGroup::Instances;

	m_buffer = GpuApi::CreateBuffer( desc );
	RED_ASSERT( m_buffer, "Failed to create instance buffer" );

	m_bufferPtr = GpuApi::GetMappedBufferPtr( m_buffer );
	CpTrackerSet( this, m_buffer ? cp2077::web::EnableMappedGpuWriteTracking( m_buffer ) : nullptr ); // [skperf] mapTrack (before any Write)
	RED_FATAL_ASSERT( ( (size_t)m_bufferPtr % 16 ) == 0, "Mapped buffer pointer is required to be 16-byte aligned" );
}

Uint32 CRenderGeometryBatcher::DynamicInstanceBuffer::Allocate( Uint32 numElements )
{
	Uint64 original, newHead;
	Uint32 offset;
	do
	{
		original = m_head.GetValue();
		newHead = original;
		offset = static_cast<Uint32>( original & ( m_capacity - 1 ) );
		if ( offset + numElements > m_capacity )
		{
			// no space at the end of the buffer - jump to the beginning
			offset = 0;
			newHead = ( original + m_capacity - 1 ) & (Uint64)( -(Int64)m_capacity ); // round up
		}

#ifndef RED_CONFIGURATION_FINAL
		if ( newHead + numElements - m_tail > m_capacity )
		{
			// ring buffer overflow
			m_overflowState.SetValue( true );
			return UINT32_MAX;
		}
#endif // RED_CONFIGURATION_FINAL

	} while ( original != m_head.CompareExchange( newHead + numElements, original ) );

	return offset;
}

Uint32 CRenderGeometryBatcher::DynamicInstanceBuffer::Write( const void* data, Uint32 numElements, ThreadData& threadData )
{
	RED_ASSERT( (size_t)data % 16 == 0, "Instance data should be aligned to 16 bytes" );
	cpGeoProbe::Seg cpSeg( 6 ); // [skperf] round 57 seg

	Uint32 offset = UINT32_MAX;

	// TODO tweak this
	const Uint32 perThreadNumElements = 128;

	if ( threadData.rangeStart >= threadData.rangeEnd ) // per-thread cache is full
	{
		if ( numElements < perThreadNumElements )
		{
			// try to allocate per-thread block
			offset = Allocate( perThreadNumElements );
			if ( offset != UINT32_MAX )
			{
				threadData.rangeStart = offset;
				threadData.rangeEnd = offset + perThreadNumElements;
			}
		}
	}

	// try to use per-thread block first
	if ( threadData.rangeStart + numElements <= threadData.rangeEnd )
	{
		offset = threadData.rangeStart;
		threadData.rangeStart += numElements;
	}

	// finally, try allocate from the buffer
	if ( offset == UINT32_MAX )
	{
		offset = Allocate( numElements );
	}

	// copy data if allocation succeeded
	if ( offset != UINT32_MAX )
	{
		void* ptr = red::OffsetPtr( m_bufferPtr, offset * m_elementSize );
		const Uint32 numVectors = numElements * m_elementSize / 16u;
		for ( Uint32 i = 0; i < numVectors; ++i )
		{
			const __m128 v = _mm_load_ps( reinterpret_cast<const Float*>( data ) + 4u * i );
			_mm_store_ps( reinterpret_cast<Float*>( ptr ) + 4u * i, v );
		}
		cp2077::web::NoteMappedGpuWrite( CpTrackerFor( this ), std::size_t( offset ) * m_elementSize, std::size_t( numElements ) * m_elementSize ); // [skperf] mapTrack
	}

	return offset;
}

Uint32 CRenderGeometryBatcher::DynamicInstanceBuffer::Write( const void* data, Uint32 numElements )
{
	RED_ASSERT( (size_t)data % 16 == 0, "Instance data should be aligned to 16 bytes" );
	cpGeoProbe::Seg cpSeg( 6 ); // [skperf] round 57 seg

	Uint32 offset = Allocate( numElements );

	// copy data if allocation succeeded
	if ( offset != UINT32_MAX )
	{
		void* ptr = red::OffsetPtr( m_bufferPtr, offset * m_elementSize );
		const Uint32 numVectors = numElements * m_elementSize / 16u;
		for ( Uint32 i = 0; i < numVectors; ++i )
		{
			const __m128 v = _mm_load_ps( reinterpret_cast<const Float*>( data ) + 4u * i );
			_mm_store_ps( reinterpret_cast<Float*>( ptr ) + 4u * i, v );
		}
		cp2077::web::NoteMappedGpuWrite( CpTrackerFor( this ), std::size_t( offset ) * m_elementSize, std::size_t( numElements ) * m_elementSize ); // [skperf] mapTrack
	}

	return offset;
}

void CRenderGeometryBatcher::DynamicInstanceBuffer::OnEndFrame()
{
	RED_SCOPE_LOCK( m_lock );

	m_usageLastFrame = static_cast<Uint32>( m_head.GetValue() - m_tail );

	// update ring buffer tail offset
	while ( !m_pendingAllocations.Empty() )
	{
		const DynamicInstanceBuffer::Allocation& allocation = m_pendingAllocations.Front();
		if ( !GpuApi::IsGpuFenceComplete( allocation.fence ) )
		{
			break;
		}

		m_tail = allocation.offset;
		m_pendingAllocations.Pop();
	}

	m_pendingAllocations.Push( { GpuApi::GetGpuFence(), m_head.GetValue() } );
	m_wasOverflowLastFrame = m_overflowState.Exchange( false );
}

void CRenderGeometryBatcher::DynamicInstanceBuffer::GetStats( rend::DynamicInstanceBufferStats& outStats )
{
	RED_SCOPE_SHARED_LOCK( m_lock );
	outStats.m_capacity = m_capacity;
	outStats.m_usage = static_cast<Uint32>( m_usageLastFrame );
	outStats.m_wasOverflow = m_wasOverflowLastFrame;
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - 

void CRenderGeometryBatcher::BindBatchGeometry( const Int32 frameIndex, const Bool isUpdatedTangents, ThreadData& threadData, Uint8& objectStencilBit )
{
	RED_ASSERT( threadData.m_geoBatchData != nullptr, "Should have valid geometry batch data" );
	RED_ASSERT( threadData.m_proxyBatchData != nullptr, "Should have valid proxy batch data" );

	const SGeometryBatchData &geoBatchData = *threadData.m_geoBatchData;

	objectStencilBit = 0;

	switch ( geoBatchData.m_type )
	{
		case GT_Mesh:
		{
			RED_FATAL_ASSERT(geoBatchData.m_meshData.m_mesh, "Expected valid mesh pointer");

			const SProxyBatchData &proxyBatchData = *threadData.m_proxyBatchData;

			const Uint32 chunkIndex = geoBatchData.m_meshData.m_chunkIndex;

			MeshQuantizationParams qParams;
			if ( isUpdatedTangents && geoBatchData.m_meshData.m_hasTopologyDataForChunk )
			{
				qParams = TangentUpdateUtils::CalculateMeshScaleAndBias_FromProxy( proxyBatchData.GetProxy(), geoBatchData.m_meshData.m_mesh, chunkIndex );
			}
			else
			{
				qParams = TangentUpdateUtils::CalculateMeshScaleAndBias_Simple( geoBatchData.m_meshData.m_mesh, chunkIndex );
			}

			objectStencilBit = proxyBatchData.GetObjectTypeID();

			GpuComputeSkinningBuffers* gpuSkinningBuffers = isUpdatedTangents ? proxyBatchData.m_gpuComputeSkinningBuffers : nullptr;

			// don't bind mesh if index count is zero, because we won't draw anything anyway
			// this is to handle some corner case in the destruction
			if ( geoBatchData.m_meshData.m_indexCount > 0 )
			{
				geoBatchData.m_meshData.m_mesh->Bind(
					chunkIndex, geoBatchData.m_meshData.m_indexBuffer, geoBatchData.m_meshData.m_indexOffset,
					frameIndex, gpuSkinningBuffers, qParams );
			}
		}
		break;

		case GT_TrailEmitter:
		case GT_Emitter:
		{
			RED_FATAL_ASSERT(geoBatchData.IsEmitter(), "");
			RED_FATAL_ASSERT(geoBatchData.m_emitterData.m_particleData != nullptr, "Expected allocated particle data buffer.");

			const IParticleData& particleData = *geoBatchData.m_emitterData.m_particleData;
			RED_ASSERT( particleData.GetNumParticles() > 0 );

			geoBatchData.m_emitterData.m_particleEmitter->Bind( geoBatchData.m_emitterData.m_particleData );

			if ( !geoBatchData.m_emitterData.m_isDecal )
			{
				objectStencilBit = MapStencilRef( geoBatchData.m_emitterData.m_renderObjectType );
			}
			else
			{
				objectStencilBit = MapStencilRef( ROT_STATIC_LAST );
			}
		}
		break;
		
		case GT_Decal:
		{
			RED_ASSERT( geoBatchData.IsDecal() );

			objectStencilBit = geoBatchData.m_decalData.m_surfaceType;

			CRenderDecalMesh::BindBox();
			GpuApi::BindBufferSRV( m_decalDataBuffer.GetBuffer(), 45 ); // hardcoded index, must match decal.fx
		}
		break;

		case GT_Debug:
		case GT_Text:
		case GT_DrawBuffer:
		{
			// empty
		}
		break;

		default:
			RED_ASSERT( !"invalid" );
	}
}

void CRenderGeometryBatcher::BindStaticInstanceBuffer( ThreadData& threadData, GeometryBatcherStats& outStats ) const
{
	if ( threadData.m_lastInstanceBufferBound != InstanceBufferType::Static )
	{
#ifndef RED_CONFIGURATION_FINAL
		outStats.m_numStaticInstanceBufferBinding++;
#endif

		const Uint8 stride = sizeof( MeshInstanceDescriptor );
		const Uint32 offset = 0;
		GpuApi::BindVertexBuffers( 7, 1, &m_staticInstanceBuffer.m_buffer, &stride, &offset );

		threadData.m_lastInstanceBufferBound = InstanceBufferType::Static;
	}
}

void CRenderGeometryBatcher::BindDynamicInstanceBuffer( ThreadData& threadData, GeometryBatcherStats& outStats ) const
{
	if ( threadData.m_lastInstanceBufferBound != InstanceBufferType::Dynamic )
	{
#ifndef RED_CONFIGURATION_FINAL
		outStats.m_numDynamicInstanceBufferBinding++;
#endif

		const Uint8 stride = sizeof( MeshInstanceDescriptor );
		const Uint32 offset = 0;
		GpuApi::BindVertexBuffers( 7, 1, &m_dynamicInstanceBuffer.GetBuffer(), &stride, &offset );

		threadData.m_lastInstanceBufferBound = InstanceBufferType::Dynamic;
	}
}

void CRenderGeometryBatcher::BindDynamicSkinnedInstanceBuffer( ThreadData& threadData, GeometryBatcherStats& outStats ) const
{
	if ( threadData.m_lastInstanceBufferBound != InstanceBufferType::DynamicSkinned )
	{
#ifndef RED_CONFIGURATION_FINAL
		outStats.m_numDynamicInstanceBufferBinding++;
#endif

		const Uint8 stride = sizeof( SkinnedMeshInstanceDescriptor );
		const Uint32 offset = 0;
		GpuApi::BindVertexBuffers( 7, 1, &m_dynamicSkinnedInstanceBuffer.GetBuffer(), &stride, &offset );

		threadData.m_lastInstanceBufferBound = InstanceBufferType::DynamicSkinned;
	}
}

void CRenderGeometryBatcher::DrawMeshInstances( const SGeometryBatchData& geoBatchData, ThreadData& threadData, Uint32 instanceOffset, Bool isMaterialTwoSided, GeometryBatcherStats& outStats )
{
	const CRenderMesh *mesh = geoBatchData.m_meshData.m_mesh;
	const Uint32 chunkIndex = geoBatchData.m_meshData.m_chunkIndex;

	if ( !mesh->IsValid() )
	{
		return;
	}

	RED_ASSERT( chunkIndex < mesh->GetNumChunks(), "Mesh chunk index out of range. %d >= %d", chunkIndex, mesh->GetNumChunks() );
	
	const Uint32 numInstances = threadData.m_instances.Size();

	Uint32 numIndices = geoBatchData.m_meshData.m_indexCount;
	if ( ( ( geoBatchData.m_meshData.m_chunkFlags & MCF_IsTwoSided ) != 0 ) && isMaterialTwoSided )
	{
		numIndices /= 2;
	}

	if ( instanceOffset == UINT32_MAX )
	{
		instanceOffset = m_dynamicInstanceBuffer.Write( threadData.m_instances.Data(), numInstances, threadData.m_dynamicInstanceBufferData );
		if ( instanceOffset != UINT32_MAX )
		{
			BindDynamicInstanceBuffer( threadData, outStats );
			GpuApi::DrawIndexedPrimitive( PRIMTYPE_TriangleList, 0, 0, numIndices, numInstances, instanceOffset );
		}
	}
	else
	{
		BindStaticInstanceBuffer( threadData, outStats );
		GpuApi::DrawIndexedPrimitive( PRIMTYPE_TriangleList, 0, 0, numIndices, numInstances, instanceOffset );
	}

#ifndef RED_CONFIGURATION_FINAL
	// NOTE: don't access rend::Chunk in final, because it casues cache miss!
	const rend::Chunk& chunk = mesh->GetChunks()[ chunkIndex ];
	outStats.AppendDrawCall( chunk.m_vertexFactory, chunk.m_numVertices, numIndices, 1, numInstances );
#endif
}

void CRenderGeometryBatcher::DrawSkinnedMeshInstances( const SGeometryBatchData& geoBatchData, ThreadData& threadData, Bool isMaterialTwoSided, GeometryBatcherStats& outStats )
{
	const CRenderMesh *mesh = geoBatchData.m_meshData.m_mesh;
	const Uint32 chunkIndex = geoBatchData.m_meshData.m_chunkIndex;

	if ( !mesh->IsValid() )
	{
		return;
	}

	RED_ASSERT( chunkIndex < mesh->GetNumChunks(), "Mesh chunk index out of range. %d >= %d", chunkIndex, mesh->GetNumChunks() );

	const Uint32 numInstances = threadData.m_skinnedInstances.Size();

	Uint32 numIndices = geoBatchData.m_meshData.m_indexCount;
	if ( ( ( geoBatchData.m_meshData.m_chunkFlags & MCF_IsTwoSided ) != 0 ) && isMaterialTwoSided )
	{
		numIndices /= 2;
	}

	const Uint32 instanceOffset = m_dynamicSkinnedInstanceBuffer.Write( threadData.m_skinnedInstances.Data(), numInstances, threadData.m_dynamicSkinnedInstanceBufferData );
	if ( instanceOffset != UINT32_MAX )
	{
		BindDynamicSkinnedInstanceBuffer( threadData, outStats );
		GpuApi::DrawIndexedPrimitive( PRIMTYPE_TriangleList, 0, 0, numIndices, numInstances, instanceOffset );
	}

#ifndef RED_CONFIGURATION_FINAL
	// NOTE: don't access rend::Chunk in final, because it casues cache miss!
	const rend::Chunk& chunk = mesh->GetChunks()[ chunkIndex ];
	outStats.AppendDrawCall( chunk.m_vertexFactory, chunk.m_numVertices, chunk.m_numIndices, 1, numInstances );
#endif
}

void CRenderGeometryBatcher::DrawDecalInstances( ThreadData& threadData, GeometryBatcherStats& outStats )
{
	const Uint32 numInstances = threadData.m_instances.Size();
	RED_ASSERT( numInstances > 0 );
	RED_ASSERT( numInstances == threadData.m_decalsData.Size() );

	const Uint32 instanceOffset = m_dynamicInstanceBuffer.Write( threadData.m_instances.Data(), numInstances, threadData.m_dynamicInstanceBufferData );
	const Uint32 decalParamsBindOffset = m_decalDataBuffer.Write( threadData.m_decalsData.Data(), numInstances );

	if ( instanceOffset == UINT32_MAX || decalParamsBindOffset == UINT32_MAX )
	{
		return;
	}

	GpuApi::SetPSC( PSC_Custom_1, decalParamsBindOffset );

	BindDynamicInstanceBuffer( threadData, outStats );
	CRenderDecalMesh::RenderBoxInstanced( numInstances, instanceOffset );

#ifndef RED_CONFIGURATION_FINAL
	outStats.AppendDrawCall( MVF_MeshStatic, 8, 12, 1, numInstances );
#endif
}

void CRenderGeometryBatcher::DrawDebugInstances( const SGeometryBatchData& batchData, ThreadData& threadData, GeometryBatcherStats& outStats )
{
	const Uint32 numInstances = threadData.m_instances.Size();
	RED_ASSERT( numInstances > 0 );

	const Uint32 instanceOffset = m_dynamicInstanceBuffer.Write( threadData.m_instances.Data(), numInstances, threadData.m_dynamicInstanceBufferData );
	if ( instanceOffset == UINT32_MAX )
	{
		// buffer overflow
		return;
	}

	BindDynamicInstanceBuffer( threadData, outStats );

	// All casts below to DebugVertexUVInstanced happen because we need the Draw functions that automatically determine the vertex format
	// from the structure type to call the proper functions and have instanced vertex formats set
	// The reason for doing this is that the current material compilation doesn't allow materials that support instancing to render without instancing
	// thus without the instanced vertex format the input layout would mismatch the shader
	const ePrimitiveType primType = batchData.m_debug.m_primitiveType;
	const Uint32 numVertices = batchData.m_debug.m_numVertices;
	const Uint32 numIndices = batchData.m_debug.m_numIndices;
	if (batchData.m_debug.m_debugMesh)
	{
		batchData.m_debug.m_debugMesh->DrawInstanced( primType, numInstances, instanceOffset );
	}
	else if (nullptr != batchData.m_debug.m_indices && numVertices > 0)
	{
		if (batchData.m_debug.m_indexType == rend::IndexType::Uint16)
		{
			GpuApi::DrawSystemIndexedPrimitive( primType, numVertices, numIndices,
				static_cast<const DebugVertexUVInstanced*>(batchData.m_debug.m_vertices),
				static_cast<const Uint16*>(batchData.m_debug.m_indices),
				numInstances, instanceOffset );
		}
		else
		{
			RED_ASSERT(batchData.m_debug.m_indexType == rend::IndexType::Uint32);
			GpuApi::DrawSystemIndexedPrimitive( primType, numVertices, numIndices, 
				static_cast<const DebugVertexUVInstanced*>(batchData.m_debug.m_vertices),
				static_cast<const Uint32*>(batchData.m_debug.m_indices),
				numInstances, instanceOffset );
		}
	}
	else if (numVertices > 0)
	{
		GpuApi::DrawSystemPrimitive( primType, numVertices, static_cast<const DebugVertexUVInstanced*>(batchData.m_debug.m_vertices), numInstances, instanceOffset );
	}

	// not reporting draw stats for debug fragments
}

static void DrawText( const SGeometryBatchData& batchData, Uint32 numInstances, Uint32 instanceOffset )
{
	Uint32 numTriangles = batchData.m_text.m_numGlyphs * 2;
	Uint32 numIndices = batchData.m_text.m_numGlyphs * 6;

	RenderGlyphCache* glyphCache = GetRenderer()->GetGlyphCache();

	const auto fillVertexBufferFunc = [&batchData, glyphCache] ( DebugVertexUVInstanced* outBuffer, Uint32 bufferSize )
	{
		glyphCache->BuildTextVertexBuffer( batchData.m_text.m_glyphBuffer, batchData.m_text.m_numGlyphs, batchData.m_text.m_font, batchData.m_text.m_fontStyle, outBuffer, bufferSize );
	};

	if ( 4 * batchData.m_text.m_numGlyphs < UINT16_MAX )
	{
		const auto fillIndexBufferFunc = [&batchData, glyphCache] ( Uint16* outBuffer, Uint32 bufferSize )
		{
			RED_FATAL_ASSERT( bufferSize == batchData.m_text.m_numGlyphs * 6 * sizeof( Uint16 ) );

			Uint32 idx = 0;
			for ( Uint32 i = 0; i < batchData.m_text.m_numGlyphs; ++i )
			{
				outBuffer[ idx++ ] = 4 * i + 0;
				outBuffer[ idx++ ] = 4 * i + 1;
				outBuffer[ idx++ ] = 4 * i + 2;
				outBuffer[ idx++ ] = 4 * i + 0;
				outBuffer[ idx++ ] = 4 * i + 2;
				outBuffer[ idx++ ] = 4 * i + 3;
			}
		};

		GpuApi::DrawSystemIndexedPrimitive<DebugVertexUVInstanced>( PRIMTYPE_TriangleList, 4 * batchData.m_text.m_numGlyphs, 6 * batchData.m_text.m_numGlyphs, fillVertexBufferFunc, fillIndexBufferFunc, numInstances, instanceOffset );
	}
	else
	{
		const auto fillIndexBufferFunc = [&batchData, glyphCache] ( Uint32* outBuffer, Uint32 bufferSize )
		{
			RED_FATAL_ASSERT( bufferSize == batchData.m_text.m_numGlyphs * 6 * sizeof( Uint32 ) );

			Uint32 idx = 0;
			for ( Uint32 i = 0; i < batchData.m_text.m_numGlyphs; ++i )
			{
				outBuffer[ idx++ ] = 4 * i + 0;
				outBuffer[ idx++ ] = 4 * i + 1;
				outBuffer[ idx++ ] = 4 * i + 2;
				outBuffer[ idx++ ] = 4 * i + 0;
				outBuffer[ idx++ ] = 4 * i + 2;
				outBuffer[ idx++ ] = 4 * i + 3;
			}
		};

		GpuApi::DrawSystemIndexedPrimitive32<DebugVertexUVInstanced>( PRIMTYPE_TriangleList, 4 * batchData.m_text.m_numGlyphs, 6 * batchData.m_text.m_numGlyphs, fillVertexBufferFunc, fillIndexBufferFunc, numInstances, instanceOffset );
	}
}

void CRenderGeometryBatcher::DrawTextInstances( const SGeometryBatchData& batchData, ThreadData& threadData, GeometryBatcherStats& outStats )
{
	const Uint32 numInstances = threadData.m_instances.Size();
	RED_ASSERT( numInstances > 0 );

	const Uint32 instanceOffset = m_dynamicInstanceBuffer.Write( threadData.m_instances.Data(), numInstances, threadData.m_dynamicInstanceBufferData );
	if ( instanceOffset != UINT32_MAX )
	{
		BindDynamicInstanceBuffer( threadData, outStats );
		DrawText( batchData, numInstances, instanceOffset );
	}
}

void CRenderGeometryBatcher::DrawBatchInstances( const SRenderChunk &chunk, const RenderContext& ctx, ThreadData& threadData )
{
	const SGeometryBatchData& geoBatchData = *threadData.m_geoBatchData;

	// Draw skinned instances
	Bool isMaterialTwoSided = chunk.m_data.IsTwoSidedMaterial();
	if ( !threadData.m_skinnedInstances.Empty() )
	{
		DrawSkinnedMeshInstances( geoBatchData, threadData, isMaterialTwoSided, ctx.stats );
		threadData.m_skinnedInstances.Clear();
	}

	// Draw regular instances
	if ( !threadData.m_instances.Empty() )
	{
		Uint32 instanceBufferOffset = UINT32_MAX;
		if ( chunk.m_key.usesStaticInstanceBuffer )
		{
			RED_FATAL_ASSERT( geoBatchData.m_type == GT_Mesh, "Only meshes can use static instance buffer" );
			instanceBufferOffset = chunk.m_data.instanceOffset;
		}

		switch ( geoBatchData.m_type )
		{
		case GT_Mesh:		DrawMeshInstances( geoBatchData, threadData, instanceBufferOffset, isMaterialTwoSided, ctx.stats ); break;
		case GT_Decal:		DrawDecalInstances( threadData, ctx.stats ); threadData.m_decalsData.Clear(); break;
		case GT_Debug:		DrawDebugInstances( geoBatchData, threadData, ctx.stats ); break;
		case GT_Text:		DrawTextInstances( geoBatchData, threadData, ctx.stats ); break;

		case GT_TrailEmitter:	// #TODO: Should these assert?
		case GT_Emitter:	/*empty*/ break;

		case GT_DrawBuffer:	RED_FATAL( "Draw buffer is not instanced" ); break;
		default:			RED_FATAL( "Invalid batch type" );
		}
		threadData.m_instances.Clear();
	}
}

static void AddChunkInstancesToBatch( const Float* instanceMatrixData, const SRenderChunk &chunk, GeometryBatcherStats& outStats, const CRenderGeometryBatcher::RenderContext& ctx, CRenderGeometryBatcher::ThreadData& threadData )
{
	RED_ASSERT( chunk.m_data.IsInstanced() );

	const Uint32 numInstances = chunk.m_data.GetInstanceCount();

	if ( chunk.m_data.IsSkinned() )
	{
		// GT_Decal is never skinned. If we had a decal in here, then we wouldn't be filling in the decal instance buffer.
		RED_ASSERT( threadData.m_geoBatchData->m_type == GT_Mesh, "Skinned objects should be meshes." );
		RED_ASSERT( !chunk.m_key.usesStaticInstanceBuffer, "Skinned objects shouldn't be using a static instance buffer" );

		const SProxyBatchData& proxyBatchData = *threadData.m_proxyBatchData;

		SkinningBindData skinningData;
		if ( const CRenderSkinningData* skinData = proxyBatchData.m_skinningData )
		{
			skinningData = skinData->GetBindData();
		}
		else
		{
			skinningData = RenderSkinningManager::GetDefaultBindData();
		}

		red::DynArray< CRenderGeometryBatcher::SkinnedMeshInstanceDescriptor >& skinnedInstances = threadData.m_skinnedInstances;
		const Uint32 baseIndex = skinnedInstances.Size();
		skinnedInstances.Grow( numInstances );

		for ( Uint32 i = 0; i < numInstances; ++i )
		{
			CRenderGeometryBatcher::SkinnedMeshInstanceDescriptor& desc = skinnedInstances[ baseIndex + i ];
			const Uint32 offsetF32 = i * CInstanceDataAllocator::MatrixNumElements;
			CInstanceDataAllocator::CopyInstanceData( desc.m_localToWorld, instanceMatrixData + offsetF32 );
			desc.m_skinningData = skinningData;
		}
	}
	else
	{
		red::DynArray< CRenderGeometryBatcher::MeshInstanceDescriptor >& instances = threadData.m_instances;
		const Uint32 baseIndex = instances.Size();
		instances.Grow( numInstances );

		// Don't need to copy instance data if we have a static instance buffer. But we do need to grow instances array,
		// since we use that to determine how many to draw... but it shouldn't have much cost, since constructor should
		// be no-op.
		if ( !chunk.m_key.usesStaticInstanceBuffer )
		{
			for ( Uint32 i = 0; i < numInstances; ++i )
			{
				CRenderGeometryBatcher::MeshInstanceDescriptor& desc = instances[baseIndex + i];
				const Uint32 offsetF32 = i * CInstanceDataAllocator::MatrixNumElements;
				CInstanceDataAllocator::CopyInstanceData( desc.m_localToWorld, instanceMatrixData + offsetF32 );
			}
		}

		const SGeometryBatchData& geoBatchData = *threadData.m_geoBatchData;
		if ( geoBatchData.m_type == GT_Decal )
		{
			RED_ASSERT( !chunk.m_key.usesStaticInstanceBuffer, "Decals shouldn't be using a static instance buffer" );

			const CRenderProxy_Decal* decalProxy = static_cast<const CRenderProxy_Decal*>( threadData.m_proxyBatchData->GetProxy() );
			RED_ASSERT( decalProxy );

			if ( decalProxy )
			{
				const Float fade = chunk.m_data.customData / 255.0f;

				fx::DecalParams decalData;
				decalData.tangent = decalProxy->GetTangent();
				decalData.bitangent = decalProxy->GetBitangent();
				decalData.up = decalProxy->GetUp().AsVector3();
				decalData.projectionMatrix = decalProxy->GetCachedProjectionTransposed();
				decalData.subUVClip = decalProxy->GetSubUVClip();
				decalData.frameOverride = (Float)decalProxy->GetFrameOverride();
				decalData.depthFade = decalProxy->GetZDepthFade();
				decalData.alpha = fade * decalProxy->GetAlpha();
				decalData.scaleInverse = decalProxy->GetScaleInverse();
				decalData.paramsPacked = decalProxy->GetParamsPacked();
				decalData.diffuseColorScale = decalProxy->GetDiffuseColorScale();
				decalData.emissiveScale = decalProxy->GetEmissiveScale();
				decalData.normalBlendingMode = (Uint32)decalProxy->GetNormalsBlendingMode();

				red::DynArray< fx::DecalParams >& decalDataBuffer = threadData.m_decalsData;
				const Uint32 baseDecalIndex = decalDataBuffer.Size();
				decalDataBuffer.Grow( numInstances );

				for ( Uint32 i = 0; i < numInstances; ++i )
				{
					decalDataBuffer[ baseDecalIndex + i ] = decalData;
				}
			}
		}
	}
}

static void DrawSingle( const Float* instanceMatrixData, const SRenderChunk &chunk, GeometryBatcherStats& outStats, const CRenderGeometryBatcher::RenderContext& ctx, CRenderGeometryBatcher::ThreadData& threadData )
{
	const Uint32 numInstances = chunk.m_data.GetInstanceCount();

	// Get skinning data
	const Bool isSkinned = chunk.m_data.IsSkinned();
	SkinningBindData skinningData = GetRenderer()->GetSkinningManager()->GetDefaultBindData();
	if ( isSkinned )
	{
		const SProxyBatchData &proxyBatchData = *threadData.m_proxyBatchData;
		const CRenderSkinningData* skinData = proxyBatchData.m_skinningData;

		// Set the skinning data via the vertex shader constants
		if ( skinData )
		{
			skinningData = skinData->GetBindData();
		}
	}

	const SGeometryBatchData& geoBatchData = *threadData.m_geoBatchData;

	RED_ASSERT( !chunk.m_data.IsInstanced() );

	RenderBindManager& stateManager = GetRenderer()->GetStateManager();

	switch ( geoBatchData.m_type )
	{
	case GT_Mesh:
		{
			if ( geoBatchData.m_meshData.m_mesh->IsValid() )
			{
				RED_ASSERT( geoBatchData.m_meshData.m_chunkIndex < geoBatchData.m_meshData.m_mesh->GetNumChunks(), "Mesh chunk index out of range. %d >= %d", geoBatchData.m_meshData.m_chunkIndex, geoBatchData.m_meshData.m_mesh->GetNumChunks() );

				GpuApi::SetVSC( VSC_SkinningData, skinningData );

				const rend::Chunk& chunkData = geoBatchData.m_meshData.m_mesh->GetChunks()[geoBatchData.m_meshData.m_chunkIndex];
				const Uint32 numIndices = (((chunkData.m_renderMask & MCF_IsTwoSided) != 0) && chunk.m_data.IsTwoSidedMaterial()) ? (chunkData.m_numIndices / 2) : chunkData.m_numIndices;
				for ( Uint32 i = 0; i < numInstances; ++i )
				{
					Matrix matrix;
					matrix.SetRowMajor4x3( instanceMatrixData + i * CInstanceDataAllocator::MatrixNumElements );

					GpuApi::SetVSC<false>( VSC_LocalToWorld, matrix );

					if ( geoBatchData.m_meshData.m_numInstancesOverride != UINT32_MAX )
					{
						GpuApi::DrawIndexedPrimitive( PRIMTYPE_TriangleList, 0, 0, numIndices, geoBatchData.m_meshData.m_numInstancesOverride );
					}
					else
					{
						GpuApi::DrawIndexedPrimitive( PRIMTYPE_TriangleList, 0, 0, numIndices );
					}
				}
			}
		} break;

	case GT_TrailEmitter:
		RED_ASSERT( geoBatchData.m_emitterData.m_particleData->GetNumParticles() > 0 );
		{
			RED_FATAL_ASSERT( numInstances == 1 );

			Matrix matrix = Matrix::IDENTITY_CONSTANT;

			if ( geoBatchData.m_emitterData.m_particleEmitter->IsSimulationLocal() )
			{
				matrix.SetRowMajor4x3( instanceMatrixData );
			}

			GpuApi::SetVSC<false>( VSC_LocalToWorld, matrix );

			const IParticleData& particleData = *geoBatchData.m_emitterData.m_particleData;
			GpuApi::DrawPrimitive( PRIMTYPE_TriangleStrip, 0, particleData.m_numVertices );

#ifdef USE_PROFILER
			const Bool notOverdrawPass = ctx.renderStageId != GetRenderer()->GetRenderStageRegistry().RegisterStageId( "renderstage_particle_overdraw", 0 ) &&
				ctx.renderStageId != GetRenderer()->GetRenderStageRegistry().RegisterStageId( "renderstage_count_lights", 0 );
			if ( notOverdrawPass )
			{
				outStats.m_numParticleEmittersRendered++;
				outStats.m_numParticlesRendered += geoBatchData.m_emitterData.m_particleData->GetNumParticles();
			}
#endif
		}
		break;
	case GT_Emitter:
		{
			RED_ASSERT( geoBatchData.m_emitterData.m_particleData->GetNumParticles() > 0 );
			{
				RED_FATAL_ASSERT( numInstances == 1 );

				Matrix matrix = Matrix::IDENTITY_CONSTANT;

				if ( geoBatchData.m_emitterData.m_particleEmitter->IsSimulationLocal() )
				{
					matrix.SetRowMajor4x3( instanceMatrixData );
				}

				GpuApi::SetVSC<false>( VSC_LocalToWorld, matrix );

				const IParticleData& particleData = *geoBatchData.m_emitterData.m_particleData;
				// 4 vertices per particle, forming quad with 2 triangles (6 indices).
				const Uint32 numIndices = (particleData.m_numVertices / 4) * 6;
				GpuApi::DrawIndexedPrimitive( PRIMTYPE_TriangleList, 0, 0, numIndices );

#ifdef USE_PROFILER
				const Bool notOverdrawPass = ctx.renderStageId != GetRenderer()->GetRenderStageRegistry().RegisterStageId( "renderstage_particle_overdraw", 0 ) &&
					ctx.renderStageId != GetRenderer()->GetRenderStageRegistry().RegisterStageId( "renderstage_count_lights", 0 );
				if ( notOverdrawPass )
				{
					outStats.m_numParticleEmittersRendered++;
					outStats.m_numParticlesRendered += geoBatchData.m_emitterData.m_particleData->GetNumParticles();
				}
#endif
			}
		} break;

	case GT_Decal: RED_HALT( "This codepath is obsolete, all decals are rendered with instancing" );
	case GT_Debug: RED_HALT( "This codepath is obsolete, all debug fragments are rendered with instancing" );

	case GT_DrawBuffer:
		{
			RED_ASSERT( numInstances == 1, "Expecting a single instance for DrawBuffer chunks" );

			// Any additional parameters will be set up by material or material modifier.
			Matrix matrix;
			matrix.SetRowMajor4x3( instanceMatrixData );
			GpuApi::SetVSC<false>( VSC_LocalToWorld, matrix );

			const rend::DrawBufferCommand* command = geoBatchData.m_drawBuffer.command;
			const rend::DrawBuffer* buffer = geoBatchData.m_drawBuffer.buffer;

			const void* vertices = buffer->GetVerticesPtr( command->vertexOffset );
			RED_ASSERT( vertices, "Must provide a vertex buffer" );
			const void* indices = command->drawType == rend::DrawType::DrawIndexed ? buffer->GetIndicesPtr( command->indexOffset ) : nullptr;

			const GpuApi::VertexLayoutRef vertexLayout = GpuApi::GetVertexLayout( DebugVertexUV::GetVertexLayoutDesc() );
			const ePrimitiveType primitiveType = MapPrimitiveType( command->primitiveType );

			if ( indices != nullptr )
			{
				GpuApi::DrawSystemIndexedPrimitive( primitiveType, command->vertexCount, command->GetNumElements(), vertexLayout, vertices, static_cast<const Uint16*>(indices), 1 );
			}
			else
			{
				GpuApi::DrawSystemPrimitive( primitiveType, command->GetNumElements(), vertexLayout, vertices, 1 );
			}
		} break;

	case GT_Text:
		{
			// Any additional parameters will be set up by material or material modifier.
			Matrix matrix;
			matrix.SetRowMajor4x3( instanceMatrixData );
			GpuApi::SetVSC<false>( VSC_LocalToWorld, matrix );

			RED_ASSERT( numInstances == 1, "Expecting a single instance for Text chunks" );
			DrawText( geoBatchData, /*numInstances*/ 1, /*instanceOffset*/ 0 );
		} break;

	default:
		RED_HALT( "Unhandled geometry batch type %d", geoBatchData.m_type );
	}
}


Vector4 BuildDiscardFlags( const CRenderDissolveSynchronizer &sc, EDissolveMode dissolveMode, Bool isMasked )
{
	const Vector4 dissolveValue = DissolveHelpers::ComputeDissolveValue( sc, dissolveMode );
	return Vector4 (
		isMasked ? 1.0f : 0.0f,
		0.0f,
		dissolveValue.X,
		dissolveValue.Y
		);
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - 

CRenderGeometryBatcher::CRenderGeometryBatcher()
	: m_threadDataPool( job::GetNumDispatcherThreads() + 1, rend::PoolBatchers() )
{
// TEMP{LI}: Just to let cmdlet bake GI properly
#ifndef RED_CONFIGURATION_FINAL
	Bool isCmdLet = red::CommandLine::Get().HasOption( "enableRender" ) ? true : false;
	m_dynamicInstanceBuffer.Init( isCmdLet ? c_cmdletDynamicInstanceBufferSize : c_dynamicInstanceBufferSize, sizeof( MeshInstanceDescriptor ), GpuWrapApi::BCC_Vertex );
	m_decalDataBuffer.Init( isCmdLet ? c_cmdletDecalDataBufferSize : c_decalDataBufferSize, sizeof( fx::DecalParams ), GpuWrapApi::BCC_Structured );
#else
	m_dynamicInstanceBuffer.Init( c_dynamicInstanceBufferSize, sizeof( MeshInstanceDescriptor ), GpuWrapApi::BCC_Vertex );
	m_decalDataBuffer.Init( c_decalDataBufferSize, sizeof( fx::DecalParams ), GpuWrapApi::BCC_Structured );
#endif // RED_CONFIGURATION_FINAL

	m_dynamicSkinnedInstanceBuffer.Init( c_dynamicSkinnedInstanceBufferSize, sizeof( SkinnedMeshInstanceDescriptor ), GpuWrapApi::BCC_Vertex );

	{
		GpuApi::BufferDesc desc;
		desc.size = c_maxStaticInstances * sizeof( MeshInstanceDescriptor );
		desc.category = GpuWrapApi::BCC_Vertex;
		desc.usage = GpuApi::BUT_Default;
		desc.group = GpuWrapApi::BufferGroup::Instances;
		m_staticInstanceBuffer.m_buffer = GpuApi::CreateBuffer( desc );
		RED_ASSERT( m_staticInstanceBuffer.m_buffer, "Failed to create instance buffer" );
		m_staticInstanceBuffer.m_allocator.Initialize( c_maxStaticInstances, 1 );
	}
}

CRenderGeometryBatcher::~CRenderGeometryBatcher()
{
	GpuApi::SafeRelease( m_staticInstanceBuffer.m_buffer );
}

Uint32 CRenderGeometryBatcher::AllocateStaticInstanceBuffer( const red::ArraySpan<const rend::RenderProxyTransform> instancesData )
{
	PC_SCOPE( AllocateStaticInstanceBuffer );

	const Uint32 numInstances = instancesData.Size();

	Uint32 offset;
	{
		RED_SCOPE_LOCK( m_staticInstanceBuffer.m_lock );
		offset = m_staticInstanceBuffer.m_allocator.Allocate( numInstances, m_staticInstanceBuffer.m_allocator.MakeAllocateRequest( numInstances ) );
	}

	if ( ( offset + numInstances ) > c_maxStaticInstances )
	{
		// allocation failed
		return UINT32_MAX;
	}

	PushStaticInstanceBufferUpload( instancesData, offset );

	return offset;
}

void CRenderGeometryBatcher::PushStaticInstanceBufferUpload( const red::ArraySpan<const rend::RenderProxyTransform> instancesData, const Uint32 staticInstanceBufferOffset )
{
	const Uint32 numInstances = instancesData.Size();

	PendingStaticInstanceBufferUpload pendingUpload;
	pendingUpload.instanceOffset = staticInstanceBufferOffset;
	pendingUpload.bufferData.Resize( numInstances * 12 );

	for ( Uint32 i = 0; i < numInstances; ++i )
	{
		static_assert( sizeof(IRenderProxyInstance) == 12*sizeof(Float), "Expect IRenderProxyInstance to just have a 3x4 transform matrix" );
		IRenderProxyInstance* instance = reinterpret_cast<IRenderProxyInstance*>(pendingUpload.bufferData.TypedData() + 12 * i);
		instance->SetInstanceMatrix( instancesData[i] );
	}

	InternalPushStaticInstanceBufferUpload( pendingUpload );
}

void CRenderGeometryBatcher::PushStaticInstanceBufferUpload( const red::ArraySpan<const IRenderProxyInstance> instancesData, const Uint32 staticInstanceBufferOffset )
{
	const Uint32 numInstances = instancesData.Size();

	PendingStaticInstanceBufferUpload pendingUpload;
	pendingUpload.instanceOffset = staticInstanceBufferOffset;
	pendingUpload.bufferData.Resize( numInstances * 12 );

	//This is safe as long as we have that static assert which makes sure that size of IRenderProxyInstance equals size of its fields
	memcpy( pendingUpload.bufferData.TypedData(), instancesData.Data(), numInstances * sizeof( IRenderProxyInstance ) );

	InternalPushStaticInstanceBufferUpload( pendingUpload );
}

void CRenderGeometryBatcher::InternalPushStaticInstanceBufferUpload( const PendingStaticInstanceBufferUpload& pendingUpload )
{
	RED_SCOPE_LOCK( m_pendingStaticInstanceBufferUploadsLock );
	m_pendingStaticInstanceBufferUploads.PushBack( std::move( pendingUpload ) );
}

void CRenderGeometryBatcher::FreeStaticInstance( Uint32 instanceOffset )
{
	RED_SCOPE_LOCK( m_staticInstanceBuffer.m_lock );
	m_staticInstanceBuffer.m_allocator.Free( instanceOffset );
}

void CRenderGeometryBatcher::GetInstanceBufferStats( rend::InstanceBufferStats& outStats )
{
#ifndef RED_CONFIGURATION_FINAL
	m_dynamicInstanceBuffer.GetStats( outStats.m_dynamicBufferStats );
	m_dynamicSkinnedInstanceBuffer.GetStats( outStats.m_dynamicSkinnedBufferStats );
	m_decalDataBuffer.GetStats( outStats.m_decalParamsBufferStats );

	{
		RED_SCOPE_SHARED_LOCK( m_staticInstanceBuffer.m_lock );
		outStats.m_staticBufferCapacity = c_maxStaticInstances;
		outStats.m_staticBufferAllocationCount = m_staticInstanceBuffer.m_allocator.GetAllocationCount();
		outStats.m_staticBufferFreeSize = m_staticInstanceBuffer.m_allocator.GetSumFreeSize();
		outStats.m_staticBufferWasted = m_staticInstanceBuffer.m_allocator.GetSumWasted();
	}
#endif // RED_CONFIGURATION_FINAL
}

void CRenderGeometryBatcher::FlushPendingStaticInstanceBufferUploads()
{
	PC_SCOPE( FlushPendingStaticInstanceBufferUploads );

	red::DynArray< PendingStaticInstanceBufferUpload > pendingUploads{ rend::PoolBatchers() };
	{
		RED_SCOPE_LOCK( m_pendingStaticInstanceBufferUploadsLock );
		pendingUploads = std::move( m_pendingStaticInstanceBufferUploads );
	}

	for ( const PendingStaticInstanceBufferUpload& upload : pendingUploads )
	{
		GpuApi::UpdateBuffer( m_staticInstanceBuffer.m_buffer, upload.instanceOffset * 12 * sizeof( Float ), upload.bufferData.DataSize(), upload.bufferData.TypedData() );
	}
}

void CRenderGeometryBatcher::OnFrameTick()
{
	PC_SCOPE( CRenderGeometryBatcher/OnFrameTick );

	FlushPendingStaticInstanceBufferUploads();

	m_dynamicInstanceBuffer.OnEndFrame();
	m_dynamicSkinnedInstanceBuffer.OnEndFrame();
	m_decalDataBuffer.OnEndFrame();

	// invalidate all per-thread data
	for ( ThreadData& threadData : m_threadDataPool )
	{
		threadData.m_dynamicInstanceBufferData.Clear();
		threadData.m_dynamicSkinnedInstanceBufferData.Clear();
	}
}

////////////////////////////////////////////////////////////////////////////////////////////////////////


void CRenderGeometryBatcher::RenderGeometry(const RenderContext& ctx, const RenderStageCollectorBucket& chunksBucket )
{
	PC_SCOPE( RenderGeometry );

	red::DynArray< SRenderChunk > sortEntries{ job::PoolJobScope() };
	const Bool cpProbe = cpGeoProbe::On(); // [skperf] round 37
	const double cpT0 = cpProbe ? emscripten_get_now() : 0;

	// fill sortEntries (continuous buffer with renderChunks)
	{
		PC_SCOPE( FillSortEntries );
		chunksBucket.ExtractChunks( sortEntries );
	}
	const double cpT1 = cpProbe ? emscripten_get_now() : 0;
	const Uint32 cpChunks = sortEntries.Size();
	double cpPortBefore[ 24 ];
	Uint32 cpEntry = 0; Bool cpConc = false;
	if ( cpProbe ) { for ( int i = 0; i < cpGeoProbe::SegCount; ++i ) { cpGeoProbe::tl_seg[ i ] = 0; cpGeoProbe::tl_segPort[ i ] = 0; cpGeoProbe::tl_segN[ i ] = 0; } cpGeoProbe::tl_segOn = true; } // [skperf] round 57
	if ( cpProbe ) { cp2077_skperf_tl_draw_take( cpPortBefore ); cpGeoProbe::tl_sortMs = 0; cpGeoProbe::tl_sliced = cpChunks;
		cpConc = cpGeoProbe::g_active.fetch_add( 1 ) > 0; cpEntry = cpGeoProbe::g_entries.fetch_add( 1 ); }

	// Render stuff
	ExecuteRenderGeometry( ctx, sortEntries, ctx.dispatcherThreadIndex );
	cpGeoProbe::tl_segOn = false; // [skperf] round 57
	if ( cpProbe ) { cpGeoProbe::g_active.fetch_sub( 1 ); if ( cpGeoProbe::g_entries.load() != cpEntry + 1 ) cpConc = true; }
	if ( cpProbe && cpChunks >= 300 )
	{
		const double cpT2 = emscripten_get_now();
		double cpPort[ 24 ]; cp2077_skperf_tl_draw_take( cpPort );
		cpGeoProbe::Note( cpT1 - cpT0, cpGeoProbe::tl_sortMs, ( cpT2 - cpT1 ) - cpGeoProbe::tl_sortMs, cpPort, cpChunks, cpGeoProbe::tl_sliced, cpConc );
	}
}


////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////


RED_FORCE_INLINE RenderBatchKey GetBatchKeyDiff( RenderBatchKey previous, RenderBatchKey next )
{
	// Difference between last and current sort key. Don't include priority, since that was just used for sorting.
	RenderBatchKey diff = (previous ^ next);
	diff.SetPriority( 0 );

	// If the next one is per-chunk geometry, force geometry to show as different.
	diff.geometry |= next.isGeometryPerChunk;

	return diff;
}


void CRenderGeometryBatcher::SortChunks( red::DynArray< SRenderChunk >& sortEntries, EChunkSortingMode mode )
{
	if ( SM_OptimizedBatching == mode ) // sort by key (excluding order), then by order
	{
		PC_SCOPE( SortChunks_OptimizedBatching );

		struct
		{
			RED_FORCE_INLINE Bool operator()( const SRenderChunk& a, const SRenderChunk& b ) const
			{
				RenderBatchKey nonPriorityKeyA = a.m_key;
				RenderBatchKey nonPriorityKeyB = b.m_key;

				nonPriorityKeyA.SetPriority( 0 );
				nonPriorityKeyB.SetPriority( 0 );

				if ( nonPriorityKeyA != nonPriorityKeyB )
				{
					return nonPriorityKeyA > nonPriorityKeyB;
				}

				return a.m_key.GetPriority() < b.m_key.GetPriority();
			}
		} cmp;

		std::sort( sortEntries.Begin(), sortEntries.End(), cmp );
	}
	else if ( SM_OptimizedDistanceBatching == mode ) // sort materials and meshes by distance, then sort chunks by key
	{
		PC_SCOPE( SortChunks_OptimizedDistanceBatching );

		using PriorityType = Uint16;
		static_assert( BATCHMASK_PRIORITY_NUMBITS <= 8 * sizeof( PriorityType ), "Priority bits won't fit in Uint16" );

		const Uint32 maxNumMaterials = 1 << BATCHMASK_PSO_INDEX_NUMBITS;
		const Uint32 maxNumMaterialParams = 1 << BATCHMASK_PARAMS_NUMBITS;
		const Uint32 maxNumGeometry = 1 << BATCHMASK_GEOMETRY_NUMBITS;

		red::FixedArray<PriorityType, maxNumMaterials> materialDistances;
		red::FixedArray<PriorityType, maxNumMaterialParams> materialParamDistances;
		red::FixedArray<PriorityType, maxNumGeometry> geometryDistances;
		red::Memset( materialDistances.Data(), UINT32_MAX, sizeof( materialDistances ) );
		red::Memset( materialParamDistances.Data(), UINT32_MAX, sizeof( materialParamDistances ) );
		red::Memset( geometryDistances.Data(), UINT32_MAX, sizeof( geometryDistances ) );

		Uint32 numMaterialsInUse = 0;
		Uint32 numMaterialParamsInUse = 0;
		Uint32 numGeometryInUse = 0;

		// compute minimum distance to each material, parameter and mesh
		{
			PC_SCOPE( ComputeDistances );

			for ( Uint32 i = 0; i < sortEntries.Size(); ++i )
			{
				const SRenderChunk& chunk = sortEntries[ i ];
				const RenderBatchKey& key = chunk.m_key;

				materialDistances[ key.psoIndex ] = Min<PriorityType>( key.priority, materialDistances[ key.psoIndex ] );
				materialParamDistances[ key.params ] = Min<PriorityType>( key.priority, materialParamDistances[ key.params ] );
				geometryDistances[ key.geometry ] = Min<PriorityType>( key.priority, geometryDistances[ key.geometry ] );

				numMaterialsInUse = Max<Uint32>( numMaterialsInUse, (Uint32)key.psoIndex + 1u );
				numMaterialParamsInUse = Max<Uint32>( numMaterialParamsInUse, (Uint32)key.params + 1u );
				numGeometryInUse = Max<Uint32>( numGeometryInUse, (Uint32)key.geometry + 1u );
			}
		}

		// reorder materials by distance
		red::FixedArray<Uint16, maxNumMaterials> reorderedMaterials;
		red::FixedArray<Uint16, maxNumMaterials> reorderedMaterialsRev;
		{
			PC_SCOPE( ReorderMaterial );
			std::iota( reorderedMaterials.Begin(), reorderedMaterials.Begin() + numMaterialsInUse, 0 );
			std::sort( reorderedMaterials.Begin(), reorderedMaterials.Begin() + numMaterialsInUse, [&] ( const Uint16 a, const Uint16 b )
			{
				return materialDistances[ a ] < materialDistances[ b ];
			} );
			for ( Uint32 i = 0; i < numMaterialsInUse; ++i )
			{
				reorderedMaterialsRev[ reorderedMaterials[ i ] ] = i;
			}
		}

		// reorder material parameters by distance
		red::FixedArray<Uint16, maxNumMaterialParams> reorderedMaterialParams;
		red::FixedArray<Uint16, maxNumMaterialParams> reorderedMaterialParamsRev;
		{
			PC_SCOPE( ReorderMaterialParams );
			std::iota( reorderedMaterialParams.Begin(), reorderedMaterialParams.Begin() + numMaterialParamsInUse, 0 );
			std::sort( reorderedMaterialParams.Begin(), reorderedMaterialParams.Begin() + numMaterialParamsInUse, [&] ( const Uint16 a, const Uint16 b )
			{
				return materialParamDistances[ a ] < materialParamDistances[ b ];
			} );
			for ( Uint32 i = 0; i < numMaterialParamsInUse; ++i )
			{
				reorderedMaterialParamsRev[ reorderedMaterialParams[ i ] ] = i;
			}
		}

		// reorder meshes by distance
		red::FixedArray<Uint16, maxNumGeometry> reorderedGeometry;
		red::FixedArray<Uint16, maxNumGeometry> reorderedGeometryRev;
		{
			PC_SCOPE( ReorderMeshes );
			std::iota( reorderedGeometry.Begin(), reorderedGeometry.Begin() + numGeometryInUse, 0 );
			std::sort( reorderedGeometry.Begin(), reorderedGeometry.Begin() + numGeometryInUse, [&] ( const Uint16 a, const Uint16 b )
			{
				return geometryDistances[ a ] < geometryDistances[ b ];
			} );
			for ( Uint32 i = 0; i < numGeometryInUse; ++i )
			{
				reorderedGeometryRev[ reorderedGeometry[ i ] ] = i;
			}
		}

		// assign temporary indices, sorted by distance
		{
			PC_SCOPE( ApplyReordering );
			for ( Uint32 i = 0; i < sortEntries.Size(); ++i )
			{
				RenderBatchKey& key = sortEntries[ i ].m_key;
				const Uint32 newPsoIndex = reorderedMaterialsRev[ key.psoIndex ];
				const Uint32 newPramsIndex = reorderedMaterialParamsRev[ key.params ];
				const Uint32 newGeometryIndex = reorderedGeometryRev[ key.geometry ];
				key.SetPSOIndex( newPsoIndex );
				key.SetParams( newPramsIndex );
				key.SetGeometry( newGeometryIndex );
			}
		}

		{
			PC_SCOPE( TheSort );
			vergesort::vergesort( sortEntries.Begin(), sortEntries.End(), [&] ( const SRenderChunk& a, const SRenderChunk& b )
			{
				return a.m_key < b.m_key;
			} );
		}

		// go back to original distances
		{
			PC_SCOPE( RevertReordering );
			for ( Uint32 i = 0; i < sortEntries.Size(); ++i )
			{
				RenderBatchKey& key = sortEntries[ i ].m_key;
				const Uint32 newPsoIndex = reorderedMaterials[ key.psoIndex ];
				const Uint32 newPramsIndex = reorderedMaterialParams[ key.params ];
				const Uint32 newGeometryIndex = reorderedGeometry[ key.geometry ];
				key.SetPSOIndex( newPsoIndex );
				key.SetParams( newPramsIndex );
				key.SetGeometry( newGeometryIndex );
			}
		}
	}
	else if ( SM_ByPriority == mode ) // sort by whole key
	{
		PC_SCOPE( SortChunks_ByPriority );

		struct
		{
			RED_FORCE_INLINE Bool operator()( const SRenderChunk& a, const SRenderChunk& b ) const
			{
				if ( a.m_key.GetPriority() != b.m_key.GetPriority() )
				{
					return a.m_key.GetPriority() > b.m_key.GetPriority();
				}

				RenderBatchKey nonPriorityKeyA = a.m_key;
				RenderBatchKey nonPriorityKeyB = b.m_key;

				nonPriorityKeyA.SetPriority( 0 );
				nonPriorityKeyB.SetPriority( 0 );

				return nonPriorityKeyA < nonPriorityKeyB;
			}
		} cmp;

		std::sort( sortEntries.Begin(), sortEntries.End(), cmp );
	}
	else if ( SM_KeepOrder == mode )
	{
		// noting to do
	}
	else
	{
		RED_FATAL( "Invalid sorting mode %d", mode );
	}
}


RED_INLINE Bool ShouldSkipRenderChunk( const SRenderChunk& chunk )
{
	// Handle ID overflow
#if ENABLE_BATCHDATA_ALLOCATOR_OVERFLOW_CHECKS
	if ( !chunk.m_key.GetGeometry() || !chunk.m_key.GetParams() || !chunk.m_key.GetPSOIndex() )
	{
		return true;
	}
#endif

	const SGeometryBatchData& geoBatchData = GetRenderer()->GetBatchDataAllocator().GetGeometryData().GetData( chunk.m_key.GetGeometry() );

#if ENABLE_BATCHDATA_ALLOCATOR_OVERFLOW_CHECKS
	switch ( geoBatchData.m_type )
	{
	case GT_TrailEmitter:
	case GT_Emitter:
	case GT_Mesh:
	case GT_Decal:
		{
			// Handle proxy ID overflow
			if ( !chunk.m_data.GetProxy() )
			{
				return true;
			}
		}
		break;
	}
#endif // ENABLE_BATCHDATA_ALLOCATOR_OVERFLOW_CHECKS

	switch ( geoBatchData.m_type )
	{
	case GT_TrailEmitter:
	case GT_Emitter:
		{
			RED_FATAL_ASSERT( geoBatchData.IsEmitter(), "" );
			RED_FATAL_ASSERT( geoBatchData.m_emitterData.m_particleData != nullptr, "Expected allocated particle data buffer." );

			const IParticleData& particleData = *geoBatchData.m_emitterData.m_particleData;
			if ( !particleData.GetNumParticles() )
			{
				return true;
			}
		}
		break;

	case GT_Mesh:
	case GT_Decal:
	case GT_Debug:
	case GT_Text:
	case GT_DrawBuffer:
		break;

	default:
		RED_HALT( "invalid geometry batch type %d", geoBatchData.m_type );
	}

	return false;
}

static void FilterNullDraws( red::DynArray< SRenderChunk >& sortEntries, EChunkSortingMode mode )
{
	PC_SCOPE( FilterNullDraws );

	// Remove anything that we aren't able to draw. This way we don't have to check for anything during the main loop.

	// #TODO: If particles were smart enough to just not collect anything if there are no particles (not sure if that's
	// possible, since simulation happens after collection), then we could skip this entirely unless there was a batch
	// data overflow detected (which shouldn't ever happen in the final game).

	if ( mode == SM_KeepOrder )
	{
		auto newEnd = std::remove_if( sortEntries.Begin(), sortEntries.End(), ShouldSkipRenderChunk );
		if ( newEnd != sortEntries.End() )
		{
			sortEntries.Remove( newEnd, sortEntries.End() );
		}
	}
	else
	{
		for ( Uint32 i = 0; i < sortEntries.Size(); ++i )
		{
			const SRenderChunk& chunk = sortEntries[i];
			if ( ShouldSkipRenderChunk( chunk ) )
			{
				sortEntries.RemoveAtReorder( i );
				--i;
			}
		}
	}
}


static void SplitInstances( red::DynArray< SRenderChunk >& sortEntries )
{
	PC_SCOPE( SplitInstances );

	for ( Uint32 i = 0, origNum = sortEntries.Size(); i < origNum; ++i )
	{
		if ( !sortEntries[i].m_data.AllowInstanceSplit() )
		{
			continue;
		}

		const Uint32 numInstances = sortEntries[i].m_data.GetInstanceCount();
		if ( numInstances <= 1 )
		{
			continue;
		}

		// modify num instances in original chunk
		sortEntries[i].m_data.SetInstanceCount( 1 );
		const Uint32 origInstanceOffset = sortEntries[i].m_data.GetInstanceOffset();

		// add new instances
		const Uint32 newChunksOffset = sortEntries.Size();
		sortEntries.Grow( numInstances - 1 );
		for ( Uint32 j = 0; j < numInstances - 1; ++j )
		{
			SRenderChunk& newChunk = sortEntries[newChunksOffset + j];
			newChunk = sortEntries[i];
			newChunk.m_data.SetInstanceOffset( origInstanceOffset + 1 + j );
		}
	}
}


void CRenderGeometryBatcher::ExecuteRenderGeometry( const RenderContext& ctx, red::DynArray< SRenderChunk >& sortEntries, Uint32 batcherThreadIndex )
{
	//
	// Prepare chunk list. Remove unnecessary entries, split instance batches, sort.
	//

	const double cpSortStart = cpGeoProbe::On() ? emscripten_get_now() : 0; // [skperf] round 37
	FilterNullDraws( sortEntries, ctx.sortingMode );
	if ( sortEntries.Empty() )
	{
		return;
	}
	
	// split chunks (needed in order for distanceSorting per instance)
	if ( ctx.allowInstancesSplit )
	{
		RED_FATAL_ASSERT( SM_KeepOrder != ctx.sortingMode, "TODO: make sure new instances adapt to given order - right now they are being added to the back" );
		SplitInstances( sortEntries );
	}

	SortChunks( sortEntries, ctx.sortingMode );
	if ( cpSortStart > 0 ) cpGeoProbe::tl_sortMs = emscripten_get_now() - cpSortStart; // [skperf] round 37

	// [skperf] round 33 reSplit: keep only this list's slice of the sorted chunks
	if ( tl_cpReSplit.n > 1 && ctx.rctx && ctx.rctx->HasCommandList() && ctx.rctx->GetCommandList().ID() == tl_cpReSplit.listId )
	{
		const Uint32 total = sortEntries.Size(), n = Uint32( tl_cpReSplit.n ), k = Uint32( tl_cpReSplit.k );
		const Uint32 begin = Uint32( ( Uint64( total ) * k ) / n ), end = Uint32( ( Uint64( total ) * ( k + 1 ) ) / n );
		if ( begin >= end )
		{
			return;
		}
		if ( begin > 0 )
		{
			for ( Uint32 i = begin; i < end; ++i ) sortEntries[ i - begin ] = sortEntries[ i ];
		}
		sortEntries.Resize( end - begin );
		cpGeoProbe::tl_sliced = end - begin; // [skperf] round 37
	}

	RED_FATAL_ASSERT( !sortEntries.Empty() );

	//
	// Set up initial state for batching.
	//

	// prepare thread-local data
	ThreadData& threadData = m_threadDataPool[batcherThreadIndex];
	threadData.m_lastInstanceBufferBound = InstanceBufferType::None;
	threadData.m_paramsBatchData = nullptr;
	RED_ASSERT( threadData.m_instances.Empty(), "Instance data array is expected to be empty at this point" );
	RED_ASSERT( threadData.m_skinnedInstances.Empty(), "Instance data array is expected to be empty at this point" );
	RED_ASSERT( threadData.m_decalsData.Empty(), "Instance data array is expected to be empty at this point" );

	const CBatchDataAllocator& batchDataAllocator = GetRenderer()->GetBatchDataAllocator();

	GetRenderer()->GetSkinningManager()->BindSkinningBuffer();
	GpuApi::SetVSC<false>( VSC_LocalToWorld, Matrix::IDENTITY_CONSTANT );

	const CRenderSceneEx* scene = ( ctx.rctx != nullptr ? ctx.rctx->GetScene() : nullptr );

	CRenderDissolveSynchronizer sc;
	if ( scene != nullptr )
	{
		sc = scene->GetDissolveSynchronizer();
	}

#ifdef RENDER_MATERIAL_ENABLE_DEBUG_NAME
	GpuApi::BeginProfilerBlock("Batcher preamble");
	#ifdef USE_DYNAMIC_PC_SCOPES
		static red::InstrumentationObject batchInstrumentationScope( "Batch" );
		if ( !batchInstrumentationScope.m_registered ) gProfilers.RegisterObject( &batchInstrumentationScope );
		const char* currentProfilerScope = "BatcherPreamble";
		gProfilers.StartBlock( &batchInstrumentationScope, currentProfilerScope );
	#endif // USE_DYNAMIC_PC_SCOPES
#endif // RENDER_MATERIAL_ENABLE_DEBUG_NAME

	const Bool isUpdatedTangents = ( ctx.rctx && ctx.rctx->Test( RFF_RuntimeTangentUpdate ) );
	const Int32 frameIndex = ( scene ? scene->GetLastAllocatedFrame() : 0 );

	MaterialModifierConstants mmConstants { GetRenderer()->GetStateManager().GetMaterialModifiersConstantBuffer() };

	SRenderChunk lastChunk;
	{
		lastChunk.m_key.Clear();
		lastChunk.m_data.customData = 0;
		lastChunk.m_key.SetDissolveMode( DISSOLVEMODE_Opaque );
		const Vector4 discardFlags = BuildDiscardFlags( sc, DISSOLVEMODE_Opaque, false );
		mmConstants.SetValue( MTL_DiscardFlags, discardFlags );
	}
	GpuApi::SetPSC( PSC_Custom_0, 0.0f );

	Bool didAddInstances = false;

	auto FlushBatch = [&]()
	{
		cpGeoProbe::Seg cpSeg( 3 ); // [skperf] round 57
		// flush params
		if ( threadData.m_paramsBatchData )
		{
			CRenderMaterialParameters::BindParameters( *threadData.m_paramsBatchData );
			threadData.m_paramsBatchData = nullptr;
		}

		if ( didAddInstances )
		{
			mmConstants.Flush();
			DrawBatchInstances( lastChunk, ctx, threadData );
			didAddInstances = false;
		}
	};

	Uint8 materialStencilBit = 0;
	Uint8 objectStencilBit = 0;
	Uint32 lastStencilRef = UINT32_MAX;
	// we want to avoid an "if" per draw, so to some masking instead, based on whether we allow stencil ref changes or not
	const Uint32 stencilOverrideMask = ctx.allowSettingStencil ? 0 : UINT32_MAX;

	//
	// Do the batching!
	//
	for ( Uint32 chunk_i = 0, chunkCount = sortEntries.Size(); chunk_i < chunkCount; ++chunk_i )
	{
		const SRenderChunk chunk = sortEntries[chunk_i];
		RED_FATAL_ASSERT( chunk.m_data.IsValid(), "invalid chunk batch data" );

		RenderBatchKey chunkKeyDiff = GetBatchKeyDiff( lastChunk.m_key, chunk.m_key );

		// using static instance buffer has to break the batch
		// per instance custom data changed - need to flush the batch
		// check if pipeline has to be updated
		if ( chunk.m_key.usesStaticInstanceBuffer | (lastChunk.m_data.customData != chunk.m_data.customData) | (chunkKeyDiff.combined != 0) )
		{
			FlushBatch();
		}

		if ( lastChunk.m_data.customData != chunk.m_data.customData )
		{
			GpuApi::SetPSC( PSC_Custom_0, (Float)chunk.m_data.customData / 255.0f );
		}

		// Material modifiers setup mask
		// Used to test whether we should process the modifiers
		MaterialModifierSetupFlags matModSetupFlags = MMSF_PerChunk;

		if ( lastChunk.m_data.GetProxy() != chunk.m_data.GetProxy() )
		{
			threadData.m_proxyBatchData = &batchDataAllocator.GetProxyData().GetData( chunk.m_data.GetProxy() );
			_mm_prefetch( (const char*)threadData.m_proxyBatchData, _MM_HINT_T0 );
			matModSetupFlags = matModSetupFlags | MMSF_PerProxy;
		}

		// Either add it to the current batch of instances, or draw it directly.
		const Float* instanceMatrixData = batchDataAllocator.GetInstanceData().GetPtr( chunk.m_data.GetInstanceOffset() );
		{
			// Prefetch a single instance matrix. Matrices will only be 16-byte aligned, so may cross a cacheline boundary,
			// so we'll fetch start and end to make sure we have the full thing.
			_mm_prefetch( (const char*)instanceMatrixData, _MM_HINT_T0 );
			_mm_prefetch( (const char*)instanceMatrixData + 32, _MM_HINT_T0 );
		}

		// If the key has changed since last chunk, check what we need to re-bind
		if ( chunkKeyDiff.combined != 0 )
		{
			// prefetch stuff
			{
				if ( chunkKeyDiff.GetPSOIndex() )
				{
					const SPSOBatchData& psoBatchData = batchDataAllocator.GetPSOData().GetData( chunk.m_key.GetPSOIndex() );
					_mm_prefetch( (const char*)&psoBatchData, _MM_HINT_T0 );
				}

				if ( chunkKeyDiff.GetParams() )
				{
					threadData.m_paramsBatchData = &batchDataAllocator.GetParamsData().GetData( chunk.m_key.GetParams() );
					_mm_prefetch( (const char*)threadData.m_paramsBatchData, _MM_HINT_T0 );
					// NOTE: params will be applied on batch flush
				}

				if ( chunkKeyDiff.GetGeometry() )
				{
					threadData.m_geoBatchData = &batchDataAllocator.GetGeometryData().GetData( chunk.m_key.GetGeometry() );
					_mm_prefetch( (const char*)threadData.m_geoBatchData, _MM_HINT_T0 );
				}
			}

			if ( chunkKeyDiff.GetDissolveMode() | chunkKeyDiff.isMasked )
			{
				const Vector4 discardFlags = BuildDiscardFlags( sc, chunk.m_key.GetDissolveMode(), chunk.m_key.isMasked != 0 );
				mmConstants.SetValue( MTL_DiscardFlags, discardFlags );
			}

			if ( chunkKeyDiff.GetPSOIndex() )
			{
				const SPSOBatchData& psoBatchData = batchDataAllocator.GetPSOData().GetData( chunk.m_key.GetPSOIndex() );
				threadData.m_psoBatchData = &psoBatchData;

				{ cpGeoProbe::Seg cpSeg( 0 ); psoBatchData.Bind( false ); } // [skperf] round 57 seg

				materialStencilBit = psoBatchData.m_materialTypeID;

				// If the PSO changed, and the new one has some material modifiers, then we need to flush the full set
				// of modifiers, to make sure they're all set properly for any changed binding slots.
				if ( !psoBatchData.m_materialModifiers.IsEmpty() )
				{
					matModSetupFlags = MMSF_ForceAll;
				}

#ifdef RENDER_MATERIAL_ENABLE_DEBUG_NAME
	#ifdef USE_DYNAMIC_PC_SCOPES
				gProfilers.StopBlock( &batchInstrumentationScope, currentProfilerScope );
	#endif // USE_DYNAMIC_PC_SCOPES

				GpuApi::EndProfilerBlock();
				GpuApi::BeginProfilerBlock( psoBatchData.m_materialName );

	#ifdef USE_DYNAMIC_PC_SCOPES
				currentProfilerScope = psoBatchData.m_materialName;
				gProfilers.StartBlock( &batchInstrumentationScope, currentProfilerScope );
	#endif // USE_DYNAMIC_PC_SCOPES
#endif // RENDER_MATERIAL_ENABLE_DEBUG_NAME
			}

			if ( chunkKeyDiff.GetGeometry() )
			{
				{ cpGeoProbe::Seg cpSeg( 1 ); BindBatchGeometry( frameIndex, isUpdatedTangents, threadData, objectStencilBit ); } // [skperf] round 57 seg
				matModSetupFlags = matModSetupFlags | MMSF_PerGeometry;
			}

			const Uint32 newStencilRef = stencilOverrideMask | MapStencilRef( (ERenderObjectType)objectStencilBit, (ERenderMaterialType)materialStencilBit );
			if ( lastStencilRef != newStencilRef )
			{
				GpuApi::SetStencilRefValue( Uint8( lastStencilRef = newStencilRef ) );
			}
		}

		// Material modifiers.
		// If the current chunk has any material modifier with update frequency that matches what we've changed, need to
		// reapply the modifiers.
		if ( matModSetupFlags & chunk.m_key.matModUpdateFrequencies )
		{
			FlushBatch();
			{ cpGeoProbe::Seg cpSeg( 2 ); MaterialModifierProcess( threadData.m_psoBatchData->m_materialModifiers, matModSetupFlags, ctx.rctx, batchDataAllocator, chunk, ctx.stats, mmConstants ); } // [skperf] round 57 seg
		}

		if ( chunk.m_data.IsInstanced() )
		{
			didAddInstances = true;
			{ cpGeoProbe::Seg cpSeg( 4 ); AddChunkInstancesToBatch( instanceMatrixData, chunk, ctx.stats, ctx, threadData ); } // [skperf] round 57 seg
		}
		else
		{
			cpGeoProbe::Seg cpSeg( 5 ); // [skperf] round 57 seg
			// flush params
			if ( threadData.m_paramsBatchData )
			{
				CRenderMaterialParameters::BindParameters( *threadData.m_paramsBatchData );
				threadData.m_paramsBatchData = nullptr;
			}

			mmConstants.Flush();
			DrawSingle( instanceMatrixData, chunk, ctx.stats, ctx, threadData );
		}

		lastChunk = chunk;
	}

	// Last batch may not be flushed, so draw it
	FlushBatch();

#ifdef RENDER_MATERIAL_ENABLE_DEBUG_NAME
	#ifdef USE_DYNAMIC_PC_SCOPES
		gProfilers.StopBlock( &batchInstrumentationScope, currentProfilerScope );
	#endif // USE_DYNAMIC_PC_SCOPES
	GpuApi::EndProfilerBlock();
#endif // RENDER_MATERIAL_ENABLE_DEBUG_NAME

	// All the instance data arrays should be cleared when flushing last batch
	RED_ASSERT( threadData.m_instances.Empty(), "Instance data was not flushed" );
	RED_ASSERT( threadData.m_skinnedInstances.Empty(), "Instance data was not flushed" );
	RED_ASSERT( threadData.m_decalsData.Empty(), "Instance data was not flushed" );
}

////////////////////////////////////////////////////////////////////////////////////////////////////////

CRenderGeometryBatcher::RenderContext::RenderContext( Uint32 dispatcherThreadIndex, GeometryBatcherStats& stats )
	: rctx( nullptr )
	, stats( stats )
	, dispatcherThreadIndex( dispatcherThreadIndex )
{
	RED_FATAL_ASSERT( -1 != dispatcherThreadIndex, "Invalid dispatcher thread index" );
}

CRenderGeometryBatcher::RenderContext::RenderContext( const SRenderNodeImplContext& _rctx, GeometryBatcherStats& stats )
	: rctx( &_rctx )
	, stats( stats )
{
	dispatcherThreadIndex = rctx->GetDispatcherThreadIndex();
}
