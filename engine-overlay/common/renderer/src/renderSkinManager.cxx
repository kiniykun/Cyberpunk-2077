/**
* Copyright (c) 2018 CD Projekt Red. All Rights Reserved.
*/

#include "build.h"
#include "renderSkinManager.h"
#include "renderSkinningData.h"
#include "../../redJobs2/include/jobBuilder.h"
#include "../../redSystem/include/utility.h"
#ifdef RED_PLATFORM_WEB
// Browser port: report GPU-readable ranges of the persistently mapped skinning buffer so submit-time uploads
// diff only live blocks instead of all 40 MiB (see gpu-api-buffer-web.cpp TrackMappedBufferRange).
namespace cp2077 { namespace web { bool TrackMappedBufferRange( const GpuApi::BufferRef&, unsigned long long, unsigned long long, bool ); } }
# define CP_SKIN_TRACK( offset, size, live ) cp2077::web::TrackMappedBufferRange( m_gpuBuffer, (unsigned long long)(offset), (unsigned long long)(size), (live) )
// [skperf] round 29 mapTrack for the skinning buffer (tc16 mapstat: this 40 MB 'live' buffer was diffed at every
// submit, ~10 ms/frame in the bar). On PC/web RENDER_SKINNING_DATA_INPLACE is off, so the ONLY CPU writes into the
// mapped memory are the identity element at construction and the FlushUploads memcpys below; both report their bytes,
// and the port then uploads just the written pages (no compare). ?mapTrack=0 restores the live-range diff.
# include "mapped-write-track-web.h"
# include <atomic>
namespace { std::atomic< const cp2077::web::MappedWriteTracker* > s_cpSkinTracker{ nullptr }; }
# define CP_SKIN_NOTE( offset, size ) cp2077::web::NoteMappedGpuWrite( s_cpSkinTracker.load( std::memory_order_acquire ), (std::size_t)(offset), (std::size_t)(size) )
#else
# define CP_SKIN_NOTE( offset, size ) ((void)0)
# define CP_SKIN_TRACK( offset, size, live ) ((void)0)
#endif

//#pragma optimize("",off)

#if defined(RED_PLATFORM_DURANGO)
#define CONSOLE_TYPE_XBOX_SERIES_S        ((CONSOLE_TYPE)5)
#define CONSOLE_TYPE_XBOX_SERIES_X        ((CONSOLE_TYPE)6)
#define CONSOLE_TYPE_XBOX_SERIES_X_DEVKIT ((CONSOLE_TYPE)7)
#endif

static constexpr Uint32 c_defaultPin = UINT32_MAX;
static constexpr Uint8 c_defaultActiveHotness = 2;
static constexpr Uint32 c_allocAlignment = 16;

RenderSkinningManager::RenderSkinningManager()
{
	RED_STATIC_ASSERT( sizeof( RenderSkinningXForm ) == 48 );

	Uint32 totalSkinBufferSize = c_skinBufferSizeInitial;
#ifdef RED_PLATFORM_DURANGO
	CONSOLE_TYPE consoleType = ::GetConsoleType();
	switch ( consoleType )
	{
	case CONSOLE_TYPE_XBOX_SERIES_S:
	case CONSOLE_TYPE_XBOX_SERIES_X:
	case CONSOLE_TYPE_XBOX_SERIES_X_DEVKIT:
		totalSkinBufferSize += c_skinBufferBonusSizeForHighCrowdDensity;
	}
#elif defined(RED_PLATFORM_ORBIS)
	//PS4, PS4 Pro, PS5 in back-compat have the same amount of memory and back-compat currently uses the same crowd density as PS4
#elif defined(RED_PLATFORM_WINPC)
	totalSkinBufferSize += c_skinBufferBonusSizeForHighCrowdDensity;
#endif

	m_skinBufferSize = totalSkinBufferSize - ( totalSkinBufferSize % c_elementSize );
	m_elementCount = m_skinBufferSize / c_elementSize;
	// GPU Buffer starts with a fixed Identity, so the usable size needs to be reduced.
	m_cpuBufferUsableSize = m_skinBufferSize - c_elementSize;
}

