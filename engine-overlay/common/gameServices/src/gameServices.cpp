/**
* Copyright (c) 2016-20 CD Projekt Red. All Rights Reserved.
*/

#include "build.h"
#include "gameServices.h"
#include "gameServicesMessages.h"
#include "gameLocalStorage.h"
#include "gameServicesUser.h"
#include "../../redReflection/include/version.h"
#include "../../redSystem/include/processUtility.h"
#include "../../redCore/include/commandline.h"
#include "../../redCore/include/absolutePath.h"
#include "../../gameSave/include/saveGame.h"
#include "../../redConfig/include/inGameConfigSystem.h"
#include "../../resource/include/archiveLanguageMap.h"
#include "../../app/include/appCensorship.h"
#include "../../redCore/include/languageRegion.h"

RTTI_BEGIN_ABSTRACT_TYPE_IN_NAMESPACE( GameServices, services );
	RTTI_PARENT_TYPE( services::GameServicesGalaxy )
RTTI_END_TYPE();

services::GameServices* GGameServices = nullptr;

namespace services
{
	Uint32 GameServices::s_autoSaveSlots = 20;
	Uint32 GameServices::s_quickSaveSlots = 10;

#if defined( RED_PLATFORM_WINPC ) || defined( RED_PLATFORM_WEB )
	Uint32 GameServices::s_manualSaveSlots = 0; // unlimited
#else
	Uint32 GameServices::s_manualSaveSlots = 20;
#endif

#if defined( RED_PLATFORM_WINPC ) || defined( RED_PLATFORM_WEB )
	Uint32 GameServices::s_pointOfNoReturnSaveSlots = 0; // unlimited
#else
	Uint32 GameServices::s_pointOfNoReturnSaveSlots = 10;
#endif

#if defined( RED_PLATFORM_WINPC ) || defined( RED_PLATFORM_WEB )
	Uint32 GameServices::s_endGameSaveSlots = 0; // unlimited
#else
	Uint32 GameServices::s_endGameSaveSlots = 10;
#endif
}

namespace Config
{
	TConfigVar< Int32 > cvMaxSaveSize( "GameServices", "MaxSaveSize", RED_MEGA_BYTE( 8 ) );
	TConfigVar< Int32 > cvMaxSettingsSize( "GameServices", "MaxSettingsSize", RED_MEGA_BYTE( 4 ) );
	TConfigVar< Int32 > cvMaxProfileSize( "GameServices", "MaxProfileSize", RED_MEGA_BYTE( 4 ) );
	TConfigVar< Int32 > cvScreenshotBufferSize( "GameServices", "ScreenshotBuffer", RED_MEGA_BYTE( 1 ) );

	// Maximum total number of bytes for save directory
	TConfigVar< Int32 > cvMaxSaveSizeTotal( "GameServices", "MaxSaveSizeTotal", RED_MEGA_BYTE( 12 ) );

	TConfigVar< Int32 > cvNumAutoSaveSlots( "Developer/SaveSlotsConfig", "NumAutoSaveSlots", services::GameServices::s_autoSaveSlots, 0, 10000, eConsoleVarFlag_Developer );
	TConfigVar< Int32 > cvNumQuickSaveSlots( "Developer/SaveSlotsConfig", "NumQuickSaveSlots", services::GameServices::s_quickSaveSlots, 0, 10000, eConsoleVarFlag_Developer );
	TConfigVar< Int32 > cvNumManualSaveSlots( "Developer/SaveSlotsConfig", "NumManualSaveSlots", services::GameServices::s_manualSaveSlots, 0, 10000, eConsoleVarFlag_Developer );
	TConfigVar< Int32 > cvNumPointOfNoReturnSaveSlots( "Developer/SaveSlotsConfig", "NumPointOfNoReturnSaveSlots", services::GameServices::s_pointOfNoReturnSaveSlots, 0, 10000, eConsoleVarFlag_Developer );
	TConfigVar< Int32 > cvNumEndGameSaveSlots( "Developer/SaveSlotsConfig", "NumEndGameSaveSlots", services::GameServices::s_endGameSaveSlots, 0, 10000, eConsoleVarFlag_Developer );
}

namespace services
{
	IGameSaveObserver::IGameSaveObserver( GameServices& gameServices )
		: m_gameServices( gameServices )
	{
		m_gameServices.RegisterSaveObserver( this );
	}

	IGameSaveObserver::~IGameSaveObserver()
	{
		m_gameServices.UnregisterSaveObserver( this );
	}

	const AnsiChar* GetDistributionTypeString( DistributionType type )
	{
		switch( type )
		{
		case services::DistributionType::Physical: return "Physical";
		case services::DistributionType::DigitalOwner: return "Digital (Owner)";
		case services::DistributionType::DigitalShared: return "Digital (Shared)";
		}

		return "Unknown";
	}

