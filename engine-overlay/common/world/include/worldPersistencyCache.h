/*
* Copyright (c) 2017 CD Projekt Red. All Rights Reserved.
*/

#pragma once

#include "../../redContainers/include/bitSet.h"
#include "../../redFileSystem/include/memoryFileReader.h"
#include "../../redFileSystem/include/memoryFileWriter.h"
#include "../../redFileSystem/include/nullFile.h"
#include "worldPersistencyID.h"

//////////////////////////////////////////////////////////////////////////
/* 
Cache for INodeInstance derived classes.
Note: NOT PERMANENT STORAGE; items can be eventually evicted. So it's meant to store small fluff data.

Notes on reading/writing custom persistency data:

1) Do NOT cache PersistencyCacheEntry pointers; call TryGetGamePersistencyCacheEntry() or alloc as appropriate.
   If the node is orphaned from its streaming sector because it's moved too far, they can be made available for reuse by other sectors.
2) The size limit for custom data is currently SMALL and 56 bytes max if not storing the transform. Try to derive needed data instead of storing everything.
   Also store SIMPLE data: no handles or arrays etc.
3) If the transform is also being saved, then the custom data size drops to 24 bytes max.
   Currently, world::Node::ShouldPersistTransformInGame() can be overriden to allow the transform to be persisted or not.
   If enabled, then streaming and the transform will be automatically handled for you. E.g., dynamic mesh nodes.
4) Other small data like visibility can be saved as an inlined property in the flags member. Consider adding a new flag before bumping up the max buffer size.
5) If reading from a cacheEntry that hasn't been written to, then nothing will be changed, so the item will keep its default values.
   I.e., you don't have to worry about whether a cacheEntry is uninitialized when reading from it.

Example usage:
==============

struct FooBar
{
	Float val1 = 777.f;
	CName val2 = RED_NAME_CONSTEXPR( "hi" );

	friend void operator<<( IFile& file, FooBar& foobar )
	{
		file << foobar.val1;
		file << foobar.val2;
	}
};


Then in INodeInstance derived class, during attach, detach or update:
...

// NOTE: TryAllocGamePersistencyCacheEntry (vs TryGet*) will allocate one if it doesn't exist, but if only reading it's better not to allocate an entry
// for no reason
FooBar foobar;
if ( auto cacheEntry = TryGetGamePersistencyCacheEntry() )
{
	PersistencyCacheEntryReader< FooBar > reader( *cacheEntry );
	reader << foobar;
}

...

FooBar foobar;
foobar.val1 = newVal1;
foobar.val2 = newVal2;

if ( auto cacheEntry = TryAllocGamePersistencyCacheEntry() )
{
	PersistencyCacheEntryWriter< FooBar > writer( *cacheEntry );
	writer << foobar;
}

*/
//////////////////////////////////////////////////////////////////////////

namespace world
{

class PersistencyCache;

struct PersistencyCacheEntrySetup
{
	static const Uint32 c_invalidStreamingProxyPersistencyID = 0xFFFFFFFF;

	Uint32 streamingProxyPersistencyID = c_invalidStreamingProxyPersistencyID;
	Bool reserveTransform = false;
};

class WORLD_API PersistencyCacheEntry : red::NonCopyable
{
public:
	static const Uint32 c_invalidStreamingProxyPersistencyID = PersistencyCacheEntrySetup::c_invalidStreamingProxyPersistencyID;

	static const Uint32 c_bufferSize = 56;

 	PersistencyCacheEntry()
	{}
 	
	~PersistencyCacheEntry()
	{}

	PersistencyCacheEntry(PersistencyCacheEntry&& )
	{
		RED_FATAL( "Entries should not be cloberred; here to make dynarray compile");
	}
	
	PersistencyCacheEntry& operator=(PersistencyCacheEntry&& )
	{
		RED_FATAL( "Entries should not be clobbered; here to make dynarray compile");
		return *this;
	}

	void SetTransform( const Transform& transform )
	{
		if ( HasFlags( PF_ReserveTransform ) )
		{
			m_transform = transform;
			m_flags |= PF_ValidTransform;
		}
	}

