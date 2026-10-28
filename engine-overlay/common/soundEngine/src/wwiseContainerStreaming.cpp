#include "build.h"
#include "wwiseContainerStreaming.h"
#include "hashIndex.h"
#include <AK/Tools/Common/AkObject.h>
#include "../../redReflection/include/resourceDepot.h"
#include "../../redReflection/include/serializationAsyncSource.h"
#include <../source/SoundEngine/Codecs/AkOpusDecoder/opusfile/include/opusfile.h>
#include "../../redIO/include/redIOAsyncIO.h"

WwiseContainerStreamingResolver g_wwiseContainerStreamingResolver;
wwiseContainerStreamingDefaultDevice g_wwiseContainerStreamingDefaultDevice;
WwiseContainerStreamingContainerDevice g_wwiseContainerStreamingContainerDevice;

static AkThread s_containerStreamingDecodeThread;

static red::InstrumentationObject containerDecodeProfileObject( "Wwise Container Decode" );

struct WwiseContainerData
{
	const Uint32* lookupTableFileIds;
	const Uint16* lookupTablePakIndicies;
	const Uint32* lookupTableFileOffsets;
	const Uint16* lookupTableRiffHeaderSizes;
	const Uint32* lookupTableCompressedFileSizes;
	const Uint32* lookupTableDecodedFileSizes;
	const Uint32* eventMediaTable;
	Uint32 lookupTableNumEntries;
	Uint32 eventMediaTableNumEntries;
	HashIndex lookupTableHashIndex;
	HashIndex eventMediaTableHashIndex;
};

#define WWISE_CONTAINER_DATA_MAX 1
static WwiseContainerData containerDatas[ WWISE_CONTAINER_DATA_MAX ];
static Uint16 s_containerDataNum;

struct WwiseContainerDepotFile
{
	Uint16 containerDepotFileId;
	Uint16 containerId;
	Uint16 pakFileIndex;
	bool loading;
	Uint32 usageId;
	Uint32 lookupTableIndex;
	atomic::TAtomic32 refCount;
	red::SharedPtr< serialization::IAsyncSource > file;
	serialization::AsyncSourceReadBuffer data;
	atomic::TAtomic32 loaded;
};

#define WWISE_CONTAINER_DEPOT_FILE_MAX_UNREFFED 10
#define WWISE_CONTAINER_DEPOT_FILE_MAX 2048
#define WWISE_CONTAINER_DEPOT_FILE_INVALID_ID ( ( Uint16 )-1 )
static WwiseContainerDepotFile containerDepotFiles[ WWISE_CONTAINER_DEPOT_FILE_MAX ];
static Uint16 containerDepotFilesNum;
static Uint16 containerDepotFilesFreeList[ WWISE_CONTAINER_DEPOT_FILE_MAX ];
static Uint16 containerDepotFilesFreeListNum;
static Uint16 containerDepotFilesUsageId;

struct WwiseContainerOpenFile
{
	void* opusFile;
	Uint32 pakFileOffset;
	Uint32 opusFilePos;
	Uint32 opusFileSize;
	Uint16 containerId;
	Uint16 containerDepotFileId;
	Uint16 riffHeaderSize;
	Uint16 pendingReads;
};

#define WWISE_CONTAINER_OPEN_FILES_MAX 2048
static WwiseContainerOpenFile containerOpenFiles[ WWISE_CONTAINER_OPEN_FILES_MAX ];
static Uint16 containerOpenFilesNum;
static Uint16 containerOpenFilesFreeList[ WWISE_CONTAINER_OPEN_FILES_MAX ];
static Uint16 containerOpenFilesFreeListNum;

#define WWISE_CONTAINER_CLOSED_FILES_MAX 2048
static Uint16 containerClosedFiles[ WWISE_CONTAINER_CLOSED_FILES_MAX ];
static Uint64 containerClosedFilesReadNum;
static atomic::TAtomic64 containerClosedFilesWriteNum;

struct WwiseContainerOpenFileNewRead
{
	AkFileDesc* fileDesc;
	AkAsyncIOTransferInfo* transferInfo;
};

#define WWISE_CONTAINER_OPEN_FILE_NEW_READ_MAX 2048
static WwiseContainerOpenFileNewRead containerOpenFileNewReads[ WWISE_CONTAINER_OPEN_FILE_NEW_READ_MAX ];
static Uint64 containerOpenFileNewReadReadNum;
static atomic::TAtomic64 containerOpenFileNewReadWriteNum;

struct WwiseContainerOpenFileRead
{
	AkFileDesc* fileDesc;
	AkAsyncIOTransferInfo* transferInfo;
};

#define WWISE_CONTAINER_OPEN_FILE_READ_MAX 2048
static WwiseContainerOpenFileRead containerOpenFileReads[ WWISE_CONTAINER_OPEN_FILE_READ_MAX ];
static Uint16 containerOpenFileReadNum;
static Uint16 containerOpenFileReadFreeList[ WWISE_CONTAINER_OPEN_FILE_READ_MAX ];
static Uint16 containerOpenFileReadFreeListNum;

struct WwiseContainerOpenFileDecode
{
	AkFileDesc* fileDesc;
	AkAsyncIOTransferInfo* transferInfo;
};

#define WWISE_CONTAINER_OPEN_FILE_READ_DECODE_MAX 2048
static WwiseContainerOpenFileDecode containerOpenFileDecodes[ WWISE_CONTAINER_OPEN_FILE_READ_DECODE_MAX ];
static Uint64 containerOpenFileDecodeNum;

static red::Semaphore containerOpenFileDecodeSemaphore( 0, WWISE_CONTAINER_OPEN_FILE_READ_DECODE_MAX );
static Bool containerThreadIsShuttingDown = false;

struct WwiseContainerCompletedTransfer
{
	AkFileDesc* fileDesc;
	AkAsyncIOTransferInfo* transferInfo;
	Uint32 success;
};

#define WWISE_CONTAINER_COMPLETED_TRANSFER_MAX 2048
static WwiseContainerCompletedTransfer containerCompletedTransfers[ WWISE_CONTAINER_COMPLETED_TRANSFER_MAX ];
static Uint64 containerCompletedTransfersReadNum;
static atomic::TAtomic64 containerCompletedTransfersWriteNum;

