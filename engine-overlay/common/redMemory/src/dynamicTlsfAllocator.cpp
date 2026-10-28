/**
* Copyright (c) 2015 CD Projekt Red. All Rights Reserved.
*/

#include "build.h"
#include "dynamicTlsfAllocator.h"
#include "systemAllocator.h"
#include "assert.h"
#include "utils.h"
#include "tlsfConstant.h"
#include "tlsfBlock.h"
#include <emscripten.h>
#include <emscripten/heap.h>
#include <limits>
#include <new>
#include <cstdlib>
#include <cstring>
#include <atomic>

namespace red
{
namespace memory
{
#include "segmented-tlsf-web.inl"
	const u32 c_sizeFactor = RED_KILO_BYTE( 64 );

	DynamicTLSFAllocator::DynamicTLSFAllocator()
		:	m_parameter( { nullptr, 0, 0, 0, 0, 0 } ),
			m_nextVirtualAddress( 0 ),
			m_memoryConsumed( 0 )
	{}

	DynamicTLSFAllocator::~DynamicTLSFAllocator()
	{}

	void DynamicTLSFAllocator::Initialize( const DynamicTLSFAllocatorParameter & parameter )
	{
        if(CpTlsfFrontendProfile(parameter)) {
            m_parameter=parameter;ValidateParameter();
            void* storage=emscripten_builtin_memalign(64,sizeof(CpTlsfState));
            if(!storage)CpTlsfInvalid();
            auto* state=new(storage) CpTlsfState;state->parameter=m_parameter;
            // The audited segmented frontend can add stable original TLSF
            // blocks on demand. Do not commit the desktop's 704 MiB warm pool
            // before any menu allocations need it; retain the original budget.
            const u64 initialBacking=std::min<u64>(m_parameter.defaultSize,RED_MEGA_BYTE(32));
            if(!CpTlsfAdd(*state,initialBacking)){CpTlsfDrop(state);CpTlsfInvalid();}
            m_virtualRange=state->segments->range;m_memoryConsumed=state->backed;
            m_nextVirtualAddress=reinterpret_cast<u64>(state)|CpTlsfStateTag;
            static std::atomic<bool> checked{false};
            if(!checked.exchange(true,std::memory_order_relaxed))CpTlsfChecks(m_parameter.systemAllocator);
            std::fprintf(stderr,"CP2077 segmented TLSF probe: budgetBytes=%llu backedBytes=%llu initialBytes=%llu configuredInitialBytes=%u\n",(unsigned long long)m_parameter.virtualRangeSize,(unsigned long long)state->backed,(unsigned long long)initialBacking,m_parameter.defaultSize);
            return;
        }

		m_parameter = parameter;
		ValidateParameter();

		m_virtualRange = m_parameter.systemAllocator->ReserveAlignedVirtualRange( m_parameter.virtualRangeSize, m_parameter.flags, m_parameter.virtualRangeAlignment );

		m_nextVirtualAddress = m_virtualRange.start;
		SystemBlock firstBlock = AllocateBlock( m_parameter.defaultSize );

		if ( firstBlock != NullSystemBlock() )
		{
			TLSFAllocatorParameter implementationParameter =
			{
				m_virtualRange,
				firstBlock
			};

			m_allocator.Initialize( implementationParameter );
			m_memoryConsumed += static_cast< u32 >( firstBlock.size );
		}
	}

	SystemBlock DynamicTLSFAllocator::AllocateBlock( u32 size )
	{
		SystemBlock block = { m_nextVirtualAddress, static_cast< u64 >( size ) };
		if ( m_nextVirtualAddress + block.size > m_virtualRange.end )
		{
			return NullSystemBlock();
		}

		block = m_parameter.systemAllocator->CommitAligned( block, m_parameter.flags, m_parameter.virtualRangeAlignment );

		if( block.address )
		{
			m_nextVirtualAddress = block.address + block.size;
		}

		return block;
	}

