/**
* Copyright (c) 2015 CD Projekt Red. All Rights Reserved.
*/

#include "build.h"

#include "../../gameCore/include/baseGameSession.h"
#include "../../gameCore/include/gameSessionProperties.h"
#include "../../gameCore/include/gameSessionDesc.h"
#include "../../gameCore/include/characterCustomizationSystemInterface.h"
#include "../../gameCore/include/telemetrySystemInterface.h"
#include "../../gameTweakDB/include/records/tweakDBLifePath.h"
#include "../../common/gameServices/include/gameLocalStorage.h"
#include "../../common/redConfig/include/inGameConfigSystem.h"
#include "../../inkCore/include/inkSystem.h"
#include "../../redConfig/include/inGameConfigGroup.h"
#include "../../redConfig/include/inGameConfigVar.h"
#include "../../inkCore/include/systemNotificationsUtilities.h"
#include "../../game/include/gogRewardsSystem.h"
#include "../../gameServices/include/gameServicesUser.h"
#include "../../gameServices/include/gameServices.h"
#include "../../world/include/gameInstance.h"
#include "preGameRequestsHandler.h"
#include "gameDefEnumeration.h"
#include "../inkCore/include/loadingScreenData.h"
// CP2077 web [missionflow]: ?start=nomad (alias ?start=nomadquest) = the real q000_nomad new game without menu clicking.
// Once the main menu is up (gameState_SessionActive.cpp sets g_cpwebMissionflowMenuReady in the PreGame session), the
// menu's own StartNewGame below is called with the default Nomad character state from the gameUI overlay
// (characterCustomizationSystem.cpp cpweb_missionflow_BuildNomadState). Everything after that is the original path:
// base/quest/cyberpunk2077.gamedef world + main quest, LifePaths.Nomad newGameSpawnTag, initial loading screen.
// &missionflow=0 or any ?jump=... disables it.
// CP2077 web [watson]: ?start=watson uses this same path (default Nomad character, real new game); the post-prologue
// q001 entry is a quest-graph redirect in gameQuest phaseInstance.cpp, the spawn point is set in gameState_Session.cpp.
#include <atomic>
#include <cstdio>
#include <mutex>
#ifdef __EMSCRIPTEN__
#include <emscripten/emscripten.h>
#endif
std::atomic< int > g_cpwebMissionflowMenuReady{ 0 };
void cpweb_missionflow_BuildNomadState( void ( *done )( const THandle< const IScriptable >& ) );
namespace cpweb_missionflow
{
	static std::mutex s_lock;
	static THandle< const IScriptable > s_state;
	static std::atomic< int > s_phase{ 0 }; // 0 wait menu, 1 building state, 2 state ready, 3 StartNewGame called, 4 failed
	static Uint32 s_menuTicks = 0;

	static Bool Enabled()
	{
		static std::atomic< Int32 > s_value{ -1 };
		Int32 v = s_value.load();
		if ( v < 0 )
		{
#ifdef __EMSCRIPTEN__
			v = MAIN_THREAD_EM_ASM_INT( {
				try {
					const q = new URLSearchParams( location.search );
					if ( q.get( 'missionflow' ) === '0' || q.has( 'jump' ) ) return 0;
					const s = q.get( 'start' );
					return ( s === 'nomad' || s === 'nomadquest' || s === 'watson' ) ? 1 : 0;
				} catch ( e ) { return 0; }
			} );
#else
			v = 0;
#endif
			s_value.store( v );
			std::fprintf( stderr, "CP2077 missionflow: direct Nomad new game %s\n", v ? "ENABLED (?start=nomad|nomadquest|watson)" : "off" );
		}
		return v > 0;
	}

	static void OnStateBuilt( const THandle< const IScriptable >& state )
	{
		std::lock_guard< std::mutex > guard( s_lock );
		s_state = state;
		s_phase.store( state ? 2 : 4 );
	}
}