struct WwiseContainerNewDynamicPrefetch
{
	Uint32 eventId;
	Uint32 addToPrefetches; // 1 will add a new prefetch ref, 0 will remove an existing ref
};

#define WWISE_CONTAINER_NEW_DYNAMIC_PREFETCH_MAX 2048
static WwiseContainerNewDynamicPrefetch containerNewDynamicPrefetches[ WWISE_CONTAINER_NEW_DYNAMIC_PREFETCH_MAX ];
static Uint64 containerNewDynamicPrefetchesReadNum;
static atomic::TAtomic64 containerNewDynamicPrefetchesWriteNum;

struct WwiseContainerDynamicPrefetch
{
	Uint32 mediaId;
	Uint32 refCount;
	AkFileDesc fileDesc;
};

#define WWISE_CONTAINER_DYNAMIC_PREFETCH_MAX 2048
static WwiseContainerDynamicPrefetch containerDynamicPrefetches[ WWISE_CONTAINER_DYNAMIC_PREFETCH_MAX ];
static Uint16 containerDynamicPrefetchesNum;
static Uint16 containerDynamicPrefetchesFreeList[ WWISE_CONTAINER_DYNAMIC_PREFETCH_MAX ];
static Uint16 containerDynamicPrefetchesFreeListNum;

static Bool vehicleStreamingModeActive = false;

#if defined( RED_PLATFORM_ORBIS ) || defined( RED_PLATFORM_LINUX ) || defined( RED_PLATFORM_WEB )
static void* wwiseContainerStreamingThreadEntryPoint( void* param );
#else
static AkUInt32 wwiseContainerStreamingThreadEntryPoint( void* param );
#endif

static Int32 wwiseContainerOpusRead( void* stream, Uint8* buffer, Int32 bytes )
{
	WwiseContainerOpenFile* openFile = ( WwiseContainerOpenFile* )stream;
	WwiseContainerDepotFile* depotFile = &containerDepotFiles[ openFile->containerDepotFileId ];

	red::BlobView fileView = depotFile->data.ValidRegion();
	if ( fileView.Empty() ) {
		return -1;
	}

	Uint32 offset = openFile->pakFileOffset + openFile->riffHeaderSize + static_cast<Uint32>( openFile->opusFilePos );
	if ( offset >= fileView.Size() ) {
		return -1;
	}

	const void* readBuffer = fileView.Data( offset );
	if ( !readBuffer ) {
		return -1;
	}

	Int32 bytesToRead = Min( bytes, static_cast<Int32>( openFile->opusFileSize - openFile->opusFilePos ) );
	if ( bytesToRead < 1 ) {
		return -1;
	}

	if ( ( offset + bytesToRead ) > fileView.Size() ) {
		return -1;
	}

	memcpy( buffer, readBuffer, bytesToRead );
	openFile->opusFilePos += bytesToRead;
	return bytesToRead;
}

//Uses raw types for compatibility with opus api
#if defined( RED_PLATFORM_ORBIS ) || defined( RED_PLATFORM_LINUX ) || defined( RED_PLATFORM_WEB )
static int wwiseContainerOpusSeek( void* stream, opus_int64 offset, signed int whence )
#else
static int wwiseContainerOpusSeek( void* stream, opus_int64 offset, signed int whence )
#endif
{
	WwiseContainerOpenFile* openFile = ( WwiseContainerOpenFile* )stream;
	Int64 pos = ( Int64 )openFile->opusFilePos;
	switch( whence )
	{
	case SEEK_SET:
		pos = offset;
		break;
	case SEEK_CUR:
		pos += offset;
		break;
	case SEEK_END:
		pos = openFile->opusFileSize - offset;
		break;
	}
	
	if ( pos < 0 ) {
		return -1;
	}

	if ( ( Uint32 )pos > openFile->opusFileSize ) {
		return -1;
	}

	openFile->opusFilePos = ( Uint32 )pos;
	return 0;
}

#if defined( RED_PLATFORM_ORBIS ) || defined( RED_PLATFORM_LINUX ) || defined( RED_PLATFORM_WEB )
static opus_int64 wwiseContainerOpusTell( void* stream )
#else
static opus_int64 wwiseContainerOpusTell( void* stream )
#endif
{
	WwiseContainerOpenFile* openFile = ( WwiseContainerOpenFile* )stream;
	return openFile->opusFilePos;
}

static Int32 wwiseContainerOpusClose( void* stream )
{
	return 0;
}

static OpusFileCallbacks wwiseContainerOpusCallbacks[] = 
{
	{
		wwiseContainerOpusRead,
		wwiseContainerOpusSeek,
		wwiseContainerOpusTell,
		wwiseContainerOpusClose
	}
};

static void AsyncReadCompleteCallback( serialization::AsyncSourceReadBuffer readBuffer, red::UniqueBuffer, void* userData, red::EAsyncResult result )
{
	WwiseContainerDepotFile* file = (WwiseContainerDepotFile*)userData;
	if( result == red::EAsyncResult::eAsyncResult_Success )
	{
		file->data = std::move(readBuffer);
		atomic::Exchange32(&file->loaded, 1);
	}
	atomic::Decrement32( &file->refCount );
}

void wwiseContainerStreamingDefaultDeviceSettings( AkDeviceSettings* deviceSettings )
{
	AK::StreamMgr::GetDefaultDeviceSettings( *deviceSettings );
	deviceSettings->uIOMemorySize = 40 * 1024 * 1024;
	deviceSettings->uIOMemoryAlignment = 16;
	deviceSettings->uGranularity = 32 * 1024; //See the comment in op_read about recommended minimum frame size of 5760
	deviceSettings->uSchedulerTypeFlags = AK_SCHEDULER_DEFERRED_LINED_UP;
	deviceSettings->bUseStreamCache = true;
	deviceSettings->uMaxConcurrentIO = 1024;
#ifdef RED_PLATFORM_DURANGO
	deviceSettings->threadProperties.dwAffinityMask = 1 << 4 | 1 << 5;
	deviceSettings->threadProperties.nPriority = THREAD_PRIORITY_HIGHEST;
#elif defined( RED_PLATFORM_ORBIS )
	deviceSettings->threadProperties.dwAffinityMask = 1 << 4 | 1 << 5;
	deviceSettings->threadProperties.nPriority = SCE_KERNEL_PRIO_FIFO_HIGHEST;
#endif

}