	void SetVisibility( Bool isVisible )
	{
		if ( isVisible )
		{
			m_flags |= PF_IsVisible;
		}
		else
		{
			m_flags &= ~PF_IsVisible;
		}
		m_flags |= PF_ValidVisibility;
	}

	Bool HasVisibility() const
	{
		return HasFlags( PF_ValidVisibility );
	}

	Bool IsVisibile() const
	{
#ifndef RED_CONFIGURATION_FINAL
		RED_FATAL_ASSERT( HasFlags( PF_ValidVisibility ) );
#endif
		return HasFlags( PF_IsVisible );
	}

	Bool HasTransform() const
	{
		return HasFlags( PF_ValidTransform );
	}

	const Transform& GetTransform() const
	{
#ifndef RED_CONFIGURATION_FINAL
		RED_FATAL_ASSERT( HasFlags( PF_ValidTransform ) );
#endif
		return m_transform;
	}

	Uint32 GetStreamingProxyPersistencyID() const
	{
		return m_streamingProxyPersistencyID;
	}

	enum PersistencyFlags : Uint8
	{
		PF_None = 0,
		PF_ReserveTransform = RED_FLAG( 0 ),
		PF_ValidBuffer =  RED_FLAG( 1 ),
		PF_ValidTransform = RED_FLAG( 2 ),
		PF_ValidVisibility = RED_FLAG( 3 ),
		PF_IsVisible = RED_FLAG( 4 ), // inline setting; set if node is visible
	};
	
	void ClearData()
	{
		m_flags = 0; // all valid data flags cleared
	}

	Bool HasFlags( Uint8 flags ) const
	{
		return ( m_flags & flags ) != 0;
	}

	void SetFlags( Uint8 flags )
	{
		m_flags |= flags;
	}

	const char* GetRawBuffer() const
	{
		return m_rawBuffer;
	}

	char* GetRawBuffer()
	{
		return m_rawBuffer;
	}

	void Initialize( const PersistencyCacheEntrySetup& setup )
	{
		m_streamingProxyPersistencyID = setup.streamingProxyPersistencyID;
		m_flags = 0;
		if ( setup.reserveTransform )
		{
			m_flags |= PF_ReserveTransform;
		}
		
		red::Memzero( m_reserved, sizeof( m_reserved ) );
	}

	// #tbd: can't just make m_rawBuffer c_bufferSize or else the Transform alignment makes the whole entry > 64 bytes
	union
	{
		Transform m_transform;
		char m_rawBuffer[ sizeof( Transform ) ];
	};
	char m_rawBufferUnalignedExtra[ c_bufferSize - sizeof( Transform ) ];

	// #tbd: could store flags in localID, and restrict localID to 3 bytes.
	// Or possibly even be a Uint16 if can guarantee number of nodes per sector
	Uint32 m_streamingProxyPersistencyID;
	Uint8 m_flags;
	char m_reserved[3];
};

static_assert( sizeof( PersistencyCacheEntry ) == 64, "PersistencyCacheEntry: unexpected size change" );

template< typename T >
class PersistencyCacheEntryWriter : red::NonCopyable
{
public:
	explicit PersistencyCacheEntryWriter( PersistencyCacheEntry& entry )
		: m_entry( entry )
	{}

