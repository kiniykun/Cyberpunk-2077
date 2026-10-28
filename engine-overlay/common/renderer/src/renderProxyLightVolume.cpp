/**
* Copyright (c) 2017-2018 CD Projekt Red. All Rights Reserved.
*/

// Renderer
#include "build.h"
#include "renderProxyLightVolume.h"
#include "../../redContainers/include/bitSet.h"
#include "../../renderData/include/renderLightBase.h"
#include "renderFrameCamera.h"
#include "renderScene.h"

// #pragma optimize("",off)

#ifdef USE_RAY_TRACING
#include "renderAccelerationStructure.h"
#endif

#include "../../gpuApi/include/gpuApiAlloc.h"

//******************************************************************************************************************************//
//******************************************************************************************************************************//

namespace
{
	RED_FORCE_INLINE static Uint32 CountTrailingZerosAndClear( Uint32& mask )
	{
#ifdef _MSC_VER
		unsigned long index = 0;
		::_BitScanForward( &index, mask );
		const Uint32 slot = index;
#else
		const Uint32 slot = __builtin_ctz( mask );
#endif
		mask &= mask - 1;
		return slot;
	}
}

namespace volume
{
	static constexpr Uint32 MAX_GI_VOLUMES_RENDERED_AT_RUNTIME = 8;

	struct InstanceData
	{
		Float		matrix[12];
		Uint32		chn;
		Uint32		padding[3];
	};

	struct MeshRef
	{
		Uint32						refCount{ 1 };		//!< Reference count
		MeshBlock					block;
		Uint32						dataHash{ 0 };

		RED_FORCE_INLINE Bool operator==( const MeshRef& other ) const { return dataHash == other.dataHash; }
	};

}

class CRenderBufferAllocator
{

	GpuApi::Buffer			m_buffer;
	GpuApi::Buffer			m_rayTracingBuffer; // shadow copy for ray tracing
	GpuApi::BuddyAllocator	m_allocator;
	red::RWSpinLock			m_lock;

public:

	enum Purpose
	{
		Vertex,
		Index
	};

public:

	CRenderBufferAllocator()
	{
	}

	void Initialize( Uint32 size, Purpose purpose, GpuWrapApi::BufferGroup group )
	{
		m_allocator.Initialize(size, 8 );

		GpuWrapApi::eBufferChunkCategory category;


		Uint32 sizeOfElement = 0;
		switch( purpose )
		{
		case Vertex:		category = GpuWrapApi::BCC_Vertex;		sizeOfElement = sizeof( Vector3 );	break;
		case Index:			category = GpuWrapApi::BCC_Index16Bit;	sizeOfElement = sizeof( Int16 );	break;
		}

		GpuApi::BufferDesc desc;
		desc.size = size * sizeOfElement;
		desc.category = category;
		desc.usage = GpuApi::BUT_Dynamic_Legacy;
		desc.group = group;
		m_buffer = GpuApi::Buffer::Create( desc );

#ifdef USE_RAY_TRACING
		if ( GpuApi::GetCapabilities().rayTracingSupport )
		{
			desc.usage = GpuApi::BUT_Default;
			m_rayTracingBuffer = GpuApi::Buffer::Create( desc );
			RED_FATAL_ASSERT( m_rayTracingBuffer );
			GpuApi::SetResourceDebugName( m_rayTracingBuffer, "CRenderBufferAllocator" );
		}
#endif // USE_RAY_TRACING

		RED_FATAL_ASSERT( !m_buffer.isNull() );
		GpuApi::SetResourceDebugName( m_buffer, "CRenderBufferAllocator" );
	}

	~CRenderBufferAllocator()
	{
	}

	// - - - - - - - - - - - - - - - - - - - - - - - - - - -

	RED_INLINE GpuApi::BufferRef	GetBuffer(Bool rayTracingBuffer = false) const { return rayTracingBuffer ? m_rayTracingBuffer : m_buffer; }

	// - - - - - - - - - - - - - - - - - - - - - - - - - - -

	red::memory::Block Allocate( uint32_t size )
	{
		RED_SCOPE_LOCK( m_lock );

		red::memory::Block block ={};

		auto rqst = m_allocator.MakeAllocateRequest( static_cast<Uint32>( size ) );

		// That should be enough - assuming that no volumes with 0 elements are present.
		RED_WARNING( rqst.blockSize, "GI Volume / Light Channel error - possibly too many of them! Or something..." );

		if( rqst.blockSize )
		{
			auto allocated = m_allocator.Allocate( size, rqst );
			block.address = allocated;
			block.size = size;

			RED_FATAL_ASSERT( block.size, "Unable to allocate more data for light volumes" );
		}

		return block;
	}

	void Free( const red::memory::Block & block )
	{
		if( block.size )
		{
			RED_SCOPE_LOCK( m_lock );
			m_allocator.Free( static_cast<Uint32>(block.address) );
		}
	}

	// - - - - - - - - - - - - - - - - - - - - - - - - - - -