bool wwiseContainerStreamingInit( AkThreadProperties* threadProps )
{
	AKPLATFORM::AkCreateThread( wwiseContainerStreamingThreadEntryPoint, nullptr, *threadProps, &s_containerStreamingDecodeThread, "wwise_container_streaming" );
	return true;
}

void wwiseContainerStreamingTerm()
{
	containerThreadIsShuttingDown = true;
	containerOpenFileDecodeSemaphore.Release();
	AKPLATFORM::AkWaitForSingleThread( &s_containerStreamingDecodeThread );
	AKPLATFORM::AkCloseThread( &s_containerStreamingDecodeThread );
}

#pragma pack(push, 1)
struct ContainerHeader
{
	constexpr static Uint32 expectedFourcc = ' DNS'; // 'S' 'N' 'D' ' ' in little endian byte order
	constexpr static Uint32 expectedVersion = 0xF0000002;
	Uint32 fourcc;
	Uint32 version;
	Uint32 numPrefetchPaks;
	Uint32 lookupTableNumEntries;
	Uint32 eventMediaTableNumEntries;
};
#pragma pack(pop)

bool wwiseContainerStreamingLoadContainer( const res::ResourcePath containerPath )
{
	Uint16 containerDataId = ( Uint16 )-1;
	if( s_containerDataNum < WWISE_CONTAINER_DATA_MAX )
	{
		containerDataId = s_containerDataNum++;
	}
	else
	{
		// no more slots - consider increasing WWISE_CONTAINER_DATA_MAX
		return false;
	}

	auto containerFile = GetResourceDepot()->CreateResourceAsyncSource( containerPath );
	if( !containerFile )
	{
		return false;
	}

	auto readSizes = containerFile->GetReadSizes( 0, containerFile->GetSizeOnDisk() );
	void* fileReadBuffer = AkAlloc( AkMemID_Object, readSizes.bufferSize );
	RED_ASSERT( fileReadBuffer != nullptr );
	if ( !containerFile->ReadInline( 0, readSizes.diskSize, red::BlobSpan( fileReadBuffer, readSizes.bufferSize ), io::eAsyncPriority_Background, io::RequestSource::AudioSystem_WwiseContainerStreaming, nullptr ) )
	{
		AkFree( AkMemID_Object, fileReadBuffer );
		return false;
	}

	// Get a view of the buffer and then extract out the valid range from the buffer, use that to parse the rest of the file
	red::BlobView fileView = red::BlobView( fileReadBuffer, readSizes.bufferSize ).Range( readSizes.bufferOffset, readSizes.diskSize );

	const ContainerHeader& header = fileView.As< const ContainerHeader >();
	if ( header.fourcc != ContainerHeader::expectedFourcc || header.version != ContainerHeader::expectedVersion || header.lookupTableNumEntries == 0 || header.eventMediaTableNumEntries == 0 )
	{
		AkFree( AkMemID_Object, fileReadBuffer );
		return false;
	}

	Uint32 fileIdsByteSize = header.lookupTableNumEntries * sizeof( Uint32 );
	Uint32 pakIndiciesByteSize = header.lookupTableNumEntries * sizeof( Uint16 );
	Uint32 fileOffsetsByteSize = header.lookupTableNumEntries * sizeof( Uint32 );
	Uint32 riffHeaderSizesByteSize = header.lookupTableNumEntries * sizeof( Uint16 );
	Uint32 compressedFileSizesByteSize = header.lookupTableNumEntries * sizeof( Uint32 );
	Uint32 decompressedFileSizesByteSize = header.lookupTableNumEntries * sizeof( Uint32 );
	Uint32 eventMediaTableByteSize = header.eventMediaTableNumEntries * sizeof( Uint32 );

	Uint32 lookupTableByteSize = fileIdsByteSize + pakIndiciesByteSize + fileOffsetsByteSize + riffHeaderSizesByteSize + compressedFileSizesByteSize + decompressedFileSizesByteSize + eventMediaTableByteSize;
	void* lookupTableBuffer = AkAlloc( AkMemID_Object, lookupTableByteSize ); //  NOTE: This memory is leaked (deliberately?)
	memcpy( lookupTableBuffer, fileView.Data( sizeof(ContainerHeader) ), lookupTableByteSize );

	red::BlobView lookupBufferView{ lookupTableBuffer, lookupTableByteSize };

	WwiseContainerData* containerData = &containerDatas[ containerDataId ];
	containerData->lookupTableNumEntries = header.lookupTableNumEntries;
	containerData->lookupTableFileIds = lookupBufferView.Pointer<Uint32>( 0 );
	containerData->lookupTablePakIndicies = lookupBufferView.Pointer<Uint16>( fileIdsByteSize );
	containerData->lookupTableFileOffsets = lookupBufferView.Pointer<Uint32>( fileIdsByteSize + pakIndiciesByteSize );
	containerData->lookupTableRiffHeaderSizes = lookupBufferView.Pointer<Uint16>( fileIdsByteSize + pakIndiciesByteSize + fileOffsetsByteSize );
	containerData->lookupTableCompressedFileSizes = lookupBufferView.Pointer<Uint32>( fileIdsByteSize + pakIndiciesByteSize + fileOffsetsByteSize + riffHeaderSizesByteSize );
	containerData->lookupTableDecodedFileSizes = lookupBufferView.Pointer<Uint32>(  fileIdsByteSize + pakIndiciesByteSize + fileOffsetsByteSize + riffHeaderSizesByteSize + compressedFileSizesByteSize );
	containerData->eventMediaTableNumEntries = header.eventMediaTableNumEntries;
	containerData->eventMediaTable = lookupBufferView.Pointer<Uint32>( fileIdsByteSize + pakIndiciesByteSize + fileOffsetsByteSize + riffHeaderSizesByteSize + compressedFileSizesByteSize + decompressedFileSizesByteSize );

	hashIndexInitialize( &containerData->lookupTableHashIndex, header.lookupTableNumEntries );
	for( Uint32 i = 0; i < header.lookupTableNumEntries; ++i )
	{
		Uint32 fileId = containerData->lookupTableFileIds[ i ];
		hashIndexAdd( &containerData->lookupTableHashIndex, fileId, i );
	}

	hashIndexInitialize( &containerData->eventMediaTableHashIndex, 1024 );
	Uint32* eventMediaTableCurrentEntry = ( Uint32* )containerData->eventMediaTable;
	for ( Uint32 i = 0; i < header.eventMediaTableNumEntries; )
	{
		Uint32 eventId = eventMediaTableCurrentEntry[ i ];
		hashIndexAdd( &containerData->eventMediaTableHashIndex, eventId, i );
		Uint32 numMedia = eventMediaTableCurrentEntry[ ++i ];
		i += ( 1 + numMedia );
	}

	// static prefetch
	for ( Uint32 i = 0; i < header.numPrefetchPaks; ++i )
	{
		Uint16 containerDepotFileId = containerDepotFilesNum++;

		auto strPath = String::Printf( "base\\sound\\soundbanks\\sfx_container_%u.opuspak", i );
		const res::ResourcePath resourcePath = res::ResourcePath::Build( strPath );
		red::SharedPtr< serialization::IAsyncSource > asyncSource = GetResourceDepot()->CreateResourceAsyncSource( resourcePath );
		if ( !asyncSource )
		{
			AkFree( AkMemID_Object, fileReadBuffer );
			return false;
		}

		WwiseContainerDepotFile* file = &containerDepotFiles[ containerDepotFileId ];
		file->containerDepotFileId = containerDepotFileId;
		file->containerId = containerDataId;
		file->pakFileIndex = i;
		file->lookupTableIndex = ( Uint32 )-1;
		file->file = asyncSource;
		file->refCount = 1;
		file->loading = false;
		file->loaded = 0;
		file->usageId = containerDepotFilesUsageId++;
	}

	AkFree( AkMemID_Object, fileReadBuffer );

	return true;
}