RenderSkinningManager::~RenderSkinningManager()
{}

namespace
{

	static inline red::memory::Block ToMemoryBlock( const RenderSkinningDataBlock& dataBlock, Uint64 baseAddr )
	{
		if ( dataBlock.IsNull() )
			return red::memory::NullBlock();

		return{ (Uint64)(baseAddr + dataBlock.bytesOffset), dataBlock.bytesCount };
	}
	static inline RenderSkinningDataBlock ToDataBlock( const red::memory::Block& memoryBlock, Uint64 baseAddr )
	{
		if ( memoryBlock.address == 0 )
			return RenderSkinningDataBlock::NullBlock();

		const Uint64 relativeOffset = memoryBlock.address - baseAddr;
		RED_FATAL_ASSERT( relativeOffset < UINT32_MAX );

		RenderSkinningDataBlock dataBlock;
		dataBlock.bytesOffset = (Uint32)(relativeOffset);
		dataBlock.bytesCount = (Uint32)memoryBlock.size;
		return dataBlock;
	}

	// The byte offsets we store are relative to the CPU-managed buffer, which doesn't include the Identity at the start
	// of the skinning buffer.
	static inline Uint32 GetPinIndex( Uint32 byteOffset )
	{
		return 48 + byteOffset;
	}
}

void RenderSkinningManager::Initialize()
{
	m_needGarbageCollector = false;
	{
		GpuApi::BufferDesc desc;
		desc.size = m_skinBufferSize;
		desc.category = GpuWrapApi::BCC_Raw;
		desc.usage = GpuApi::BUT_Mapped;
		desc.group = GpuWrapApi::BufferGroup::Skinning;
		m_gpuBuffer = GpuApi::CreateBuffer( desc );
		GpuApi::SetResourceDebugName( m_gpuBuffer, "skinningbuffer" );
	}

	{
		RenderSkinningXForm* mappedBuffer = (RenderSkinningXForm*)GpuApi::GetMappedBufferPtr( m_gpuBuffer );
		mappedBuffer[0] = RenderSkinningXForm::FromMatrix( Matrix::IDENTITY() );
		CP_SKIN_TRACK( 0, c_elementSize, true ); // the static identity element is always readable
#ifdef RED_PLATFORM_WEB
		s_cpSkinTracker.store( cp2077::web::EnableMappedGpuWriteTracking( m_gpuBuffer ), std::memory_order_release ); // [skperf] round 29 (first flush diffs everything once)
#endif

#ifdef RENDER_SKINNING_DATA_INPLACE
		m_dataBuffer = (Uint8*)mappedBuffer;
		m_dataBuffer += c_elementSize;
#else
		m_dataBuffer = (Uint8*)RED_ALLOCATE_ALIGNED( rend::PoolSkinningData, m_cpuBufferUsableSize, 64 );
#endif

		red::memory::StaticTLSFAllocatorParameter allocatorParam;
		allocatorParam.buffer = m_dataBuffer;
		allocatorParam.bufferSize = m_cpuBufferUsableSize;
		m_allocator.Initialize( allocatorParam );
	}

	{
		m_buffers.Resize( c_maxBuffers );
		m_generation.Resize( c_maxBuffers, 1 );
		m_activeData.Resize( c_maxActiveBuffers );
	}
}
void RenderSkinningManager::Uninitialize()
{
	for ( red::BitSetIterator<LiveIDMask> it( m_liveIdMask ); it.IsValid(); it.FindNext() )
	{
		const Uint32 index = it.GetIndex();
		ReleaseData( index );
	}

	m_allocator.Uninitialize();

#ifndef RENDER_SKINNING_DATA_INPLACE
	RED_FREE( rend::PoolSkinningData, m_dataBuffer );
#endif

	m_dataBuffer = nullptr;
#ifdef RED_PLATFORM_WEB
	s_cpSkinTracker.store( nullptr, std::memory_order_release ); // [skperf] round 29
#endif
	GpuApi::SafeRelease( m_gpuBuffer );

	m_liveIdMask.ClearAll();
	m_activeData.Clear();
	m_activeIds.Clear();
}