	template <typename T>
	red::memory::Block Allocate( red::ArraySpan<const T> data )
	{
		RED_FATAL_ASSERT( !m_buffer.isNull() );

		auto block = Allocate( data.SizeInBytes() / sizeof(T) );

		if( block.size )
		{
			GpuApi::LoadBufferData( m_buffer, (Uint32)block.address * sizeof( T ), data.SizeInBytes(), data.Data() );
		}

		return block;
	}

	// - - - - - - - - - - - - - - - - - - - - - - - - - - -
};

class CRenderVolumeMeshAllocator
{
	CRenderBufferAllocator		m_vertices;
	CRenderBufferAllocator		m_indices;

public:

	CRenderVolumeMeshAllocator()
	{
	}

	~CRenderVolumeMeshAllocator()
	{
	}

	RED_INLINE const CRenderBufferAllocator&	GetVertices() const { return m_vertices; }

	RED_INLINE const CRenderBufferAllocator&	GetIndices() const { return m_indices; }

	void Initialize( GpuWrapApi::BufferGroup group )
	{
		const Uint32 scale = ::IsGameMode() ? 1 : 4;

		m_vertices.Initialize( static_cast<Uint32>(scale*red::RoundUpToPowerOf2( static_cast<Uint64>( RED_KILO_BYTE( 256 ) / sizeof( Vector3 ) ) )), CRenderBufferAllocator::Purpose::Vertex, group );
		m_indices.Initialize( static_cast<Uint32>(scale*red::RoundUpToPowerOf2( static_cast<Uint64>( RED_KILO_BYTE( 128 ) / sizeof( Int16 ) ) )), CRenderBufferAllocator::Purpose::Index, group );
	}

	volume::MeshBlock Alloc( red::ArraySpan<const Vector3> v, red::ArraySpan<const Uint16> i )
	{
		volume::MeshBlock ret;

		auto vBlock = m_vertices.Allocate( v );
		auto iBlock = m_indices.Allocate( i );

		ret.vertices.offset = (Uint32)vBlock.address;
		ret.vertices.size = (Uint32)vBlock.size;

		ret.indices.offset = (Uint32)iBlock.address;
		ret.indices.size = (Uint32)iBlock.size;

		return ret;
	}

	void Free( const volume::MeshBlock& block )
	{
		m_vertices.Free( red::memory::Block{ (Uint64)block.vertices.offset, (Uint64)block.vertices.size } );
		m_indices.Free( red::memory::Block{ (Uint64)block.indices.offset, (Uint64)block.indices.size } );
	}
};

//******************************************************************************************************************************//
//******************************************************************************************************************************//

class CRenderLightVolumeAllocator : public ISceneStorageCustomData
{
	RTTI_DECLARE_TYPE( CRenderLightVolumeAllocator );
	DECLARE_CUSTOM_SCENE_DATA( 6 );

	CRenderVolumeMeshAllocator					m_allocator;

	red::RWSpinLock								m_allocLock;

	red::DynArray< volume::MeshRef >			m_meshes;

	GpuApi::BufferRef							m_parameters;
	GpuApi::BufferRef							m_drawIndirect;

	typedef GpuApi::SIndirectIndexedDrawArgs	TIndirectArg;

#ifdef USE_RAY_TRACING
	Bool										m_requireBufferUpdate = true;
	GpuApi::AccelerationStructureRef			m_topLevelAccelerationStructure = GpuApi::AccelerationStructureRef::Null();

	rayTracing::UploadableBuffer<rayTracing::framesInFlight> m_instanceDataBuffer;

	GpuApi::BufferRef							m_scratchBuffer = GpuApi::BufferRef::Null();
	Uint32										m_scratchBufferSize = RED_MEGA_BYTE( 2 ); // #TODO: FIXMEE
	Uint32										m_scratchBufferOffset = 0;

	red::Map<Uint32, GpuApi::AccelerationStructureRef> m_accelerationStructures;
#endif

public:

	CRenderLightVolumeAllocator()
		: m_meshes{ rend::PoolLighting() }
#ifdef USE_RAY_TRACING
		, m_accelerationStructures{ rend::PoolRaytrace() }
#endif
	{
	}

	~CRenderLightVolumeAllocator()
	{
		GpuApi::SafeRelease( m_parameters );
		GpuApi::SafeRelease( m_drawIndirect );

#ifdef USE_RAY_TRACING
		GpuApi::SafeRelease( m_topLevelAccelerationStructure );
		GpuApi::SafeRelease( m_scratchBuffer );
		m_instanceDataBuffer.Release();

		for ( auto it : m_accelerationStructures )
		{
			GpuApi::SafeRelease( it.Value() );
		}
#endif
	}

	// - - - - - - - - - - - - - - - - - - - - - - - - - - -

	RED_INLINE CRenderVolumeMeshAllocator&	GetAllocator() { return m_allocator; }

	// - - - - - - - - - - - - - - - - - - - - - - - - - - -