namespace Config
{
#if defined( INK_ENABLE_FUNCTIONAL_TESTING )
	TConfigVar< String > cvUiFunctionalTestWorld( "PreGameRequestsHandler", "UiFunctionalTestWorld", "test\\func_test\\assets\\gameplay\\ui_functional_tests_world\\ui_functional_tests_gamedef.gamedef" );
#endif
}

namespace gsm
{
	PreGameRequestsHandler::PreGameRequestsHandler( IEventSink& sink, const game::BaseGameSession& gameSession )
		: BaseRequestsHandler( sink )
		, m_gameSession( gameSession )
		, m_worldInfos( ink::PoolInk_Uncategorized() )
		, m_playerGenders( ink::PoolInk_Uncategorized() )
		, m_sessions( ink::PoolInk_Uncategorized() )
		, m_mpWorldInfos( ink::PoolInk_Uncategorized() )
		, m_playerRecordIds( ink::PoolInk_Uncategorized() )
		, m_sessionTypes( ink::PoolInk_Uncategorized() )
	{
		InitWorlds();

		m_playerGenders = { "Default", "Female", "Male" };
		m_playerRecordIds = { "dummy_player", "dummy_player_fpp", "Character.Player_Puppet_Base", "Multiplayer.character" };
		m_sessionTypes = { "LAN", "Internet" };
	}

	void PreGameRequestsHandler::Update()
	{
		TBaseClass::Update();

		m_sessionSearch.Update();

		// [missionflow] direct Nomad new game (see top of file)
		if ( cpweb_missionflow::Enabled() )
		{
			const int phase = cpweb_missionflow::s_phase.load();
			if ( phase == 0 && g_cpwebMissionflowMenuReady.load() )
			{
				// a short settle after the menu becomes active (the menu's own scripts finish their intro first)
				if ( ++cpweb_missionflow::s_menuTicks == 60 )
				{
					cpweb_missionflow::s_phase.store( 1 );
					std::fprintf( stderr, "CP2077 missionflow: main menu active -> building the default Nomad character (no menu input)\n" );
					cpweb_missionflow_BuildNomadState( &cpweb_missionflow::OnStateBuilt );
				}
			}
			else if ( phase == 2 )
			{
				THandle< const IScriptable > state;
				{
					std::lock_guard< std::mutex > guard( cpweb_missionflow::s_lock );
					state = cpweb_missionflow::s_state;
					cpweb_missionflow::s_state.Reset();
				}
				cpweb_missionflow::s_phase.store( 3 );
				std::fprintf( stderr, "CP2077 missionflow: calling PreGameRequestsHandler::StartNewGame (menu path, Nomad)\n" );
				StartNewGame( state );
			}
			else if ( phase == 4 )
			{
				cpweb_missionflow::s_phase.store( 5 );
				std::fprintf( stderr, "CP2077 missionflow: ERROR no character state; staying in the main menu\n" );
			}
		}
	}