	GameServices::GameServices()
		: m_lastError( ServiceError::ServiceError_OK )
#if defined( RED_DISTRIBUTION_CHECK_ENABLED )
		, m_distributionType( DistributionType::Unknown )
#endif
		, m_saveViews( PoolGameServices() )
		, m_saveSlot( nullptr )
		, m_saveID( 0 )
		, m_screenshotSize( 0 )
		, m_isBusy( false )
		, m_online( false )
		, m_isStreamingOnAir( false )
		, m_achievementRequests( PoolGameServices() )
		, m_requests( PoolGameServices() )
		, m_currentRequest( nullptr )
		, m_saveObservers( PoolGameServices() )
		, m_pendingMessages( PoolGameServices() )
		, m_userSignOutCallbacks( m_nextSignStateCallbackId )
		, m_userSignInCallbacks( m_nextSignStateCallbackId )
		, m_broadcastingStateChangeCallbacks( m_nextBroadcastingStateChangeCallbackId )
		, m_broadcastingExitSuspendedStateCallbacks( m_nextBroadcastingExitSuspendedStateCallbackId )
		, m_appType( red::AppType::Game )
	{
		m_requests.Insert( RequestType::InGameConfigLoad, red::DynArray< red::UniquePtr< Request > >( PoolGameServices() ) );
		m_requests.Insert( RequestType::InGameConfigSave, red::DynArray< red::UniquePtr< Request > >( PoolGameServices() ) );

		m_requests.Insert( RequestType::UserFileLoad, red::DynArray< red::UniquePtr< Request > >( PoolGameServices() ) );
		m_requests.Insert( RequestType::UserFileSave, red::DynArray< red::UniquePtr< Request > >( PoolGameServices() ) );

#if defined( RED_DISTRIBUTION_CHECK_ENABLED )
		m_requests.Insert( RequestType::CheckDistribution, red::DynArray< red::UniquePtr< Request > >( PoolGameServices() ) );
#endif

		m_requests.Insert( RequestType::SavesGather, red::DynArray< red::UniquePtr< Request > >( PoolGameServices() ) );
		m_requests.Insert( RequestType::Save, red::DynArray< red::UniquePtr< Request > >( PoolGameServices() ) );
		m_requests.Insert( RequestType::Load, red::DynArray< red::UniquePtr< Request > >( PoolGameServices() ) );
		m_requests.Insert( RequestType::MetadataLoad, red::DynArray< red::UniquePtr< Request > >( PoolGameServices() ) );
		m_requests.Insert( RequestType::SaveRemove, red::DynArray< red::UniquePtr< Request > >( PoolGameServices() ) );

		m_requests.Insert( RequestType::RichPresenceSet, red::DynArray< red::UniquePtr< Request > >( PoolGameServices() ) );
		m_requests.Insert( RequestType::ScreenshotCreate, red::DynArray< red::UniquePtr< Request > >( PoolGameServices() ) );

#if defined( RED_SAVE_SERVER_ENABLED )
		m_requests.Insert( RequestType::SaveDownload, red::DynArray< red::UniquePtr< Request > >( PoolGameServices() ) );
		m_requests.Insert( RequestType::SaveUpload, red::DynArray< red::UniquePtr< Request > >( PoolGameServices() ) );
#endif

#if !defined(RED_CONFIGURATION_FINAL) && defined(RED_PLATFORM_WINPC)
		m_requests.Insert( RequestType::FakeSignout, red::DynArray< red::UniquePtr< Request > >( PoolGameServices() ) );
#endif

#if defined(RED_PLATFORM_ORBIS) || defined(RED_PLATFORM_DURANGO)
		m_requests.Insert( RequestType::LanguagePackInstall, red::DynArray< red::UniquePtr< Request > >( PoolGameServices() ) );
#endif
	}

	GameServices::~GameServices() = default;

	Bool GameServices::Initialize( const red::AppType appType )
	{
		m_appType = appType;
		if ( appType == red::AppType::Game )
		{
			SlotsConfig cfg{};

			cfg.m_numSlots[SlotType::AutoSave] = Config::cvNumAutoSaveSlots.Get();
			cfg.m_numSlots[SlotType::QuickSave] = Config::cvNumQuickSaveSlots.Get();
			cfg.m_numSlots[SlotType::ManualSave] = Config::cvNumManualSaveSlots.Get();
			cfg.m_numSlots[SlotType::PointOfNoReturn] = Config::cvNumPointOfNoReturnSaveSlots.Get();
			cfg.m_numSlots[SlotType::EndGameSave] = Config::cvNumPointOfNoReturnSaveSlots.Get();

			cfg.m_autoOverwriting[SlotType::AutoSave] = true;
			cfg.m_autoOverwriting[SlotType::QuickSave] = true;
			cfg.m_autoOverwriting[SlotType::ManualSave] = false;
			cfg.m_autoOverwriting[SlotType::PointOfNoReturn] = true;
			// FIXME(#jprzystolik): They shouldn't overwrite without prompt. This needs a separate handling.
			cfg.m_autoOverwriting[SlotType::EndGameSave] = true;

			m_saveSlots = red::CreateUniquePtr< SlotsUtility >( cfg );
			m_screenshotBuffer = red::CreateUniqueBuffer< PoolGameServices >( GetMaxScreenshotSize(), 8 );

			m_gameLocalStorage = red::CreateUniquePtr< GameLocalStorage >();

			InGameConfig::System::GetInstance().RegisterNotifier( [this]( InGameConfig::NotificationType type )
			{
				if ( type == InGameConfig::NotificationType::LoadInternalError )
				{
					SaveInGameConfig();
				}
			} );

			m_broadcastingStateChangeCallback = SubscribeBroadcastingStateChangeEvent( [this]( const Bool state )
			{
				m_isBroadcasting = state;
			} );
		}

		return true;
	}

	void GameServices::Shutdown()
	{
		if( m_broadcastingStateChangeCallback != c_invalidBroadcastingStateChangeCallbackId )
		{
			UnsubscribeBroadcastingStateChangeEvent( m_broadcastingStateChangeCallback );
			m_broadcastingStateChangeCallback = c_invalidBroadcastingStateChangeCallbackId;
		}

		m_gameLocalStorage.Reset();
		m_saveSlots = nullptr;
	}