	virtual void Initialize() override
	{
		m_allocator.Initialize( GpuWrapApi::BufferGroup::Lights );

		{
			GpuApi::BufferDesc desc;
			desc.size = sizeof( volume::InstanceData ) * volume::MAX_VOLUMES_RENDERED;
			desc.structureStride = sizeof( volume::InstanceData );
			desc.category = GpuWrapApi::BCC_Structured;
			desc.usage = GpuApi::BUT_Dynamic_Legacy;
			desc.group = GpuWrapApi::BufferGroup::Lights;
			m_parameters = GpuApi::CreateBuffer( desc );
		}

		{
			GpuApi::BufferDesc desc;
			desc.size = sizeof( TIndirectArg ) * volume::MAX_VOLUMES_RENDERED;
			desc.category = GpuWrapApi::BCC_IndirectUAV;
			desc.usage = GpuApi::BUT_Default;
			desc.group = GpuWrapApi::BufferGroup::Lights;
			m_drawIndirect = GpuApi::CreateBuffer( desc );
		}

#ifdef USE_RAY_TRACING
		if (GpuApi::GetCapabilities().rayTracingSupport)
		{
			{
				GpuApi::AccelerationStructureDesc desc = {};
				desc.type = GpuApi::AccelerationStructureType::TOP_LEVEL;
				desc.buildFlags = GpuApi::AccelerationStructureBuildFlagBits::PREFER_FAST_TRACE;
				desc.instanceNum = volume::MAX_VOLUMES_RENDERED;
				m_topLevelAccelerationStructure = GpuApi::CreateAccelerationStructure( desc );

				RED_ASSERT( m_topLevelAccelerationStructure );

				GpuApi::AllocateAccelerationStructure( m_topLevelAccelerationStructure );
			}

			GpuApi::AccelerationStructurePrebuildInfo prebuildInfo;
			GpuApi::GetAccelerationStructurePrebuildInfo( m_topLevelAccelerationStructure, prebuildInfo );

			{
				GpuApi::BufferDesc desc;
				desc.size = m_scratchBufferSize + prebuildInfo.scratchDataSize;
				desc.category = GpuWrapApi::eBufferChunkCategory::BCC_VertexUAV;
				desc.usage = GpuApi::BUT_Default;
				desc.group = GpuWrapApi::BufferGroup::Raytracing;
				m_scratchBuffer = GpuApi::CreateBuffer( desc );
			}

			rayTracing::AllocateBufferWithUploadPool( m_instanceDataBuffer, volume::MAX_VOLUMES_RENDERED, sizeof( GpuApi::GeometryObjectInstance ), "m_instanceData" );
		}
#endif // USE_RAY_TRACING
	}

	virtual void Prepare( const SRenderNodeImplContext& rctx, const CRenderFrameCameraStorage& sceneData ) override
	{
	}

	virtual void Evict() override
	{
	}

	// - - - - - - - - - - - - - - - - - - - - - - - - - - -

	void Allocate( CRenderProxy_LightVolume* volume )
	{
		const Uint32 hash = volume->GetDataHash();

		RED_SCOPE_LOCK( m_allocLock );

		for( auto& mesh : m_meshes )
		{
			if( mesh.dataHash == hash )
			{
				volume->SetMeshBlock( mesh.block );

				RED_VERIFY( ++mesh.refCount > 1 );

				return;
			}
		}

		volume::MeshRef newRef;
		newRef.dataHash = hash;
		newRef.block	= m_allocator.Alloc( volume->GetVertices(), volume->GetIndices() );

		m_meshes.PushBack( newRef );

		volume->SetMeshBlock( newRef.block );

#ifdef USE_RAY_TRACING
		m_requireBufferUpdate = true;
#endif // USE_RAY_TRACING
	}

	void Release( CRenderProxy_LightVolume* volume )
	{
		const Uint32 hash = volume->GetDataHash();

		RED_SCOPE_LOCK( m_allocLock );

		for( auto& mesh : m_meshes )
		{
			if( mesh.dataHash == hash )
			{
				if( 0 == --mesh.refCount )
				{
					m_allocator.Free( mesh.block );

					m_meshes.Remove( mesh );

#ifdef USE_RAY_TRACING
					auto it = m_accelerationStructures.Find( hash );
					if (it != m_accelerationStructures.End())
					{
						GpuApi::SafeRelease( it.Value() );
						m_accelerationStructures.Remove( it );
					}
#endif // USE_RAY_TRACING
				}

				return;
			}
		}

		RED_FATAL( "Unable to find mesh by data hash" );
	}

	// - - - - - - - - - - - - - - - - - - - - - - - - - - -