	void PreGameRequestsHandler::GetTransitionArgs( const world::WorldID & worldID,
													const res::ResourcePath & startQuestPath,
													const red::TagList & spawnTags,
													const THandle< const game::ui::ICharacterCustomizationState >& state,
													Args & transitionArgs )
	{
		game::SessionConfig sessionConfig;
		sessionConfig.m_worldID = worldID;

		game::QuestConfig questConfig;
		questConfig.m_runQuest = !startQuestPath.IsEmpty();
		questConfig.m_questPath = startQuestPath;

		game::GameSessionDesc gameSessionDesc( sessionConfig );
		gameSessionDesc.SetQuestConfig( questConfig );
		gameSessionDesc.SetCharacterCustomizationState( state );

		transitionArgs.Set( RED_NAME_CONSTEXPR( "gameSessionDesc" ), std::move( gameSessionDesc ) );
		transitionArgs.Set( RED_NAME_CONSTEXPR( "spawnTags" ), spawnTags );
	}

#if defined( INK_ENABLE_FUNCTIONAL_TESTING )
	void PreGameRequestsHandler::RunUiFunctionalTestWorld()
	{
		const String gameDefinition( Config::cvUiFunctionalTestWorld.Get() );

		res::ResourceTokenHandle token = GResourceLoader->IssueLoadingRequest( res::ResourcePath::Build( gameDefinition ) );
		res::LoadedCallback onLoaded =
			[ weakSelf = WeakHandleFromThis<PreGameRequestsHandler>() ]( THandle< CResource > res )
		{
			const auto& self = weakSelf.ToHandle();
			const auto& gameDef = Cast< GameDefinition >( res );
			if ( self && gameDef )
			{
				const world::WorldID worldID = gameDef->GetWorldID();
				const red::TagList& spawnTags = gameDef->GetSpawnPointTags();
				const res::ResourcePath& startQuestPath = gameDef->GetMainQuestPath();

				RED_LOG( "PreGameSession: Start definition: '%hs'", worldID.GetDisplayName().AsChar() );
				RED_LOG_INFO( "PreGameSession: spawnTags=%hs", spawnTags.ToString().AsChar() );

				Args transitionArgs;

				transitionArgs.Set( RED_NAME_CONSTEXPR( "inputDeviceID" ), self->GetDeviceId() );

				self->GetTransitionArgs( worldID, startQuestPath, spawnTags, nullptr, transitionArgs );

				const TransitionRequest& transitionRequest = TransitionRequest::ReplaceAll( "Session", transitionArgs );
				self->SetTransitionRequest( transitionRequest );
			}
		};

		token->OnLoaded( std::move( onLoaded ) );
	}
#endif

	void PreGameRequestsHandler::InitWorlds()
	{
		// enumerate worlds
		m_worldInfos = world::WorldEnumerator::GetInstance().GetWorlds();

#ifndef RED_CONFIGURATION_FINAL
		// filter out singleplayer worlds
		m_mpWorldInfos.Clear();
		for ( Uint32 i = 0; i < m_worldInfos.Size(); ++i )
		{
			if ( red::paths::GetFileName( m_worldInfos[ i ].GetWorldPath().ToStringView() ).StartsWith( "mp_" ) )
			{
				m_mpWorldInfos.EmplaceBack( m_worldInfos[ i ] );
			}
		}
#endif
	}

	void PreGameRequestsHandler::UpdateLaunchCounter()
	{
		if ( !services::GGameLocalStorage )
		{
			return;
		}

		Int32 counterValue = services::GGameLocalStorage->GetInt32( RED_NAME_CONSTEXPR( "LaunchCounter" ) );
		counterValue++;
		services::GGameLocalStorage->SetInt32( RED_NAME_CONSTEXPR( "LaunchCounter" ), counterValue );
		RequestLocalStorageSave();
	}