	void GameServices::Update()
	{
		RED_FATAL_ASSERT( ::SIsMainThread(), "Calls to game services may only be made on the main thread." );
		PC_SCOPE( GameServices::Update );

		InGameConfig::System &system = InGameConfig::System::GetInstance();

		if ( system.CanSaveUserSettings() && system.RequestedSaveDuringLoad() )
		{
			SaveInGameConfig();
			system.ResetRequestedSaveDuringLoad();
		}

		if ( !m_isBusy.GetValue() )
		{
			if ( !m_currentRequest )
			{
				RED_SCOPE_LOCK( m_requestsLock );

				for ( auto it = m_achievementRequests.Begin(); it != m_achievementRequests.End(); ++it )
				{
					auto& nextRequest = *it;
					m_currentRequest = std::move( nextRequest );
					m_achievementRequests.RemoveAt( 0 );
					break;
				}

				if ( !m_currentRequest )
				{
					for ( auto it = m_requests.Begin(); it != m_requests.End(); ++it )
					{
						auto& requests = it.Value();
						if ( !requests.Empty() )
						{
							auto& nextRequest = requests.Front();
							m_currentRequest = std::move( nextRequest );
							requests.RemoveAt( 0 );
							break;
						}
					}
				}
			}

			if ( m_currentRequest )
			{
				RED_FATAL_ASSERT( !m_isBusy.GetValue(), "GameServices: Invalid internal state. Game services is busy, but it shouldn't be." );
				m_isBusy.SetValue( true );
				m_currentRequest->Execute( *this );
			}
		}
	}

	red::AppType GameServices::GetAppType() const
	{
		return m_appType;
	}

	void GameServices::AssignUser( Uint64 controllerID )
	{
	}

	Bool GameServices::HasUserAssigned() const
	{
		return false;
	}

	IGameServicesUser* GameServices::GetUser() const
	{
		return nullptr;
	}

	void GameServices::OnExitSuspendedState()
	{
		DispatchBroadcastingExitSuspendedStateEvent();
	}

#if !defined(RED_CONFIGURATION_FINAL) && defined(RED_PLATFORM_WINPC)
	void GameServices::FakeSignout()
	{
		SubmitRequest( RequestType::FakeSignout, red::CreateUniquePtr< FakeSignoutRequest >( [] {} ) );
	}
#endif

#if defined( RED_DISTRIBUTION_CHECK_ENABLED )
	void GameServices::CheckDistribution( OnDistributionCheckedCallback&& onDistributionCheckedCallback )
	{
		SubmitRequest( RequestType::CheckDistribution, red::CreateUniquePtr< CheckDistributionRequest >( std::move( onDistributionCheckedCallback ) ) );
	}
#endif

	void GameServices::RegisterSaveObserver( IGameSaveObserver* observer )
	{
		RED_SCOPE_LOCK( m_savesLock );
		m_saveObservers.PushBack( observer );
	}

	void GameServices::UnregisterSaveObserver( IGameSaveObserver* observer )
	{
		RED_SCOPE_LOCK( m_savesLock );
		m_saveObservers.Remove(
			std::remove_if( m_saveObservers.Begin(), m_saveObservers.End(), [observer]( const IGameSaveObserver* otherObserver ){ return otherObserver == observer; } )
			, m_saveObservers.End() );
	}

	Bool GameServices::IsBusy() const
	{
		return m_isBusy.GetValue();
	}

	void GameServices::RequestSaves( OnGameSavesGatheredCallback&& onGameSavesGathered )
	{
		if( m_savesReady )
		{
			onGameSavesGathered( m_saveViews, ServiceError_OK );
			return;
		}

		SubmitRequest( RequestType::SavesGather, red::CreateUniquePtr< GatherSavesRequest >( std::move( onGameSavesGathered ) ) );
	}

	void GameServices::Save( const save::MetadataPtr& metadata, OnSaveStartedCallback&& onStarted, OnSaveFinishedCallback&& onFinished )
	{
		SubmitRequest( RequestType::Save, red::CreateUniquePtr< SaveRequest >( metadata, std::move( onStarted ), std::move( onFinished ) ) );
	}

	void GameServices::Load( const save::MetadataPtr& metadata, OnLoadStartedCallback&& onStarted, OnLoadFinishedCallback&& onFinished )
	{
		SubmitRequest( RequestType::Load, red::CreateUniquePtr< LoadRequest >( metadata, std::move( onStarted ), std::move( onFinished ) ) );
	}

	void GameServices::LoadMetadata( const GameSaveView& saveView, OnLoadedMetadataCallback&& onLoaded )
	{
		SubmitRequest( RequestType::MetadataLoad, red::CreateUniquePtr< LoadMetadataRequest >( saveView, std::move( onLoaded ) ) );
	}

	void GameServices::CancelLoadMetadataRequests()
	{
		ClearRequests( RequestType::MetadataLoad );
	}

	save::MetadataPtr GameServices::GetLatestSaveMetadata() const
	{
		return m_latestSaveMetadata;
	}

	void GameServices::RemoveSave( const GameSaveView& saveView, OnSaveRemovedCallback&& onRemoved )
	{
		SubmitRequest( RequestType::SaveRemove, red::CreateUniquePtr< RemoveSaveRequest >( saveView, std::move( onRemoved ) ) );
	}

	void GameServices::RemoveAllSaves( OnSavesRemovedCallback&& onRemoved )
	{
		SubmitRequest( RequestType::SavesRemove, red::CreateUniquePtr< RemoveSavesRequest >( std::move( onRemoved ) ) );
	}

	void GameServices::UnlockAchievement( const THandle< game::data::Achievement_Record >& achievement, Float percentComplete, OnAchievementUnlockedCallback&& onUnlocked )
	{
		RED_SCOPE_LOCK( m_requestsLock );

		m_achievementRequests.PushBack( red::CreateUniquePtr< UnlockAchievementRequest >( achievement, percentComplete, std::move( onUnlocked ) ) );
	}

	void GameServices::LoadInGameConfig()
	{
		auto callback = []( red::UniquePtr< IFile > file, const ServiceError errorCode )
		{
			InGameConfig::System::GetInstance().LoadUserSettings( file.Get(), errorCode );
		};

		SubmitRequest( RequestType::InGameConfigLoad, red::CreateUniquePtr< LoadInGameConfigRequest >( callback ) );
	}