static void wwiseContainerProcessNewReads()
{
	Uint64 newReadsWriteNum = containerOpenFileNewReadWriteNum;
	for( Uint64 i = containerOpenFileNewReadReadNum; i < newReadsWriteNum; ++i )
	{
		Uint16 newReadIndex = i % WWISE_CONTAINER_OPEN_FILE_NEW_READ_MAX;
		WwiseContainerOpenFileNewRead* newRead = &containerOpenFileNewReads[ newReadIndex ];
		AkFileDesc* fileDesc = newRead->fileDesc;
		AkAsyncIOTransferInfo* transferInfo = newRead->transferInfo;

		Uint16 containerReadId = ( Uint16 )-1;
		if( containerOpenFileReadFreeListNum )
		{
			containerReadId = containerOpenFileReadFreeList[ --containerOpenFileReadFreeListNum ];
		}
		else if( containerOpenFileReadNum < WWISE_CONTAINER_OPEN_FILE_READ_MAX )
		{
			containerReadId = containerOpenFileReadNum++;
		}
		else
		{
			// run out of space for reads - consider increasing WWISE_CONTAINER_READ_MAX
			continue;
		}

		WwiseContainerOpenFileRead* containerRead = &containerOpenFileReads[ containerReadId ];
		containerRead->fileDesc = fileDesc;
		containerRead->transferInfo = transferInfo;

		Uint16 containerOpenFileIndex = ( Uint16 )(uintptr_t)fileDesc->pCustomParam;
		WwiseContainerOpenFile* openFile = &containerOpenFiles[ containerOpenFileIndex ];
		openFile->pendingReads += 1;
		WwiseContainerDepotFile* depotFile = &containerDepotFiles[ openFile->containerDepotFileId ];
		depotFile->usageId = containerDepotFilesUsageId++;
		atomic::Increment32( &depotFile->refCount );
	}
	containerOpenFileNewReadReadNum = newReadsWriteNum;
}

static void wwiseContainerCleanupDepotFiles()
{
	Uint32 numUnreffed = 0;
	Uint32 oldestUsageId = ( Uint32 )-1;
	Uint32 oldestUsageIndex = ( Uint32 )-1;
	for ( Int32 i = 0; i < ( Int32 )containerDepotFilesNum; ++i )
	{
		WwiseContainerDepotFile* containerDepotFile = &containerDepotFiles[ i ];
		if ( containerDepotFile->containerDepotFileId == WWISE_CONTAINER_DEPOT_FILE_INVALID_ID )
			continue;
		if ( atomic::FetchValue32( &containerDepotFile->refCount ) == 0 )
		{
			numUnreffed += 1;
			if ( containerDepotFile->usageId < oldestUsageId )
			{
				oldestUsageId = containerDepotFile->usageId;
				oldestUsageIndex = i;
			}
		}
	}

	//TODO [CW] still we might want this flexible caching but at the moment it caches too much
	/*
	Uint64 allocatedMemory = red::memory::GetPoolTotalBytesAllocated< audio::PoolAudio >();
	//NB: opuspacs aren't exactly 1MB so this is an approximation
	Uint64 allocatedMBs = allocatedMemory / RED_MEGA_BYTE(1);
	allocatedMBs -= numUnreffed;
	//200MB is an arbitrary amount just a little over the budget over the audio pool budget. Keep a minimum of 10MBs worth of cache to protect IO.
	//TODO: this could potentially be more dynamic by considering the total memory and then flushing unreffed packs if there is a mem spike. 
	Uint64 availableMBs = allocatedMBs < 50 ? 60 - allocatedMBs : 10;
	*/

	if (numUnreffed > WWISE_CONTAINER_DEPOT_FILE_MAX_UNREFFED) 
	{
		WwiseContainerDepotFile* containerDepotFile = &containerDepotFiles[ oldestUsageIndex ];
		containerDepotFile->containerDepotFileId = WWISE_CONTAINER_DEPOT_FILE_INVALID_ID;
		containerDepotFile->file.Reset();
		containerDepotFile->data.Reset();
		containerDepotFilesFreeList[ containerDepotFilesFreeListNum++ ] = oldestUsageIndex;
	}
}

