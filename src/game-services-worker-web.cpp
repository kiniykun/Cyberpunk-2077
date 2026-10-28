#include "persistence-web.h"
/**
* Copyright (c) 2020 CD Projekt Red. All Rights Reserved.
*/

#include "common/core/include/corePublic.h"
#include "common/gameServices/include/gameServicesInternal.h"
#include "game-services-worker-web.h"
#include "common/gameServices/include/gameServices.h"
#include "common/gameSave/include/saveVersion.h"
#include "common/redMemory/include/uniqueBuffer.h"
#include "common/resource/include/jsonSerializer.h"
#include "common/redFileSystem/include/fileSys.h"
#include "common/redFileSystem/include/system.h"
#include "common/redFileSystem/include/memoryFileWriter.h"
#include "common/redFileSystem/include/rawFileReader.h"
#include "common/redFileSystem/include/rawFileWriter.h"
#include "common/redSystem/include/applicationErrorDumper.h"
#include "common/redContainers/include/string/stringUtils.h"
#include "common/gameSave/include/saveGame.h"

#if defined( RED_CONFIGURATION_FINAL ) && !defined( USE_PROFILER )
// Make sure that we do not store user data in dumps
#define CLEAR_USER_DATA_ON_CRASH
#endif


namespace services {
static ServiceError CommitWebMutation() {
    return cp2077::web::FlushPersistentStorage() ? ServiceError_OK : ServiceError_InternalError;
}
}

namespace
{
	constexpr const char c_saveFilenameExtension[] = "*.dat";
	constexpr const char c_saveFilename[] = "sav.dat";
	constexpr const char c_screenshotFilename[] = "screenshot.png";
	constexpr const Uint32 c_maxSaveNameLength = 200;
	constexpr Uint32 c_saveRequestAttemptCount = 3;
	constexpr Uint32 c_saveRequestAttemptSleep = 500;

	Bool DeleteSave( const red::String& saveName )
	{
		auto saveDir = red::paths::GetSaveDirectory().AddDirPath( saveName );

		red::DynArray< red::AbsolutePath > saveFiles{ red::PoolEngine() };
		GFileManager->FindFiles( saveDir, "*", saveFiles, true );

		if ( saveFiles.Empty() )
		{
			return false;
		}

		for( const auto& file : saveFiles )
		{
			if ( !CSystemIO::DeleteFile( file.AsChar() ) )
			{
				return false;
			}
		}

		return CSystemIO::RemoveDirectory( saveDir.AsChar() );
	}
}

namespace services
{
	namespace crashData
	{
		static red::String s_lastSaveFileName;
		static red::String s_lastMetaFileName;
		static red::String s_lastScreenshotFileName;

		static void RegisterSaveFilesAsCrashDumpAttachments( const red::err::ApplicationDataDumpContext& context )
		{
			RED_UNUSED( context );
			if( !s_lastSaveFileName.Empty() )
			{
				red::RegisterAttachmentForErrorReport( s_lastSaveFileName.AsChar() );
			}
			if( !s_lastMetaFileName.Empty() )
			{
				red::RegisterAttachmentForErrorReport( s_lastMetaFileName.AsChar() );
			}
			if( !s_lastScreenshotFileName.Empty() )
			{
				red::RegisterAttachmentForErrorReport( s_lastScreenshotFileName.AsChar() );
			}
#ifdef CLEAR_USER_DATA_ON_CRASH
			// We crashed so we need wipe out info about user paths. Just clear pointers to the memory.
			// There will be no extra leak as we crashed and the whole process memory will be released by the OS.
			red::Memzero( &s_lastSaveFileName, sizeof( s_lastSaveFileName ) );
			red::Memzero( &s_lastMetaFileName, sizeof( s_lastMetaFileName ) );
			red::Memzero( &s_lastScreenshotFileName, sizeof( s_lastScreenshotFileName ) );
#endif
		}
	}