	void GameServices::SaveInGameConfig()
	{
		auto callback = []( red::UniquePtr< IFile > file, const ServiceError errorCode )
		{
			auto &system = InGameConfig::System::GetInstance();
			const Uint32 written = system.SaveUserSettings( file.Get(), errorCode );

			return written;
		};

		SubmitRequest( RequestType::InGameConfigSave, red::CreateUniquePtr< SaveInGameConfigRequest >( callback ) );
	}

	void GameServices::ApplyCensorship()
	{
		if ( auto currentUser = GetUser() )
		{
			auto userRegion = currentUser->GetUserRegion();
			if ( userRegion.Empty() )
			{
				// Empty region means some sort of error
				return;
			}

			app::ApplyCensorshipFromRegion( RED_NAME( userRegion.AsChar() ), app::CensorRegionSource::FromOnlineStore );

			SaveInGameConfig();
		}
	}

	void GameServices::RequestLoadUserFile( const red::String& fileName, OnLoadUserFileCallback&& callback )
	{
		SubmitRequest( RequestType::UserFileLoad, red::CreateUniquePtr< LoadUserFileRequest >( fileName, std::move( callback ) ) );
	}

	void GameServices::SaveUserFile( const red::String& fileName, OnSaveUserFileDataCallback&& onData, OnSaveUserFileFinishedCallback&& onFinished )
	{
		SubmitRequest( RequestType::UserFileSave, red::CreateUniquePtr< SaveUserFileRequest >( fileName, std::move( onData ), std::move( onFinished ) ) );
	}

	void GameServices::TakeFullScreenshot( OnScreenshotCreatedCallback&& callback )
	{
		SubmitRequest( RequestType::ScreenshotCreate, red::CreateUniquePtr< TakeScreenshotRequest >( std::move( callback ) ) );
	}

	void GameServices::OpenWebBrowser( const red::String& url )
	{
		OnOpenWebBrowser( url );
	}

#if defined( RED_SAVE_SERVER_ENABLED )
	void GameServices::DownloadSave( const GameSaveView& saveView, OnGameSaveDownloadedCallback&& onDownloaded )
	{
		SubmitRequest( RequestType::SaveDownload, red::CreateUniquePtr< DownloadSaveRequest >( saveView, std::move( onDownloaded ) ) );
	}

	void GameServices::UploadSave( red::UniqueBuffer&& save, const red::String& metadataName, const save::MetadataPtr& metadata, const img::ImagePtr& screenshot, OnGameSaveUploadedCallback&& onUploaded )
	{
		SubmitRequest( RequestType::SaveUpload, red::CreateUniquePtr< UploadSaveRequest >( std::move( save ), metadataName, metadata, screenshot, std::move( onUploaded ) ) );
	}
#endif

#if defined(RED_PLATFORM_ORBIS) || defined(RED_PLATFORM_DURANGO)
	void GameServices::RequestInstallLanguagePack( const CName languageCodeName )
	{
		const auto langID = res::ToLanguageID( languageCodeName.AsStringView() );
		SubmitRequest( RequestType::LanguagePackInstall, red::CreateUniquePtr< InstallLanguagePackRequest >( langID ) );
	}

	void GameServices::OnLanguagePackInstallFinished()
	{
		ResetState();
	}
#endif

	void GameServices::SetRichPresence( const RichPresenceStatus status )
	{
		SubmitRequest( RequestType::RichPresenceSet, red::CreateUniquePtr< SetRichPresenceRequest >( status ) );
	}

	Bool GameServices::HasFreeSaveSlot( const red::String& saveName ) const
	{
		const SlotType type = SlotsUtility::NameToSlotType( saveName );

		RED_SCOPE_LOCK( m_savesLock );
		return m_saveSlots->HasFreeSlot( type );
	}

	Bool GameServices::FindFreeSaveSlot( const save::MetadataPtr& metadata, SlotInfo*& freeSlot, Bool& willOverwrite )
	{
		const SlotType type = SlotsUtility::NameToSlotType( metadata->m_name );

		{
			RED_SCOPE_LOCK( m_savesLock );
			if ( m_saveSlots->FindSlot( type, metadata->m_name, freeSlot ) )
			{
				willOverwrite = true;
			}
			else if( !m_saveSlots->FindFreeSlot( type, freeSlot, metadata->m_name, willOverwrite ) )
			{
				RED_LOG_CATEGORY_ERROR( red::LoggerCategory_Services, "Failed to find save slot for '%s'", metadata->m_name.AsChar() );
				return false;
			}
		}

		RED_LOG_CATEGORY_INFO( red::LoggerCategory_Services, "%s save data '%s'", willOverwrite ? "Overwriting" : "Creating new", metadata->m_name.AsChar() );
		m_cachedSaveName = metadata->m_name;
		return true;
	}

	void GameServices::ResetUserData()
	{
		{
			RED_SCOPE_LOCK( m_savesLock );
			m_savesReady = false;
			m_saveViews.Clear();
		}

		if( m_gameLocalStorage )
		{
			m_gameLocalStorage->CleanupOnUserSignOut();
		}
	}

	Uint32 GameServices::GetMaxSaveSize() const
	{
		return Config::cvMaxSaveSize.Get();
	}

	Uint32 GameServices::GetMaxSaveSizeTotal() const
	{
		return Config::cvMaxSaveSizeTotal.Get();
	}

	Uint32 GameServices::GetMaxScreenshotSize() const
	{
		return Config::cvScreenshotBufferSize.Get();
	}

	Uint32 GameServices::GetMaxSettingsSize() const
	{
		return Config::cvMaxSettingsSize.Get();
	}

	Uint32 GameServices::GetMaxProfileSize() const
	{
		return Config::cvMaxProfileSize.Get();
	}