static void wwiseContainerProcessCompletedTransfers()
{
	Uint64 completedTransfersWriteNum = containerCompletedTransfersWriteNum;
	for( Uint64 i = containerCompletedTransfersReadNum; i < completedTransfersWriteNum; ++i )
	{
		Uint16 completedTransferIndex = i % WWISE_CONTAINER_COMPLETED_TRANSFER_MAX;
		WwiseContainerCompletedTransfer* completedTransfer = &containerCompletedTransfers[ completedTransferIndex ];
		AkFileDesc* fileDesc = completedTransfer->fileDesc;
		AkAsyncIOTransferInfo* transferInfo = completedTransfer->transferInfo;
		Uint16 containerOpenFileIndex = ( Uint16 )(uintptr_t)fileDesc->pCustomParam;
		WwiseContainerOpenFile* openFile = &containerOpenFiles[ containerOpenFileIndex ];
		openFile->pendingReads -= 1;

		WwiseContainerDepotFile* depotFile = &containerDepotFiles[ openFile->containerDepotFileId ];
		if ( atomic::Decrement32( &depotFile->refCount ) == 0 )
			wwiseContainerCleanupDepotFiles();

		transferInfo->pCallback( transferInfo, completedTransfer->success ? AK_Success : AK_Fail );

		completedTransfer->fileDesc = nullptr;
		completedTransfer->transferInfo = nullptr;
		completedTransfer->success = 0;
	}
	containerCompletedTransfersReadNum = completedTransfersWriteNum;
}

void wwiseContainerStreamingPreCallback( AK::IAkGlobalPluginContext* context, AkGlobalCallbackLocation location, void* cookie )
{
	wwiseContainerProcessNewReads();
	wwiseContainerProcessCompletedTransfers();
}

static void wwiseContainerProcessFileIO()
{
	for ( Int32 i = 0; i < ( Int32 )containerDepotFilesNum; ++i )
	{
		WwiseContainerDepotFile* containerDepotFile = &containerDepotFiles[ i ];
		if ( containerDepotFile->containerDepotFileId == WWISE_CONTAINER_DEPOT_FILE_INVALID_ID || containerDepotFile->file == nullptr )
			continue;
		if ( atomic::FetchValue32( &containerDepotFile->refCount ) && !containerDepotFile->loading )
		{
			containerDepotFile->loading = true;
			atomic::Increment32( &containerDepotFile->refCount );

			auto& asyncSource = containerDepotFile->file;
			auto readSizes = asyncSource->GetReadSizes( 0, asyncSource->GetSizeOnDisk() );
			auto buffer = io::ShareableIOMemory(red::CreateUniqueBuffer< audio::PoolAudio >( readSizes.bufferSize, 16 ));

			serialization::IAsyncSource::CallbackContext callbackContext;
			callbackContext.userData = containerDepotFile;
			callbackContext.callback = &AsyncReadCompleteCallback;

			serialization::IAsyncSource::ReadAsyncParams params;
			params.requestSource = io::RequestSource::AudioSystem_WwiseContainerStreaming;
			params.offset = 0;
			params.size = asyncSource->GetSizeOnDisk();
			params.priority = vehicleStreamingModeActive ? io::eAsyncPriority_GAME : io::eAsyncPriority_AUDIO;
			params.allocDeserializationMemoryForDecompressorIfCompressed = false; // not compressed with kraken

			RED_FATAL_ASSERT( buffer.Data() );

			asyncSource->ReadAsync( params, serialization::AsyncSourceReadBuffer( std::move( buffer ), 0, readSizes ), callbackContext );
		}
	}
}

static void wwiseContainerProcessClosedFiles()
{
	Uint64 closedFilesWriteNum = containerClosedFilesWriteNum;
	for( Uint64 i = containerClosedFilesReadNum; i < closedFilesWriteNum; ++i )
	{
		Uint16 closedFilesIndex = i % WWISE_CONTAINER_CLOSED_FILES_MAX;
		Uint16* containerOpenFileIndex = &containerClosedFiles[ closedFilesIndex ];
		WwiseContainerOpenFile* openFile = &containerOpenFiles[ *containerOpenFileIndex ];
		WwiseContainerData* containerData = &containerDatas[ openFile->containerId ];

		AKASSERT( openFile->pendingReads == 0 );

		if ( openFile->opusFile )
		{
			op_free( ( OggOpusFile* )openFile->opusFile );
		}

		WwiseContainerDepotFile* containerDepotFile = &containerDepotFiles[ openFile->containerDepotFileId ];

		if ( atomic::Decrement32( &containerDepotFile->refCount ) == 0 )
		{
			wwiseContainerCleanupDepotFiles();
		}

		memset( openFile, 0, sizeof( WwiseContainerOpenFile ) );
		containerOpenFilesFreeList[ containerOpenFilesFreeListNum++ ] = ( Uint16 )*containerOpenFileIndex;
		*containerOpenFileIndex = 0;
	}
	containerClosedFilesReadNum = closedFilesWriteNum;
}

static void wwiseContainerStreamingAddPrefetchMediaId( Uint32 mediaId )
{
	if ( !mediaId )
		return;

	for ( int i = 0; i < ( int )containerDynamicPrefetchesNum; ++i )
	{
		WwiseContainerDynamicPrefetch* prefetch = &containerDynamicPrefetches[ i ];
		if ( prefetch->mediaId == mediaId )
		{
			prefetch->refCount += 1;
			return;
		}
	}

	AkFileDesc fileDesc;
	if ( !wwiseContainerStreamingOpenFile( mediaId, &fileDesc ) )
	{
		return;
	}

	WwiseContainerDynamicPrefetch* prefetch = NULL;

	if ( containerDynamicPrefetchesFreeListNum )
	{
		prefetch = &containerDynamicPrefetches[ containerDynamicPrefetchesFreeList[ --containerDynamicPrefetchesFreeListNum ] ];
	}

	if ( !prefetch )
	{
		if ( containerDynamicPrefetchesNum == WWISE_CONTAINER_DYNAMIC_PREFETCH_MAX )
		{
			// no more slots - consider increasing WWISE_CONTAINER_DYNAMIC_PREFETCH_MAX
			return;
		}
		prefetch = &containerDynamicPrefetches[ containerDynamicPrefetchesNum++ ];
	}

	prefetch->mediaId = mediaId;
	prefetch->refCount = 1;
	prefetch->fileDesc = fileDesc;
}