	GameServicesWebWorker::GameServicesWebWorker(
		red::Mutex& workerMutex,
		red::ConditionVariable& wakeUpWorker )
		: red::Thread( "GameServicesWebWorker" )
		, m_workerMutex( workerMutex )
		, m_wakeUpWorker( wakeUpWorker )
		, m_requests( PoolGameServices() )
		, m_exit( false )
	{
		red::err::ApplicationErrorDumper::RegisterDumperOnce( &crashData::RegisterSaveFilesAsCrashDumpAttachments );
		m_saveBuffer = red::CreateUniqueBuffer< save::PoolGMPL_Save >( RED_MEGA_BYTE( 8 ), RED_KILO_BYTE( 4 ) ); // TODO move
	}

	void GameServicesWebWorker::ThreadFunc()
	{
		while ( !m_exit.GetValue() )
		{
			while ( true )
			{
				{
					RED_SCOPE_LOCK( m_requestsLock );
					if ( !m_requests.Empty() )
					{
						auto& nextRequest = m_requests.Front();
						m_currentRequest = std::move( nextRequest );
						m_requests.RemoveAt( 0 );
					}
					else
					{
						break;
					}
				}

				if ( m_currentRequest )
				{
					m_currentRequest->Execute();

					while ( !m_currentRequest->IsFinished() )
					{
						red::SleepOnCurrentThread( m_currentRequest->GetSleepMs() );

						m_currentRequest->Update();
					}

					m_currentRequest = nullptr;
				}
			}

			if ( m_exit.GetValue() )
			{
				return;
			}

			RED_SCOPE_LOCK( m_workerMutex );
			{
				RED_SCOPE_LOCK(m_requestsLock);
				if (!m_requests.Empty() || m_exit.GetValue()) continue;
			}
			m_wakeUpWorker.Wait( m_workerMutex );
		}
	}

	void GameServicesWebWorker::SavesGather( Uint32 maxAutoSavesCounter, Uint32 maxQuickSavesCounter, OnGameSavesGatheredCallback&& onGathered )
	{
		RED_SCOPE_LOCK(m_workerMutex);
		RED_SCOPE_LOCK( m_requestsLock );
		m_requests.PushBack( red::CreateUniquePtr< SavesGatherRequest >(
			maxAutoSavesCounter,
			maxQuickSavesCounter,
			std::move( onGathered ) ) );
	}

	void GameServicesWebWorker::SavePrepare( save::Metadata& metadata, Bool willOverwrite, OnSavePreparedCallback&& onPrepared )
	{
		RED_SCOPE_LOCK(m_workerMutex);
		RED_SCOPE_LOCK( m_requestsLock );
		m_requests.PushBack( red::CreateUniquePtr< SavePrepareRequest >(
			m_saveBuffer,
			metadata,
			willOverwrite,
			std::move( onPrepared ) ) );
	}

	void GameServicesWebWorker::SaveProcess( const save::MetadataPtr& metadata, OnSaveFinishedCallback&& onFinished )
	{
		RED_SCOPE_LOCK(m_workerMutex);
		RED_SCOPE_LOCK( m_requestsLock );
		m_requests.PushBack( red::CreateUniquePtr< SaveProcessRequest >(
			m_saveBuffer,
			metadata,
			std::move( onFinished ) ) );
	}

	void GameServicesWebWorker::LoadPrepare( const save::MetadataPtr& metadata, ServiceError& errorCode, red::Atomic< Bool >& isFinished, OnLoadStartedCallback&& onPrepared )
	{
		RED_SCOPE_LOCK(m_workerMutex);
		RED_SCOPE_LOCK( m_requestsLock );
		m_requests.PushBack( red::CreateUniquePtr< LoadPrepareRequest >(
			metadata,
			errorCode,
			isFinished,
			std::move( onPrepared ) ) );
	}