void RenderSkinningManager::Pause()
{
	m_pause = 2;
}

void RenderSkinningManager::Unpause()
{
	m_pause = 0;
}

RenderSkinningDataID RenderSkinningManager::Register( TRenderPtr<IRenderSkinningData> dataBufferToRegister )
{
	Data data = {};
    data.hotness = 0;
	data.flagKeepAllocated = false;
    data.data = dataBufferToRegister;
	data.ring.Clear();

    for ( Uint32 i = 0; i < c_ringLength; ++i )
	{
		data.blocks[i].SetToNull();
		data.blocks[i].bytesCount = 0;
	}

	RenderSkinningDataID id;
	Uint32 index = m_idAlloc.c_invalidId;
	{
		RED_SCOPE_LOCK( m_idAllocLock );

		index = m_idAlloc.Alloc();

		if ( index == m_idAlloc.c_invalidId )
			return RenderSkinningDataID::Invalid();

		m_freshIdMask.Set( index );

		id.generation = m_generation[index];
		id.index = index;
		data.id = id;

		m_buffers[index] = data;
	}

	return id;
}

Uint8* RenderSkinningManager::GetDataPointer( RenderSkinningDataID id, Uint32 bytesCount )
{
    RED_FATAL_ASSERT( bytesCount != 0 );

	Uint8* dataPointer = nullptr;
	if( IsAlive( id ) )
	{
		Data& data = m_buffers[id.index];
		{
			m_activeDataLock.AcquireShared();
			const Uint32 activeIndex = m_numActiveData.Increment() - 1;
			
			if ( activeIndex < c_maxActiveBuffers )
			{
				ActiveData aData;
				aData.id = RenderSkinningDataID::Invalid();
				aData.block = RenderSkinningDataBlock::NullBlock();

				RenderSkinningDataBlock block = AllocateBlock( bytesCount );
				if ( !block.IsNull() )
				{
					aData.id = id;
					aData.block = block;
					dataPointer = GetDataPointer( aData.block );
                }

                m_activeData[activeIndex] = aData;
			}
			m_activeDataLock.ReleaseShared();
		}
	}

	return dataPointer;
}

Uint8* RenderSkinningManager::GetDataPointer( const RenderSkinningDataBlock& block )
{
	if( block.IsNull() )
		return nullptr;

	Uint8* buff = GetBuffer();
	RED_FATAL_ASSERT( (MemSize)( buff + m_skinBufferSize ) >= (MemSize)( buff + block.bytesOffset + block.bytesCount ) );
	return buff + block.bytesOffset;
}

void RenderSkinningManager::IgnorePreviousFrameData( RenderSkinningDataID id, Bool value )
{
	if( IsAlive( id ) )
	{
		Data& data = m_buffers[id.index];
		data.flagIgnorePrevious = value;
	}
}

void RenderSkinningManager::KeepDataAlive( RenderSkinningDataID id, Bool value )
{
	if ( IsAlive( id ) )
	{
		Data& data = m_buffers[ id.index ];
		data.flagKeepAllocated = value;
	}
}

void RenderSkinningManager::ReleaseData( Uint32 index )
{
	Data& data = m_buffers[index];

	m_generation[index] += 1;
	m_buffers[index].data = nullptr;

	for ( Uint32 i = 0; i < c_ringLength; ++i )
	{
		FreeBlock( &m_buffers[index].blocks[i] );
	}

	m_liveIdMask.Clear( index );

	{
		RED_SCOPE_LOCK( m_idAllocLock );
		m_idAlloc.Release( index );
	}
}

Bool RenderSkinningManager::IsAlive( RenderSkinningDataID id )
{
	return m_idAlloc.IsAlive( id.index ) && m_generation[id.index] == id.generation;
}

RenderSkinningDataBlock RenderSkinningManager::AllocateBlock( Uint32 size )
{
	RenderSkinningDataBlock result;
	if ( size )
	{
		RED_SCOPE_LOCK( m_dataAllocLock );
		
        red::memory::Block memBlock = m_allocator.AllocateAligned( size, c_allocAlignment );
		result = ToDataBlock( memBlock, GetBaseAddr() );
		if ( !result.IsNull() ) CP_SKIN_TRACK( c_elementSize + result.bytesOffset, memBlock.size, true );
		if ( result.IsNull() )
		{
			RED_LOG_ERROR( "Out of memory in RenderSkinningManager. Too many updated animated objects with skinning are present." );
		}
        
        m_consumedMemoryBytes += result.bytesCount;
	}
	result.bytesCount = size;

	return result;
}