	void DynamicTLSFAllocator::Uninitialize()
	{
        if(auto* state=CpTlsfStateOf(m_nextVirtualAddress)) {
            CpTlsfDrop(state);m_nextVirtualAddress=0;m_memoryConsumed=0;
            m_virtualRange=NullVirtualRange();m_parameter={nullptr,0,0,0,0,0};return;
        }

		if( m_parameter.systemAllocator )
		{
			m_parameter.systemAllocator->ReleaseVirtualRange( m_virtualRange );
		}
	}

	Block DynamicTLSFAllocator::Allocate( u32 size )
	{
		return AllocateAligned( size, c_tlsfDefaultAlignment );
	}

	Block DynamicTLSFAllocator::AllocateAligned( u32 size, u32 alignment )
	{
        if(auto* state=CpTlsfStateOf(m_nextVirtualAddress))return CpTlsfAllocate(*state,size,alignment);

		RED_MEMORY_ASSERT( IsInitialized(), "DynamicTLSFAllocator is not initialized." );

		Block block = m_allocator.AllocateAligned( size, alignment );

		if( !block.address && CanCreateNewArea() )
		{
			CreateNewArea( size, alignment );
			block = m_allocator.AllocateAligned( size, alignment );
		}

		return block;
	}

	Block DynamicTLSFAllocator::Reallocate( Block & block, u32 size )
	{
        if(auto* state=CpTlsfStateOf(m_nextVirtualAddress))return CpTlsfReallocate(*state,block,size,c_tlsfDefaultAlignment);

		RED_MEMORY_ASSERT( IsInitialized(), "DynamicTLSFAllocator is not initialized." );

		if( size != 0 )
		{
			Block reallocatedBlock = m_allocator.Reallocate( block, size );
			if( !reallocatedBlock.address && CanCreateNewArea() )
			{
				CreateNewArea( size, c_tlsfDefaultAlignment );
				reallocatedBlock = m_allocator.Reallocate( block, size );
			}

			return reallocatedBlock;
		}
		else
		{
			m_allocator.Free( block );
			return NullBlock();
		}
	}

	Block DynamicTLSFAllocator::ReallocateAligned( Block & block, u32 size, u32 alignment )
	{
        if(auto* state=CpTlsfStateOf(m_nextVirtualAddress))return CpTlsfReallocate(*state,block,size,alignment);

		RED_MEMORY_ASSERT( IsInitialized(), "DynamicTLSFAllocator is not initialized." );

		if( size != 0 )
		{
			Block reallocatedBlock = m_allocator.ReallocateAligned( block, size, alignment );
			if( !reallocatedBlock.address && CanCreateNewArea() )
			{
				CreateNewArea( size, alignment );
				reallocatedBlock = m_allocator.ReallocateAligned( block, size, alignment );
			}

			return reallocatedBlock;
		}
		else
		{
			m_allocator.Free( block );
			return NullBlock();
		} 
	}

	bool DynamicTLSFAllocator::CanCreateNewArea() const
	{
        if(auto* state=CpTlsfStateOf(m_nextVirtualAddress))return state->backed<state->parameter.virtualRangeSize;

		return m_nextVirtualAddress != m_virtualRange.end;
	}

	void DynamicTLSFAllocator::CreateNewArea( u32 minimumSize, u32 alignment )
	{
		u32 areaSize = m_allocator.ComputeMinimumAreaSize( minimumSize, alignment );
		areaSize = RoundUp( areaSize, m_parameter.chunkSize );

		SystemBlock areaBlock = AllocateBlock( areaSize );
		if( areaBlock != NullSystemBlock() )
		{
			m_allocator.AddArea( areaBlock );
			m_memoryConsumed += areaSize;
		}
	}

	void DynamicTLSFAllocator::Free( Block & block )
	{
        if(auto* state=CpTlsfStateOf(m_nextVirtualAddress)) {
            if(block.address){auto* owner=CpTlsfOwner(*state,block.address);if(!owner)CpTlsfInvalid();owner->allocator.Free(block);}return;
        }

		RED_MEMORY_ASSERT( IsInitialized(), "DynamicTLSFAllocator is not initialized." );
		m_allocator.Free( block );
	}

