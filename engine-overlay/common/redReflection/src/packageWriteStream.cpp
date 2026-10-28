/**
* Copyright (c) 2007 CD Projekt Red. All Rights Reserved.
*/

#include "build.h"
#include "packageWriteStream.h"

namespace red
{
	const Uint32 c_packageWriteStreamChunkSize = RED_MEGA_BYTE( 1 );

	PackageWriteStream::PackageWriteStream()
	{}

	PackageWriteStream::~PackageWriteStream()
	{}

	void PackageWriteStream::Initialize( Uint32 reservedSize )
	{
		m_buffer = red::CreateUniqueBuffer< red::PoolEngine >( reservedSize, 16 ); // ctremblay: Is this going to be in runtime ? Most likely. Gotta need to select pool.
	}

	void * PackageWriteStream::GetWriteCursor() const
	{
		return static_cast< Uint8* >( m_buffer.Get() ) + GetPosition();
	}

	red::BlobView PackageWriteStream::GetBufferView() const
	{
		return red::BlobView( m_buffer.Get(), GetWriteCursor() );
	}

	red::BlobSpan PackageWriteStream::GetBuffer()
	{
		return red::BlobSpan( m_buffer.Get(), GetWriteCursor() );
	}

	red::BlobView PackageWriteStream::GetRange( Uint64 start, Uint64 end ) const
	{
		return GetBufferView().Range( static_cast< Uint32 >( start ), static_cast< Uint32 >( end - start ) );
	}

	red::BlobSpan PackageWriteStream::GetRange( Uint64 start, Uint64 end )
	{
		return red::BlobSpan( static_cast< Uint8* >( m_buffer.Get() ) + start, end - start );
	}

	red::UniqueBuffer PackageWriteStream::ReleaseBuffer()
	{
		// CP2077 web [nomad2]: do NOT shrink in place. A big stream (e.g. the 2 MB persistent-state save-compile stream)
		// shrinking under the TLSF limit goes through DefaultAllocator::ReallocateFromBigSizeAllocator's alloc+copy
		// fallback; on this build the checkpoint-save package came out of that step (n3: table-compiler output OK, compiled package not) with
		// object data that no longer matched its table (garbage property counts, then runaway remaps / crashes on the
		// next save and on load). Copy into an exact-size buffer explicitly instead.
		const Uint32 cpwebSize = static_cast< Uint32 >( GetPosition() );
		red::UniqueBuffer cpwebExact = red::CreateUniqueBuffer< red::PoolEngine >( cpwebSize ? cpwebSize : 1, 16 );
		if( cpwebSize )
		{
			red::Memcpy( cpwebExact.Get(), m_buffer.Get(), cpwebSize );
		}
		cpwebExact.PatchSize( cpwebSize );
		m_buffer.Reset();
		return cpwebExact;
	}

	Uint64 PackageWriteStream::OnSeek( Uint64 requestedPosition )
	{
		if( requestedPosition > m_buffer.GetSize() )
		{
			m_buffer.Reallocate( red::memory::RoundUp( static_cast< Uint32 >( requestedPosition ), c_packageWriteStreamChunkSize ) );
		}

		return requestedPosition;
	}

	Uint64 PackageWriteStream::OnRead( void * data, Uint64 size, Uint64 position )
	{
		return 0;
	}

	Uint64 PackageWriteStream::OnWrite( const void * data, Uint64 size, Uint64 position )
	{
		if( position + size > m_buffer.GetSize() )
		{
			m_buffer.Reallocate( red::memory::RoundUp( static_cast< Uint32 >( size + position ), c_packageWriteStreamChunkSize ) );
		}

		void * writeCursor = static_cast< Uint8* >( m_buffer.Get() ) + position;
		red::Memcpy( writeCursor, data, size );
		return size; 
	}

	red::UniquePtr< PackageWriteStream > CreateWriteStream( Uint32 reservedSize )
	{
		red::UniquePtr< PackageWriteStream > stream = red::CreateUniquePtr< PackageWriteStream >();
		stream->Initialize( reservedSize ) ;
		return stream;
	}
}
