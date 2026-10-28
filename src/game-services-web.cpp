/**
* Copyright (c) 2020 CD Projekt Red. All Rights Reserved.
*/

#include "common/core/include/corePublic.h"
#include "common/gameServices/include/gameServicesInternal.h"
#include "game-services-web.h"
#include "game-services-worker-web.h"
#include "persistence-web.h"
#include "galaxy-state-web.h"
#include "common/redFileSystem/include/fileSys.h"
#include "common/redFileSystem/include/nullFile.h"
#include "common/redConfig/include/inGameConfigSystem.h"
#include "common/gameSave/include/saveGame.h"
#include <emscripten.h>
#include <emscripten/threading.h>
#include <emscripten/proxying.h>
#include <cstdio>
#include <cstring>
#include <cerrno>
#include <unistd.h>
RTTI_BEGIN_TYPE_IN_NAMESPACE(GameServicesWeb, services);
    RTTI_PARENT_TYPE(services::GameServices)
RTTI_END_TYPE();

EM_JS(void, BrowserLanguage, (char* output, size_t capacity), {
    // [lang-fix] only English text is shipped; a browser language (pt-br, tr-tr, ...) selected a missing text pack = blank UI labels.
    const language = 'en-us';
    stringToUTF8(language, Number(output), Number(capacity));
});
EM_JS(int, BrowserOpenURL, (const char* address), {
    const url = UTF8ToString(Number(address));
    try {
        const target = new URL(url);
        if (target.protocol !== 'https:' && target.protocol !== 'http:') return 0;
        return window.open(target.href, '_blank', 'noopener') ? 1 : 0;
    } catch (error) { return 0; }
});
namespace {
bool OpenBrowserURL(const char* url) {
    struct Call { const char* url; bool opened = false; } call{url};
    static em_proxying_queue* queue = em_proxying_queue_create();
    if (!queue) return false;
    auto invoke = [](void* p) {
        auto* call = static_cast<Call*>(p);
        call->opened = BrowserOpenURL(call->url) != 0;
    };
    if (emscripten_is_main_runtime_thread()) invoke(&call);
    else if (!emscripten_proxy_sync(queue, emscripten_main_runtime_thread_id(), invoke, &call)) return false;
    return call.opened;
}
bool LoadBrowserProfile(Uint64& id) {
    constexpr char magic[8] = {'C','P','W','E','B','0','1','\0'};
    constexpr const char* path = "/userdata/browser-profile.bin";
    if (FILE* file = std::fopen(path, "rb")) {
        char savedMagic[8];
        bool valid = std::fread(savedMagic, 1, 8, file) == 8 &&
                     std::memcmp(savedMagic, magic, 8) == 0 &&
                     std::fread(&id, 1, sizeof(id), file) == sizeof(id) && id != 0 &&
                     std::fgetc(file) == EOF && !std::ferror(file);
        valid = std::fclose(file) == 0 && valid;
        return valid; // Do not replace an unreadable/corrupt existing profile.
    }
    if (errno != ENOENT) return false;
    do { if (getentropy(&id, sizeof(id)) != 0) return false; } while (!id);
    FILE* file = std::fopen(path, "wb");
    if (!file) return false;
    bool written = std::fwrite(magic, 1, 8, file) == 8 &&
                   std::fwrite(&id, 1, sizeof(id), file) == sizeof(id);
    written = std::fflush(file) == 0 && written;
    written = std::fclose(file) == 0 && written;
    return written && cp2077::web::FlushPersistentStorage();
}
}
namespace services {
class BrowserProfile final : public IGameServicesUser {
public:
    explicit BrowserProfile(Uint64 id) : m_id(id) {}
    UserId GetUserId() const override { return m_id; }
    String GetUserDisplayName() const override { return "Browser local profile"; }
    String GetUserRegion() const override { return ""; }
    Bool IsGuest() const override { return true; }
private:
    Uint64 m_id;
};
}

namespace services {
enum class GameServicesWeb::State : Uint32
	{
		Idle,

		SavesGathered,
		SaveRemoved,
		SavesRemoved,

		SaveStarted,
		SaveWaiting,
		SaveProcess,
		SaveFinished,

		LoadStarted,
		LoadWaiting,
		LoadFinished,

		MetadataLoaded,

		ScreenshotExported,

#if defined( RED_SAVE_SERVER_ENABLED )
		SaveDownloaded,
		SaveUploaded
#endif
	};
	GameServicesWeb::GameServicesWeb()
		: m_state( State::Idle )
		, m_hasAssignedUser( false )
	{}

	GameServicesWeb::~GameServicesWeb() = default;