	void GameServicesWebWorker::MetadataLoad( const GameSaveView& saveView, OnLoadedMetadataCallback&& onLoaded )
	{
		RED_SCOPE_LOCK(m_workerMutex);
		RED_SCOPE_LOCK( m_requestsLock );
		m_requests.PushBack( red::CreateUniquePtr< MetadataLoadRequest >(
			saveView,
			std::move( onLoaded ) ) );
	}

	void GameServicesWebWorker::SaveRemove( const GameSaveView& saveView, OnSaveRemovedCallback&& onRemoved )
	{
		RED_SCOPE_LOCK(m_workerMutex);
		RED_SCOPE_LOCK( m_requestsLock );
		m_requests.PushBack( red::CreateUniquePtr< SaveRemoveRequest >(
			saveView,
			std::move( onRemoved ) ) );
	}

	void GameServicesWebWorker::SavesRemove( OnSavesRemovedCallback&& onRemoved )
	{
		RED_SCOPE_LOCK(m_workerMutex);
		RED_SCOPE_LOCK( m_requestsLock );
		m_requests.PushBack( red::CreateUniquePtr< SavesRemoveRequest >(
			std::move( onRemoved ) ) );
	}

	void GameServicesWebWorker::ScreenshotCreate(
		Uint32& screenshotSize,
		red::UniqueBuffer& screenshotBuffer,
		OnScreenshotCreatedCallback&& onCreated )
	{
		RED_SCOPE_LOCK(m_workerMutex);
		RED_SCOPE_LOCK( m_requestsLock );
		m_requests.PushBack( red::CreateUniquePtr< ScreenshotCreateRequest >(
			screenshotSize,
			screenshotBuffer,
			std::move( onCreated ) ) );
	}

#if defined( RED_SAVE_SERVER_ENABLED )
	void GameServicesWebWorker::SaveDownload(
		GameSaveView& saveView,
		red::UniqueBuffer& saveBuffer,
		const save::MetadataPtr& metadata,
		OnGameSaveDownloadedCallback&& onDownloaded )
	{
		RED_SCOPE_LOCK(m_workerMutex);
		RED_SCOPE_LOCK( m_requestsLock );
		m_requests.PushBack( red::CreateUniquePtr< SaveDownloadRequest >(
			saveView,
			saveBuffer,
			metadata,
			std::move( onDownloaded ) ) );
	}

	void GameServicesWebWorker::SaveUpload(
		red::UniqueBuffer&& save,
		const red::String& metadataName,
		const save::MetadataPtr& metadata,
		const img::ImagePtr& image,
		OnGameSaveUploadedCallback&& onUploaded )
	{
		RED_SCOPE_LOCK(m_workerMutex);
		RED_SCOPE_LOCK( m_requestsLock );
		m_requests.PushBack( red::CreateUniquePtr< SaveUploadRequest >(
			std::move( save ),
			metadataName,
			metadata,
			image,
			std::move( onUploaded ) ) );
	}
#endif

	void GameServicesWebWorker::Exit()
	{
		RED_SCOPE_LOCK(m_workerMutex);
		m_exit.SetValue(true);
	}

	GameServicesWebWorker::SavesGatherRequest::SavesGatherRequest(
		const Uint32 maxAutoSavesCount,
		const Uint32 maxQuickSavesCount,
		OnGameSavesGatheredCallback&& onGathered )
		: GameServicesWorkerGatherSavesRequest( std::move( onGathered ) )
		, maxAutoSavesCount( maxAutoSavesCount )
		, maxQuickSavesCount( maxQuickSavesCount )
	{}