	void PreGameRequestsHandler::RequestTelemetryConsent( Bool fromSettings )
	{
		if( auto telemetrySystem = m_gameSession.GetGameInstance().GetSystemPtr< game::ITelemetrySystem >() )
		{
			const Bool requestedConsentNotification = telemetrySystem->HandlePreGameConsentRequest( fromSettings );

			if( requestedConsentNotification )
			{
				m_consentNotification = RequestSystemNotification( ESystemNotificationTypes::EnableTelemetry );

				if( m_consentNotification )
				{
					red::FixedSizeFunction< void() > accept = [requestWeak = WeakHandleFromThis<PreGameRequestsHandler>()]()
					{
						auto requestStrong = requestWeak.ToHandle();
						if( !requestStrong )
						{
							return;
						}

						InGameConfig::System& configSystem = InGameConfig::System::GetInstance();
						InGameConfig::VarBool& enableTelemetryVar = configSystem.GetGroup( RED_NAME_CONSTEXPR( "/gameplay/misc" ) ).GetVarBool( RED_NAME_CONSTEXPR( "EnableTelemetry" ) );
						if( enableTelemetryVar.GetValue() == true )
						{
							//if for any reason the value is already set to true, force notifier because we expect false by default.
							enableTelemetryVar.SetReasonOfChange( InGameConfig::ChangeReason::NeedsRestart );
							configSystem.QueueVarNotification( &enableTelemetryVar );
						}
						else
						{
							enableTelemetryVar.SetValue( true, InGameConfig::Source::Code );
						}

						requestStrong->RequestSaveUserSettings();
						requestStrong->m_consentNotification = nullptr;
					};

					m_consentNotification->SetAcceptCallback( accept );

					red::FixedSizeFunction< void() > cancel = [requestWeak = WeakHandleFromThis<PreGameRequestsHandler>()]()
					{
						auto requestStrong = requestWeak.ToHandle();
						if( !requestStrong )
						{
							return;
						}

						InGameConfig::System& configSystem = InGameConfig::System::GetInstance();
						InGameConfig::VarBool& enableTelemetryVar = configSystem.GetGroup( RED_NAME_CONSTEXPR( "/gameplay/misc" ) ).GetVarBool( RED_NAME_CONSTEXPR( "EnableTelemetry" ) );
						if( enableTelemetryVar.GetValue() == false )
						{
							enableTelemetryVar.SetReasonOfChange( InGameConfig::ChangeReason::NeedsRestart );
							configSystem.QueueVarNotification( &enableTelemetryVar );
						}
						else
						{
							enableTelemetryVar.SetValue( false, InGameConfig::Source::Code );
						}

						requestStrong->RequestSaveUserSettings();
						requestStrong->m_consentNotification = nullptr;
					};

					m_consentNotification->SetCancelCallback( cancel );
				}
			}
		}
	}

	Bool PreGameRequestsHandler::IsBroadcastingModeEnabled() const
	{
		if( GGameServices && GGameServices->IsBroadcasting() )
		{
			return true;
		}

		InGameConfig::System &system = InGameConfig::System::GetInstance();
		if( system.GetMode() == InGameConfig::Mode::Game )
		{
			const InGameConfig::Group &miscGroup = system.GetGroup( RED_NAME_CONSTEXPR( "/audio/misc" ) );
			const Bool isStreamerMode = miscGroup.GetVarBool( RED_NAME_CONSTEXPR( "StreamerMode" ) ).GetRequestedValue();
			if( isStreamerMode )
			{
				return true;
			}
		}
		return false;
	}

	Bool PreGameRequestsHandler::ShouldDisplayGog()
	{
		services::GameLocalStorage* const localStorage = services::GGameLocalStorage;
		if ( !localStorage )
		{
			return false;
		}

		const Bool gogPopupShown = localStorage->GetBool( RED_NAME_CONSTEXPR( "WasGOGPopupShown" ) );
		if( gogPopupShown )
		{
			return false;
		}

		if( IsBroadcastingModeEnabled() )
		{
			return false;
		}

		const Uint32 counterValue = localStorage->GetInt32( RED_NAME_CONSTEXPR( "LaunchCounter" ) );
		auto rewardsSystem = game::GOGRewardsSystem::GetActiveInstance();

		if ( ( rewardsSystem->GetStatus() == game::GOGRewardsSystemStatus::RegistrationPending ) && ( m_currentUser == nullptr || !m_currentUser->IsGuest() ) )
		{
			if( counterValue >= c_gogDisplayLaunchMin )
			{
				localStorage->SetBool( RED_NAME_CONSTEXPR( "WasGOGPopupShown" ), true );
				RequestLocalStorageSave();
				return true;
			}
		}

		return false;
	}

	void PreGameRequestsHandler::RequestLocalStorageSave()
	{
		++m_runningLocalStorageSaves;
		m_operationNotification = RequestSystemNotification( ESystemNotificationTypes::DiscOperationIndicator );

		services::GGameLocalStorage->RequestSave( [ weakSelf = WeakHandleFromThis<PreGameRequestsHandler>() ]( const services::GameLocalStorageSaveResult result )
		{
			const Bool success = result == services::GameLocalStorageSaveResult::Success;
			RED_LOG_ERROR( "[PreGameRequestsHandler::RequestLocalStorageSave][Callback] Save %s", success ? "succeeded" : "failed" );

			auto handleSelf = weakSelf.ToHandle();

			if ( !handleSelf )
			{
				return;
			}

			if( --handleSelf->m_runningLocalStorageSaves == 0 )
			{
				handleSelf->m_operationNotification = nullptr;
			}
		} );
	}