	Bool PrepareAllocs( red::ArraySpan<CRenderProxy_LightVolume* const> volumes )
	{
		if( volumes.Empty() )
			return false;

		volume::InstanceData instanceData[volume::MAX_VOLUMES_RENDERED];
		TIndirectArg drawIndirect[volume::MAX_VOLUMES_RENDERED];

		Uint32 drawIdx = 0;

		for( auto vol : volumes )
		{
			if( !vol->IsAllocated() )
			{
				Allocate( vol );
			}

			// Prepare indirect draw
			if( vol->IsAllocated() )
			{
				const volume::MeshBlock& meshBlock = vol->GetMeshBlock();

				static_assert(sizeof( vol->m_instanceMatrix ) == sizeof( instanceData->matrix ), "Size don match");

				memcpy( instanceData[drawIdx].matrix, vol->m_instanceMatrix.m_asFloats, sizeof( instanceData->matrix ) );

				instanceData[drawIdx].chn = vol->GetChannels();

				drawIndirect[drawIdx].BaseVertexLocation	= meshBlock.vertices.offset;
				drawIndirect[drawIdx].StartIndexLocation	= meshBlock.indices.offset;
				drawIndirect[drawIdx].IndexCountPerInstance	= meshBlock.indices.size;
				drawIndirect[drawIdx].StartInstanceLocation	= drawIdx;
				drawIndirect[drawIdx].InstanceCount			= 1;

				++drawIdx;
			}
		}

		GpuApi::LoadBufferData( m_parameters, 0, sizeof( volume::InstanceData ) * volumes.Size(), instanceData );
		GpuApi::LoadBufferData( m_drawIndirect, 0, sizeof( TIndirectArg ) * volumes.Size(), drawIndirect );

		return true;
	}

#ifdef USE_RAY_TRACING
	void UpdateGeometryData( const SRenderNodeImplContext& rctx )
	{
		if (m_requireBufferUpdate)
		{
			m_requireBufferUpdate = false;

			{ // Vertex buffer
				GpuApi::BufferRef vertexBuffer = m_allocator.GetVertices().GetBuffer( true );
				GpuApi::BufferDesc desc = GpuApi::GetBufferDesc( vertexBuffer );
				GpuApi::CopyBuffer( vertexBuffer, 0, m_allocator.GetVertices().GetBuffer(), 0, desc.size );
				GpuApi::TransitionBufferSRV( vertexBuffer );
			}

			{ // Index buffer
				GpuApi::BufferRef indexBuffer = m_allocator.GetIndices().GetBuffer( true );
				GpuApi::BufferDesc desc = GpuApi::GetBufferDesc( indexBuffer );
				GpuApi::CopyBuffer( indexBuffer, 0, m_allocator.GetIndices().GetBuffer(), 0, desc.size );
				GpuApi::TransitionBufferSRV( indexBuffer );
			}
		}

		m_scratchBufferOffset = 0;
	}
#endif // USE_RAY_TRACING
};

class CRenderLightVolumeCollector : public ICameraStorageCustomData
{
	RTTI_DECLARE_TYPE( CRenderLightVolumeCollector );
	DECLARE_CUSTOM_CAMERA_DATA( 11 );

	static red::Atomic<Bool>					m_wasOverflow;

	struct Bucket
	{
		red::DynArray<CRenderProxy_LightVolume*>	m_volumes{ rend::PoolBatchers() };
		red::Atomic<Uint32>							m_numVolumes{ 0 };
		red::Atomic<Uint32>							m_maskVolumes{ 0 };
		red::Atomic<Bool>							m_sorted{ 0 };
		red::RWSpinLock								m_lock;
	};

	Bucket											m_buckets[rend::LCG_Max];

	static constexpr Uint32 GetLimitForGroup( rend::LightChannelGroup group )
	{
		return  group == rend::LCG_GIInterior ? volume::MAX_GI_VOLUMES_RENDERED_AT_RUNTIME : volume::MAX_VOLUMES_RENDERED;
	}

public:

	CRenderLightVolumeCollector()
	{
		m_buckets[rend::LCG_LightClippingVolume].m_volumes.Resize( volume::MAX_VOLUMES_RENDERED * 2 );
		m_buckets[rend::LCG_GIInterior].m_volumes.Resize( volume::MAX_GI_VOLUMES_RENDERED_AT_RUNTIME * 4 );
	}

	~CRenderLightVolumeCollector()
	{
	}

	// - - - - - - - - - - - - - - - - - - - - - - - - - - -

	static RED_FORCE_INLINE Bool WasLastFrameOverflowed()
	{
		return m_wasOverflow.Exchange( false );
	}

	RED_FORCE_INLINE Uint32		GetNumVisibleVolumes( rend::LightChannelGroup group ) const
	{
		Uint32 limit = GetLimitForGroup( group );

		return Min( m_buckets[ group ].m_numVolumes.GetValue(), limit );
	}

	RED_FORCE_INLINE Uint32		GetMaskVisibleVolumes( rend::LightChannelGroup group ) const
	{
		return m_buckets[group].m_maskVolumes.GetValue();
	}

	RED_FORCE_INLINE red::ArraySpan< CRenderProxy_LightVolume* const > GetVolumes( rend::LightChannelGroup group ) const
	{
		const Uint32 limit = GetLimitForGroup( group );

		auto& bucket = m_buckets[group];

		return red::ArraySpan< CRenderProxy_LightVolume* const >( bucket.m_volumes.TypedData(), Clamp( bucket.m_numVolumes.GetValue(), 0u, limit - 1 ) );
	}

	// - - - - - - - - - - - - - - - - - - - - - - - - - - -