	void GameServices::GetMessages( MessageQueue& queue )
	{
		RED_SCOPE_LOCK( m_pendingMessagesLock );
		if( m_pendingMessages.Empty() )
		{
			return;
		}
		queue.PushBack( m_pendingMessages );
		m_pendingMessages.Clear();
	}

#if defined( RED_DISTRIBUTION_CHECK_ENABLED )
	void GameServices::OnDistributionChecked()
	{
		RED_LOG_INFO( "GameServicesDurango::CheckDistributionRequest: Detected \"%s\" distribution", GetDistributionTypeString( m_distributionType ) );
		static_cast< CheckDistributionRequest* >( m_currentRequest.Get() )->callback( m_distributionType, m_lastError );
		m_distributionType = DistributionType::Unknown;

		ResetState();
	}
#endif // RED_DISTRIBUTION_CHECK_ENABLED

	void GameServices::OnSavesGathered()
	{
		RED_SCOPE_LOCK( m_savesLock );

		RED_FATAL_ASSERT( m_saveSlots != nullptr, "Save slots does not exist." );
		std::sort( m_saveViews.Begin(), m_saveViews.End(), std::greater<>() );
		m_saveSlots->Populate( m_saveViews );

		const auto saveError = m_lastError;
		if( saveError == ServiceError_OK )
		{
			m_savesReady = true;
		}

		static_cast< GatherSavesRequest* >( m_currentRequest.Get() )->callback( m_saveViews, saveError );

		ResetState();
	}

	void GameServices::OnSaveStarted( const red::StringView saveName, save::IStream& stream, save::Metadata& metadata )
	{
		m_isSaveFinished.SetValue( false );
		auto saveError = m_lastError;

		const auto request = static_cast< SaveRequest* >( m_currentRequest.Get() );
		request->onStarted( saveName, stream, metadata, m_isSaveFinished, saveError );

		m_lastError = saveError;
	}

	void GameServices::OnSaveFinished( const GameSaveView& saveView, SlotInfo* saveSlot )
	{
		const auto request = static_cast< SaveRequest* >( m_currentRequest.Get() );
		request->onFinished( saveView, m_lastError );

		if ( m_lastError == ServiceError_OK )
		{
			if ( saveSlot )
			{
				saveSlot->id = saveView.id;
				saveSlot->date = saveView.date;
			}

			m_latestSaveMetadata = m_metadata;

			// don't take into account end game save for the "Continue" button functionality
			if( saveSlot->type != SlotType::EndGameSave )
			{
				services::GGameLocalStorage->SetString( RED_NAME_CONSTEXPR( "LastSaveName" ), saveView.name );
				services::GGameLocalStorage->RequestSave();
			}

			RED_SCOPE_LOCK( m_savesLock );

			const auto it = std::find_if( m_saveViews.Begin(), m_saveViews.End(), [saveID = saveView.id]( const GameSaveView& saveView ){ return saveView.id == saveID; } );
			if ( it != m_saveViews.End() )
			{
				*it = saveView;
			}
			else
			{
				m_saveViews.PushBack( saveView );
			}

			for ( auto* observer : m_saveObservers )
			{
				observer->OnGameSaved( saveView );
			}
		}

		ResetState();
	}

	void GameServices::OnLoadStarted( const red::StringView saveName, LoadPtr save )
	{
		m_isLoadFinished.SetValue( false );
		auto saveError = m_lastError;

		const auto request = static_cast< LoadRequest* >( m_currentRequest.Get() );
		request->onStarted( saveName, std::move( save ), m_isLoadFinished, saveError );

		m_lastError = saveError;
	}

	void GameServices::OnLoadFinished( const red::StringView saveName )
	{
		const auto saveError = m_lastError;

		const auto request = static_cast< LoadRequest* >( m_currentRequest.Get() );
		request->onFinished( saveName, saveError );

		services::GGameLocalStorage->SetString( RED_NAME_CONSTEXPR( "LastSaveName" ), saveName );
		services::GGameLocalStorage->RequestSave();

		ResetState();
	}

	void GameServices::OnMetadataLoadFinished( const GameSaveView& saveView, const save::MetadataPtr& metadata, const img::ImagePtr& image )
	{
		const auto request = static_cast< LoadMetadataRequest* >( m_currentRequest.Get() );
		request->onFinished( saveView, metadata, image, m_lastError );

		ResetState();
	}

	void GameServices::OnRemoveSaveFinished( const GameSaveView& saveView )
	{
		const auto saveError = m_lastError;

		{
			RED_SCOPE_LOCK( m_savesLock );
			m_saveSlots->FreeSlot( saveView );
			m_saveViews.Remove( m_saveView );
		}

		const auto request = static_cast< RemoveSaveRequest* >( m_currentRequest.Get() );
		request->onRemoved( saveView, saveError );

		ResetState();
	}

	void GameServices::OnRemoveSavesFinished()
	{
		const auto saveError = m_lastError;

		const auto request = static_cast< RemoveSavesRequest* >( m_currentRequest.Get() );
		request->onRemoved( saveError );

		{
			RED_SCOPE_LOCK( m_savesLock );
			m_saveSlots->Clear();
			m_saveViews.Clear();
		}

		ResetState();
	}

	void GameServices::OnAchievementUnlocked()
	{
		const auto saveError = m_lastError;

		const auto request = static_cast< UnlockAchievementRequest* >( m_currentRequest.Get() );
		request->onUnlocked( saveError );

		ResetState();
	}

	void GameServices::OnInGameConfigLoadStarted( InGameConfigPtr ptr )
	{
		const auto request = static_cast< LoadInGameConfigRequest* >( m_currentRequest.Get() );
		request->onLoaded( std::move( ptr ), m_lastError );
	}

	void GameServices::OnInGameConfigLoadFinished()
	{
		ResetState();
	}

