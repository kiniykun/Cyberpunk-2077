#include "build.h"
#include "wwiseLowLevelIO.h"
#include "wwiseContainerStreaming.h"
#include "audioResourceUtils.h"

#include "../../redReflection/include/resourceDepot.h"
#include "../../redReflection/include/serializationAsyncSource.h"
#include "../../redSystem/include/hex.h"

//------------------------------------------------------------------------------

struct AsyncFileEntry
{
	RED_USE_MEMORY_POOL( audio::PoolAudio );

	serialization::AsyncSourcePtr asyncSource;
};

struct AsyncFileReadEntry
{
	RED_USE_MEMORY_POOL( audio::PoolAudio );

	serialization::AsyncSourcePtr asyncSource;
	serialization::AsyncSourceReadSizes readSizes;
	AkAsyncIOTransferInfo* transferInfo;
};

bool OpenResourceDepotFile( const res::ResourcePath& path, AkFileDesc& fileDesc )
{
	auto asyncSource = GetResourceDepot()->CreateResourceAsyncSource( path );
	if ( !asyncSource )
	{
		return false;
	}

	AsyncFileEntry* result = RED_NEW(AsyncFileEntry);
	result->asyncSource = asyncSource;

	fileDesc.hFile = 0;
	fileDesc.deviceID = 0;
	fileDesc.iFileSize = asyncSource->GetSizeOnDisk(); // We store all the audio files uncompressed in the archives or on disk
	fileDesc.pCustomParam = result;
	fileDesc.uCustomParamSize = 0;
	fileDesc.uSector = 0;

	return true;
}

void CloseResourceDepotFile( AkFileDesc& fileDesc )
{
	auto entry = reinterpret_cast<AsyncFileEntry*>( fileDesc.pCustomParam );
	if ( entry )
	{
		RED_DELETE( entry );
		fileDesc.pCustomParam = nullptr;
	}
}

void OnReadComplete( serialization::AsyncSourceReadBuffer readBuffer, red::UniqueBuffer, void* userData, red::EAsyncResult )
{
	auto entry = reinterpret_cast<AsyncFileReadEntry*>( userData );

	io::ShareableIOMemory shareableBuffer = readBuffer.Release();

	RED_FATAL_ASSERT(shareableBuffer.GetUnderlyingBuffer().Data() == entry->transferInfo->pBuffer );
	shareableBuffer.GetUnderlyingBuffer().Release(); // Always release it as we don't own this memory

	entry->transferInfo->pCallback( entry->transferInfo, AK_Success );

	RED_DELETE( entry );
}

//------------------------------------------------------------------------------

// TODO(dan.murray): this doesn't handle language specific resolves yet

AKRESULT WwiseContainerStreamingResolver::Open( AkOSChar const* filenameWide, AkOpenMode openMode, AkFileSystemFlags* flags, bool& syncOpen, AkFileDesc& fileDesc )
{
	if( !m_isEnabled )
	{
		return AK_Success;
	}

#if !defined( RED_PLATFORM_ORBIS ) && !defined( RED_PLATFORM_LINUX ) && !defined( RED_PLATFORM_WEB )
	const char* str = UNICODE_TO_ANSI( filenameWide );
#else
	const char* str = filenameWide;
#endif

	// Wwise adds .bnk to the file name
	red::StringView strView = red::paths::GetFileStem( str );

	res::ResourcePath resourcePath;
	if ( !audio::HashHexStringToResourcePath( strView.Data(), strView.Length(), resourcePath ) )
	{
		return AK_FileNotFound;
	}

	if ( !OpenResourceDepotFile( resourcePath, fileDesc ) )
	{
		return AK_FileNotFound;
	}

	syncOpen = true;
	return AK_Success;
}

AKRESULT WwiseContainerStreamingResolver::Open( AkFileID fileId, AkOpenMode openMode, AkFileSystemFlags* flags, bool& syncOpen, AkFileDesc& fileDesc )
{
	if ( !m_isEnabled )
	{
		return AK_Success;
	}

	res::ResourcePath resourcePath;

	//This is based on the assumtion that external sources with a fileId rather than a fileName are coming from PinFileToCache
	if (flags->uCompanyID == AKCOMPANYID_AUDIOKINETIC_EXTERNAL)
	{
		char *str = static_cast< char * >( flags->pCustomParam );

		if ( !audio::HashHexStringToResourcePath( str, flags->uCustomParamSize, resourcePath ) )
		{
			return AK_FileNotFound;
		}
	}
	else
	{
		// TODO(dan.murray): can we use flags->uCodecID to avoid searching containers?
		if (wwiseContainerStreamingOpenFile(fileId, &fileDesc))
		{
			syncOpen = true;
			return AK_Success;
		}

		// TODO: Remove this string usage if possible before final release
		// Need to still use strings for loose files though
		auto resourceStringPath = String::Printf("base\\sound\\soundbanks\\%u.wem", fileId);
		resourcePath = res::ResourcePath::Build(resourceStringPath);
	}

	if ( !OpenResourceDepotFile( resourcePath, fileDesc ) )
	{
		return AK_FileNotFound;
	}

	syncOpen = true;
	return AK_Success;
}

void WwiseContainerStreamingResolver::ShutDown()
{
	m_isEnabled = false;
}

//------------------------------------------------------------------------------