static void wwiseContainerStreamingRemovePrefetchMediaId( Uint32 mediaId )
{
	if ( !mediaId )
		return;

	int found = -1;
	for ( int i = 0; i < ( int )containerDynamicPrefetchesNum; ++i )
	{
		WwiseContainerDynamicPrefetch* prefetch = &containerDynamicPrefetches[ i ];
		if ( prefetch->mediaId == mediaId )
		{
			found = i;
			break;
		}
	}

	if ( found == -1 )
		return;

	WwiseContainerDynamicPrefetch* prefetch = &containerDynamicPrefetches[ found ];
	prefetch->refCount -= 1;

	if ( prefetch->refCount == 0 )
	{
		wwiseContainerStreamingCloseFile( &prefetch->fileDesc );
		prefetch->mediaId = 0;
		containerDynamicPrefetchesFreeList[ containerDynamicPrefetchesFreeListNum++ ] = found;
	}
}

static void wwiseContainerStreamingProcessPrefetches()
{
	Uint64 newDynamicPrefetchesWriteNum = containerNewDynamicPrefetchesWriteNum;
	for( Uint64 i = containerNewDynamicPrefetchesReadNum; i < newDynamicPrefetchesWriteNum; ++i )
	{
		Uint16 newDynamicPrefetchesIndex = i % WWISE_CONTAINER_NEW_DYNAMIC_PREFETCH_MAX;
		WwiseContainerNewDynamicPrefetch* newDynamicPrefetch = &containerNewDynamicPrefetches[ newDynamicPrefetchesIndex ];
		Uint32 eventId = newDynamicPrefetch->eventId;

		Uint16 containerId = ( Uint16 )-1;
		Uint32 eventIndex = ( Uint32 )-1;
		for( Uint16 i = 0; i < s_containerDataNum; ++i )
		{
			WwiseContainerData* containerData = &containerDatas[ i ];
			HashIndex* eventMediaTableHashIndex = &containerData->eventMediaTableHashIndex;
			for( int j = hashIndexGetFirst( eventMediaTableHashIndex, eventId ); j != -1; j = hashIndexGetNext( eventMediaTableHashIndex, eventId, j ) )
			{
				int index = eventMediaTableHashIndex->entries[ j ].index;
				if( containerData->eventMediaTable[ index ] == eventId )
				{
					containerId = i;
					eventIndex = index;
					break;
				}
			}
		}
		
		if ( eventIndex == ( Uint32 )-1 )
		{
			continue;
		}

		WwiseContainerData* containerData = &containerDatas[ containerId ];
		const Uint32* eventMediaTableEntry = &containerData->eventMediaTable[ eventIndex ];
		Uint32 numMedia = *( eventMediaTableEntry + 1 );

		switch( newDynamicPrefetch->addToPrefetches )
		{
			case 0:
			{
				for ( Uint32 j = 0; j < numMedia; ++j )
				{
					Uint32 mediaId = *( eventMediaTableEntry + 2 + j );
					wwiseContainerStreamingRemovePrefetchMediaId( mediaId );
				}
				break;
			}
			default:
			{
				for ( Uint32 j = 0; j < numMedia; ++j )
				{
					Uint32 mediaId = *( eventMediaTableEntry + 2 + j );
					wwiseContainerStreamingAddPrefetchMediaId( mediaId );
				}
				break;
			}
		}
	}
	containerNewDynamicPrefetchesReadNum = newDynamicPrefetchesWriteNum;
}

void wwiseContainerStreamingPostCallback( AK::IAkGlobalPluginContext* context, AkGlobalCallbackLocation location, void* cookie )
{
	wwiseContainerProcessClosedFiles();
	wwiseContainerStreamingProcessPrefetches();
	wwiseContainerProcessNewReads();
	wwiseContainerProcessFileIO();

	Uint16 openFileDecodesCreated = 0;
	for( Uint32 i = 0; i < containerOpenFileReadNum; ++i )
	{
		WwiseContainerOpenFileRead* containerRead = &containerOpenFileReads[ i ];
		AkFileDesc* file_desc = containerRead->fileDesc;

		if ( !file_desc )
		{
			// this is a free slot, keep going...
			continue;
		}

		Uint16 containerOpenFileIndex = ( Uint16 )( uintptr_t )file_desc->pCustomParam;
		WwiseContainerOpenFile* openFile = &containerOpenFiles[ containerOpenFileIndex ];
		WwiseContainerDepotFile* depotFile = &containerDepotFiles[ openFile->containerDepotFileId ];

		if ( openFile->opusFile == nullptr )
		{
			if ( atomic::FetchValue32( &depotFile->loaded ) == 1 )
			{
				OggOpusFile* opus_file = op_open_callbacks( openFile, wwiseContainerOpusCallbacks, nullptr, 0, nullptr );
				if( !opus_file )
				{
					containerRead->transferInfo->pCallback( containerRead->transferInfo, AK_Fail );
					openFile->pendingReads -= 1;
					if ( atomic::Decrement32( &depotFile->refCount ) == 0 )
						wwiseContainerCleanupDepotFiles();
					memset( containerRead, 0, sizeof( WwiseContainerOpenFileRead ) );
					containerOpenFileReadFreeList[ containerOpenFileReadFreeListNum++ ] = i;
					continue;
				}
				op_set_dither_enabled( opus_file, 0 );
				openFile->opusFile = ( void* )opus_file;
			}
		}

		OggOpusFile* opusFile = ( OggOpusFile* )openFile->opusFile;
		if( opusFile == nullptr )
		{
			// can't read yet...
			continue;
		}

		// ready to decode...

		Uint16 open_file_decode_index = containerOpenFileDecodeNum % WWISE_CONTAINER_OPEN_FILE_READ_DECODE_MAX;
		WwiseContainerOpenFileDecode* open_file_decode = &containerOpenFileDecodes[ open_file_decode_index ];
		open_file_decode->fileDesc = containerRead->fileDesc;
		open_file_decode->transferInfo = containerRead->transferInfo;
		containerOpenFileDecodeNum += 1;
		openFileDecodesCreated += 1;

		memset( containerRead, 0, sizeof( WwiseContainerOpenFileRead ) );
		containerOpenFileReadFreeList[ containerOpenFileReadFreeListNum++ ] = i;
	}

	if( openFileDecodesCreated )
	{
		containerOpenFileDecodeSemaphore.Release( openFileDecodesCreated );
	}
}