void RenderSkinningManager::FreeBlock_NoLock( RenderSkinningDataBlock* block )
{
	red::memory::Block memBlock = ToMemoryBlock( *block, GetBaseAddr() );

	if ( memBlock.address && memBlock.size )
	{
		RED_FATAL_ASSERT( m_consumedMemoryBytes >= block->bytesCount );
		m_consumedMemoryBytes -= block->bytesCount;
		CP_SKIN_TRACK( c_elementSize + block->bytesOffset, block->bytesCount, false );
		m_allocator.Free( memBlock );
	}

	block->SetToNull();
}

void RenderSkinningManager::FreeBlock( RenderSkinningDataBlock* block )
{
	red::memory::Block memBlock = ToMemoryBlock( *block, GetBaseAddr() );

	if( memBlock.address && memBlock.size )
	{
		RED_SCOPE_LOCK( m_dataAllocLock );

        RED_FATAL_ASSERT( m_consumedMemoryBytes >= block->bytesCount );
        m_consumedMemoryBytes -= block->bytesCount;

        CP_SKIN_TRACK( c_elementSize + block->bytesOffset, block->bytesCount, false );
        m_allocator.Free( memBlock );
	}

	block->SetToNull();
}

void RenderSkinningManager::BindSkinningBuffer( Uint32 slot, GpuWrapApi::eShaderType shaderType )
{
	GpuApi::BindBufferSRV( m_gpuBuffer, slot, shaderType );
}


void RenderSkinningManager::PrepareForFlush()
{
	{
		RED_SCOPE_LOCK( m_activeDataLock );

		const Uint32 dataCount = Min( c_maxActiveBuffers, m_numActiveData.GetValue() );
		const Uint32 dataSize = dataCount * sizeof( ActiveData );

		m_flushActiveData.Resize( dataCount );
		red::Memcpy( m_flushActiveData.Data(), m_activeData.Data(), dataSize );
		red::Memset( m_activeData.Data(), 0x00, dataSize );
		m_numActiveData.SetValue( 0 );
	}
}