	void operator<<( const T& value )
	{	
		const Uint32 position = m_entry.HasFlags( PersistencyCacheEntry::PF_ReserveTransform ) ? sizeof( Transform ) : 0;

#ifndef RED_CONFIGURATION_FINAL
		{
			// #tbd: CMemoryFileWriterExternalBuffer truncates but the reader will fatal assert and be harder to debug why. So check now if won't fit.
			// Should also make sure nothing complicated is serialized, custom mapper to check.
			// Could just forget RTTI serialization and do a direct memcpy, but then complicates CNames or other simple custom-serializable types.
			CNullFileWriter nullWriter;
			nullWriter << const_cast< T& >( value );
			const Uint32 availableSpace = PersistencyCacheEntry::c_bufferSize - position;
			RED_FATAL_ASSERT( nullWriter.GetSize() <= availableSpace, "Value too large to fit in buffer! actual=%u, available=%u", nullWriter.GetSize(), availableSpace );
			RED_TOUCH( availableSpace );
		}
#endif
		CMemoryFileWriterExternalBuffer writer( m_entry.GetRawBuffer() + position, PersistencyCacheEntry::c_bufferSize - position );
		writer << const_cast< T& >( value );
		m_entry.SetFlags( PersistencyCacheEntry::PF_ValidBuffer );
	}

private:
	PersistencyCacheEntry& m_entry;
};

template< typename T >
class PersistencyCacheEntryReader : red::NonCopyable
{
public:
	explicit PersistencyCacheEntryReader( const PersistencyCacheEntry& entry )
		: m_entry( entry )
	{}

	void operator<<( T& outValue )
	{
		if ( !m_entry.HasFlags( PersistencyCacheEntry::PF_ValidBuffer ) )
		{
			return;
		}

		const Uint32 position = m_entry.HasFlags( PersistencyCacheEntry::PF_ReserveTransform ) ? sizeof( Transform ) : 0;
		CMemoryFileReaderExternalBuffer reader( m_entry.GetRawBuffer() + position, PersistencyCacheEntry::c_bufferSize - position );
		reader << outValue;
	}

private:
	const PersistencyCacheEntry& m_entry;
};

struct PersistencyCacheSetup
{
	PersistencyCacheSetup()
		: m_maxSectors( 0 )
		, m_poolBudgetKB( 0 )
		, m_maxEntriesPerPage( 0 )
	{}

	Uint32 m_maxSectors;
	Uint32 m_poolBudgetKB;
	Uint32 m_maxEntriesPerPage;
};

class WORLD_API PersistencyCache : red::NonCopyable
{
public:
	static const Uint32 c_maxPersistedNodesPerSector = 64*1024;

	explicit PersistencyCache( const PersistencyCacheSetup& setup );
	~PersistencyCache();

	void LockSector( SectorPersistencyID persistencyID );
	void UnlockSector( SectorPersistencyID persistencyID );

	PersistencyCacheEntry* AllocEntry( SectorPersistencyID persistencyID, const PersistencyCacheEntrySetup& setup );

	typedef red::StaticArray< PersistencyCacheEntry*, c_maxPersistedNodesPerSector > TPersistencyCacheEntries;
	void CollectCacheEntries( SectorPersistencyID persistencyID, TPersistencyCacheEntries& outEntries ) const;

	// Not thread safe
	void Serialize( IFile& file );

	friend void operator<<( IFile& file, PersistencyCache& entry )
	{
		entry.Serialize( file );
	}

	struct DebugStats
	{
		Uint64 m_numPagesAllocated = 0;	
		Uint64 m_numPagesTotal = 0;
		Uint64 m_numEntriesAllocated = 0;
		Uint64 m_numEntriesTotal = 0;

		// The closer to 1 the better, which means we're not creating too many partially filled pages
		Float m_globalPageFillRatio = 0.f;
	};

	void GetDebugStats( DebugStats& outDebugStats ) const;

private:
	static const Uint32 c_version;
	static const Uint32 c_invalidPageIndex = 0xFFFFFFFF;

	struct SectorHeader
	{
		SectorPersistencyID m_persistencyID;
		Uint32 m_headPageIndex;
		Uint32 m_numPagesAllocatedTotal;
		Uint32 m_numEntriesAllocatedForHeadPage;
		Uint32 m_lruNextIndex;
		Uint32 m_lruPrevIndex;
		
		static const Uint32 c_invalidPageIndex = PersistencyCache::c_invalidPageIndex;

		explicit SectorHeader( SectorPersistencyID persistencyID = SectorPersistencyID() )
			: m_persistencyID( persistencyID )
			, m_headPageIndex( c_invalidPageIndex )
			, m_numPagesAllocatedTotal( 0 )
			, m_numEntriesAllocatedForHeadPage( 0 )
			, m_lruNextIndex( c_invalidPageIndex )
			, m_lruPrevIndex( c_invalidPageIndex )
		{
		}