AKRESULT WwiseContainerStreamingContainerDevice::Read( AkFileDesc& fileDesc, AkIoHeuristics const& heuristics, AkAsyncIOTransferInfo& transferInfo )
{
	auto result = wwiseContainerStreamingRead( &fileDesc, &transferInfo );
	return result ? AK_Success : AK_Fail;
}

AKRESULT WwiseContainerStreamingContainerDevice::Write( AkFileDesc& fileDesc, AkIoHeuristics const& heuristics, AkAsyncIOTransferInfo& transferInfo )
{
	return AK_Fail;
}

void WwiseContainerStreamingContainerDevice::Cancel( AkFileDesc& fileDesc, AkAsyncIOTransferInfo& transferInfo, bool& cancelAllTransfers )
{
	cancelAllTransfers = false;
}

AKRESULT WwiseContainerStreamingContainerDevice::Close( AkFileDesc& fileDesc )
{
	wwiseContainerStreamingCloseFile( &fileDesc );
	return AK_Success;
}

AkUInt32 WwiseContainerStreamingContainerDevice::GetBlockSize( AkFileDesc& fileDesc )
{
	return 1;
}

AkUInt32 WwiseContainerStreamingContainerDevice::GetDeviceData()
{
	return 0;
}

void WwiseContainerStreamingContainerDevice::GetDeviceDesc( AkDeviceDesc& deviceDesc )
{
	deviceDesc.bCanRead = true;
	deviceDesc.bCanWrite = false;
	deviceDesc.deviceID = 1;
	deviceDesc.szDeviceName[ 0 ] = '\0';
	deviceDesc.uStringSize = 0;
}

//------------------------------------------------------------------------------

AKRESULT wwiseContainerStreamingDefaultDevice::Read( AkFileDesc& fileDesc, AkIoHeuristics const& heuristics, AkAsyncIOTransferInfo& transferInfo )
{
	auto fileEntry = reinterpret_cast<AsyncFileEntry*>( fileDesc.pCustomParam );
	const auto& asyncSource = fileEntry->asyncSource;

	const Uint32 diskSize = asyncSource->GetSizeOnDisk();
	const Uint32 readOffset = static_cast<Uint32>( transferInfo.uFilePosition );
	const Uint32 readSize = static_cast<Uint32>( transferInfo.uRequestedSize );
	RED_FATAL_ASSERT( readOffset + readSize <= diskSize );

	auto readEntry = RED_NEW(AsyncFileReadEntry);
	readEntry->asyncSource = asyncSource;
	readEntry->transferInfo = &transferInfo; // Note: Alive until the callback is called
	readEntry->readSizes = asyncSource->GetReadSizes( readOffset, readSize );
	RED_FATAL_ASSERT( readEntry->readSizes.bufferOffset == 0, "Must not have an offset into the buffer, not supported by Wwise" );

	// Wrap up the buffer in a UniqueBuffer so we can transfer it along
	// NOTE: Must release the buffer before destroying the unique buffer because we don't own the memory
	auto shareableBuffer = io::ShareableIOMemory( red::MakeUniqueBuffer< audio::PoolWwise >( transferInfo.pBuffer, static_cast<Uint32>( transferInfo.uBufferSize ), 16 ) );

	serialization::IAsyncSource::CallbackContext callbackContext;
	callbackContext.callback = &OnReadComplete;
	callbackContext.userData = readEntry;

	serialization::IAsyncSource::ReadAsyncParams params;
	params.requestSource = io::RequestSource::AudioSystem_WwiseLowLevelIO;
	params.offset = readOffset;
	params.size = readSize;
	params.priority = io::eAsyncPriority_AUDIO;
	params.allocDeserializationMemoryForDecompressorIfCompressed = false;

	asyncSource->ReadAsync( params, serialization::AsyncSourceReadBuffer( std::move(shareableBuffer), 0, readEntry->readSizes ), callbackContext );
	return AK_Success;
}

AKRESULT wwiseContainerStreamingDefaultDevice::Write( AkFileDesc& fileDesc, AkIoHeuristics const& heuristics, AkAsyncIOTransferInfo& transferInfo )
{
	return AK_Fail;
}

void wwiseContainerStreamingDefaultDevice::Cancel( AkFileDesc& fileDesc, AkAsyncIOTransferInfo& transferInfo, bool& cancelAllTransfers )
{
	// Cannot cancel any transfers because it would cancel all reading from the archive
	cancelAllTransfers = false;
}

AKRESULT wwiseContainerStreamingDefaultDevice::Close( AkFileDesc& fileDesc )
{
	CloseResourceDepotFile( fileDesc );
	return AK_Success;
}

AkUInt32 wwiseContainerStreamingDefaultDevice::GetBlockSize( AkFileDesc& fileDesc )
{
	return RED_KILO_BYTE( 4 );
}

AkUInt32 wwiseContainerStreamingDefaultDevice::GetDeviceData()
{
	return 0;
}

void wwiseContainerStreamingDefaultDevice::GetDeviceDesc( AkDeviceDesc& deviceDesc )
{
	deviceDesc.bCanRead = true;
	deviceDesc.bCanWrite = false;
	deviceDesc.deviceID = 0;
	deviceDesc.szDeviceName[ 0 ] = '\0';
	deviceDesc.uStringSize = 0;
}