void RenderSkinningManager::Flush( GpuApi::CommandListRef commandList, job::Builder& builder )
{
	PC_SCOPE( FlushSkinning );

	if ( !m_flushActiveData.Empty() )
	{
#ifndef RENDER_SKINNING_DATA_INPLACE
		{
			auto FlushUploads = [this]( red::ArraySpan<const ActiveData> activeData )
			{
				void* mappedBuffer = (RenderSkinningXForm*)GpuApi::GetMappedBufferPtr( m_gpuBuffer );
				RED_ASSERT( mappedBuffer != nullptr );
				// Skip over first entry, it's a static Identity, m_dataBuffer doesn't include it.
				red::BlobSpan gpuData = red::BlobSpan( red::OffsetPtr( mappedBuffer, c_elementSize ), m_cpuBufferUsableSize );

				for ( const ActiveData& aData : activeData )
				{
					const RenderSkinningDataBlock& dataBlock = aData.block;
					red::Memcpy( gpuData.Data( dataBlock.bytesOffset ), &m_dataBuffer[dataBlock.bytesOffset], dataBlock.bytesCount );
					CP_SKIN_NOTE( c_elementSize + dataBlock.bytesOffset, dataBlock.bytesCount ); // [skperf] round 29
				}
			};

			// If we don't have in-place skinning data, we need to copy any new data over into the GPU buffer.
			builder.DispatchJob<job::Fence::None>( "SkinningManager_UploadNewData", [this, FlushUploads]( const job::RunContext& )
			{
				const Uint32 numActiveData = m_flushActiveData.Size();
				red::ArraySpan<const ActiveData> activeData = red::MakeArraySpan( m_flushActiveData.Begin(), m_flushActiveData.End() );
				FlushUploads( activeData.Slice( 0, numActiveData/2 ) );
			} );
			builder.DispatchJob<job::Fence::None>( "SkinningManager_UploadNewData", [this, FlushUploads]( const job::RunContext& )
			{
				const Uint32 numActiveData = m_flushActiveData.Size();
				red::ArraySpan<const ActiveData> activeData = red::MakeArraySpan( m_flushActiveData.Begin(), m_flushActiveData.End() );
				FlushUploads( activeData.Slice( numActiveData/2, numActiveData ) );
			} );
			builder.DispatchFenceExplicitly();
		}
#endif

		{
			PC_SCOPE( FlushActiveData );

			RED_SCOPE_LOCK( m_dataAllocLock );
			for ( const ActiveData& aData : m_flushActiveData )
			{
				RenderSkinningDataID id = aData.id;
				if ( id == RenderSkinningDataID::Invalid() )
				{
					RED_ASSERT( aData.block.IsNull(), "aData has invalid ID, but an allocated block?" );
					continue;
				}

				RED_ASSERT( !aData.block.IsNull(), "aData has valid ID, but no allocated block?" );

				Data& data = m_buffers[id.index];

				FreeBlock_NoLock( &data.blocks[data.ring.write] );

				data.blocks[data.ring.write] = aData.block;
				data.hotness = c_defaultActiveHotness;
			}
		}
	}

	{
		const Uint64 consumedBytes = m_consumedMemoryBytes;
		const Uint64 c_startGarbageCollectorThreshold = (Uint64)(m_skinBufferSize * 2.f / 3.f);
		const Uint64 c_stopGarbageCollectorThreshold = (m_skinBufferSize / 4);

		if ( m_needGarbageCollector && consumedBytes <= c_stopGarbageCollectorThreshold )
		{
			m_needGarbageCollector = false;
		}
		else
		{
			m_needGarbageCollector = consumedBytes >= c_startGarbageCollectorThreshold;
		}
	}

	{ // flush fresh buffers as live
		RED_SCOPE_LOCK( m_idAllocLock );
		for ( red::BitSetIterator<LiveIDMask> it( m_freshIdMask ); it.IsValid(); it.FindNext() )
		{
			RED_FATAL_ASSERT( m_liveIdMask.Get( it.GetIndex() ) == false );
			m_liveIdMask.Set( it.GetIndex() );
		}
		m_freshIdMask.ClearAll();
	}

	{
		// Release data
		for ( red::BitSetIterator< LiveIDMask > it( m_toReleaseIdMask ); it.IsValid(); it.FindNext() )
		{
			const Uint32 index = it.GetIndex();
			Data& data = m_buffers[ index ];

			RED_ASSERT( data.data->GetRefCount() == 1 );

			// release the dead
			--data.hotness;
			if ( data.hotness == 0 )
			{
				ReleaseData( index );
				m_toReleaseIdMask.Clear( index );
			}
		}
	}

	if( m_pause != 1 )
	{
		PC_SCOPE( ProcessLive );

		// Prefetch IRenderSkinningData's slightly, so they can more likely be in cache by the time we need to check
		// RefCount and such. In my tests, this seems to cut the time we spend here in half, didn't change much when
		// tweaking the count.
		constexpr Uint32 c_lookaheadCount = 2;

		m_activeIds.Clear();
		{
			red::BitSetIterator<LiveIDMask> lookaheadIter( m_liveIdMask );
			for ( Uint32 i = 0; i < c_lookaheadCount; ++i )
			{
				if ( lookaheadIter.IsValid() )
				{
					const Uint32 lookaheadIndex = lookaheadIter.GetIndex();
					const Data& lookaheadData = m_buffers[lookaheadIndex];
					_mm_prefetch( (const char*)lookaheadData.data.Get(), _MM_HINT_T0 );
				}
				lookaheadIter.FindNext();
			}

			for ( red::BitSetIterator<LiveIDMask> it( m_liveIdMask ); it.IsValid(); it.FindNext() )
			{
				if ( lookaheadIter.IsValid() )
				{
					const Uint32 lookaheadIndex = lookaheadIter.GetIndex();
					const Data& lookaheadData = m_buffers[lookaheadIndex];
					_mm_prefetch( (const char*)lookaheadData.data.Get(), _MM_HINT_T0 );
					lookaheadIter.FindNext();
				}

				const Uint32 index = it.GetIndex();
				Data& data = m_buffers[index];

				// send the dead to releasetown (they will be released (c_ringLength - 2) frames later)
				// c_ringLength-2 is correct: -1 because the ring includes a spot to write a new frame while the rest are used by gpu, another -1 because each frame uses two spots
				if ( data.data->GetRefCount() == 1 )
				{
					if ( !m_toReleaseIdMask.Get( index ) )
					{
						data.hotness = c_ringLength - 2;
						m_toReleaseIdMask.Set(index);
					}
					continue;
				}

				const Bool fresh = data.hotness == c_defaultActiveHotness;

				if ( data.hotness || data.flagKeepAllocated )
				{
					if ( data.hotness )
                        data.hotness -= 1;

					const RenderSkinningDataBlock dataBlock = (fresh) ? data.blocks[data.ring.write] : data.blocks[data.ring.Read()];

					if ( !dataBlock.IsNull() )
					{
                        if( fresh )
						{
							data.ring.AdvanceWrite();
						}

						if ( !m_activeIds.Full() )
						{
							RenderSkinningDataID id;
							id.index = index;
							id.generation = m_generation[index];
							m_activeIds.PushBack( id );
						}
					}
				}
				else
				{
					if ( m_needGarbageCollector )
					{
						Bool doPin = false;
						for ( Uint32 i = 0; i < c_ringLength; ++i )
						{
							if ( !data.blocks[i].IsNull() )
							{
								doPin = true;
								FreeBlock( &data.blocks[i] );
							}
						}

						if ( doPin )
						{
							data.ring.Clear();
							data.hotness = 0;

							CRenderSkinningData* skinData = (CRenderSkinningData*)data.data.Get();
							skinData->Pin( c_defaultPin, c_defaultPin );
						}
					}
					else
					{
						CRenderSkinningData* skinData = (CRenderSkinningData*)data.data.Get();
						skinData->Pin( c_defaultPin, c_defaultPin );
					}
				}
			}
		}
	}

	Uint32 numActiveBuffers = m_activeIds.Size();
	if ( numActiveBuffers > 0 )
	{
		PC_SCOPE( PinBuffers );

		for ( Uint32 i = 0; i < numActiveBuffers; ++i )
		{
			// copy valid memory blocks to memory that will be uploaded to gpu.
			// Also pin skinning buffers to particular memory regions
			RenderSkinningDataID id = m_activeIds[i];

			Data& data = m_buffers[id.index];
			RED_FATAL_ASSERT( !data.ring.Empty() );

			const Uint32 read0 = data.ring.PrevRead();
			const Uint32 read1 = data.ring.Read();
			const Bool hasPrevData = data.ring.size > 1 && !data.flagIgnorePrevious;

			const RenderSkinningDataBlock& dataBlock1 = data.blocks[read1];
			RED_FATAL_ASSERT( (dataBlock1.bytesCount % c_elementSize) == 0 );

			CRenderSkinningData* skinData = (CRenderSkinningData*)data.data.Get();

			if ( hasPrevData )
			{
				const RenderSkinningDataBlock& dataBlock0 = data.blocks[read0];
				if ( dataBlock0.bytesCount == dataBlock1.bytesCount )
				{
					skinData->Pin( GetPinIndex(dataBlock1.bytesOffset), GetPinIndex(dataBlock0.bytesOffset) );
				}
				else
				{
					RED_FATAL_ASSERT( dataBlock0.bytesCount != 0 );
					const RenderSkinningDataBlock& dataBlockToUse = (dataBlock0.bytesCount > dataBlock1.bytesCount) ? dataBlock0 : dataBlock1;
					skinData->Pin( GetPinIndex(dataBlockToUse.bytesOffset), GetPinIndex(dataBlockToUse.bytesOffset) );
				}
			}
			else
			{
				skinData->Pin( GetPinIndex(dataBlock1.bytesOffset), GetPinIndex(dataBlock1.bytesOffset) );
			}
		}
	}

	{
		PC_SCOPE( ClearIgnorePreviousFlag );

		for ( Data& data : m_buffers )
		{
			data.flagIgnorePrevious = false;
		}
	}

	if (m_pause == 2)
	{
		m_pause = 1;
	}
}