	void PreGameRequestsHandler::GetGameDefCategories( red::DynArray< String >& gameDefinitionCategories )
	{
		gameDefinitionCategories = gsm::GameDefEnumerator::GetInstance().GetCategoryNames();
	}

	void PreGameRequestsHandler::GetGameDefinitions( Int32 categoryId, red::DynArray<String>& gameDefinitionNames )
	{
		gameDefinitionNames.Clear();

		auto gameDefs = gsm::GameDefEnumerator::GetInstance().GetGameDefs( categoryId );
		for ( Int32 i : gameDefs.Indices() )
		{
			const auto& gameDef = gameDefs[ i ];
			auto shortName = red::paths::ExtractFileStem( gameDef.GetPath().ToStringView() );
			gameDefinitionNames.PushBack( String::Printf( "[%u/%u] %s", i + 1, gameDefs.Size(), shortName.AsChar() ) );
		}
	}

	void PreGameRequestsHandler::GetGenders( red::DynArray<String>& genderNames )
	{
		genderNames.Clear();

		for ( Int32 i : m_playerGenders.Indices() )
		{
			genderNames.PushBack( String::Printf( "[%u/%u] %s", i + 1, m_playerGenders.Size(), m_playerGenders[ i ].AsChar() ) );
		}
	}

	void PreGameRequestsHandler::StartGameDefinition( Int32 categoryId, Int32 gamedefId, Int32 genderId )
	{
		const auto enumerator = gsm::GameDefEnumerator::GetInstance();
		const auto categories = enumerator.GetCategoryNames();
		if ( categoryId < 0 || categoryId >= static_cast<Int32>( categories.Size() ) )
		{
			RED_LOG_ERROR( "PreGameSession: Invalid category id %d supplied to start the game definition, maximum categories: %u", categoryId, categories.Size() );
			return;
		}

		const auto gameDefinitions = enumerator.GetGameDefs( categoryId );
		if ( gamedefId < 0 || gamedefId >= static_cast<Int32>( gameDefinitions.Size() ) )
		{
			RED_LOG_ERROR( "PreGameSession: Invalid game definition %d specified for category %d, maximum game definition: %u", gamedefId, categoryId, gameDefinitions.Size() );
			return;
		}

		if ( genderId < 0 || genderId >= static_cast<Int32>( m_playerGenders.Size() ) )
		{
			RED_LOG_ERROR( "PreGameSession: Invalid gender Id %d specified, maximum id %u", genderId, m_playerGenders.Size() );
			return;
		}

		const auto& gameDef = gameDefinitions[ gamedefId ];
		if ( gameDef )
		{
			const Bool isStreamingTest = categories[ categoryId ] == "StreamingTests";

			res::LoadedCallback onLoaded{ ink::PoolInk_Uncategorized(), [ self = HandleFromThis<PreGameRequestsHandler>(), this, genderId, isStreamingTest ]( THandle< CResource > res )
			{
				auto gamedefResource = Cast< gsm::GameDefinition >( res );
				RED_ASSERT( gamedefResource );

				const world::WorldID worldID = gamedefResource->GetWorldID();
				const red::TagList& spawnTags = gamedefResource->GetSpawnPointTags();
				const res::ResourcePath& startQuestPath = gamedefResource->GetMainQuestPath();

				RED_LOG( "PreGameSession: Start definition: '%hs'", worldID.GetDisplayName().AsChar() );
				RED_LOG_INFO( "PreGameSession: spawnTags=%hs", spawnTags.ToString().AsChar() );

				Args transitionArgs;
				transitionArgs.Set( RED_NAME_CONSTEXPR( "playerGender" ), m_playerGenders[ genderId ] );
				transitionArgs.Set( RED_NAME_CONSTEXPR( "inputDeviceID" ), GetDeviceId() );
				if( isStreamingTest )
				{
					transitionArgs.Set( RED_NAME_CONSTEXPR( "streamingTest" ), true );
				}
				PreGameRequestsHandler::GetTransitionArgs( worldID, startQuestPath, spawnTags, nullptr, transitionArgs );

				const TransitionRequest& transitionRequest = TransitionRequest::ReplaceAll( "Session", transitionArgs );
				self->SetTransitionRequest( transitionRequest );
			} };

			auto token = gameDef.IssueLoadingRequest();
			token->OnLoaded( std::move( onLoaded ) );
		}
	}

