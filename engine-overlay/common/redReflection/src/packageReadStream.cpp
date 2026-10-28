/**
* Copyright (c) 2017 CD Projekt Red. All Rights Reserved.
*/

#include "build.h"
#include "packageReadStream.h"

// CP2077 web [saves]: bounds guard for package reads (see OnRead). Additive; class layout unchanged.
#include <atomic>
#include <cstdio>
#include <cstring>
#ifdef __EMSCRIPTEN__
#include <emscripten/emscripten.h>
#include <emscripten/heap.h>
#endif

namespace cpweb_package_guard
{
	static std::atomic< Uint32 > s_overruns{ 0 };
	static std::atomic< Uint32 > s_watchArmed{ 0 };
	static std::atomic< Uint32 > s_watchBaseline{ 0 };

	void ReportOverrun( const char* what, Uint64 position, Uint64 size, Uint64 bufferSize, const void* base, const void* dest )
	{
		const Uint32 n = s_overruns.fetch_add( 1 ) + 1;
		if ( n <= 8 || ( n & ( n - 1 ) ) == 0 )
		{
#ifdef __EMSCRIPTEN__
			const unsigned long long heap = (unsigned long long)emscripten_get_heap_size();
#else
			const unsigned long long heap = 0;
#endif
			std::fprintf( stderr, "CP2077 package read guard: %s overrun #%u position=%llu size=%llu buffer=%llu base=%p dest=%p heap=%llu watch=%u\n",
				what, n, (unsigned long long)position, (unsigned long long)size, (unsigned long long)bufferSize, base, dest, heap, s_watchArmed.load() );
#ifdef __EMSCRIPTEN__
			if ( n == 1 )
			{
				emscripten_log( EM_LOG_ERROR | EM_LOG_C_STACK, "CP2077 package read guard: first overrun stack" );
			}
#endif
		}
	}
}

extern "C"
{
	// Total out-of-range package reads/descriptors seen in this process.
	Uint32 cpweb_PackageReadOverrunCount() { return cpweb_package_guard::s_overruns.load(); }

	// Save-load watch: armed when a saved session starts loading (BaseGameSession::Initialize with a
	// load stream), disarmed when SessionActive starts. State_Session polls it and fails the load.
	void cpweb_SaveLoadWatchArm()
	{
		cpweb_package_guard::s_watchBaseline.store( cpweb_package_guard::s_overruns.load() );
		cpweb_package_guard::s_watchArmed.store( 1 );
	}
	void cpweb_SaveLoadWatchDisarm() { cpweb_package_guard::s_watchArmed.store( 0 ); }
	Uint32 cpweb_SaveLoadWatchTripped()
	{
		return ( cpweb_package_guard::s_watchArmed.load() && cpweb_package_guard::s_overruns.load() > cpweb_package_guard::s_watchBaseline.load() ) ? 1u : 0u;
	}
}

namespace red
{
	PackageReadStream::PackageReadStream()
	{}

	PackageReadStream::~PackageReadStream()
	{}

	void PackageReadStream::SetBuffer( const red::BlobView & blobView )
	{
		m_data = blobView;
		Seek( 0 );
	}

	const void * PackageReadStream::GetReadCursor() const
	{
		return m_data.Data( static_cast< Uint32 >( GetPosition() ) );
	}

	Uint64 PackageReadStream::OnWrite( const void * data, Uint64 size, Uint64 position )
	{
		return 0;
	}

	Uint64 PackageReadStream::OnRead( void * data, Uint64 size, Uint64 position )
	{
		// CP2077 web [saves]: the original read had no bounds check. A corrupted or incompatible package
		// (a save whose property descriptors point past the buffer) read far outside the blob and trapped
		// 'memory access out of bounds' in wasm. Out-of-range reads now copy what exists, zero-fill the rest,
		// and are counted + logged; the save-load watch turns them into a normal 'save corrupted' error.
		const Uint64 bufferSize = m_data.Size();
		if ( size > bufferSize || position > bufferSize - size )
		{
			cpweb_package_guard::ReportOverrun( "read", position, size, bufferSize, m_data.Data( 0 ), data );
			const Uint64 available = ( position < bufferSize ) ? ( bufferSize - position ) : 0;
			if ( available )
			{
				std::memcpy( data, m_data.Data( static_cast< Uint32 >( position ) ), static_cast< size_t >( available ) );
			}
			std::memset( static_cast< Uint8* >( data ) + available, 0, static_cast< size_t >( size - available ) );
			return size;
		}
		const void * readCursor = m_data.Data( static_cast< Uint32 >( position ) );
		red::Memcpy( data, readCursor, size );
		return size;
	}

	Uint64 PackageReadStream::OnSeek( Uint64 positionRequest )
	{
		return positionRequest;
	}

	Uint32 PackageReadStream::GetBufferSize() const
	{
		return m_data.Size();
	}

	red::UniquePtr< PackageReadStream > CreateReadStream( const red::BlobView & blobView )
	{
		red::UniquePtr< PackageReadStream > stream = red::CreateUniquePtr< PackageReadStream >();
		stream->SetBuffer( blobView );
		return stream;
	}
}