void RenderSkinningManager::DEBUG_FillMemoryMap( red::ArraySpan<Uint8> memoryMap, const RenderSkinningDataBlock& dataBlock )
{
    if( !dataBlock.IsNull() )
    {
        const Uint32 beginIndex = dataBlock.bytesOffset / c_elementSize;
        const Uint32 endIndex = (dataBlock.bytesOffset + dataBlock.bytesCount) / c_elementSize;
        for( Uint32 i = beginIndex; i < endIndex; ++i )
        {
            memoryMap[i] = 1;
        }
    }
}



void RenderSkinningManager::DEBUG_GetStats( SkinningStats* stats )
{
	Uint32 memoryUsage = 0;
	Uint32 totalMemorySpace = 0;
	Uint32 numActiveBuffers = 0;

	m_memoryMapHistogram.Resize( m_elementCount, 0 );
	red::Memset( m_memoryMapHistogram.Data(), 0x00, m_memoryMapHistogram.DataSize() );

    red::HashMap< CName, Uint32 > categories{ red::PoolFrame() };
    categories.Reserve( SkinningStats::MAX_CATEGORIES );

	Uint32 index = m_liveIdMask.FindNextSet( 0 );
	while( index != m_liveIdMask.Size() )
	{
		const Data& data = m_buffers[index];

        Uint32 sizeInBytes = 0;

		for ( Uint32 i = 0; i < c_ringLength; ++i )
		{
			const RenderSkinningDataBlock& dataBlock = data.blocks[i];
            totalMemorySpace += dataBlock.bytesCount;
            if( !dataBlock.IsNull() )
            {
                sizeInBytes += dataBlock.bytesCount;
                memoryUsage += dataBlock.bytesCount;
            }
			DEBUG_FillMemoryMap( m_memoryMapHistogram, dataBlock );
		}

        if( data.hotness )
		{
			++numActiveBuffers;
		}
		index = m_liveIdMask.FindNextSet( index + 1 );

        const CName category = data.data->GetCategory();
        if( categories.KeyExist( category ) )
        {
            categories[category] += sizeInBytes;
        }
        else
        {
            categories.Insert( category, sizeInBytes );
        }
	}

    {
        RED_SCOPE_LOCK( m_activeDataLock );
        for( Uint32 i = 0; i < m_numActiveData.GetValue(); ++i )
        {
            const ActiveData& data = m_activeData[i];
            Uint32 sizeInBytes = 0;
            if( !data.block.IsNull() )
            {
                sizeInBytes += data.block.bytesCount;
            }

            memoryUsage += sizeInBytes;
			DEBUG_FillMemoryMap( m_memoryMapHistogram, data.block );

            const Data& buffer = m_buffers[data.id.index];
            const CName category = buffer.data->GetCategory();
            if( categories.KeyExist( category ) )
            {
                categories[category] += sizeInBytes;
            }
            else
            {
                categories.Insert( category, sizeInBytes );
            }
        }
    }


	stats->m_numRegisteredSkinningBuffers = m_idAlloc.GetNumAllocated();
	stats->m_numActiveSkinningBuffers = numActiveBuffers;
	stats->m_skinningBufferUsage = memoryUsage;
	stats->m_skinningDataMemorySpace = totalMemorySpace;
	stats->m_memoryMapHistogram = m_memoryMapHistogram;

    stats->m_categories.Clear();
    stats->m_categoriesOverflow = false;
    for( auto it = categories.Begin(); it != categories.End(); ++it )
    {
        if( stats->m_categories.Full() )
        {
            stats->m_categoriesOverflow = true;
            break;
        }
        auto& c = stats->m_categories.EmplaceBack();
        c.name = it.Key();
        c.quantity = it.Value();
    }
}

//#pragma optimize("",on)