	Uint32 GameServices::OnInGameConfigSaveStarted( InGameConfigPtr ptr )
	{
		const auto request = static_cast< SaveInGameConfigRequest* >( m_currentRequest.Get() );
		const auto bytesWritten = request->onSaved( std::move( ptr ), m_lastError );

		return bytesWritten;
	}

	void GameServices::OnInGameConfigSaveFinished()
	{
		ResetState();
	}

	Uint32 GameServices::OnUserFileSaveStarted( UserFilePtr ptr )
	{
		const auto request = static_cast< SaveUserFileRequest* >( m_currentRequest.Get() );
		const auto bytesWritten = request->onData( std::move( ptr ) );

		return bytesWritten;
	}

	void GameServices::OnUserFileSaveFinished()
	{
		const auto request = static_cast< SaveUserFileRequest* >( m_currentRequest.Get() );
		request->onFinished( m_lastError );

		ResetState();
	}

	void GameServices::OnUserFileLoadStarted( UserFilePtr ptr )
	{
		const auto request = static_cast< LoadUserFileRequest* >( m_currentRequest.Get() );
		request->onLoadUserFileCallback( std::move( ptr ), m_lastError );
	}

	void GameServices::OnUserFileLoadFinished()
	{
		ResetState();
	}

	void GameServices::OnScreenshotCreated()
	{
		const auto request = static_cast< TakeScreenshotRequest* >( m_currentRequest.Get() );
		request->onCreated( m_lastError );

		ResetState();
	}

#if defined( RED_SAVE_SERVER_ENABLED )
	void GameServices::OnSaveDownloadFinished( const GameSaveView& saveView, const red::UniqueBuffer& save, Uint32 saveSize, const red::String& metadataName, const save::MetadataPtr& metadata, const img::ImagePtr& image )
	{
		const auto saveError = m_lastError;

		const auto request = static_cast< DownloadSaveRequest* >( m_currentRequest.Get() );
		request->onDownloaded( saveView, save, saveSize, metadataName, metadata, image, saveError );

		ResetState();
	}

	void GameServices::OnSaveUploadFinished( const GameSaveView& saveView, SlotInfo* saveSlot )
	{
		const auto saveError = m_lastError;

		const auto request = static_cast< UploadSaveRequest* >( m_currentRequest.Get() );
		request->onUploaded( saveView, saveError );

		if ( saveError == ServiceError_OK )
		{
			if ( saveSlot )
			{
				saveSlot->id = saveView.id;
				saveSlot->date = saveView.date;
			}

			RED_SCOPE_LOCK( m_savesLock );

			const auto it = std::find_if( m_saveViews.Begin(), m_saveViews.End(), [saveID = saveView.id]( const GameSaveView& saveView ){ return saveView.id == saveID; } );
			if ( it != m_saveViews.End() )
			{
				*it = saveView;
			}
			else
			{
				m_saveViews.PushBack( saveView );
			}

			for ( auto* observer : m_saveObservers )
			{
				observer->OnGameSaved( saveView );
			}
		}

		ResetState();
	}
#endif

	void GameServices::OnRichPresenceSetFinished()
	{
		ResetState();
	}

	void GameServices::ResetState()
	{
		m_currentRequest = nullptr;
		m_lastError = ServiceError_OK;
		m_isBusy.SetValue( false );
	}

	Bool GameServices::IsOnline() const
	{
		return m_online;
	}

	void GameServices::PushMessage( const CName messageName, const red::String& messageParams )
	{
		ExternalMessage msg( messageName, messageParams );
		RED_SCOPE_LOCK( m_pendingMessagesLock );
		m_pendingMessages.PushBack( std::move( msg ) );
	}

	Int32 GameServices::GetCachedSavesCount() const
	{
		RED_SCOPE_LOCK( m_savesLock );
		return m_saveViews.Size();
	}

	Bool GameServices::IsSaveExist( const red::String& saveName ) const
	{
		RED_ASSERT( m_savesReady );
		RED_SCOPE_LOCK( m_savesLock );
		return m_saveViews.End() != std::find_if( m_saveViews.Begin(), m_saveViews.End(), [&saveName]( const GameSaveView& saveView ) -> Bool { return saveView.name == saveName; } );
	}

	void GameServices::OnUserSignOut( const Bool currentUser )
	{
		red::String msgParam;
		if( currentUser )
		{
			ResetUserData();
			DispatchUserSignOutCallback();

			msgParam = "CurrentUser";
		}
		PushMessage( RED_NAME_CONSTEXPR( "GameServicesUserSignOutComplete" ), msgParam );
	}

	Bool GameServices::RequiresAutoSignIn() const
	{
		const auto& commandLine = red::CommandLine::Get();
		return commandLine.HasOption( "functionalTestsMode" ) || commandLine.HasOption( "gameDefinition" );
	}

	template<typename IdType, typename CallbackType, typename... Args >
	IdType GameServices::UserCallback<IdType, CallbackType, Args...>::Subscribe( CallbackType&& callback )
	{
		RED_SCOPE_LOCK( m_lock );
		RED_ASSERT( !m_dispatching, "Subscribing new callback from the callback is not allowed." );

		const IdType callbackId = m_idGenerator->Increment();

		CallbackData callbackData;
		callbackData.id = callbackId;
		callbackData.callback = std::move( callback );
		m_callbacks.PushBack( std::move( callbackData ) );
		return callbackId;
	}

	template<typename IdType, typename CallbackType, typename... Args >
	void GameServices::UserCallback<IdType, CallbackType, Args...>::Unsubscribe( const IdType callbackId )
	{
		RED_SCOPE_LOCK( m_lock );
		RED_ASSERT( !m_dispatching, "Unsubscribing callback from the callback is not allowed." );

		auto it = std::find_if( m_callbacks.Begin(), m_callbacks.End(), [ callbackId ]( const CallbackData& data )
		{
			return data.id == callbackId;
		} );

		if( it == m_callbacks.End() )
		{
			return;
		}

		m_callbacks.Remove( it );
	}