	Bool GameServicesWeb::Initialize( const red::AppType appType )
	{
        if (!GameServices::Initialize(appType)) return false;
        if (appType != red::AppType::Game) return true;
        Uint64 profileID = 0;
        if (!LoadBrowserProfile(profileID)) {
            RED_LOG_ERROR("Browser local profile initialization failed");
            GameServices::Shutdown();
            return false;
        }
        char language[32] = {};
        BrowserLanguage(language, sizeof(language));
        m_language = language;
        m_user = red::CreateUniquePtr<BrowserProfile>(profileID);
        m_worker = red::CreateUniquePtr<GameServicesWebWorker>(m_workerMutex, m_wakeUpWorker);
        m_worker->InitThread();
        m_worker->SetPriority(red::TP_Normal);
        AssignUser(0);
        return true;
	}

	void GameServicesWeb::Shutdown()
	{
        if (m_worker) {
            m_worker->Exit();
            m_wakeUpWorker.WakeAll();
            m_worker->JoinThread();
            m_worker.Reset();
        }
        m_user.Reset();
        m_hasAssignedUser.SetValue(false);
        cpweb::DiscardGalaxyCallbacks();
        GameServices::Shutdown();
	}

	void GameServicesWeb::Update()
	{
		RED_FATAL_ASSERT( ::SIsMainThread(), "Calls to game services may only be made on the main thread." );


		if ( GetAppType() == red::AppType::Tools )
		{
			 return;
		}

		GameServices::Update();
        cpweb::ProcessGalaxyCallbacks();

		const State state = m_state.Exchange( State::Idle );
		switch ( state )
		{
		case State::Idle:
			break;

		case State::SavesGathered:
			OnSavesGathered();
			break;
		case State::SaveRemoved:
			OnRemoveSaveFinished( m_saveView );
			m_saveView = GameSaveView();
			break;
		case State::SavesRemoved:
			OnRemoveSavesFinished();
			break;

		case State::SaveStarted:
			OnSaveStarted();
			break;
		case State::SaveWaiting:
			OnSaveWaiting();
			break;
		case State::SaveProcess:
			OnSaveProcess();
			break;
		case State::SaveFinished:
			OnSaveFinished();
			break;

		case State::LoadStarted:
			OnLoadStarted();
			break;
		case State::LoadWaiting:
			OnLoadWaiting();
			break;
		case State::LoadFinished:
			OnLoadFinished();
			break;

		case State::MetadataLoaded:
			OnMetadataLoadFinished( m_saveView, m_metadata, m_screenshot );
			m_saveView = GameSaveView();
			m_metadata = nullptr;
			m_screenshot = nullptr;
			break;

		case State::ScreenshotExported:
			OnScreenshotCreated();
			break;

#if defined( RED_SAVE_SERVER_ENABLED )
		case State::SaveDownloaded:
			OnSaveDownloadFinished( m_saveView, m_saveBuffer, m_saveBuffer.GetSize(), m_metadataName, m_metadata, m_screenshot );
			m_saveView = GameSaveView();
			m_saveBuffer.Reset();
			m_metadata = nullptr;
			m_screenshot = nullptr;
			break;

		case State::SaveUploaded:
			OnSaveUploadFinished( m_saveView, m_saveSlot );
			m_saveView = GameSaveView();
			m_saveSlot = nullptr;
			break;
#endif

		default:
			RED_LOG_ERROR( "[GameServicesOrbis::Update] Unknown state %u", static_cast<Uint32>(state) );
			break;
		}
	}

	red::Platform GameServicesWeb::GetPlatform() const
	{
        return red::Platform::Web;
	}

	red::StringView GameServicesWeb::GetPlatformName() const
	{
        return { "Browser" };
	}

	red::StringView GameServicesWeb::GetLanguageCodeName() const
	{
        return m_language;
	}

	void GameServicesWeb::AssignUser( Uint64 controllerID )
	{
        if (!m_worker || !m_user) return;
        RequestSaves([this](const GameSaveViews& saves, const ServiceError errorCode) {
            if (errorCode != ServiceError_OK) {
                m_hasAssignedUser.SetValue(false);
                PushMessage(RED_NAME_CONSTEXPR("GameServicesAssignUser"), "failure");
                return;
            }
            if (saves.Empty()) { OnUserAssigned(); return; }
            LoadMetadata(saves[0], [this](const GameSaveView&, const save::MetadataPtr& metadata,
                                        const img::ImagePtr&, const ServiceError metadataError) {
                if (metadataError == ServiceError_OK) m_latestSaveMetadata = metadata;
                else RED_LOG_WARNING("Browser latest-save metadata could not be read");
                OnUserAssigned();
            });
        });
	}