	void PreGameRequestsHandler::GetWorlds( red::DynArray<String>& worldNames )
	{
		worldNames.Clear();

		for ( Int32 i : m_worldInfos.Indices() )
		{
			worldNames.PushBack( String::Printf( "[%u/%u] %s", i + 1, m_worldInfos.Size(), m_worldInfos[ i ].GetDisplayName().AsChar() ) );
		}
	}

	void PreGameRequestsHandler::StartWorld( Int32 worldId, Int32 genderId )
	{
		if ( worldId >= 0 && worldId < ( Int32 ) m_worldInfos.Size() &&
				 genderId >= 0 && genderId < ( Int32 ) m_playerGenders.Size() )
		{
			const world::WorldID worldID = m_worldInfos[ worldId ];

			RED_LOG( "PreGameSession: Start world: '%hs'", worldID.GetDisplayName().AsChar() );

			Args transitionArgs;
			transitionArgs.Set( RED_NAME_CONSTEXPR( "playerGender" ), m_playerGenders[ genderId ] );
			transitionArgs.Set( RED_NAME_CONSTEXPR( "inputDeviceID" ), GetDeviceId() );

			GetTransitionArgs( worldID, res::ResourcePath(), red::TagList(), nullptr, transitionArgs );

			const TransitionRequest& transitionRequest = TransitionRequest::ReplaceAll( "Session", transitionArgs );
			SetTransitionRequest( transitionRequest );
		}
	}

	res::GatheredResourceReference g_startingGameDefinition( RED_CONST_RESOURCEPATH( "base\\quest\\cyberpunk2077.gamedef" ), GRF_Default );