	void GameServicesWebWorker::SavesGatherRequest::OnExecute()
	{
		const auto& saveDir = red::paths::GetSaveDirectory();
		red::DynArray<red::AbsolutePath> saveFiles{ red::PoolEngine() };
		GFileManager->FindFiles( saveDir, c_saveFilenameExtension, saveFiles, true );

		GameSaveViews saveViews{ PoolGameServices() };
		saveViews.Reserve( saveFiles.Size() );

		Uint32 autoSavesCount = 0;
		Uint32 quickSavesCount = 0;

		for( const auto& save : saveFiles )
		{
			if ( save.AsString().Empty() || save.AsString().Length() > c_maxSaveNameLength )
			{
				RED_LOG_WARNING( "GameServicesWebWorker::GatherSavesRequest: Received save with invalid name" );
				continue;
			}

			GameSaveView saveView;

			saveView.id = red::CalculatePathHash32( save.AsChar(), save.Length() );
			saveView.name = red::paths::ExtractParentPathName( save );
			saveView.date = GFileManager->GetFileTime( save );
			saveViews.PushBack( std::move( saveView ) );
		}

		onGathered( saveViews, ServiceError_OK );
	}

	GameServicesWebWorker::SavePrepareRequest::SavePrepareRequest(
		red::UniqueBuffer& buffer,
		save::Metadata& metadata,
		Bool willOverwrite,
		OnSavePreparedCallback&& onPrepared )
		: GameServicesWorkerPrepareSaveEnvironmentRequest( metadata, willOverwrite, std::move( onPrepared ) )
		, buffer( buffer )
	{}

	void GameServicesWebWorker::SavePrepareRequest::OnExecute()
	{
		const auto saveDirectoryPath = red::paths::GetSaveDirectory().AddDirPath( metadata.m_name );

		if ( willOverwrite )
		{
			if ( !GFileManager->FileExist( saveDirectoryPath ) )
			{
				RED_LOG_WARNING( "GameServicesWebWorker::PrepareSaveEnvironmentRequest: Inconsistent save slot list. Trying to overwrite a save that doesn't exist" );
			}
			else if ( !DeleteSave( metadata.m_name ) )
			{
				RED_LOG_ERROR( "GameServicesNullWorker::PrepareSaveEnvironmentRequest: Failed to delete save '%s'", metadata.m_name.AsChar() );
				onPrepared( metadata.m_name, nullptr, metadata, ServiceError_InternalError );
				return;
			}
		}

#if !defined( RED_CONFIGURATION_FINAL )
		{
			AnsiChar username[256];
			DWORD len = 256;
			GetUserNameA( username, &len );
			metadata.m_userName = username;
		}
#endif

		// TODO request screenshot (like Durango and Orbis)

		auto file = red::CreateUniquePtr< CMemoryFileWriterExternalUniqueBuffer >( buffer );

		onPrepared( metadata.m_name, std::move( file ), metadata, ServiceError_OK );
	}

	GameServicesWebWorker::SaveProcessRequest::SaveProcessRequest(
		red::UniqueBuffer& buffer,
		const save::MetadataPtr& metadata,
		OnSaveFinishedCallback&& onFinished )
		: GameServicesWorkerProcessSavingRequest( metadata, std::move( onFinished ) )
		, buffer( buffer )
	{}

	void GameServicesWebWorker::SaveProcessRequest::Execute()
	{
		OnExecute();
	}