	Bool GameServicesWeb::HasUserAssigned() const
	{
		return m_hasAssignedUser.GetValue();
	}

	void GameServicesWeb::OnUserAssigned()
	{
		m_hasAssignedUser.SetValue( true );
		PushMessage( RED_NAME_CONSTEXPR( "GameServicesAssignUser" ), "success" );
		PushMessage( RED_NAME_CONSTEXPR( "GameServicesUserSignInComplete" ) );
	}

	void GameServicesWeb::OnRequestSaves()
	{
		m_worker->SavesGather( GameServices::s_autoSaveSlots, GameServices::s_quickSaveSlots, [this]( const GameSaveViews& saveViews, const ServiceError errorCode )
		{
			m_saveViews = saveViews;
			m_lastError = errorCode;
			m_state.SetValue( State::SavesGathered );
		} );

		m_wakeUpWorker.WakeAll();
	}

	void GameServicesWeb::OnSave( const save::MetadataPtr& metadata )
	{
		m_metadata = metadata;
		m_saveView.name = metadata->m_name;
		m_saveSlot = nullptr;

		Bool willOverwrite = false;
		if ( !FindFreeSaveSlot( metadata, m_saveSlot, willOverwrite ) )
		{
			RED_LOG_ERROR( "GameServicesWeb::OnSave: Could not find empty slot" );
			m_lastError = ServiceError_NotEnoughSlots;
			m_state.SetValue( State::SaveFinished );
		}
		else
		{
			m_saveView.name = metadata->m_name;
			RED_FATAL_ASSERT( m_saveSlot );
			RED_LOG_CATEGORY_INFO( red::LoggerCategory_Services, "Requested save '%s'", m_saveView.name.AsChar() );

			m_lastError = ServiceError_OK;

			m_worker->SavePrepare( *metadata, willOverwrite, [this]( const red::StringView saveName, red::UniquePtr< IFile > file, const save::Metadata& metadata, const ServiceError errorCode )
			{
				m_lastError = errorCode;
				m_saveFile = std::move( file ); // capture IFile from the worker
				m_state.SetValue( State::SaveStarted );
			} );

			m_wakeUpWorker.WakeAll();
		}
	}

	void GameServicesWeb::OnLoad( const save::MetadataPtr& metadata )
	{
		m_metadata = metadata;
		m_saveView.name = metadata->m_name;

		m_worker->LoadPrepare( metadata, m_lastError, m_isLoadFinished, [this]( const red::StringView saveName, services::LoadPtr loadPtr, red::Atomic< Bool >& isFinished, ServiceError& errorCode )
		{
			m_lastError = errorCode;
			m_loadPtr = std::move( loadPtr );
			m_state.SetValue( State::LoadStarted );
		} );

		m_wakeUpWorker.WakeAll();
	}

	void GameServicesWeb::OnMetadataLoad( const GameSaveView& saveView )
	{
		m_saveView = saveView;
		m_worker->MetadataLoad( m_saveView, [this]( const GameSaveView& saveView, const save::MetadataPtr& metadata, const img::ImagePtr& image, const ServiceError errorCode )
		{
			m_lastError = errorCode;
			m_metadata = metadata;
			m_screenshot = image;
			m_state.SetValue( State::MetadataLoaded );
		} );

		m_wakeUpWorker.WakeAll();
	}

	void GameServicesWeb::OnRemoveSave( const GameSaveView& saveView )
	{
		m_worker->SaveRemove( saveView, [this]( const GameSaveView& saveView, const ServiceError errorCode )
		{
			m_lastError = errorCode;
			m_saveView = saveView;
			m_state.SetValue( State::SaveRemoved );
		} );

		m_wakeUpWorker.WakeAll();
	}

	void GameServicesWeb::OnRemoveSaves()
	{
		m_worker->SavesRemove( [this]( const ServiceError errorCode )
		{
			m_lastError = errorCode;
			m_state.SetValue( State::SavesRemoved );
		} );

		m_wakeUpWorker.WakeAll();
	}

	void GameServicesWeb::OnAchievementUnlock( const THandle< game::data::Achievement_Record >& achievement, Float percentComplete )
	{
        m_lastError = ServiceError_NotSupported;
        OnAchievementUnlocked();
	}

	void GameServicesWeb::OnRichPresenceSet( const RichPresenceStatus status )
	{
        m_lastError = ServiceError_NotSupported;
        OnRichPresenceSetFinished();
	}