	virtual void Initialize() override
	{
	}

	virtual void Prepare( const SRenderNodeImplContext& rctx, const CRenderFrameCameraStorage::SCameraData& cameraData ) override
	{
		for( auto& it : m_buckets )
		{
			it.m_numVolumes.SetValue( 0 );
			it.m_maskVolumes.SetValue( 0 );
			it.m_sorted.SetValue( false );
		}
	}

	virtual void Evict() override
	{
	}

	// - - - - - - - - - - - - - - - - - - - - - - - - - - -

	void SortVolumes( const CRenderCamera& camera, rend::LightChannelGroup group )
	{
		auto& bucket = m_buckets[group];

		RED_SCOPE_LOCK( bucket.m_lock );

		if( bucket.m_sorted.Exchange( true ) )
			return;

		const Vector4 cameraPos = camera.GetPosition().AsVector4();

		const Uint32 limit = GetLimitForGroup( group );
		const Uint32 count = bucket.m_numVolumes.GetValue();

		struct SortEntry
		{
			CRenderProxy_LightVolume*		m_vol;
			Float							m_distance;
		};

		// If the amount of Volumes exceeds the limit of stuff to render
		// Sort them by the distance to the camera.
		if( count > limit )
		{
			const Uint32 inBucket = Min( count, bucket.m_volumes.Size() );

			red::DynArray< SortEntry > tempBucket{ job::PoolJobScope() };
			tempBucket.Resize( inBucket );

			// Read back to temp array and precache distances
			for( Uint32 i = 0; i < inBucket; ++i )
			{
				tempBucket[i].m_vol = bucket.m_volumes[i];
				tempBucket[i].m_distance = tempBucket[i].m_vol->GetBoundingBox().SquaredDistance( cameraPos );
			}

			// Sort temp array
			std::sort( tempBucket.TypedData(), tempBucket.TypedData() + inBucket, []( const auto& A, const auto& B ) { return A.m_distance < B.m_distance; } );

			// Write back objects with new order. We care just about First limited batch
			for( Uint32 i = 0; i < limit; ++i )
			{
				bucket.m_volumes[i] = tempBucket[i].m_vol;
			}

			m_wasOverflow.SetValue( true );
		}

		// Do presorting and assigning dynamic light channels
		{
			// Render dynamic allocated lightChannels
			for( Uint32 i = 0, end = Min( count, limit ); i < end; ++i )
			{
				auto vol = bucket.m_volumes[i];

				if( rend::IsAutomatedChannel( (rend::LightChannel)vol->GetChannels(), vol->IsComponent() ) )
				{
					// In another case, try to find some other that will be any of the non touching one
					Uint8 bestFreeChannel = 0;

					Uint32 nonTouchingChannels = rend::LightChannel::LC_AllChannels;

					for( Uint32 i = 0, end = Min( count, limit ); i < end; ++i )
					{
						auto other = bucket.m_volumes[i];

						if( other == vol ) continue;

						// If touching. Clear those bits from the free list
						if( other->GetBoundingBox().Touches( vol->GetBoundingBox() ) )
						{
							nonTouchingChannels &= (~other->GetRuntimeChannels()) & rend::LightChannel::LC_AllChannels;
						}

					}

					// If free one is NOT our one. Do change LC
					if( 0 == ( nonTouchingChannels & vol->GetChannels() ) )
					{
						// Get this one
						bestFreeChannel = 1u << CountTrailingZerosAndClear( nonTouchingChannels );
					}

					// Set best candidate. Can be 0 and in that case it will fallback to the original one but then some visual issues can be visible
					vol->SetRuntimeChannels( bestFreeChannel );
				}

			}

		}

	}

	// - - - - - - - - - - - - - - - - - - - - - - - - - - -

	void Collect( CRenderProxy_LightVolume* volume )
	{
		RED_FATAL_ASSERT( volume );

		// Discard Exterior GI Volumes Collection.
		if ( volume->GetGroup() == rend::LCG_GIInterior )
		{
			if ( volume->GetChannels() == 0 )
				return;
		}

		auto& bucket = m_buckets[ volume->GetGroup() ];

		const Uint32 idx = bucket.m_numVolumes.PostIncrement();

		if ( idx < bucket.m_volumes.Size() )
		{
			bucket.m_volumes[idx] = volume;

#ifndef RED_CONFIGURATION_FINAL
			bucket.m_maskVolumes.Or( volume->GetChannels() );
#endif
		}
		else
		{
			m_wasOverflow.SetValue( true );
		}
	}

	// - - - - - - - - - - - - - - - - - - - - - - - - - - -