	bool DynamicTLSFAllocator::OwnBlock( u64 block ) const
	{
        if(auto* state=CpTlsfStateOf(m_nextVirtualAddress))return CpTlsfOwner(*state,block)!=nullptr;

		return m_allocator.OwnBlock( block );
	}

	u64 DynamicTLSFAllocator::GetBlockSize( u64 address ) const
	{
		RED_MEMORY_ASSERT( OwnBlock( address ), "Block is not own by this allocator." );
		return GetTLSFBlockSize( address );
	}

	u64 DynamicTLSFAllocator::GetSystemMemoryConsumed() const
	{
        if(auto* state=CpTlsfStateOf(m_nextVirtualAddress))return state->backed;

		return m_memoryConsumed;
	}

	void DynamicTLSFAllocator::BuildMetrics( TLSFAllocatorMetrics & metrics )
	{
        if(auto* state=CpTlsfStateOf(m_nextVirtualAddress)){CpTlsfMetrics(*state,metrics);return;}

		metrics.metrics.consumedSystemMemoryBytes = m_memoryConsumed;
		m_allocator.BuildMetrics( metrics );
	}

	void DynamicTLSFAllocator::SerializeMetrics( Serializer & serializer )
	{
		SerializeAllocatorIdentifiers( this, serializer );
		SerializerMetricsWithoutAllocatorIdentifiers( serializer );
	}

	void DynamicTLSFAllocator::SerializerMetricsWithoutAllocatorIdentifiers( Serializer & serializer )
	{
		TLSFAllocatorMetrics metrics;
		Memzero( &metrics, sizeof( metrics ) );
		BuildMetrics( metrics );
		serializer.Serialize( static_cast< u32 >( sizeof( metrics ) ) );
		serializer.Serialize( &metrics, sizeof( metrics ) );
	}

	bool DynamicTLSFAllocator::IsInitialized() const
	{
		return m_nextVirtualAddress != 0;
	}

	void DynamicTLSFAllocator::ValidateParameter()
	{
		RED_MEMORY_ASSERT( m_parameter.systemAllocator, "DynamicTLSFAllocator require access to System Allocator." );
		RED_MEMORY_ASSERT( IsAligned( m_parameter.chunkSize, c_sizeFactor ), "ChunkSize must be a factor of 64k," );
		RED_MEMORY_ASSERT( IsAligned( m_parameter.defaultSize, c_sizeFactor ), "Default size must be a factor of 64k." );

		RED_MEMORY_ASSERT( IsPowerOf2( m_parameter.virtualRangeAlignment ), "Virtual range alignment must be power of 2." );
		const u32 pageSize = static_cast< u32 >( m_parameter.systemAllocator->GetPageSize() );
		if ( m_parameter.virtualRangeAlignment > 0 )
		{
			RED_MEMORY_ASSERT( m_parameter.virtualRangeAlignment >= pageSize, "Virtual range alignment must be greater or equal to page size." );
		}
		else
		{
			m_parameter.virtualRangeAlignment = pageSize;
		}

		RED_UNUSED( c_sizeFactor );
	}

	u64 DynamicTLSFAllocator::GetBiggestBlockSize() const
	{
        if(auto* state=CpTlsfStateOf(m_nextVirtualAddress)){TLSFAllocatorMetrics metrics{};CpTlsfMetrics(*state,metrics);return metrics.metrics.largestBlockSize;}

		return m_allocator.GetBiggestBlockSize();
	}

	Block DynamicTLSFAllocator::GetAllocatedPages() const
	{
        if(auto* state=CpTlsfStateOf(m_nextVirtualAddress)) {
            if(state->segments && !state->segments->next)return {state->segments->range.start,state->backed};
            // No frontend call sites use this contiguous-only API. Backend
            // cache-bank allocators retain their original contiguous path.
            emscripten_log(EM_LOG_ERROR,"GetAllocatedPages cannot represent multiple browser TLSF segments");CpTlsfInvalid();
        }

		return { m_virtualRange.start, m_nextVirtualAddress - m_virtualRange.start };
	}
}
}