		Bool operator==( const SectorHeader& rhs )
		{
			return m_persistencyID == rhs.m_persistencyID;
		}

		Bool operator!=( const SectorHeader& rhs )
		{
			return !( *this == rhs );
		}

		Bool operator<( const SectorHeader& rhs )
		{
			return m_persistencyID < rhs.m_persistencyID;
		}
	};

	struct CacheConfig
	{		
		void Serialize( IFile& file )
		{
			file << m_maxEntriesPerPage;
			file << m_maxSectors;
			file << m_cacheEntrySizeBytes;
			file << m_pageSizeBytes;
			file << m_numPages;
			file << m_numCacheEntries;
		}

		friend void operator<<( IFile& file, CacheConfig& config )
		{
			config.Serialize( file );
		}

		Uint32 m_maxEntriesPerPage = 0;
		Uint32 m_maxSectors = 0;
		Uint32 m_cacheEntrySizeBytes = 0;
		Uint32 m_pageSizeBytes = 0;
		Uint32 m_numPages = 0;
		Uint32 m_numCacheEntries = 0;
	};

	void InitConfig( const PersistencyCacheSetup& setup, CacheConfig& outConfig ) const;
	
	Uint32 GetNumEntriesAllocated_NoLock( SectorHeader& sectorHeader ) const
	{
		if ( sectorHeader.m_numPagesAllocatedTotal > 0 )
		{
			return sectorHeader.m_numEntriesAllocatedForHeadPage + ( sectorHeader.m_numPagesAllocatedTotal - 1 ) * m_config.m_maxEntriesPerPage;		
		}

		return 0;
	}

	struct PageLink
	{
		PageLink()
			: m_nextPageIndex( c_invalidPageIndex )
		{
		}

		Uint32 m_nextPageIndex;
	};

	void ReleasePages_NoLock( SectorHeader& sectorHeader );
	void UpdateLRU_NoLock( SectorHeader& sectorHeader );
	SectorHeader* TryReleaseSectorPagesLRU_NoLock();

	const SectorHeader* FindSectorHeader_NoLock( SectorPersistencyID persitencyID ) const;
	SectorHeader* FindSectorHeader_NoLock( SectorPersistencyID persitencyID );

	SectorHeader& InsertSectorHeader_NoLock( SectorPersistencyID persitencyID );

	const PersistencyCacheEntry& GetCacheEntry_NoLock( Uint32 pageIndex, Uint32 entryIndex ) const;
	PersistencyCacheEntry& GetCacheEntry_NoLock( Uint32 pageIndex, Uint32 entryIndex );
	void CollectCacheEntriesForPage_NoLock( Uint32 pageIndex, Uint32 numEntries, TPersistencyCacheEntries& outCollector ) const;

	typedef red::SpinLock TLock;
	typedef red::ScopedLock< TLock > TScopedLock;

	red::DynArray< SectorHeader > m_sectorHeaders;
	red::DynArray< PageLink > m_pageLinks;
	red::DynArray< PersistencyCacheEntry >  m_cacheEntries;

	CacheConfig m_config;
	Uint64 m_statsNumEntriesAllocatedTotal;
	Uint32 m_lruSectorIndexHead;
	Uint32 m_lruSectorIndexTail;
	IDAllocatorDynamic m_pageAllocator;
	struct SectorWorkingSetEntry
	{
		SectorPersistencyID m_persistencyID;
		Uint32 m_sectorIndex;

		Bool operator==( const SectorWorkingSetEntry& rhs ) const
		{
			// Only comparing the ID
			return m_persistencyID == rhs.m_persistencyID;
		}
	};
	red::StaticArray< SectorWorkingSetEntry, 32 > m_sectorWorkingSetLookup;
	red::BitSet64Dynamic m_lockedSectorIndices{ red::PoolStreaming() }; // not serialized
	mutable TLock m_allocatorLock;
};

}