	void GameServicesWebWorker::SaveProcessRequest::OnExecute()
	{
		saveView.name = metadata->m_name;

		const auto saveDirectoryPath = red::paths::GetSaveDirectory().AddDirPath( saveView.name );
		const auto saveFilePath = saveDirectoryPath.AddFilePath( c_saveFilename );

		crashData::s_lastSaveFileName = saveFilePath.AsString();

		// write sav.dat
		{
			red::UniquePtr< IFile > file;

			for ( Uint32 i = 0; i < c_saveRequestAttemptCount; ++i )
			{
				// No buffering required here, the save::Writer will handle its own buffering requirements
				// TODO pass in parameters to control buffering
				file = GFileManager->CreateFileWriter( saveFilePath );
				if ( file )
				{
					break;
				}

				RED_LOG_ERROR( "GameServicesWebWorker::PrepareSaveEnvironmentRequest: Failed to create save '%s' %s", saveFilePath.ToDebugString(), i + 1 < c_saveRequestAttemptCount ? "Trying again..." : "" );
				red::SleepOnCurrentThread( c_saveRequestAttemptSleep );
			}

			if( !file )
			{
				RED_LOG_ERROR( "GameServicesWebWorker::ProcessSavingRequest: Save file serialization failed" );
				onFinished( saveView, ServiceError_InternalError );
				isFinished.SetValue( true );
				return;
			}

			file->Serialize( buffer.Data(), metadata->m_fileSize );
			file->Flush();
			if (file->HasErrors()) {
				onFinished(saveView, ServiceError_InternalError);
				isFinished.SetValue(true);
				return;
			}
		}

		// write metadata
		{
			red::String outString;
			if ( !save::Metadata::Save( *metadata, outString ) )
			{
				RED_LOG_ERROR( "GameServicesWebWorker::ProcessSavingRequest: Medatada serialization failed" );
				onFinished( saveView, ServiceError_InternalError );
				isFinished.SetValue( true );
				return;
			}

			auto metaFile = saveDirectoryPath.AddFilePath( red::String::Printf( "metadata.%d.json", save::MetadataVersion::Current ) );
			crashData::s_lastMetaFileName = metaFile.AsString();
			if (!red::SaveStringToFile(metaFile, outString)) {
				onFinished(saveView, ServiceError_InternalError);
				isFinished.SetValue(true);
				return;
			}
		}

		// Initialize save identity even when an optional screenshot cannot be created.
		saveView.id = red::CalculatePathHash32(saveFilePath.AsChar(), saveFilePath.Length());
		saveView.date = GFileManager->GetFileTime(saveFilePath);
		// write screenshot
		{
			auto screenshotFile = saveDirectoryPath.AddFilePath( c_screenshotFilename );
			crashData::s_lastScreenshotFileName = screenshotFile.AsString();

			m_screenshotFile = GFileManager->CreateFileWriter( screenshotFile, FOF_Buffered );
			if ( !m_screenshotFile )
			{
				RED_LOG_ERROR( "GameServicesWebWorker::ProcessSavingRequest: Failed to create screenshot file" );

				onFinished( saveView, CommitWebMutation() ); // FIXME? error
				isFinished.SetValue( true );
				return;
			}

			// TODO move screenshot request to SavePrepare
			auto onScreenshotCreated = [this, saveFilePath]( const red::String&, const bool succeeded ) mutable
			{
				if ( !succeeded )
				{
					RED_LOG_ERROR( "GameServicesWebWorker::ProcessSavingRequest: Failed to create screenshot file" );
				}

				saveView.id = red::CalculatePathHash32( saveFilePath.AsChar(), saveFilePath.Length() );

				saveView.name = red::paths::ExtractParentPathName( saveFilePath );
				saveView.date = GFileManager->GetFileTime( saveFilePath );

				m_screenshotFile->Flush();
				onFinished( saveView, m_screenshotFile->HasErrors() ? ServiceError_InternalError : CommitWebMutation() );
				isFinished.SetValue( true );
			};

			RequestScreenshot( true, m_screenshotFile.Get(), std::move( onScreenshotCreated ) );
		}
	}

	void GameServicesWebWorker::LoadPrepareRequest::OnExecute()
	{
		auto saveFile = red::paths::GetSaveDirectory().AddDirPath( metadata->m_name );
		saveFile.AppendFilePath( c_saveFilename );

		{
			auto metaFile = red::paths::ParentAbsolutePath( saveFile );
			auto screenshotFile = red::paths::ParentAbsolutePath( saveFile );

			for( auto i = static_cast< Int32 >( save::MetadataVersion::Current ); i >= 0; --i )
			{
				auto metadataName = red::String::Printf( "metadata.%d.json", i );
				auto metaFilePath = metaFile;
				metaFilePath.AppendFilePath( metadataName );
				if ( GFileManager->FileExist( metaFilePath ) )
				{
					metaFile = metaFilePath;
					break;
				}
			}
			screenshotFile.AppendFilePath( c_screenshotFilename );

			crashData::s_lastSaveFileName = saveFile.AsString();
			crashData::s_lastMetaFileName = metaFile.AsString();
			crashData::s_lastScreenshotFileName = screenshotFile.AsString();
		}

		error = ServiceError_OK;
		auto file = GFileManager->CreateFileReader( saveFile, FOF_Buffered );
		if ( !file )
		{
			RED_LOG_ERROR( "GameServicesNullWorker::PrepareLoadEnvironmentRequest: Failed to load file at location %s", saveFile.AsChar() );
			error = ServiceError_InternalError;
		}

		onStarted( metadata->m_name, std::move( file ), isFinished, error );
	}