	void RenderVolumes( const SRenderNodeImplContext& rctx, rend::LightChannelGroup group )
	{

		auto& bucket = m_buckets[group];

		// Its commented out for now. This requires having some gl_DrawID-like constant
#if 0
		GpuApi::BindIndirectArgs( m_drawIndirect );
		GpuApi::BindBufferSRV( m_parameters, 0, GpuWrapApi::VertexShader );

		GpuApi::DrawIndexedPrimitiveIndirect( PRIMTYPE_TriangleList, m_volumes.Size() );
#else

		const Uint32 limit = GetLimitForGroup( group );
		const Uint32 count = Min( bucket.m_numVolumes.GetValue(), limit );

		auto sceneLayer = rctx.GetCameraData()->GetInfo().m_sceneLayer;

		// First pass for static light volumes
		for( Uint32 i = 0; i < count; ++i )
		{
			auto vol = bucket.m_volumes[i];

			if( !CheckRenderSceneLayerMask( vol->GetSceneLayerMask(), sceneLayer ) )
				return;

			const volume::MeshBlock& meshBlock = vol->GetMeshBlock();

			const Uint32 BaseVertexLocation		= meshBlock.vertices.offset;
			const Uint32 NumVertices			= meshBlock.vertices.size;
			const Uint32 StartIndexLocation		= meshBlock.indices.offset;
			const Uint32 IndexCountPerInstance	= meshBlock.indices.size;

			volume::InstanceData data;
			memcpy( data.matrix, vol->m_instanceMatrix.m_asFloats, sizeof( data.matrix ) );
			data.chn = static_cast<Uint32>(vol->GetRuntimeChannels());

			rctx.SetConstant<eVERTEX>( data );

			GpuApi::DrawIndexedPrimitive( PRIMTYPE_TriangleList, BaseVertexLocation, StartIndexLocation, IndexCountPerInstance );
		}

#endif
	}

#ifdef USE_RAY_TRACING
	void UpdateAccelerationStructures( const SRenderNodeImplContext& rctx )
	{
		auto& allocator = rctx.GetCameraStorage()->GetCustomData<CRenderLightVolumeAllocator>();

		// Prepare geometry data
		allocator.UpdateGeometryData( rctx );

		GpuApi::BufferRef vertexBuffer = allocator.GetAllocator().GetVertices().GetBuffer( true );
		GpuApi::BufferRef indexBuffer = allocator.GetAllocator().GetIndices().GetBuffer( true );
		GpuApi::BufferRef uploadBuffer = allocator.m_instanceDataBuffer.GetUploadBuffer( rctx );
		GpuApi::GeometryObjectInstance* geometryObjectInstance = (GpuApi::GeometryObjectInstance*)GpuApi::GetMappedBufferPtr( uploadBuffer );

		SortVolumes( rctx.GetRenderCamera(), rend::LCG_LightClippingVolume );

		auto volumes = GetVolumes( rend::LCG_LightClippingVolume );

		Uint32 volumeNum = 0;

		for( auto volume : volumes )
		{
			const Uint32 hash = volume->GetDataHash();

			GpuApi::AccelerationStructureRef accelerationStructure = GpuApi::AccelerationStructureRef::Null();
			auto it = allocator.m_accelerationStructures.Find( hash );
			if (it == allocator.m_accelerationStructures.End())
			{
				const auto& meshBlock = volume->GetMeshBlock();

				GpuApi::GeometryObject geometryObject;
				geometryObject.hasTransparentGeometry = true;
				geometryObject.triangles.vertexBuffer = vertexBuffer;
				geometryObject.triangles.vertexOffset = meshBlock.vertices.offset * sizeof( Vector3 );
				geometryObject.triangles.vertexNum = meshBlock.vertices.size;
				geometryObject.triangles.vertexStride = sizeof( Vector3 );
				geometryObject.triangles.vertexPackingType = GpuWrapApi::VertexPacking::PT_Float3;
				geometryObject.triangles.indexBuffer = indexBuffer;
				geometryObject.triangles.indexOffset = meshBlock.indices.offset * sizeof( Uint16 );
				geometryObject.triangles.indexNum = meshBlock.indices.size;

				GpuApi::AccelerationStructureDesc desc;
				desc.type = GpuApi::AccelerationStructureType::BOTTOM_LEVEL;
				desc.buildFlags = GpuApi::AccelerationStructureBuildFlagBits::PREFER_FAST_TRACE;
				desc.geometryObjectNum = 1;
				desc.geometryObjects = &geometryObject;

				accelerationStructure = GpuApi::CreateAccelerationStructure( desc );
				if (!accelerationStructure || !GpuApi::AllocateAccelerationStructure( accelerationStructure ))
				{
					continue;
				}

				GpuApi::AccelerationStructurePrebuildInfo prebuildInfo;
				GpuApi::GetAccelerationStructurePrebuildInfo( accelerationStructure, prebuildInfo );
				Uint32 postBuildScratchBufferOffset = allocator.m_scratchBufferOffset + rayTracing::GetAccelerationStructureDataAlignedSize( prebuildInfo.scratchDataSize );
				if (postBuildScratchBufferOffset > allocator.m_scratchBufferSize)
				{
					GpuApi::SafeRelease( accelerationStructure );
					break;
				}

				GpuApi::AccelerationStructureBuildFlagMask buildFlags = GpuApi::AccelerationStructureBuildFlagBits::PREFER_FAST_TRACE;
				GpuApi::BuildBottomLevelAccelerationStructure( accelerationStructure, 1, &geometryObject,
					allocator.m_scratchBuffer, allocator.m_scratchBufferOffset, buildFlags, false, GpuApi::BufferRef::Null(), 0 );

				allocator.m_scratchBufferOffset = postBuildScratchBufferOffset;
				allocator.m_accelerationStructures.Insert( hash, accelerationStructure );
			}
			else
			{
				accelerationStructure = it.Value();
			}

			Uint32 instanceID = volume->GetChannels();
			Uint32 instanceFlags = GpuApi::InstanceFlagBits::TRIANGLE_CULL_DISABLE;

			volume->m_instanceMatrix.GetUnpackedMatrix3x4( geometryObjectInstance[volumeNum].transform );
			geometryObjectInstance[volumeNum].SetInstanceIDAndMask( instanceID, 0xff );
			geometryObjectInstance[volumeNum].SetInstanceOffsetAndFlags( 0, instanceFlags );
			geometryObjectInstance[volumeNum].accelerationStructureHandle = GpuApi::GetAccelerationStructureHandle( accelerationStructure );

			++volumeNum;
		}

		if (volumeNum)
		{
			GpuApi::CopyBuffer( allocator.m_instanceDataBuffer.m_buffer, 0, allocator.m_instanceDataBuffer.GetUploadBuffer( rctx ), 0, volumeNum * sizeof( GpuApi::GeometryObjectInstance ) );
		}

		GpuApi::BuildTopLevelAccelerationStructure( allocator.m_topLevelAccelerationStructure, volumeNum,
			allocator.m_instanceDataBuffer.m_buffer, 0, allocator.m_scratchBuffer, allocator.m_scratchBufferOffset,
			GpuApi::AccelerationStructureBuildFlagBits::PREFER_FAST_TRACE );
	}
#endif