#if defined( RED_PLATFORM_ORBIS ) || defined( RED_PLATFORM_LINUX ) || defined( RED_PLATFORM_WEB )
static void* wwiseContainerStreamingThreadEntryPoint( void* param )
{
#else
static AkUInt32 wwiseContainerStreamingThreadEntryPoint( void* param )
{
#endif
	PROFILER_InitThread( "WwiseDecode", 16384 );
	Uint64 currentOpenFileDecodeCount = 0;
	for ( ; ; )
	{
		containerOpenFileDecodeSemaphore.Acquire();
		if ( containerThreadIsShuttingDown )
			return 0;

		PC_SCOPE( containerDecodeProfileObject );

		Uint32 openFileDecodesIndex = currentOpenFileDecodeCount % WWISE_CONTAINER_OPEN_FILE_READ_DECODE_MAX;
		WwiseContainerOpenFileDecode* openFileDecode = &containerOpenFileDecodes[ openFileDecodesIndex ];
		AkFileDesc* fileDesc = openFileDecode->fileDesc;
		AkAsyncIOTransferInfo* transferInfo = openFileDecode->transferInfo;

		Uint16 containerOpenFileIndex = ( Uint16 )( uintptr_t )fileDesc->pCustomParam;
		WwiseContainerOpenFile* openFile = &containerOpenFiles[ containerOpenFileIndex ];
		WwiseContainerDepotFile* depotFile = &containerDepotFiles[ openFile->containerDepotFileId ];
		WwiseContainerData* containerData = &containerDatas[ openFile->containerId ];
		OggOpusFile* opusFile = ( OggOpusFile* )openFile->opusFile;

		if ( !opusFile || ( depotFile->data.ValidRegion().Size() < ( openFile->pakFileOffset + openFile->riffHeaderSize + openFile->opusFileSize ) ) )
		{
			Uint16 completedTransferIndex = containerCompletedTransfersWriteNum % WWISE_CONTAINER_COMPLETED_TRANSFER_MAX;
			WwiseContainerCompletedTransfer* completedTransfer = &containerCompletedTransfers[ completedTransferIndex ];
			completedTransfer->fileDesc = fileDesc;
			completedTransfer->transferInfo = transferInfo;
			completedTransfer->success = 0;
			atomic::Increment64( &containerCompletedTransfersWriteNum );
			currentOpenFileDecodeCount += 1;
			continue;
		}

		Uint16 numChannels = op_channel_count( opusFile, 0 );
		Uint32 bytesRemaining = transferInfo->uRequestedSize;
		Uint32 transferSuccessful = 1;

		if( transferInfo->uFilePosition < openFile->riffHeaderSize )
		{
			Uint32 bytesRead = ( Uint32 )transferInfo->uFilePosition;
			while( bytesRead < openFile->riffHeaderSize )
			{
				Uint8* writeBuffer = ( Uint8* )( transferInfo->pBuffer ) + bytesRead;
				const void* readBuffer = depotFile->data.ValidRegion().Data( openFile->pakFileOffset + bytesRead );
				memcpy( writeBuffer, readBuffer, openFile->riffHeaderSize );
				bytesRead += openFile->riffHeaderSize;
				bytesRemaining -= openFile->riffHeaderSize;
			}
		}

		Int64 pcmOffset = ( transferInfo->uFilePosition - openFile->riffHeaderSize + ( transferInfo->uRequestedSize - bytesRemaining ) ) / numChannels / sizeof( Int16 );
		op_pcm_seek( opusFile, pcmOffset );

		while( bytesRemaining )
		{
			Uint8* bufferOffset = ( ( Uint8* )transferInfo->pBuffer ) + ( transferInfo->uRequestedSize - bytesRemaining );
			Uint32 samplesInBuffer = bytesRemaining / sizeof( Int16 );
			Int32 samplesRead = op_read( opusFile, ( Int16* )bufferOffset, samplesInBuffer, nullptr );
			if ( samplesRead < 0 )
			{
				transferSuccessful = 0;
				break;
			}
			if ( samplesRead == 0 )
				break;
			Uint32 bytesRead = ( Uint32 )samplesRead* numChannels * sizeof( Int16 );
			bytesRemaining -= bytesRead;
		}

		if( bytesRemaining )
		{
			Uint8* bufferOffset = ( ( Uint8* )transferInfo->pBuffer ) + ( transferInfo->uRequestedSize - bytesRemaining );
			memset( bufferOffset, 0, bytesRemaining );
		}

		Uint16 completedTransferIndex = containerCompletedTransfersWriteNum % WWISE_CONTAINER_COMPLETED_TRANSFER_MAX;
		WwiseContainerCompletedTransfer* completedTransfer = &containerCompletedTransfers[ completedTransferIndex ];
		completedTransfer->fileDesc = fileDesc;
		completedTransfer->transferInfo = transferInfo;
		completedTransfer->success = transferSuccessful;
		atomic::Increment64( &containerCompletedTransfersWriteNum );

		currentOpenFileDecodeCount += 1;
	}

	return 0;
}

void wwiseContainerStreamingAddPrefetch( Uint32 eventId )
{
	if ( containerThreadIsShuttingDown )
		return;

	Uint16 containerNewDynamicPrefetchesIndex = containerNewDynamicPrefetchesWriteNum % WWISE_CONTAINER_NEW_DYNAMIC_PREFETCH_MAX;
	WwiseContainerNewDynamicPrefetch* newDynamicPrefetch = &containerNewDynamicPrefetches[ containerNewDynamicPrefetchesIndex ];
	newDynamicPrefetch->eventId = eventId;
	newDynamicPrefetch->addToPrefetches = 1;
	atomic::Increment64( &containerNewDynamicPrefetchesWriteNum );
}

void wwiseContainerStreamingRemovePrefetch( Uint32 eventId )
{
	if ( containerThreadIsShuttingDown )
		return;

	Uint16 containerNewDynamicPrefetchesIndex = containerNewDynamicPrefetchesWriteNum % WWISE_CONTAINER_NEW_DYNAMIC_PREFETCH_MAX;
	WwiseContainerNewDynamicPrefetch* newDynamicPrefetch = &containerNewDynamicPrefetches[ containerNewDynamicPrefetchesIndex ];
	newDynamicPrefetch->eventId = eventId;
	newDynamicPrefetch->addToPrefetches = 0;
	atomic::Increment64( &containerNewDynamicPrefetchesWriteNum );
}

void SOUND_ENGINE_API wwiseContainerSetVehicleStreaming(Bool active)
{
	vehicleStreamingModeActive = active;
}

bool wwiseContainerStreamingOpenFile( Uint32 fileId, AkFileDesc* fileDesc )
{
	if( containerThreadIsShuttingDown )
	{
		return false;
	}

	Uint16 containerId = ( Uint16 )-1;
	Uint32 lookupTableIndex = ( Uint32 )-1;
	for( Uint16 i = 0; i < s_containerDataNum; ++i )
	{
		WwiseContainerData* containerData = &containerDatas[ i ];
		HashIndex* lookupTableHashIndex = &containerData->lookupTableHashIndex;
		for( int j = hashIndexGetFirst( lookupTableHashIndex, fileId ); j != -1; j = hashIndexGetNext( lookupTableHashIndex, fileId, j ) )
		{
			int index = lookupTableHashIndex->entries[ j ].index;
			if( containerData->lookupTableFileIds[ index ] == fileId )
			{
				containerId = i;
				lookupTableIndex = index;
				break;
			}
		}
	}

	if( ( containerId == ( Uint16 )-1 ) || ( lookupTableIndex == ( Uint32 )-1 ) )
	{
		return false;
	}

	WwiseContainerData* containerData = &containerDatas[ containerId ];
	Uint16 pakFileIndex = containerData->lookupTablePakIndicies[ lookupTableIndex ];

	Uint16 containerOpenFileIndex = ( Uint16 )-1;
	if( containerOpenFilesFreeListNum )
	{
		containerOpenFileIndex = containerOpenFilesFreeList[ --containerOpenFilesFreeListNum ];
	}
	else if( containerOpenFilesNum < WWISE_CONTAINER_OPEN_FILES_MAX )
	{
		containerOpenFileIndex = containerOpenFilesNum++;
	}
	else
	{
		// no more space for open files consider increasing WWISE_CONTAINER_OPEN_FILES_MAX
		return false;
	}

	Uint16 containerDepotFileId = WWISE_CONTAINER_DEPOT_FILE_INVALID_ID;
	for ( Int32 i = 0; i < ( Int32 )containerDepotFilesNum; ++i )
	{
		WwiseContainerDepotFile* file = &containerDepotFiles[ i ];
		if ( file->containerDepotFileId == WWISE_CONTAINER_DEPOT_FILE_INVALID_ID )
		{
			continue;
		}
		if ( file->pakFileIndex == pakFileIndex )
		{
			containerDepotFileId = file->containerDepotFileId;
			atomic::Increment32( &file->refCount );
			break;
		}
	}

	Uint32 fileOffset = containerData->lookupTableFileOffsets[ lookupTableIndex ];
	Uint16 riffHeaderSize = containerData->lookupTableRiffHeaderSizes[ lookupTableIndex ];
	Uint32 fileSize = containerData->lookupTableCompressedFileSizes[ lookupTableIndex ];

	if ( containerDepotFileId == WWISE_CONTAINER_DEPOT_FILE_INVALID_ID )
	{
		if ( containerDepotFilesFreeListNum )
		{
			containerDepotFileId = containerDepotFilesFreeList[ --containerDepotFilesFreeListNum ];
		}
		else if ( containerDepotFilesNum < WWISE_CONTAINER_DEPOT_FILE_MAX )
		{
			containerDepotFileId = containerDepotFilesNum++;
		}
		else
		{
			// no more space for container depot files consider increasing WWISE_CONTAINER_DEPOT_FILE_MAX
			return false;
		}

		// TODO(dan.murray): need the actual container name?
		auto strPath = String::Printf( "base\\sound\\soundbanks\\sfx_container_%u.opuspak", pakFileIndex );
		const res::ResourcePath resourcePath = res::ResourcePath::Build( strPath );
		red::SharedPtr< serialization::IAsyncSource > asyncSource = GetResourceDepot()->CreateResourceAsyncSource( resourcePath );
		if ( !asyncSource )
		{
			return false;
		}

		WwiseContainerDepotFile* file = &containerDepotFiles[ containerDepotFileId ];
		file->containerDepotFileId = containerDepotFileId;
		file->containerId = containerId;
		file->pakFileIndex = pakFileIndex;
		file->lookupTableIndex = lookupTableIndex;
		file->file = asyncSource;
		file->refCount = 1;
		file->loading = false;
		file->loaded = 0;
		file->usageId = containerDepotFilesUsageId++;
	}

	WwiseContainerOpenFile* openFile = &containerOpenFiles[ containerOpenFileIndex ];
	openFile->containerId = containerId;
	openFile->pakFileOffset = fileOffset;
	openFile->riffHeaderSize = riffHeaderSize;
	openFile->opusFilePos = 0;
	openFile->opusFileSize = fileSize - riffHeaderSize;
	openFile->containerDepotFileId = containerDepotFileId;

	Uint64 decodedFileSize = containerData->lookupTableDecodedFileSizes[ lookupTableIndex ];

	fileDesc->iFileSize = ( AkInt64 )( decodedFileSize );
	fileDesc->uSector = 0;
	fileDesc->uCustomParamSize = 0;
	fileDesc->pCustomParam = ( void* )( uintptr_t )containerOpenFileIndex;
	fileDesc->hFile = 0; // don't use because its a different type on ps4
	fileDesc->deviceID = 1;

	return true;
}

void wwiseContainerStreamingCloseFile( AkFileDesc* fileDesc )
{
	if ( containerThreadIsShuttingDown )
		return;

	Uint16 closedFilesIndex = containerClosedFilesWriteNum % WWISE_CONTAINER_CLOSED_FILES_MAX;
	Uint16* containerOpenFileIndex = &containerClosedFiles[ closedFilesIndex ];
	*containerOpenFileIndex = ( Uint16 )( uintptr_t )fileDesc->pCustomParam;
	atomic::Increment64( &containerClosedFilesWriteNum );
}

bool wwiseContainerStreamingRead( AkFileDesc* fileDesc, AkAsyncIOTransferInfo* transferInfo )
{
	if ( containerThreadIsShuttingDown )
		return false;

	Uint16 newReadIndex = containerOpenFileNewReadWriteNum % WWISE_CONTAINER_OPEN_FILE_NEW_READ_MAX;
	WwiseContainerOpenFileNewRead* newRead = &containerOpenFileNewReads[ newReadIndex ];
	newRead->fileDesc = fileDesc;
	newRead->transferInfo = transferInfo;
	atomic::Increment64( &containerOpenFileNewReadWriteNum );
	return true;
}