	void GameServicesWeb::OnInGameConfigLoad()
	{
		const auto& path = InGameConfig::System::GetUserSettingsPath();

		m_lastError = ServiceError_OK;

		if ( !GFileManager->FileExist( path ) )
		{
			RED_LOG_ERROR( "GameServicesWeb::OnLoadInGameConfig: Failed to open file '%s'. File not found", path.ToDebugString() );
			m_lastError = ServiceError_FileNotFound;

			GameServices::OnInGameConfigLoadStarted( nullptr );
		}
		else
		{
			auto file = GFileManager->CreateFileReader( path, FOF_Buffered );
			if ( !file )
			{
				RED_LOG_ERROR( "GameServicesWeb::OnLoadInGameConfig: Failed to open file '%s'", path.ToDebugString() );
				m_lastError = ServiceError_InternalError;
			}

			GameServices::OnInGameConfigLoadStarted( std::move( file ) );
		}

		GameServices::OnInGameConfigLoadFinished();
	}

	void GameServicesWeb::OnInGameConfigSave()
	{
		const auto& path = InGameConfig::System::GetUserSettingsPath();

		m_lastError = ServiceError_OK;
		auto file = GFileManager->CreateFileWriter( path, FOF_Buffered );
		if ( !file )
		{
			RED_LOG_ERROR( "GameServicesWeb::OnSaveInGameConfig: Failed to open file '%s'", path.ToDebugString() );
			m_lastError = ServiceError_InternalError;
		}

		GameServices::OnInGameConfigSaveStarted( std::move( file ) );
		if (m_lastError == ServiceError_OK && !cp2077::web::FlushPersistentStorage())
            m_lastError = ServiceError_InternalError;
        GameServices::OnInGameConfigSaveFinished();
	}

	void GameServicesWeb::OnUserFileSave( const red::String& fileName )
	{
		const auto path = red::paths::GetSaveDirectory().AddFilePath( fileName );

		m_lastError = ServiceError_OK;
		auto file = GFileManager->CreateFileWriter( path, FOF_Buffered );
		if( !file )
		{
			RED_LOG_ERROR( "GameServicesWeb::OnSaveUserFile: Failed to open file '%s'", path.ToDebugString() );
			m_lastError = ServiceError_InternalError;
		}

		GameServices::OnUserFileSaveStarted( std::move( file ) );
		if (m_lastError == ServiceError_OK && !cp2077::web::FlushPersistentStorage())
            m_lastError = ServiceError_InternalError;
        GameServices::OnUserFileSaveFinished();
	}

	void GameServicesWeb::OnUserFileLoad( const red::String& fileName )
	{
		const auto path = red::paths::GetSaveDirectory().AddFilePath( fileName );

		m_lastError = ServiceError_OK;

		if( !GFileManager->FileExist( path ) )
		{
			RED_LOG_ERROR( "GameServicesWeb::OnLoadUserFile: Failed to open file '%s'. File not found", path.ToDebugString() );
			m_lastError = ServiceError_FileNotFound;

			GameServices::OnUserFileLoadStarted( nullptr );
		}
		else
		{
			auto file = GFileManager->CreateFileReader( path, FOF_Buffered );
			if( !file )
			{
				RED_LOG_ERROR( "GameServicesWeb::OnLoadUserFile: Failed to open file '%s'", path.ToDebugString() );
				m_lastError = ServiceError_InternalError;
			}

			GameServices::OnUserFileLoadStarted( std::move( file ) );
		}

		GameServices::OnUserFileLoadFinished();
	}

	void GameServicesWeb::OnScreenshotCreate()
	{
		m_worker->ScreenshotCreate( m_screenshotSize, m_screenshotBuffer, [this]( const ServiceError errorCode )
		{
			m_lastError = errorCode;
			m_state.SetValue( State::ScreenshotExported );
		} );

		m_wakeUpWorker.WakeAll();
	}

	void GameServicesWeb::OnOpenWebBrowser( const red::String& url )
	{
        if (!OpenBrowserURL(url.AsChar())) {
            RED_LOG_WARNING("Browser could not open external URL");
        }
	}

#if defined( RED_SAVE_SERVER_ENABLED )
	void GameServicesWeb::OnSaveDownload( const GameSaveView& saveView )
	{
		m_saveView = saveView;
		m_metadata = red::CreateSharedPtr< save::Metadata >();
		m_saveBuffer = red::CreateUniqueBuffer< red::PoolDebug >( GetMaxSaveSize(), 1 );
		m_worker->SaveDownload( m_saveView, m_saveBuffer, m_metadata, [this]( const GameSaveView& saveView, const red::UniqueBuffer& save, const Uint32 saveSize, const red::String& metadataName, const save::MetadataPtr& metadata, const img::ImagePtr& image, const ServiceError errorCode )
		{
			m_lastError = errorCode;
			m_screenshot = image;
			m_saveBuffer.PatchSize( saveSize );
			m_metadataName = metadataName;
			m_state.SetValue( State::SaveDownloaded );
		} );

		m_wakeUpWorker.WakeAll();
	}