	void GameServicesWebWorker::MetadataLoadRequest::OnExecute()
	{
		if ( saveView.id == s_invalidGameSaveId )
		{
			onLoaded( saveView, nullptr, nullptr, ServiceError_InvalidSaveID );
			return;
		}

		auto metaFile = red::paths::GetSaveDirectory().AddDirPath( saveView.name );
		auto screenshotFile = red::paths::GetSaveDirectory().AddDirPath( saveView.name );

		{
			for ( auto i = static_cast< Int32 >( save::MetadataVersion::Current ); i >= 0; --i )
			{
				auto metaFilePath = metaFile;
				auto metadataName = red::String::Printf( "metadata.%d.json", i );
				metaFilePath.AppendFilePath( metadataName );
				if ( GFileManager->FileExist( metaFilePath ) )
				{
					metaFile = metaFilePath;
					break;
				}
			}

			const auto file = GFileManager->CreateFileReader( metaFile );
			if ( !file || !save::Metadata::Load( *file, *metadata ) )
			{
				RED_LOG_ERROR( "GameServicesWebWorker::LoadMetadataRequest: Failed load save metadata" );
				onLoaded( saveView, nullptr, nullptr, ServiceError_InternalError );
				return;
			}
		}

		// screenshot
		img::ImagePtr image;
		{
			screenshotFile.AppendFilePath( c_screenshotFilename );
			if ( !GFileManager->FileExist( screenshotFile ) )
			{
				RED_LOG_ERROR( "GameServicesWebWorker::LoadMetadataRequest: Failed load save screenshot" );
				onLoaded( saveView, metadata, nullptr, ServiceError_InternalError );
				return;
			}

			const auto reader = fs::RawFileReader::Create( screenshotFile );
			image = img::BaseImageLoader::Load( *reader, "png" );
		}

		onLoaded( saveView, metadata, image, ServiceError_OK );
	}

	void GameServicesWebWorker::SaveRemoveRequest::OnExecute()
	{
		if ( !DeleteSave( saveView.name ) )
		{
			RED_LOG_ERROR( "GameServicesWebWorker::RemoveSaveRequest: Failed to delete save '%s'", saveView.name.AsChar() );
			onRemoved( saveView, ServiceError_InternalError );
		}
		else
		{
			onRemoved( saveView, CommitWebMutation() );
		}
	}

	void GameServicesWebWorker::SavesRemoveRequest::OnExecute()
	{
		const auto& saveDir = red::paths::GetSaveDirectory();
		red::DynArray< red::AbsolutePath > saveFiles{ red::PoolEngine() };
		GFileManager->FindFiles( saveDir, c_saveFilename, saveFiles, true );

		for( const auto& save : saveFiles )
		{
			auto saveName = red::paths::ExtractParentPathName( save );
			if ( !DeleteSave( saveName ) )
			{
				RED_LOG_ERROR( "GameServicesWebWorker::RemoveSavesRequest: Failed to delete save '%s'", saveName.AsChar() );
				onRemoved( ServiceError_InternalError );
				return;
			}
		}

		onRemoved( CommitWebMutation() );
	}

	GameServicesWebWorker::ScreenshotCreateRequest::ScreenshotCreateRequest(
		Uint32& screenshotSize,
		red::UniqueBuffer& screenshotBuffer,
		OnScreenshotCreatedCallback&& onCreated )
		: screenshotSize( screenshotSize )
		, screenshotBuffer( screenshotBuffer )
		, onCreated( std::move( onCreated ) )
	{}