	void PreGameRequestsHandler::StartNewGame( const THandle< const IScriptable >& data )
	{
		const THandle< const game::ui::ICharacterCustomizationState >& state = Cast<game::ui::ICharacterCustomizationState>( data );
		if ( state && state->IsFinalized() )
		{
			res::ResourceTokenHandle token = GResourceLoader->IssueLoadingRequest( g_startingGameDefinition.GetPath() );
			res::LoadedCallback onLoaded{ ink::PoolInk_Uncategorized(),
				[ weakSelf = WeakHandleFromThis<PreGameRequestsHandler>(), state ]( THandle< CResource > res )
			{
				const auto& self = weakSelf.ToHandle();
				const auto gameDef = Cast< GameDefinition >( res );
				if ( gameDef )
				{
					const world::WorldID worldID = gameDef->GetWorldID();
					const res::ResourcePath& startQuestPath = gameDef->GetMainQuestPath();
					red::TagList spawnTags = gameDef->GetSpawnPointTags();

					// If the span tag is defined in life path then us it instead of the one from game definition.
					auto lifePathRecord = game::data::GetRecordHandle< game::data::LifePath_Record >( state->GetLifePath() );
					if ( lifePathRecord && lifePathRecord->newGameSpawnTag() )
					{
						spawnTags.Clear();
						spawnTags.AddTag( lifePathRecord->newGameSpawnTag() );
					}

					RED_LOG( "PreGameSession: Start definition: '%hs'", worldID.GetDisplayName().AsChar() );
					RED_LOG_INFO( "PreGameSession: spawnTags=%hs", spawnTags.ToString().AsChar() );
					// [missionflow] log what the new game starts with (menu path and direct ?start=nomad alike)
					std::fprintf( stderr, "CP2077 missionflow: StartNewGame world=%s mainQuest=%s spawnTags=%s lifepathRecord=%d lifepathSpawnTag=%s newGame=%d male=%d\n",
						worldID.GetDisplayName().AsChar(), startQuestPath.ToDebugString(), spawnTags.ToString().AsChar(),
						int( bool( lifePathRecord ) ), ( lifePathRecord && lifePathRecord->newGameSpawnTag() ) ? CName( lifePathRecord->newGameSpawnTag() ).AsChar() : "-",
						int( state->IsNewGame() ), int( state->IsBodyGenderMale() ) );

					Args transitionArgs;
					transitionArgs.Set( RED_NAME_CONSTEXPR( "inputDeviceID" ), self->GetDeviceId() );

					self->GetTransitionArgs( worldID, startQuestPath, spawnTags, state, transitionArgs );

					const TransitionRequest& transitionRequest = TransitionRequest::ReplaceAll( "Session", transitionArgs );
					self->SetTransitionRequest( transitionRequest );
				}
			} };

			red::SharedPtr< ink::InitialLoadingScreenSaveData > data = red::CreateSharedPtr< ink::InitialLoadingScreenSaveData >();
			data->m_initialLoadingTweakID = TweakDBID(RED_NAME_CONSTEXPR("InitLoadingScreen.DefaultInitialLoadingScreen"));

			GetInkSystem().GetLoadingScreen()->SetLoadingScreenData( data );
			GetInkSystem().GetLoadingScreen()->SetNextLoadingType( ink::LoadingScreenType::Initial );

			token->OnLoaded( std::move( onLoaded ) );
		}
	}

	void PreGameRequestsHandler::UnpauseGame()
	{
		m_sink.PostMessage( RED_NAME_CONSTEXPR( "Unpause" ), String::EMPTY() );
	}

	void PreGameRequestsHandler::ManualSave( const String& saveName )
	{
		RED_FATAL_ASSERT( true, "The game can't be saved from the pregame menu");
	}

	void PreGameRequestsHandler::QuickSave()
	{
		RED_FATAL_ASSERT( true, "The game can't be saved from the pregame menu" );
	}

	void PreGameRequestsHandler::RequestLANServers()
	{
		net::LanSearch lanSearch;
		lanSearch.m_params.m_propertyFilters.EmplaceBack( game::SessionPropertyID::RTTIHash, ( Int32 ) rep::IRTTIService::GetInstance().GetRTTIHash(), net::PropertyFilter::Operator::Equal );
		lanSearch.m_params.m_maxQueryResults = 10;

		m_sessionSearch.FindSessions( lanSearch, [ this ] ( red::UniquePtr<net::ISearchResult>& searchResult )
		{
			RED_FATAL_ASSERT( searchResult );
			OnSearchResults( std::move( searchResult ) );
		} );
	}

	void PreGameRequestsHandler::RequestInternetServers()
	{
		net::InternetSearch intSearch;
		intSearch.m_params.m_propertyFilters.EmplaceBack( game::SessionPropertyID::RTTIHash, ( Int32 ) rep::IRTTIService::GetInstance().GetRTTIHash(), net::PropertyFilter::Operator::Equal );
		intSearch.m_params.m_maxQueryResults = 10;

		m_sessionSearch.FindSessions( intSearch, [ this ] ( red::UniquePtr<net::ISearchResult>& searchResult )
		{
			RED_FATAL_ASSERT( searchResult );
			OnSearchResults( std::move( searchResult ) );
		} );
	}