	// - - - - - - - - - - - - - - - - - - - - - - - - - - -

};

RTTI_BEGIN_TYPE( CRenderLightVolumeAllocator );
{
	RTTI_PARENT_TYPE( ISceneStorageCustomData );
}
RTTI_END_TYPE();


RTTI_BEGIN_TYPE( CRenderLightVolumeCollector );
{
	RTTI_PARENT_TYPE( ICameraStorageCustomData );
}
RTTI_END_TYPE();

//******************************************************************************************************************************//
//******************************************************************************************************************************//

CRenderProxy_LightVolume::CRenderProxy_LightVolume( const RenderProxyInitInfo& initInfo )
	: BaseClass( initInfo )
	, m_vertices{ rend::PoolLighting() }
	, m_indices{ rend::PoolLighting() }
	, m_runtimeChannels( 0x0 )
	, m_channels( 0x01 )
	, m_group( rend::LCG_LightClippingVolume )
	, m_isPointInVolumeCallback( nullptr )
	, m_dataHash( UINT32_MAX )
	, m_component( false )
{
	RED_ASSERT( initInfo.m_componentData && RPT_LightVolume == initInfo.m_componentData->m_type );

	if( initInfo.m_componentData && RPT_LightVolume == initInfo.m_componentData->m_type )
	{
		auto data = static_cast<const RenderProxyLightChannelComponentInitData*>(initInfo.m_componentData);

		m_channels	= data->m_channels;
		m_group		= data->m_group;
		m_component	= data->m_component;

		// Set autohide distance
		m_autoHideDistance		= data->m_autoHideDistanceFar;

		if( 0 == m_channels )
		{
			RED_LOG_WARNING( "Channels cannot be empty" );
		}

		RED_FATAL_ASSERT( !data->m_vertices.Empty() );
		RED_FATAL_ASSERT( !data->m_indices.Empty() );

		m_vertices = std::move( data->m_vertices );
		m_indices = std::move( data->m_indices );

		m_isPointInVolumeCallback = data->m_isPointInVolumeCallback;
	}
}

CRenderProxy_LightVolume::~CRenderProxy_LightVolume()
{
	RED_FATAL_ASSERT( !IsAllocated() );
}

void CRenderProxy_LightVolume::CollectElements( SUpdateContext& ctx )
{
	UpdateOncePerFrame( ctx );

	RED_FATAL_ASSERT( GetScene() );

	if( ctx.cameraData && ctx.isMainCameraView )
	{
		const Float distanceToCameraSqr = (ctx.cameraPosition - GetTranslation()).SquareMag3();

		if( distanceToCameraSqr < GetAutoHideDistanceSquared() )
		{
			auto& data = ctx.cameraData->GetCustomData<CRenderLightVolumeCollector>();

			data.Collect( this );
		}
	}
}


void CRenderProxy_LightVolume::AttachToScene( CRenderSceneEx* scene )
{
	BaseClass::AttachToScene( scene );
}

void CRenderProxy_LightVolume::DetachFromScene( CRenderSceneEx* scene, const red::DynArray<CRenderFrameCameraStorage::SCameraData*>& camData )
{
	if( IsAllocated() )
	{
		auto& data = scene->GetCameraStorage()->GetCustomData<CRenderLightVolumeAllocator>();

		data.Release( this );

		m_meshRegion = volume::MeshBlock();
	}

	BaseClass::DetachFromScene( scene, camData );
}