	void GameServicesWebWorker::ScreenshotCreateRequest::Execute()
	{
		OnExecute();
	}

	static red::AbsolutePath GenerateScreenshotPath()
	{
		CDateTime currentTime;
		red::Clock::GetInstance().GetLocalTime( currentTime );
		red::String dateTimeString;
		ToString( dateTimeString, currentTime );
		dateTimeString.ReplaceAll( " ", "_" );
		dateTimeString.ReplaceAll( "/", "" );
		dateTimeString.ReplaceAll( ":", "" );
		const auto& basePath = red::paths::GetScreenshotDirectory();
		return basePath.AddFilePath( red::StrCat( "photomode_", dateTimeString, ".png" ) );
	}

	void GameServicesWebWorker::ScreenshotCreateRequest::OnExecute()
	{
		const auto filePath = GenerateScreenshotPath();
		screenshotFile = GFileManager->CreateFileWriter( filePath, FOF_Buffered );
		if ( !screenshotFile )
		{
			RED_LOG_ERROR( "GameServicesWebWorker::TakeScreenshotRequest: Failed to create screenshot file" );
			onCreated( ServiceError_InternalError );
			isFinished.SetValue( true );
		}
		else
		{
			auto screenshotGenerationCallback = [this]( const red::String&, bool succeded ) mutable
			{
				if ( !succeded )
				{
					RED_LOG_ERROR( "GameServicesWebWorker::TakeScreenshotRequest: Failed to create screenshot file" );
					onCreated( ServiceError_InternalError );
				}
				else
				{
					screenshotFile->Flush();
					onCreated( screenshotFile->HasErrors() ? ServiceError_InternalError : CommitWebMutation() );
				}

				isFinished.SetValue( true );
			};

			RequestScreenshot( false, screenshotFile.Get(), std::move( screenshotGenerationCallback ) );
		}
	}

#if defined( RED_SAVE_SERVER_ENABLED )
	void GameServicesWebWorker::SaveDownloadRequest::OnExecute()
	{
		if ( saveView.id == s_invalidGameSaveId )
		{
			onDownloaded( saveView, {}, 0, "", nullptr, nullptr, ServiceError_InvalidSaveID );
			return;
		}

		const auto saveDirectoryPath = red::paths::GetSaveDirectory().AddDirPath( saveView.name );

		Uint32 saveSize = 0;

		// sav.dat
		{
			const auto saveFile = saveDirectoryPath.AddFilePath( c_saveFilename );
			if ( !GFileManager->FileExist( saveFile ) )
			{
				onDownloaded( saveView, {}, 0, "", nullptr, nullptr, ServiceError_InvalidSavePath );
				return;
			}

			auto file = GFileManager->CreateFileReader( saveFile, FOF_Encrypted );
			if ( file )
			{
				file->Serialize( saveBuffer.Get(), file->GetSize() );
				saveSize = static_cast< Uint32 >( file->GetSize() );
			}
			else
			{
				RED_LOG_ERROR( "GameServicesNullInterface::OnDownloadSave(): Failed to load file at location %s", saveFile.AsChar() );
				onDownloaded( saveView, {}, 0, "", nullptr, nullptr, ServiceError_InternalError );
				return;
			}
		}

		// metadata
		red::String metadataName;
		{
			auto metaFile = saveDirectoryPath;
			for ( auto i = static_cast< Int32 >( save::MetadataVersion::Current ); i >= 0; --i )
			{
				metadataName = red::String::Printf( "metadata.%d.json", i );
				auto metaFilePath = saveDirectoryPath.AddFilePath( metadataName );
				if ( GFileManager->FileExist( metaFilePath ) )
				{
					metaFile = metaFilePath;
					break;
				}
			}

			const auto file = GFileManager->CreateFileReader( metaFile );
			if ( !file || !save::Metadata::Load( *file, *metadata ) )
			{
				onDownloaded( saveView, saveBuffer, saveSize, metadataName, nullptr, nullptr, ServiceError_InternalError );
				return;
			}
		}

		// screenshot
		img::ImagePtr image;
		{
			const auto screenshotFile = saveDirectoryPath.AddFilePath( c_screenshotFilename );
			if ( !GFileManager->FileExist( screenshotFile ) )
			{
				onDownloaded( saveView, saveBuffer, saveSize, metadataName, metadata, nullptr, ServiceError_InvalidSaveScreenshotPath );
				return;
			}

			const auto reader = fs::RawFileReader::Create( screenshotFile );
			image = img::BaseImageLoader::Load( *reader, "png" );
		}

		onDownloaded( saveView, saveBuffer, saveSize, metadataName, metadata, image, ServiceError_OK );
	}

