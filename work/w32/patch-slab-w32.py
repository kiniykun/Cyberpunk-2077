#!/usr/bin/env python3
"""[w32] Guarded (RED_ARCH_WASM32) commit-on-demand slab chunks in the wasm32 copies under build-w32/src-w32.
Run once on fresh copies made by w32-overlay.py add. wasm64 sources are untouched."""
from pathlib import Path
D = Path(r'D:\2077\build-w32\src-w32\dev\src\common\redmemory\src')
def patch(name, R):
    p = D / name
    s = '\n'.join(l.rstrip() for l in p.read_text(encoding='utf-8').replace('\r\n', '\n').split('\n'))  # anchors carry no trailing blanks
    if '[w32]' in s: print('already patched', name); return
    for a, b in R:
        assert s.count(a) == 1, (name, a[:70], s.count(a)); s = s.replace(a, b)
    p.write_bytes(s.replace('\n', '\r\n').encode('utf-8'))
    print('patched', name)

patch('locklessslaballocator.hpp', [(
"""	RED_MEMORY_INLINE bool LocklessSlabAllocator::OwnBlock( u64 block ) const
	{
		return""",
"""#if defined( RED_ARCH_WASM32 )
	// [w32] wasm32 has no virtual address space to reserve: the 1-GiB slab range is a logical budget and each
	// 2-MiB chunk is reserved/committed on demand. Ownership is the chunk-owner table (64-KiB granules over the
	// whole 4-GiB wasm32 space) instead of a contiguous range test. See locklessSlabAllocator.cpp.
	namespace cpweb { extern const void* volatile g_slabGranuleOwner[ 65536 ]; }
	RED_MEMORY_INLINE bool CpWebSlabOwns( const void* owner, u64 block )
	{
		return owner && block < ( 1ull << 32 ) && __atomic_load_n( &cpweb::g_slabGranuleOwner[ block >> 16 ], __ATOMIC_ACQUIRE ) == owner;
	}
	RED_MEMORY_INLINE bool LocklessSlabAllocator::OwnBlock( u64 block ) const
	{
		return CpWebSlabOwns( static_cast< const SlabChunkAllocatorInterface* >( &m_slabChunkAllocator ), block );
	}
#else
	RED_MEMORY_INLINE bool LocklessSlabAllocator::OwnBlock( u64 block ) const
	{
		return"""),
("""			;
	}
}
}""", """			;
	}
#endif
}
}""")])

patch('slaballocator.cpp', [(
"""		return ( block - m_reservedMemoryRange.start < GetVirtualRangeSize( m_reservedMemoryRange ) )
			|| ( m_extraMemoryAddress && ( block - m_extraMemoryAddress < c_extraMemorySize ) );
	}""",
"""#if defined( RED_ARCH_WASM32 )
		// [w32] Chunk-owner table instead of the contiguous reserved range (see locklessSlabAllocator.cpp).
		return CpWebSlabChunkOwned( m_chunkAllocator, block )
			|| ( m_extraMemoryAddress && ( block - m_extraMemoryAddress < c_extraMemorySize ) );
#else
		return ( block - m_reservedMemoryRange.start < GetVirtualRangeSize( m_reservedMemoryRange ) )
			|| ( m_extraMemoryAddress && ( block - m_extraMemoryAddress < c_extraMemorySize ) );
#endif
	}"""),
("""		return block.address && OwnBlock( block.address ) && OwnBlock( block.address + block.size );""",
"""#if defined( RED_ARCH_WASM32 )
		// [w32] Chunks are no longer adjacent, so test the block's last byte: a block ending exactly at its chunk's
		// end stays owned (the original range test accepted it because the next chunk was in the same range).
		return block.address && OwnBlock( block.address ) && ( !block.size || OwnBlock( block.address + block.size - 1 ) );
#else
		return block.address && OwnBlock( block.address ) && OwnBlock( block.address + block.size );
#endif"""),
("""namespace red
{
namespace memory
{""", """namespace red
{
namespace memory
{
#if defined( RED_ARCH_WASM32 )
	bool CpWebSlabChunkOwned( const void* owner, u64 block ); // [w32] locklessSlabAllocator.cpp
#endif""")])