	template<typename IdType, typename CallbackType, typename... Args >
	void GameServices::UserCallback<IdType, CallbackType, Args...>::Dispatch( Args... args )
	{
		RED_SCOPE_LOCK( m_lock );
		m_dispatching = true;

		for( const CallbackData& callbackData : m_callbacks )
		{
			callbackData.callback( std::forward<Args>(args)... );
		}

		m_dispatching = false;
	}

	UserSignStateCallbackId GameServices::SubscribeUserSignOutCallback( OnUserSignOutCallback&& callback )
	{
		return m_userSignOutCallbacks.Subscribe( std::move( callback ) );
	}

	void GameServices::UnsubscribeUserSignOutCallback( const UserSignStateCallbackId callbackId )
	{
		m_userSignOutCallbacks.Unsubscribe( callbackId );
	}

	void GameServices::DispatchUserSignOutCallback()
	{
		m_userSignOutCallbacks.Dispatch();
	}

	UserSignStateCallbackId GameServices::SubscribeUserSignInCallback( OnUserSignInCallback&& callback )
	{
		return m_userSignInCallbacks.Subscribe( std::move( callback ) );
	}

	void GameServices::UnsubscribeUserSignInCallback( const UserSignStateCallbackId callbackId )
	{
		m_userSignInCallbacks.Unsubscribe( callbackId );
	}

	void GameServices::DispatchUserSignInCallback()
	{
		m_userSignInCallbacks.Dispatch();
	}

	BroadcastingStateChangeCallbackId GameServices::SubscribeBroadcastingStateChangeEvent( OnBroadcastingStateChangeCallback&& callback )
	{
		return m_broadcastingStateChangeCallbacks.Subscribe( std::move( callback ) );
	}

	void GameServices::UnsubscribeBroadcastingStateChangeEvent( const BroadcastingStateChangeCallbackId callbackId )
	{
		m_broadcastingStateChangeCallbacks.Unsubscribe( callbackId );
	}

	void GameServices::DispatchBroadcastingStateChangeCallback( Bool isStreaming )
	{
		m_broadcastingStateChangeCallbacks.Dispatch( isStreaming );
	}

	BroadcastingExitSuspendedStateCallbackId GameServices::SubscribeBroadcastingExitSuspendedStateEvent( OnBroadcastingExitSuspendedStateCallback&& callback )
	{
		return m_broadcastingExitSuspendedStateCallbacks.Subscribe( std::move( callback ) );
	}

	void GameServices::UnsubscribeBroadcastingExitSuspendedStateEvent( const BroadcastingExitSuspendedStateCallbackId callbackId )
	{
		m_broadcastingExitSuspendedStateCallbacks.Unsubscribe( callbackId );
	}

	void GameServices::DispatchBroadcastingExitSuspendedStateEvent()
	{
		m_broadcastingExitSuspendedStateCallbacks.Dispatch();
	}

	Bool GameServices::IsBroadcasting() const
	{
		return m_isBroadcasting;
	}

	void GameServices::ClearRequests( const RequestType type )
	{
		RED_SCOPE_LOCK( m_requestsLock );

		auto requests = m_requests.Find( type );
		RED_FATAL_ASSERT( requests != m_requests.End() );
		requests.Value().Clear();
	}

	void GameServices::SubmitRequest( const RequestType type, red::UniquePtr< Request > request )
	{
		RED_SCOPE_LOCK( m_requestsLock );

		auto requests = m_requests.Find( type );
		RED_FATAL_ASSERT( requests != m_requests.End() );
		requests.Value().PushBack( std::move( request ) );
	}

	GameServices::Request::~Request() = default;

	void GameServices::Request::Execute( GameServices& gameServices )
	{
		OnExecute( gameServices );
	}

#if defined( RED_DISTRIBUTION_CHECK_ENABLED )
	GameServices::CheckDistributionRequest::CheckDistributionRequest( OnDistributionCheckedCallback&& callback )
		: callback( callback )
	{}

	void GameServices::CheckDistributionRequest::OnExecute( GameServices& gameServices )
	{
		gameServices.OnCheckDistribution();
	}
#endif

	GameServices::GatherSavesRequest::GatherSavesRequest( const OnGameSavesGatheredCallback& callback )
		: callback( callback )
	{}

	void GameServices::GatherSavesRequest::OnExecute( GameServices& gameServices )
	{
		gameServices.OnRequestSaves();
	}

	 GameServices::SaveRequest::SaveRequest( const save::MetadataPtr& metadata, OnSaveStartedCallback&& onStarted, OnSaveFinishedCallback&& onFinished )
		: metadata( metadata )
		, onStarted( std::move( onStarted ) )
		, onFinished( std::move( onFinished ) )
	{}

	void GameServices::SaveRequest::OnExecute( GameServices& gameServices )
	{
		gameServices.OnSave( metadata );
	}

	GameServices::LoadRequest::LoadRequest( const save::MetadataPtr& metadata, OnLoadStartedCallback&& onStarted, OnLoadFinishedCallback&& onFinished )
		: metadata( metadata )
		, onStarted( std::move( onStarted ) )
		, onFinished( std::move( onFinished ) )
	{}

	void GameServices::LoadRequest::OnExecute( GameServices& gameServices )
	{
		gameServices.OnLoad( metadata );
	}

	GameServices::LoadMetadataRequest::LoadMetadataRequest( const GameSaveView& saveView, OnLoadedMetadataCallback&& onFinished )
		: saveView( saveView )
		, onFinished( std::move( onFinished ) )
	{}

