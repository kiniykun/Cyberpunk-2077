#include "build.h"
#include "io.h"

#include "../../common/profiling/ScopedProfiler.h"

//////////////////////////////////////////////////////////////
Bool recCore::ioLayer::StoreAtFileEnd( const char* filename, const void* stream, const Uint32 streamSize )
{
	PROFILE_SCOPE( "ioLayer::StoreAtFileEnd(): requested size: %i bytes", streamSize );

	::FILE* file = NULL;
	if ( (file = fopen(filename, "ab")) != nullptr )
	{
		Bool success = fwrite( stream, sizeof( Uint8 ), streamSize, file ) == streamSize;

		fclose( file );

		return success;
	}

	return false;
}

//////////////////////////////////////////////////////////////
Bool recCore::ioLayer::StoreAtFileOffset( const char* filename, const Uint32 offsetFromBeggining,  const void* stream, Uint32 streamSize )
{
	PROFILE_SCOPE( "ioLayer::StoreAtFileOffset(): requested size: %i bytes", streamSize );

	::FILE* file = NULL;
	if ( (file = fopen(filename, "rb+")) != nullptr )
	{
		fseek( file, offsetFromBeggining, SEEK_SET );

		Bool success = fwrite( stream, sizeof( Uint8 ), streamSize, file ) == streamSize;

		fclose( file );

		return success;
	}

	return false;
}

//////////////////////////////////////////////////////////////
Bool recCore::ioLayer::LoadFromFile( const char* filename, const Uint32 bufferSize, void* out_buffer, Uint32& out_loadedSize )
{
	PROFILE_SCOPE( "ioLayer::LoadFromFile(): requested size: %i bytes", bufferSize );

	::FILE* file = NULL;

	if ( (file = fopen(filename, "rb")) != nullptr )
	{
		out_loadedSize = 0;

		if ( !file )
		{
			fclose( file );
			return false;
		}

		fseek( file, 0 , SEEK_END );
		Uint32 filesize = ftell( file );
		rewind( file );

		Uint32 finalLoadSize = filesize;

		if ( bufferSize < filesize )
		{
			finalLoadSize = bufferSize;
		}

		// TODO: take header into account to make sure that we always loading whole frames( ? )
		out_loadedSize = (Uint32) fread( out_buffer, sizeof( Uint8 ), finalLoadSize, file );

		fclose( file );

		return out_loadedSize == finalLoadSize;
	}
	return false;
}

//////////////////////////////////////////////////////////////
Bool recCore::ioLayer::LoadChunkOfFile( const char* filename, const Uint32 offsetFromBeggining, const Uint32 chunkSize, void* outBuffer, Uint32& out_loadedSize )
{
	PROFILE_SCOPE( "ioLayer::LoadChunkOfFile(): requested size: %i bytes", chunkSize );

	::FILE* file = NULL;

	if ( (file = fopen(filename, "rb")) != nullptr )
	{
		out_loadedSize = 0;

		if ( !file )
		{
			fclose( file );
			return false;
		}

		fseek( file, 0 , SEEK_END );
		Uint32 filesize = ftell( file );
		
		if ( filesize < offsetFromBeggining )
		{
			fclose( file );
			return false;
		}

		fseek( file, offsetFromBeggining, SEEK_SET );

		Uint32 finalLoadSize = filesize > (offsetFromBeggining + chunkSize) ? chunkSize : (filesize - offsetFromBeggining);

		// TODO: take header into account to make sure that we always loading whole frames( ? )
		out_loadedSize = (Uint32) fread( outBuffer, sizeof( Uint8 ), finalLoadSize, file );

		fclose( file );

		return out_loadedSize == finalLoadSize;
	}
	return false;
}