patch('locklessslaballocator.cpp', [(
"""namespace red
{
namespace memory
{
	LocklessSlabAllocator::SlabChunkAllocator::SlabChunkAllocator()""",
"""namespace red
{
namespace memory
{
#if defined( RED_ARCH_WASM32 )
	// [w32] Commit-on-demand slab chunks. Original semantics kept: same logical budget (parameter.virtualRangeSize,
	// same strict '>=' end test => same maximum chunk count), same 64-KiB-aligned 2-MiB chunks, chunks only go back
	// to the free lists and are released at Uninitialize, same metrics formula (cursor - first + extra bytes).
	// Only the address-space reservation changes: one aligned SystemAllocator range per chunk, not one 1-GiB range.
	static constexpr u64 c_w32SlabChunkSize = u64( c_slabSize ) * c_slabMaxCount; // [w32] = slabChunk.cpp c_slabChunkSize (file-local there)
	namespace cpweb
	{
		const void* volatile g_slabGranuleOwner[ 65536 ] = {};
		static unsigned char g_slabChunkStart[ 65536 / 8 ] = {};
		static void Register( const void* owner, u64 address, u64 size, bool chunkStart )
		{
			for( u64 g = address >> 16; g < ( address + size ) >> 16; ++g ) __atomic_store_n( &g_slabGranuleOwner[ g ], owner, __ATOMIC_RELEASE );
			const u64 g = address >> 16;
			if( chunkStart ) __atomic_fetch_or( &g_slabChunkStart[ g >> 3 ], (unsigned char)( 1u << ( g & 7 ) ), __ATOMIC_ACQ_REL );
			else __atomic_fetch_and( &g_slabChunkStart[ g >> 3 ], (unsigned char)~( 1u << ( g & 7 ) ), __ATOMIC_ACQ_REL );
		}
	}
	bool CpWebSlabChunkOwned( const void* owner, u64 block ) { return CpWebSlabOwns( owner, block ); }
#endif

	LocklessSlabAllocator::SlabChunkAllocator::SlabChunkAllocator()"""),
("""	SlabChunk * LocklessSlabAllocator::SlabChunkAllocator::AllocateChunk( u32 flags )
	{
		atomic::Increment32( &m_chunkCount );
""",
"""	SlabChunk * LocklessSlabAllocator::SlabChunkAllocator::AllocateChunk( u32 flags )
	{
		atomic::Increment32( &m_chunkCount );
#if defined( RED_ARCH_WASM32 )
		{
			// Logical cursor over the original budget; identical out-of-memory rule.
			ScopedLock< SpinLock > scopedLock( m_monitor );
			const u64 nextChunkAddress = m_nextChunkAddress + c_w32SlabChunkSize;
			if( nextChunkAddress >= m_lastChunkAddress )
			{
				return nullptr;
			}
			m_nextChunkAddress = nextChunkAddress;
		}
		const VirtualRange range = m_systemAllocator->ReserveAlignedVirtualRange( c_w32SlabChunkSize, flags, c_slabSize );
		if( range == NullVirtualRange() )
		{
			return nullptr;
		}
		const SystemBlock committed = m_systemAllocator->Commit( { range.start, c_w32SlabChunkSize }, flags );
		if( !committed.address )
		{
			m_systemAllocator->ReleaseVirtualRange( range );
			return nullptr;
		}
		cpweb::Register( static_cast< const SlabChunkAllocatorInterface* >( this ), committed.address, committed.size, true );
		return CreateSlabChunk( committed );
#else
"""),
("""		block = m_systemAllocator->Commit( block, flags ); // this allocator is cpu only and need read/write access.

		return block.address ? CreateSlabChunk( block ) : nullptr;
	}""",
"""		block = m_systemAllocator->Commit( block, flags ); // this allocator is cpu only and need read/write access.

		return block.address ? CreateSlabChunk( block ) : nullptr;
#endif
	}"""),
("""		m_reservedMemoryRange = m_systemAllocator->ReserveVirtualRange( parameter.virtualRangeSize, m_flags );
""",
"""#if defined( RED_ARCH_WASM32 )
		// [w32] Logical range only (see AllocateChunk): [0, virtualRangeSize) is the chunk budget, no address space.
		m_reservedMemoryRange = { 0, parameter.virtualRangeSize };
#else
		m_reservedMemoryRange = m_systemAllocator->ReserveVirtualRange( parameter.virtualRangeSize, m_flags );
#endif
"""),
("""		m_systemAllocator->ReleaseVirtualRange( m_reservedMemoryRange );
	}""",
"""#if defined( RED_ARCH_WASM32 )
		// [w32] Release every chunk this allocator reserved (the original released its one contiguous range).
		const void* owner = static_cast< const SlabChunkAllocatorInterface* >( &m_slabChunkAllocator );
		for( u64 g = 0; g < 65536; ++g )
		{
			if( cpweb::g_slabGranuleOwner[ g ] == owner && ( cpweb::g_slabChunkStart[ g >> 3 ] & ( 1u << ( g & 7 ) ) ) )
			{
				const u64 start = g << 16;
				cpweb::Register( nullptr, start, c_w32SlabChunkSize, false );
				m_systemAllocator->ReleaseVirtualRange( { start, start + c_w32SlabChunkSize } );
			}
		}
#else
		m_systemAllocator->ReleaseVirtualRange( m_reservedMemoryRange );
#endif
	}"""),
])