//******************************************************************************************************************************//
//******************************************************************************************************************************//

Bool lightVolume::HasAnyVolumes( const SRenderNodeImplContext& rctx, rend::LightChannelGroup group )
{
	auto& data = rctx.GetCameraData()->GetCustomData<CRenderLightVolumeCollector>();

	return data.GetNumVisibleVolumes( group ) > 0;
}

rend::LightChannel lightVolume::GetBestLightChannel( const SRenderNodeImplContext& rctx, const Vector3& position, Float radius /*= 0.2f*/ )
{
	auto& data = rctx.GetCameraData()->GetCustomData<CRenderLightVolumeCollector>();

	// Prepare sorting list
	data.SortVolumes( rctx.GetRenderCamera(), rend::LightChannelGroup::LCG_LightClippingVolume );

	auto volumes = data.GetVolumes( rend::LightChannelGroup::LCG_LightClippingVolume );

	math::Box sphere( position, radius );

	for( auto vol : volumes )
	{
		if( vol->IsComponent() && vol->GetBoundingBox().Touches( sphere ) )
		{
			return rend::LightChannel( vol->GetRuntimeChannels() );
		}
	}

	return rend::LightChannel::LC_DefaultAll;
}

red::Atomic<Bool> CRenderLightVolumeCollector::m_wasOverflow;
Bool lightVolume::WasLastFrameOverflowed()
{
	return CRenderLightVolumeCollector::WasLastFrameOverflowed();
}

void lightVolume::RenderVolumes( const SRenderNodeImplContext& rctx, rend::LightChannelGroup group )
{
	auto& data = rctx.GetCameraData()->GetCustomData<CRenderLightVolumeCollector>();
	auto& alloc = rctx.GetCameraStorage()->GetCustomData<CRenderLightVolumeAllocator>();

	// Prepare sorting list
	data.SortVolumes( rctx.GetRenderCamera() , group );

	// Allocated queued volumes
	if( !alloc.PrepareAllocs( data.GetVolumes( group ) ) )
		return;

	RenderBindManager::SetCamera( rctx, rctx.GetRenderCamera(), rctx.GetWidth(), rctx.GetHeight(), nullptr );

	GpuApi::BindIndexBuffer( alloc.GetAllocator().GetIndices().GetBuffer() );

	const GpuApi::BufferRef	buffers[] ={ alloc.GetAllocator().GetVertices().GetBuffer() };
	const Uint8				strides[] ={ sizeof( Vector3 ) };
	const Uint32			offsets[] ={ 0 };

	GpuApi::SetVertexFormatRaw( GpuApi::GetVertexLayout( DebugVertexBase::GetVertexLayoutDesc() ), false );

	GpuApi::BindVertexBuffers( 0, 1, buffers, strides, offsets );

	data.RenderVolumes( rctx, group );

#ifndef RED_CONFIGURATION_FINAL
	{
		CRenderCollector &collector = *rctx.GetCollector();

		switch( group )
		{
		case rend::LCG_LightClippingVolume:
			collector.m_renderingStats.m_lightingStats.m_numVisibleLightChannels	+= data.GetNumVisibleVolumes( group );
			collector.m_renderingStats.m_lightingStats.m_maskVisibleLightChannels	|= data.GetMaskVisibleVolumes( group );
			break;

		case rend::LCG_GIInterior:
			collector.m_renderingStats.m_lightingStats.m_numVisibleGIVolumes		+= data.GetNumVisibleVolumes( group );
			break;
		}
	}
#endif

	GpuApi::BindIndirectArgs( GpuApi::BufferRef::Null() );
	GpuApi::BindIndexBuffer( GpuApi::BufferRef::Null() );
	GpuApi::BindNullVertexBuffers();

}

const red::ArraySpan< CRenderProxy_LightVolume* const> lightVolume::GetVolumes( const CRenderFrameCameraStorage::SCameraData& rfCamStorage, rend::LightChannelGroup group )
{
	auto& data = rfCamStorage.GetCustomData<CRenderLightVolumeCollector>();

	// Prepare sorting list
	data.SortVolumes( rfCamStorage.GetRenderCamera() , group );

	return data.GetVolumes( group );
}

#ifdef USE_RAY_TRACING
void lightVolume::UpdateAccelerationStructures( const SRenderNodeImplContext& rctx )
{
	auto& lightVolumeCollector = rctx.GetCameraData()->GetCustomData<CRenderLightVolumeCollector>();

	lightVolumeCollector.UpdateAccelerationStructures( rctx );
}

GpuApi::AccelerationStructureRef lightVolume::GetAccelerationStructure( const SRenderNodeImplContext& rctx )
{
	auto& lightVolumeAllocator = rctx.GetCameraStorage()->GetCustomData<CRenderLightVolumeAllocator>();

	return lightVolumeAllocator.m_topLevelAccelerationStructure;
}
#endif

//******************************************************************************************************************************//
//******************************************************************************************************************************//