	void GameServices::LoadMetadataRequest::OnExecute( GameServices& gameServices )
	{
		gameServices.OnMetadataLoad( saveView );
	}

	GameServices::RemoveSaveRequest::RemoveSaveRequest( const GameSaveView& saveView, OnSaveRemovedCallback&& onRemoved )
		: saveView( saveView )
		, onRemoved( std::move( onRemoved ) )
	{}

	void GameServices::RemoveSaveRequest::OnExecute( GameServices& gameServices )
	{
		gameServices.OnRemoveSave( saveView );
	}

	GameServices::RemoveSavesRequest::RemoveSavesRequest( OnSavesRemovedCallback&& onRemoved )
		: onRemoved( std::move( onRemoved ) )
	{}

	void GameServices::RemoveSavesRequest::OnExecute( GameServices& gameServices )
	{
		gameServices.OnRemoveSaves();
	}

	GameServices::UnlockAchievementRequest::UnlockAchievementRequest(
		const THandle< game::data::Achievement_Record >& achievement,
		const Float percentComplete,
		OnAchievementUnlockedCallback&& onUnlocked )
		: achievement( achievement )
		, percentComplete( percentComplete )
		, onUnlocked( std::move( onUnlocked ) )
	{}

	void GameServices::UnlockAchievementRequest::OnExecute( GameServices& gameServices )
	{
		gameServices.OnAchievementUnlock( achievement, percentComplete );
	}

	GameServices::LoadInGameConfigRequest::LoadInGameConfigRequest( OnLoadInGameConfigCallback&& onLoaded )
		: onLoaded( std::move( onLoaded ) )
	{}

	void GameServices::LoadInGameConfigRequest::OnExecute( GameServices& gameServices )
	{
		gameServices.OnInGameConfigLoad();
	}

	GameServices::SetRichPresenceRequest::SetRichPresenceRequest(
		const RichPresenceStatus status )
		: status( status )
	{}

	void GameServices::SetRichPresenceRequest::OnExecute( GameServices& gameServices )
	{
		gameServices.OnRichPresenceSet( status );
	}


#if defined(RED_PLATFORM_ORBIS) || defined(RED_PLATFORM_DURANGO)
	GameServices::InstallLanguagePackRequest::InstallLanguagePackRequest( const res::LanguageID languageID )
		: languageID{ languageID }
	{}

	void GameServices::InstallLanguagePackRequest::OnExecute( GameServices& gameServices )
	{
		gameServices.OnLanguagePackInstall( languageID );
	}
#endif

	GameServices::SaveInGameConfigRequest::SaveInGameConfigRequest( OnSaveInGameConfigCallback&& onSaved )
		: onSaved( std::move( onSaved ) )
	{}

	void GameServices::SaveInGameConfigRequest::OnExecute( GameServices& gameServices )
	{
		gameServices.OnInGameConfigSave();
	}

#if !defined( RED_CONFIGURATION_FINAL ) && defined( RED_PLATFORM_WINPC )
	GameServices::FakeSignoutRequest::FakeSignoutRequest( OnFakeSignoutCallback&& onSignout )
		: onSignout( std::move( onSignout ) )
	{}

	void GameServices::FakeSignoutRequest::OnExecute( GameServices& gameServices )
	{
		gameServices.OnFakeSignout();
	}
#endif

	GameServices::SaveUserFileRequest::SaveUserFileRequest( const red::String& name, OnSaveUserFileDataCallback&& onData, OnSaveUserFileFinishedCallback&& onFinished )
		: onData( std::move( onData ) )
		, onFinished( std::move( onFinished ) )
		, fileName( name )
	{}

	void GameServices::SaveUserFileRequest::OnExecute( GameServices& gameServices )
	{
		gameServices.OnUserFileSave( fileName );
	}

	GameServices::LoadUserFileRequest::LoadUserFileRequest( const red::String& name, OnLoadUserFileCallback&& onFinished )
		: onLoadUserFileCallback( std::move( onFinished ) )
		, fileName( name )
	{}

	void GameServices::LoadUserFileRequest::OnExecute( GameServices& gameServices )
	{
		gameServices.OnUserFileLoad( fileName );
	}

	GameServices::TakeScreenshotRequest::TakeScreenshotRequest( OnScreenshotCreatedCallback&& onCreated )
		: onCreated( std::move( onCreated ) )
	{
	}

	void GameServices::TakeScreenshotRequest::OnExecute( GameServices& gameServices )
	{
		gameServices.OnScreenshotCreate();
	}

#if defined( RED_SAVE_SERVER_ENABLED )
	GameServices::DownloadSaveRequest::DownloadSaveRequest( const GameSaveView& saveView, OnGameSaveDownloadedCallback&& onDownloaded )
		: saveView( saveView )
		, onDownloaded( std::move( onDownloaded ) )
	{}

	void GameServices::DownloadSaveRequest::OnExecute( GameServices& gameServices )
	{
		gameServices.OnSaveDownload( saveView );
	}

	GameServices::UploadSaveRequest::UploadSaveRequest( red::UniqueBuffer&& save, const red::String& metadataName, const save::MetadataPtr& metadata, const img::ImagePtr& screenshot, OnGameSaveUploadedCallback&& onUploaded )
		: save( std::move( save ) )
		, metadataName( metadataName )
		, metadata( metadata )
		, screenshot( screenshot )
		, onUploaded( std::move( onUploaded ) )
	{}

	void GameServices::UploadSaveRequest::OnExecute( GameServices& gameServices )
	{
		gameServices.OnSaveUpload( std::move( save ), metadataName, metadata, screenshot );
	}
#endif

#ifdef RED_PLATFORM_DURANGO
	extern String GetMarketplaceRegion();

	String GetConsoleRegion()
	{
		return GetMarketplaceRegion();
	}
#endif

}