	void GameServicesWeb::OnSaveUpload(
		red::UniqueBuffer&& save,
		const red::String& metadataName,
		const save::MetadataPtr& metadata,
		const img::ImagePtr& screenshot )
	{
		metadata->m_name = metadata->m_name.StringBefore( "-" );
		metadata->m_name += "-";

		m_saveView.name = metadata->m_name;
		SavePtr savePtr;

		Bool willOverwrite = false;
		m_saveSlot = nullptr;
		if ( !FindFreeSaveSlot( metadata, m_saveSlot, willOverwrite ) )
		{
			RED_LOG_ERROR( "GameServicesWeb::OnUploadSave: Could not find empty slot" );
			m_lastError = ServiceError_NotEnoughSlots;
			m_state.SetValue( State::SaveUploaded );
		}
		else
		{
			m_saveView.name = metadata->m_name;
			RED_FATAL_ASSERT( m_saveSlot );

			m_lastError = ServiceError_OK;

			m_worker->SaveUpload( std::move( save ), metadataName, metadata, screenshot, [this]( const GameSaveView& saveView, const ServiceError errorCode )
			{
				m_lastError = errorCode;
				m_saveView = saveView;
				m_state.SetValue( State::SaveUploaded );
			} );

			m_wakeUpWorker.WakeAll();
		}
	}
#endif

	void GameServicesWeb::OnSaveStarted()
	{
		// TODO clean up
		if ( !m_saveFile )
		{
			RED_LOG_ERROR( "Save file not set" );
			m_saveFile = red::CreateUniquePtr< CNullFileWriter >();
			m_state.SetValue( State::SaveFinished );
		}

		RED_ASSERT( m_saveFile );
		m_saveStream = save::CreateSaveStream( *m_saveFile, *m_metadata );
		GameServices::OnSaveStarted( m_saveView.name, *m_saveStream, *m_metadata );
		m_state.SetValue( State::SaveWaiting);
	}

	void GameServicesWeb::OnSaveWaiting()
	{
		if ( m_isSaveFinished.GetValue() )
		{
			// Close file handle here
			m_saveStream = nullptr;
			m_saveFile = nullptr;

			m_state.SetValue( State::SaveProcess );
		}
		else
		{
			m_state.SetValue( State::SaveWaiting );
		}
	}

	void GameServicesWeb::OnSaveProcess()
	{
		if ( m_lastError != ServiceError_OK )
		{
			m_state.SetValue( State::SaveFinished );
			return;
		}

		m_worker->SaveProcess( m_metadata, [this]( const GameSaveView& saveView, ServiceError errorCode )
		{
			m_lastError = errorCode;
			m_saveView = saveView;
			m_state.SetValue( State::SaveFinished );
		} );
		m_wakeUpWorker.WakeAll();
	}

	void GameServicesWeb::OnSaveFinished()
	{
		GameServices::OnSaveFinished( m_saveView, m_saveSlot );

		m_saveView = {};
		m_saveSlot = nullptr;
		m_metadata = nullptr;
	}

	void GameServicesWeb::OnLoadStarted()
	{
		GameServices::OnLoadStarted( m_saveView.name, std::move( m_loadPtr ) );

		if ( m_lastError != ServiceError_OK )
		{
			PushMessage( RED_NAME_CONSTEXPR( "GameServicesSaveGameLoadFailed" ) );
		}

		m_state.SetValue( State::LoadWaiting );
	}

	void GameServicesWeb::OnLoadWaiting()
	{
		if ( m_isLoadFinished.GetValue() )
		{
			m_state.SetValue( State::LoadFinished );
		}
		else
		{
			m_state.SetValue( State::LoadWaiting );
		}
	}

	void GameServicesWeb::OnLoadFinished()
	{
		GameServices::OnLoadFinished( m_saveView.name );

		m_saveView = {};
		m_saveSlot = nullptr;
		m_metadata = nullptr;
	}

#if !defined(RED_CONFIGURATION_FINAL) && defined(RED_PLATFORM_WINPC)
	void GameServicesWeb::OnFakeSignout()
	{
		OnUserSignOut( true );
		OnInGameConfigSaveFinished();
	}
#endif

}