	void PreGameRequestsHandler::OnSearchResults( red::SharedPtr< net::ISearchResult > searchResult )
	{
		m_sessions.Clear();

		Uint32 sessionCount{ searchResult->GetSessionCount() };

		red::DynArray< THandle<ink::ServerInfo> > servers{ red::PoolFrame() };
		servers.Reserve( sessionCount );

		for ( Uint32 idx = 0; idx < sessionCount; ++idx )
		{
			m_sessions.EmplaceBack( searchResult, idx );

			THandle< ink::ServerInfo > serverInfo = CreateHandle< ink::ServerInfo >();
			auto session = m_sessions.Back();

			String kind;
			switch ( session.GetSessionConfig().m_multiplayer.m_sessionType )
			{
			case net::SessionType::Internet:
				kind = "WEB";
				break;
			case net::SessionType::LAN:
				kind = "LAN";
				break;
			case net::SessionType::Singleplayer:
				kind = "Single";
				break;
			default:
				kind = "N/A";
			}

			serverInfo->m_number = idx;
			serverInfo->m_kind = kind;
			serverInfo->m_hostname = session.GetHostName();
			serverInfo->m_address = "N/A";
			serverInfo->m_worldDescription = session.GetWorldName();

			servers.EmplaceBack( serverInfo );
		}

		OnServersSearchResult( servers );
	}

	void PreGameRequestsHandler::CloudQuickmatch( const String& groupToken )
	{
		net::CloudSearch cloudSearch;
		cloudSearch.m_params.m_propertyFilters.EmplaceBack( game::SessionPropertyID::RTTIHash, (Int32)rep::IRTTIService::GetInstance().GetRTTIHash(), net::PropertyFilter::Operator::Equal );
		cloudSearch.m_params.m_maxQueryResults = 1;
		cloudSearch.m_groupToken = groupToken;

		m_sessionSearch.FindSessions( cloudSearch, [ this ]( red::UniquePtr<net::ISearchResult>& searchResult )
		{
			RED_FATAL_ASSERT( searchResult );
			OnSearchResults( std::move( searchResult ) );
		} );
	}

	void PreGameRequestsHandler::JoinServer( Int32 serverId )
	{
		if ( serverId >= 0 && serverId < ( Int32 ) m_sessions.Size() )
		{
			Args transitionArgs;
			transitionArgs.Set( RED_NAME_CONSTEXPR( "gameSessionDesc" ), m_sessions[ serverId ] );
			transitionArgs.Set( RED_NAME_CONSTEXPR( "inputDeviceID" ), GetDeviceId() );

			const TransitionRequest& transitionRequest = TransitionRequest::ReplaceAll( "Session", transitionArgs );
			SetTransitionRequest( transitionRequest );
		}
	}

	void PreGameRequestsHandler::GetMultiplayerWorlds( red::DynArray<String>& worldNames )
	{
		worldNames.Clear();

		for ( const auto& info : m_mpWorldInfos )
		{
			worldNames.PushBack( info.GetDisplayName() );
		}
	}

	void PreGameRequestsHandler::GetPlayerRecordIds( red::DynArray<String>& playerRecordIds )
	{
		playerRecordIds = m_playerRecordIds;
	}

	void PreGameRequestsHandler::GetSessionTypes( red::DynArray<String>& sessionTypes )
	{
		sessionTypes = m_sessionTypes;
	}

	void PreGameRequestsHandler::GetSessionTimeLimits( red::DynArray<String>& sessionTimeLimits )
	{
		sessionTimeLimits.Clear();

		for ( Uint32 time = 1; time <= 16; time *= 2 )
		{
			sessionTimeLimits.PushBack( String::Printf( "%d", time ) );
		}
	}

	void PreGameRequestsHandler::GetSessionPlayersLimits( red::DynArray<String>& sessionPlayersLimits )
	{
		sessionPlayersLimits.Clear();
		for ( Uint32 count = 1; count <= game::MaxPlayers; ++count )
		{
			sessionPlayersLimits.PushBack( String::Printf( "%d", count ) );
		}
	}

	void PreGameRequestsHandler::GetRecords( red::DynArray<String>& records )
	{
		records.Clear();
	}

	void PreGameRequestsHandler::PlayRecord( const String& filename )
	{
	}
};