	void GameServicesWebWorker::SaveUploadRequest::OnExecute()
	{
		GameSaveView saveView;
		saveView.name = metadata->m_name;

		metadata->m_fileSize = static_cast< Uint32>( save.GetSize() );

		auto saveFile = red::paths::GetSaveDirectory().AddDirPath( saveView.name );
		red::AbsolutePath metaFile = saveFile;
		red::AbsolutePath screenshotFile = saveFile;

		// Delete old save if any
		DeleteSave( saveView.name );

		saveFile.AppendFilePath( c_saveFilename );
		saveView.id = red::CalculatePathHash32( saveFile.AsChar(), saveFile.Length() );


		auto file = red::UniquePtr< IFile >( GFileManager->CreateFileWriter( saveFile, FOF_Encrypted ) );
		file->Serialize( save.Get(), save.GetSize() );

		saveView.name = red::paths::ExtractParentPathName( saveFile );
		saveView.date = GFileManager->GetFileTime( saveFile );

		// write metadata
		{
#if !defined( RED_CONFIGURATION_FINAL )
			{
				AnsiChar username[ 256 ];
				DWORD len = 256;
				GetUserNameA( username, &len );
				metadata->m_userName = username;
			}
#endif

			red::String outString;
			auto container = CreateHandle< save::MetadataContainer >( *metadata );
			serialization::json::JsonSerializer metadataSerializer( container );
			metadataSerializer.Serialize( outString );

			metaFile.AppendFilePath( metadataName );
			if ( GFileManager->FileExist( metaFile ) )
			{
				RED_LOG_ERROR( "GameServicesWebWorker::UploadSaveRequest(): Cannot store save metadata at '%s', because such file already exists", metaFile.AsChar());
				onUploaded( saveView, ServiceError_InvalidSaveMetadataPath );
				return;
			}

			if ( !red::SaveStringToFile( metaFile, outString ) )
			{
				RED_LOG_ERROR( "GameServicesWebWorker::UploadSaveRequest(): Cannot store save metadata at '%s' due to internal error", metaFile.AsChar() );
				onUploaded( saveView, ServiceError_InternalError );
				return;
			}
		}

		// write screenshot
		if ( image )
		{
			screenshotFile.AppendFilePath( c_screenshotFilename );
			if ( GFileManager->FileExist( screenshotFile ) )
			{
				RED_LOG_ERROR( "GameServicesWebWorker::UploadSaveRequest(): Cannot store save screenshot at '%s', because such file already exists", screenshotFile.AsChar() );
				onUploaded( saveView, ServiceError_InvalidSaveScreenshotPath );
				return;
			}

			const auto writer = fs::RawFileWriter::Create( screenshotFile, false );
			if ( !img::BaseImageSaver::Save( *writer, image, "png" ) )
			{
				RED_LOG_ERROR( "GameServicesWebWorker::UploadSaveRequest(): Cannot store save screenshot at '%s' due to internal error", screenshotFile.AsChar() );
				onUploaded( saveView, ServiceError_InternalError );
				return;
			}
		}

		onUploaded( saveView, CommitWebMutation() );
	}
#endif

}
